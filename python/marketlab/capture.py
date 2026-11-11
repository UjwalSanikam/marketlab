"""
MarketLab Public Market-Data Capture Collector (Binance Spot)
Schema Version: marketlab.raw.v1

Captures raw WebSocket streams (depth@100ms and trade) and REST depth snapshots
from Binance Spot for BTCUSDT without requiring API keys or trading credentials.
Records raw payloads, monotonic receipt timing, UTC timestamps, local capture
sequences, connection lifecycle events, and bounded buffer overflow markers.
"""

import sys
import os
import json
import time
import uuid
import argparse
import asyncio
import signal
from typing import Optional, Dict, Any
import urllib.request
import urllib.error

SCHEMA_VERSION = "marketlab.raw.v1"
DEFAULT_SYMBOL = "BTCUSDT"
DEFAULT_WS_URL = "wss://stream.binance.com:9443/stream"
DEFAULT_REST_URL = "https://api.binance.com/api/v3/depth"
DEFAULT_BUFFER_SIZE = 10000


def get_current_utc_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def fetch_rest_snapshot(symbol: str, limit: int = 1000) -> Dict[str, Any]:
    url = f"{DEFAULT_REST_URL}?symbol={symbol.upper()}&limit={limit}"
    req = urllib.request.Request(url, headers={"User-Agent": "MarketLab/0.1.0"})
    with urllib.request.urlopen(req, timeout=10.0) as resp:
        body = resp.read().decode("utf-8")
        return json.loads(body)


