# MarketLab: Deterministic Exchange Simulator & Trading Research Platform

MarketLab is a deterministic exchange simulator and trading research platform engineered to study how execution latency, queue dynamics, market data failures, and pre-trade risk controls alter algorithmic strategy outcomes, fills, and portfolio survival.

This repository implements **Milestone 1: The Deterministic Matching Engine Core** with comprehensive arithmetic hardening, deterministic input validation, SHA-256 reproducibility, and differential verification.

---

## 1. What Currently Works (Milestone 1 Hardened)

- **C++20 Limit Order Book Engine**:
  - Price-time (FIFO) priority at every price level.
  - Limit orders (GTC and IOC) and Market orders.
  - Partial fills and resting residual quantities.
  - Cancellation by Order ID with $O(1)$ order index unlinking.
  - Exact integer ticks and lots with checked arithmetic (`CheckedMath`).
  - Strict absence of floating-point arithmetic throughout the matching core.
  - **Deterministic Overflow Protection**: Pre-checks prospective resting residuals against side and level aggregate bounds before acceptance; rejects potential overflows upfront with `QUANTITY_OVERFLOW` without emitting partial or orphan trades.
- **Strict Input Validation & Parsing**:
  - Validates numeric ranges with strict integer conversion (`from_chars`), disallowing decimals and trailing junk.
  - Enforces explicit `TimeInForce` validation (`GTC` and `IOC` only; invalid strings such as `"INVALID"` trigger parse errors).
  - Provides line-numbered error diagnostics with nonzero exit codes upon encountering malformed input.
  - Documented file parsing policy: blank lines and `#` comment lines are intentionally ignored.
- **Deterministic Command & Event Journal**:
  - Strongly typed input commands (`NewOrderCommand`, `CancelOrderCommand`) with sequential numbers.
  - Deterministic event generation (`OrderAcceptedEvent`, `TradeEvent`, `OrderCanceledEvent`, `OrderRejectedEvent`).
  - Strict absence of nondeterministic timestamps in canonical event logs.
  - **Canonical Binary Exports & SHA-256 Reproducibility**: Export files are written in binary mode with explicit UTF-8 and LF (`0x0A`) line endings across all platforms, preventing Windows CRLF substitution. The printed SHA-256 digests hash the exact binary bytes of the exported files, matching both internal calculations and Windows `Get-FileHash`.
- **Auditing & Invariant Verification**:
  - Price level order sum equals level total quantity.
  - Book totals match constituent level totals.
  - Strict descending order for bids and ascending order for asks.
  - Crossed book prevention (`best_bid < best_ask` after every command).
  - Complete lifecycle accounting: every accepted order's initial quantity reconciles identically to `filled_qty + canceled_qty + remaining_resting_qty`.
- **Differential Testing**:
  - Independent naive reference matcher (`ReferenceMatcher`) with brute-force flat linear search.
  - Differential fuzzer that processes seeded random command streams and verifies 100% identical event streams and final book states.
- **CLI & Replay Tool (`marketlab`)**:
  - Built-in demonstration (`marketlab demo`).
  - Synthetic deterministic workload generation (`marketlab generate`).
  - Replay of saved command streams (`marketlab replay`).
  - Differential testing (`marketlab test-differential`).
  - In-memory service-time benchmark runner (`marketlab benchmark` / `marketlab_benchmark.exe`).
- **Python Research Environment**:
  - Python virtual environment created in `.venv` with `marketlab` installed in editable mode (`python/marketlab`).

---

## 2. Order Semantics & Schema

### Matching Rules
1. **Execution Price**: Trades execute strictly at the **resting order's (maker's) price**.
2. **Limit Orders (GTC)**: Crosses available liquidity at resting maker prices. Any unfilled remainder rests on the book at its limit price in FIFO order. If resting the remainder would cause aggregate side or level quantity to overflow `int64_t` max, the order is rejected upfront with `QUANTITY_OVERFLOW`.
3. **Limit Orders (IOC)**: Crosses available liquidity up to its limit price. Any unfilled remainder is immediately canceled with `IOC_REMAINDER`.
4. **Market Orders**: Crosses resting depth regardless of price. A market order never rests; any unfilled remainder (or full quantity if the book is empty) is immediately canceled with `MARKET_REMAINDER`. Market orders must carry price `0`.
5. **Cancellations**: Canceling an active order immediately unlinks it and emits `OrderCanceledEvent` with `USER_REQUESTED`. Canceling an unknown ID or an order that was already filled or canceled returns deterministic `UNKNOWN_ORDER` or `ORDER_ALREADY_FILLED_OR_CANCELLED`.
6. **Duplicate IDs**: Resubmitting an order ID that was previously used in the run returns deterministic `DUPLICATE_ORDER_ID`.

