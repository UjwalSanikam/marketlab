# MarketLab Market Data Protocol (Binance Spot BTCUSDT)

## 1. Selected Exchange and Instrument
- **Exchange**: Binance Spot (Public Market Data API).
- **Instrument**: `BTCUSDT` (Bitcoin / Tether spot pair).
- **Credentials**: None required. Read-only public streaming and REST depth endpoints.

---

## 2. Exchange Endpoints and Channel Semantics

### REST Depth Snapshot
- **Endpoint**: `https://api.binance.com/api/v3/depth?symbol=BTCUSDT&limit=1000`
- **Method**: HTTP GET
- **Payload Schema**:
  ```json
  {
    "lastUpdateId": 48291039401,
    "bids": [["60000.00", "1.50000"], ...],
    "asks": [["60010.00", "0.80000"], ...]
  }
  ```
- **Properties**:
  - `lastUpdateId`: Sequence identifier of the latest update included in the snapshot.
  - `bids` / `asks`: Arrays of `[price, quantity]` tuples ordered from best to worst.

### WebSocket Combined Stream
- **Endpoint**: `wss://stream.binance.com:9443/stream?streams=btcusdt@depth@100ms/btcusdt@trade`
- **Channels**:
  1. `btcusdt@depth@100ms`: Aggregated incremental order book updates pushed every 100ms.
     ```json
     {
       "stream": "btcusdt@depth@100ms",
       "data": {
         "e": "depthUpdate",
         "E": 1700000000150,
         "s": "BTCUSDT",
         "U": 48291039395,
         "u": 48291039405,
         "b": [["60000.00", "1.80000"], ["59990.00", "0.00000"]],
         "a": [["60010.00", "1.20000"]]
       }
     }
     ```
     - `U`: First update ID in this event.
     - `u`: Final update ID in this event.
     - `b` / `a`: Bid and ask level changes. Quantity `0.00000` indicates that the price level is deleted.
  2. `btcusdt@trade`: Real-time public trade execution stream.
     ```json
     {
       "stream": "btcusdt@trade",
       "data": {
         "e": "trade",
         "E": 1700000000200,
         "s": "BTCUSDT",
         "t": 32910294,
         "p": "60010.00",
         "q": "0.35000",
         "b": 10019283,
         "a": 10019290,
         "T": 1700000000200,
         "m": false,
         "M": true
       }
     }
     ```
     - `t`: Unique trade ID.
     - `p`: Executed trade price.
     - `q`: Executed trade quantity.
     - `m`: Buyer is market maker boolean flag.
       - If `m == true`: The buyer is maker; the seller is taker (Aggressive side is `SELL`).
       - If `m == false`: The seller is maker; the buyer is taker (Aggressive side is `BUY`).

---

## 3. Snapshot Synchronization Algorithm

The reconstructed order book strictly adheres to the official Binance depth synchronization protocol:

```
[ UNINITIALIZED ]
       │
       ▼ (Receive REST Snapshot)
[ SYNCHRONIZING ]
       │
       ├─► (u <= lastUpdateId) ──► Drop stale update
       ├─► (U > lastUpdateId + 1) ──► [ INVALID ] (Bridge gap)
       ▼ (U <= lastUpdateId + 1 <= u)
[ VALID ] ──► Apply levels, set lastUpdateId = u
       │
       ├─► (u <= lastUpdateId) ──► Drop duplicate
       ├─► (U <= lastUpdateId + 1) ──► Apply levels, set lastUpdateId = u
       ▼ (U > lastUpdateId + 1)
[ INVALID ] (Sequence gap detected)
```

1. **State `UNINITIALIZED`**:
   The engine starts in `UNINITIALIZED` state. WebSocket streaming updates are dropped or buffered until a REST snapshot is received.
2. **State `SYNCHRONIZING`**:
   Upon receipt of the REST snapshot, resting levels are populated, `last_update_id` is set, and status becomes `SYNCHRONIZING`.
   - Any depth update where $u \le \text{lastUpdateId}$ is dropped as stale.
   - The first valid update bridging the snapshot must satisfy:
     $$U \le \text{lastUpdateId} + 1 \le u$$
     When encountered, level updates are applied, `lastUpdateId` is updated to $u$, and status transitions to `VALID`.
   - If the first update has $U > \text{lastUpdateId} + 1$, the bridge is broken and status immediately transitions to `INVALID`.
3. **State `VALID`**:
   - Updates with $u \le \text{lastUpdateId}$ are ignored as duplicates.
   - Updates satisfying $U \le \text{lastUpdateId} + 1$ apply price level updates (adds, updates, deletions for quantity `0`) and set `lastUpdateId = u`.
   - Any gap where $U > \text{lastUpdateId} + 1$ triggers immediate transition to `INVALID`.
4. **State `INVALID`**:
   All new strategy orders are rejected with `INVALID_FEED_STATE`. In-flight cancellations remain permitted.

---

## 4. Exact Integer Decimal Scaling (No Floating-Point)

To ensure bitwise deterministic cross-platform replay and eliminate floating-point precision discrepancies, all prices and quantities are converted directly from raw string tokens to exact signed 64-bit integers:
- **Price Scale**: 2 decimal places ($10^2$ multiplier).
  $$\text{Price}(\text{ticks}) = \text{round\_down}(\text{price\_str} \times 100)$$
  *Example*: `"60000.50"` $\rightarrow$ `6000050` ticks.
- **Quantity Scale**: 5 decimal places ($10^5$ multiplier).
  $$\text{Quantity}(\text{lots}) = \text{round\_down}(\text{qty\_str} \times 100000)$$
  *Example*: `"1.50000"` $\rightarrow$ `150000` lots.
- **Sub-Tick / Sub-Lot Validation**:
  Any non-zero digits beyond target scale (e.g. `"60000.505"`) or non-numeric trailing garbage are rejected upfront.

---

## 5. Clock Semantics and Monotonic Ordering

- **Local Receipt Time (`recv_mono_ns`)**:
  Recorded using high-resolution monotonic clock (`time.monotonic_ns()`) immediately upon socket frame receipt. Local receipt time governs virtual replay scheduling and eliminates leap seconds or wall-clock skew.
- **Exchange Event Time (`exchange_ts_ms`)**:
  Exchange timestamp in UTC milliseconds recorded in metadata for observational latency analysis.
- **Virtual Replay Clock**:
  In replay mode, the virtual scheduler steps through events ordered strictly by `recv_mono_ns`. Market observations and execution reports are delivered through simulated network latencies relative to this virtual clock.

---

## 6. Buffer Limits and Fault Handling

- **Collector Bounded Queue**:
  The Python collector uses an `asyncio.Queue` capped at `buffer_size=10000`.
- **Overflow / Data Loss**:
  If the downstream disk writer stalls and the buffer fills, the collector immediately emits a canonical `DATA_LOSS` record (`record_type: "DATA_LOSS"`, `reason: "BUFFER_OVERFLOW"`).
- **Disconnection Handling**:
  WebSocket disconnections trigger explicit `CONNECTION_EVENT` records. In replay, these events transition the reconstructed order book status to `INVALID`.