class MarketDataCollector:
    def __init__(
        self,
        symbol: str = DEFAULT_SYMBOL,
        output_file: Optional[str] = None,
        duration_sec: Optional[float] = None,
        max_messages: Optional[int] = None,
        buffer_size: int = DEFAULT_BUFFER_SIZE,
        ws_url: str = DEFAULT_WS_URL,
    ):
        self.symbol = symbol.upper()
        self.output_file = output_file
        self.duration_sec = duration_sec
        self.max_messages = max_messages
        self.buffer_size = buffer_size
        self.ws_url = ws_url

        self.session_id = uuid.uuid4().hex
        self.capture_seq = 0
        self.queue: asyncio.Queue = asyncio.Queue(maxsize=buffer_size)
        self.running = False
        self.out_fp = None
        self.total_written = 0
        self.start_mono_ns = 0

    def write_record(self, record: Dict[str, Any]):
        line = json.dumps(record, separators=(",", ":")) + "\n"
        if self.out_fp:
            self.out_fp.write(line)
            self.out_fp.flush()
        else:
            sys.stdout.write(line)
            sys.stdout.flush()
        self.total_written += 1

    def make_record(
        self,
        record_type: str,
        payload: Any,
        exchange_ts_ms: Optional[int] = None,
        source_seq: Optional[int] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        self.capture_seq += 1
        now_mono = time.monotonic_ns()
        rec = {
            "schema_version": SCHEMA_VERSION,
            "session_id": self.session_id,
            "capture_seq": self.capture_seq,
            "record_type": record_type,
            "venue": "BINANCE",
            "symbol": self.symbol,
            "recv_utc": get_current_utc_iso(),
            "recv_mono_ns": now_mono,
            "exchange_ts_ms": exchange_ts_ms,
            "source_seq": source_seq,
            "payload": payload,
        }
        if metadata:
            rec["metadata"] = metadata
        return rec

    async def writer_loop(self):
        while self.running or not self.queue.empty():
            try:
                record = await asyncio.wait_for(self.queue.get(), timeout=0.5)
                self.write_record(record)
                self.queue.task_done()
                if self.max_messages and self.total_written >= self.max_messages:
                    self.running = False
                    break
            except asyncio.TimeoutError:
                continue
            except Exception as e:
                sys.stderr.write(f"Error writing capture record: {e}\n")

    async def run(self):
        try:
            import websockets
        except ImportError:
            sys.stderr.write("Error: 'websockets' package is required. Install via pip install websockets.\n")
            sys.exit(1)

        if self.output_file:
            os.makedirs(os.path.dirname(os.path.abspath(self.output_file)), exist_ok=True)
            self.out_fp = open(self.output_file, "w", encoding="utf-8", newline="\n")

        self.running = True
        self.start_mono_ns = time.monotonic_ns()

        # 1. Record session start
        start_rec = self.make_record(
            "SESSION_START",
            {
                "ws_url": self.ws_url,
                "symbol": self.symbol,
                "duration_sec": self.duration_sec,
                "max_messages": self.max_messages,
                "buffer_size": self.buffer_size,
            },
        )
        self.write_record(start_rec)

        writer_task = asyncio.create_task(self.writer_loop())

        # Build stream URL: subscribe to @depth@100ms and @trade
        stream_path = f"{self.symbol.lower()}@depth@100ms/{self.symbol.lower()}@trade"
        combined_url = f"{self.ws_url}?streams={stream_path}"

        start_time = time.monotonic()
        reconnect_delay = 1.0

        try:
            while self.running:
                if self.duration_sec and (time.monotonic() - start_time) >= self.duration_sec:
                    break
                if self.max_messages and self.total_written >= self.max_messages:
                    break

                try:
                    conn_rec = self.make_record("CONNECTION_EVENT", {"action": "CONNECTING", "url": combined_url})
                    self.write_record(conn_rec)

                    async with websockets.connect(combined_url, ping_interval=20, ping_timeout=10) as ws:
                        open_rec = self.make_record("CONNECTION_EVENT", {"action": "CONNECTED"})
                        self.write_record(open_rec)
                        reconnect_delay = 1.0

                        # 2. Acquire initial REST snapshot concurrently after websocket is established
                        try:
                            snapshot_data = fetch_rest_snapshot(self.symbol, limit=1000)
                            snap_rec = self.make_record(
                                "SNAPSHOT",
                                snapshot_data,
                                exchange_ts_ms=None,
                                source_seq=snapshot_data.get("lastUpdateId"),
                                metadata={"rest_endpoint": DEFAULT_REST_URL, "limit": 1000},
                            )
                            self.write_record(snap_rec)
                        except Exception as e:
                            err_rec = self.make_record("ERROR", {"stage": "REST_SNAPSHOT", "error": str(e)})
                            self.write_record(err_rec)

                        # 3. Stream processing loop
                        while self.running:
                            if self.duration_sec and (time.monotonic() - start_time) >= self.duration_sec:
                                self.running = False
                                break
                            if self.max_messages and self.total_written >= self.max_messages:
                                self.running = False
                                break

                            try:
                                msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                            except asyncio.TimeoutError:
                                continue

                            try:
                                data = json.loads(msg)
                            except Exception:
                                data = {"raw": msg}

                            stream_name = data.get("stream", "")
                            inner = data.get("data", data)
                            event_type = inner.get("e", "")
                            event_ts = inner.get("E")
                            source_seq = inner.get("u") or inner.get("t")

                            rec = self.make_record(
                                "STREAM_MESSAGE",
                                data,
                                exchange_ts_ms=event_ts,
                                source_seq=source_seq,
                                metadata={"stream": stream_name, "event_type": event_type},
                            )

                            try:
                                self.queue.put_nowait(rec)
                            except asyncio.QueueFull:
                                # Explicitly record buffer overflow and data loss
                                drop_rec = self.make_record(
                                    "DATA_LOSS",
                                    {"reason": "BUFFER_OVERFLOW", "buffer_capacity": self.buffer_size},
                                )
                                self.write_record(drop_rec)
                                sys.stderr.write("CRITICAL: Market data capture buffer overflow! Marking data loss.\n")
                                self.running = False
                                break

                except (websockets.exceptions.ConnectionClosed, asyncio.TimeoutError, OSError) as e:
                    err_rec = self.make_record("CONNECTION_EVENT", {"action": "DISCONNECTED", "error": str(e)})
                    self.write_record(err_rec)
                    if not self.running:
                        break
                    sys.stderr.write(f"WebSocket disconnected ({e}). Reconnecting in {reconnect_delay}s...\n")
                    await asyncio.sleep(reconnect_delay)
                    reconnect_delay = min(reconnect_delay * 2.0, 30.0)
                except Exception as e:
                    err_rec = self.make_record("ERROR", {"stage": "WS_LOOP", "error": str(e)})
                    self.write_record(err_rec)
                    if not self.running:
                        break
                    await asyncio.sleep(reconnect_delay)

        finally:
            self.running = False
            # Finish writing remaining queued messages
            await writer_task

            # Record session end
            end_rec = self.make_record(
                "SESSION_END",
                {
                    "total_records": self.total_written + 1,
                    "elapsed_sec": (time.monotonic_ns() - self.start_mono_ns) / 1e9,
                },
            )
            self.write_record(end_rec)

            if self.out_fp:
                self.out_fp.close()
                self.out_fp = None


def main():
    parser = argparse.ArgumentParser(description="MarketLab Public Market-Data Capture (Binance Spot)")
    parser.add_argument("--symbol", default=DEFAULT_SYMBOL, help="Trading pair symbol (default: BTCUSDT)")
    parser.add_argument("--output", "-o", default=None, help="Output file path for raw JSONL capture (default: stdout)")
    parser.add_argument("--duration", "-d", type=float, default=None, help="Capture duration limit in seconds")
    parser.add_argument("--limit", "-n", type=int, default=None, help="Maximum number of messages to capture")
    parser.add_argument("--buffer-size", type=int, default=DEFAULT_BUFFER_SIZE, help="Internal bounded buffer size")
    args = parser.parse_args()

    collector = MarketDataCollector(
        symbol=args.symbol,
        output_file=args.output,
        duration_sec=args.duration,
        max_messages=args.limit,
        buffer_size=args.buffer_size,
    )

    def handle_signal(sig, frame):
        sys.stderr.write("\nInterrupted by signal. Stopping capture...\n")
        collector.running = False

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    asyncio.run(collector.run())


if __name__ == "__main__":
    main()