### Input Command Formats (JSON Lines)
```json
{"type":"NEW","seq":1,"id":1001,"side":"BUY","order_type":"LIMIT","price":100,"qty":50,"tif":"GTC"}
{"type":"NEW","seq":2,"id":1002,"side":"SELL","order_type":"MARKET","price":0,"qty":20,"tif":"IOC"}
{"type":"CANCEL","seq":3,"id":1001}
```

### Output Event Formats (JSON Lines)
```json
{"event":"ACCEPTED","seq":1,"event_id":1,"id":1001,"side":"BUY","type":"LIMIT","price":100,"qty":50,"tif":"GTC"}
{"event":"TRADE","seq":2,"event_id":2,"taker_id":1002,"maker_id":1001,"price":100,"qty":20,"taker_side":"SELL","maker_rem":30}
{"event":"CANCELED","seq":3,"event_id":3,"id":1001,"canceled_qty":30,"reason":"USER_REQUESTED"}
{"event":"REJECTED","seq":4,"event_id":4,"id":9999,"reason":"UNKNOWN_ORDER"}
```

---

## 3. Architecture & Design Choices

```text
                  +---------------------------+
                  |   Input Command Stream    |
                  +-------------+-------------+
                                |
                                v
                  +---------------------------+
                  |    OrderBook Validator    |
                  |  (CheckedMath & Overflow) |
                  +-------------+-------------+
                                |
                 +--------------+--------------+
                 |                             |
                 v                             v
       +-------------------+         +-------------------+
       |    Bids (Map)     |         |    Asks (Map)     |
       |  Price -> Level   |         |  Price -> Level   |
       |    (std::greater) |         |    (std::less)    |
       +---------+---------+         +---------+---------+
                 |                             |
                 +--------------+--------------+
                                |
                                v
                  +---------------------------+
                  |  Order Index (Hash Map)   |
                  |  OrderId -> IndexEntry    |
                  +-------------+-------------+
                                |
                                v
                  +---------------------------+
                  | Deterministic Event Stream|
                  |  (with Canonical SHA-256) |
                  +---------------------------+
```

1. **Price Levels**: `std::map<Price, PriceLevel, std::greater<Price>>` for Bids and `std::map<Price, PriceLevel, std::less<Price>>` for Asks.
2. **Order Queues**: `std::list<RestingOrder>` inside each level guarantees iterator stability upon insertions and removals.
3. **Order Index**: `std::unordered_map<OrderId, IndexEntry>` enables $O(1)$ order lookup and list unlinking on cancellation or partial fill.
4. **Empty Level Pruning**: Levels whose constituent orders are fully depleted are immediately removed from the map.
5. **Checked Integer Arithmetic**: `CheckedMath` guards against overflow, underflow, and negative values across individual orders, price levels, book aggregates, and lifecycle audits.

---

## 4. Build and Verification Instructions

### Prerequisites
- CMake >= 3.20
- C++20 compliant compiler (Clang / GCC / MSVC)
- Ninja build system

### Configure & Build
```powershell
# Configure Release build with Clang and Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release

# Build all targets (core library, CLI, test suite, regressions, benchmark)
cmake --build build
```

### Run Automated Tests & Regression Suite
```powershell
# Run the complete test suite and hardening regression checks via CTest
ctest --test-dir build --output-on-failure

# Or run executables directly:
.\build\marketlab_tests.exe
.\build\marketlab_regressions.exe
```
All unit tests, differential tests, and hardening regression tests pass with 100% success.

---

## 5. CLI Commands & Demonstration

### 1. Interactive Demo
Demonstrates FIFO priority, multi-level sweeps, partial fills, cancellations, deterministic rejections, and quantity overflow guards:
```powershell
.\build\marketlab.exe demo
```

### 2. Generate Synthetic Workload
Generates a deterministic synthetic stream of orders from a seed:
```powershell
.\build\marketlab.exe generate --seed 42 --count 10000 --output commands.jsonl
```

### 3. Replay Workload with SHA-256 Hashes & Invariant Audit
Replays a saved command stream, exports events and state in binary mode (strict UTF-8/LF), reports canonical SHA-256 hashes matching `Get-FileHash`, and verifies all invariants:
```powershell
.\build\marketlab.exe replay --input scratch/review/overflow.jsonl --events-out events.jsonl --state-out state.json --verify
```
Example Output:
```text
Loaded 2 commands from scratch/review/overflow.jsonl
Replay completed in 28 us (71428.6 cmds/sec)
Exported 2 events to events.jsonl
Exported final state to state.json
Canonical Events SHA-256: 9ebfd2fd9cb1c1895fb642effd23e446f8cbaffdd9de2175f342e4b3842bbc71
Canonical State  SHA-256: 80420a419497fc64066ccb846a30620ead3e7edbc4b5685b2120f363cc5d4fee
Audit verification: ALL INVARIANTS AND RECONCILIATIONS PASSED!
```
Verification via Windows PowerShell:
```powershell
(Get-FileHash events.jsonl -Algorithm SHA256).Hash -eq "9EBFD2FD9CB1C1895FB642EFFD23E446F8CBAFFDD9DE2175F342E4B3842BBC71"
# Returns: True
(Get-FileHash state.json -Algorithm SHA256).Hash -eq "80420A419497FC64066CCB846A30620EAD3E7EDBC4B5685B2120F363CC5D4FEE"
# Returns: True
```

### 4. Differential Verification Against Reference Matcher
Executes randomized generated workloads through both the production `OrderBook` and the naive `ReferenceMatcher`, comparing every event and state:
```powershell
.\build\marketlab.exe test-differential --seed 12345 --count 5000
```

---

## 6. In-Memory Service-Time Benchmark Results

The benchmark measures in-memory single-threaded service time separately from CLI JSON parsing, network stacks, and file I/O.

### Environment & Calibration
- **Platform**: Windows 11 (x86_64)
- **Compiler**: Clang 23.1.4 (x86_64-w64-windows-gnu)
- **Build Mode**: Release (`-O3`, `NDEBUG`, `-static`)
- **System Timer**: `std::chrono::steady_clock` (Windows QPC)
- **Timer Call Overhead**: ~19.9 ns / call
- **Timer Resolution**: ~100 ns minimum observable tick
- *Note on Latency Interpretation*: Zero-duration latency samples (`0.0 ns`) occur when a command completes faster than the timer tick resolution (< 100 ns).

### Workload Outcomes (1,000,000 Commands, Seed 42)
- **Total commands processed**: 1,000,000
- **Accepted new orders**: 800,033
- **Successful cancellations**: 44,142
- **Rejected cancellations**: 155,825 (`ORDER_ALREADY_FILLED_OR_CANCELLED`: 155,825)
- **Remainder cancellations**: 21,922 (IOC and Market order residuals)
- **Trades executed**: 718,701
- **Rejected new orders**: 0
- **Active resting book at finish**: 8,012 orders (489 bid levels, 606 ask levels)
- **Book invariant check**: PASS

### Repeated Throughput Trials (5 Trials)
- **Trial 1**: 7,953,052 commands/sec (0.1257 s)
- **Trial 2**: 8,025,875 commands/sec (0.1246 s)
- **Trial 3**: 8,137,849 commands/sec (0.1229 s)
- **Trial 4**: 8,548,601 commands/sec (0.1170 s)
- **Trial 5**: 7,817,924 commands/sec (0.1279 s)
- **Mean Throughput**: **8,096,660 commands/sec** (~8.1M cmds/sec)
- **Throughput Range**: [7,817,924, 8,548,601] commands/sec
- **Standard Deviation**: +/- 248,707 commands/sec (3.07%)

### Instrumented Service-Time Latency Distribution
- **Min**: 0.0 ns
- **Mean**: 143.4 ns
- **p50**: 100.0 ns (0.100 µs)
- **p90**: 200.0 ns (0.200 µs)
- **p99**: 400.0 ns (0.400 µs)
- **p99.9**: 1,200.0 ns (1.200 µs)
- **Max**: 8,460.9 µs

### Limitations
- Measurements reflect pure single-threaded in-memory service time; network latency and serialization overhead are excluded.
- The current Milestone 1 book uses node-based `std::map` and `std::list`. Tail latency reflects maximum observed duration including OS thread scheduling jitter and memory allocation pauses.
- Milestone 6 will introduce a bounded flat price ladder and order pool to eliminate tree-node dynamic allocations.

---

## 7. Python Virtual Environment

A Python virtual environment is configured at `.venv`:
```powershell
# Activate the virtual environment
.\.venv\Scripts\Activate.ps1

# Verify marketlab python package
python -c "import marketlab; print(marketlab.__version__)"
```

---

## 8. Next Milestone (Milestone 2)

Milestone 2 will introduce:
1. Virtual clock and deterministic event scheduler.
2. Market maker baseline strategy (inventory-aware).
3. Fill ledger, portfolio cash/inventory tracking, and pre-trade reservation controls.
4. End-to-end simulated trading loop with zero wall-clock dependencies.
