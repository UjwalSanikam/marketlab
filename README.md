# MarketLab: Deterministic Exchange Simulator & Trading Research Platform

MarketLab is a deterministic exchange simulator and trading research platform engineered to study how execution latency, queue dynamics, market data failures, and pre-trade risk controls alter algorithmic strategy outcomes, fills, and portfolio survival.

This repository implements **Milestone 1: The Deterministic Matching Engine Core**.

---

## 1. What Currently Works (Milestone 1)

- **C++20 Limit Order Book Engine**:
  - Price-time (FIFO) priority at every price level.
  - Limit orders (GTC and IOC) and Market orders.
  - Partial fills and resting residual quantities.
  - Cancellation by Order ID with $O(1)$ order index unlinking.
  - Exact integer ticks and lots with checked arithmetic (no floating-point rounding errors).
  - Explicit deterministic rejections (duplicate IDs, invalid ticks/lots, non-positive prices/quantities, unknown or completed cancellations).
- **Deterministic Command & Event Journal**:
  - Strongly typed input commands (`NewOrderCommand`, `CancelOrderCommand`) with sequential numbers.
  - Deterministic event generation (`OrderAcceptedEvent`, `TradeEvent`, `OrderCanceledEvent`, `OrderRejectedEvent`).
  - Strict absence of nondeterministic timestamps in canonical event logs.
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
  - In-memory benchmark runner (`marketlab benchmark` / `marketlab_benchmark.exe`).
- **Python Research Environment**:
  - Python virtual environment created in `.venv` with `marketlab` installed in editable mode (`python/marketlab`).

---

## 2. Order Semantics & Schema

### Matching Rules
1. **Execution Price**: Trades execute strictly at the **resting order's (maker's) price**.
2. **Limit Orders (GTC)**: Crosses available liquidity at resting maker prices. Any unfilled remainder rests on the book at its limit price in FIFO order.
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
                  +---------------------------+
```

1. **Price Levels**: `std::map<Price, PriceLevel, std::greater<Price>>` for Bids and `std::map<Price, PriceLevel, std::less<Price>>` for Asks. This guarantees strictly sorted price levels and instant access to best bid/ask.
2. **Order Queues**: `std::list<RestingOrder>` inside each level guarantees iterator stability: pointers/iterators to queued orders remain valid upon insertions and removals elsewhere.
3. **Order Index**: `std::unordered_map<OrderId, IndexEntry>` enables $O(1)$ order lookup and list unlinking upon cancellation or partial fill.
4. **Empty Level Pruning**: Levels whose constituent orders are fully depleted are immediately removed from the map.
5. **Checked Integer Arithmetic**: `CheckedMath` guards against overflow, underflow, and negative values. Prices and quantities are integer multiples of instrument ticks and lots.

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

# Build all targets (core library, CLI, test suite, and benchmark)
cmake --build build
```

### Run Automated Tests & Invariant Verification
```powershell
# Run the test suite directly
.\build\marketlab_tests.exe

# Or run via CTest
ctest --test-dir build --output-on-failure
```
All unit tests and differential tests pass with 100% success.

---

## 5. CLI Commands & Demonstration

### 1. Interactive Demo
Runs a step-by-step demonstration illustrating FIFO priority, multi-level sweeps, partial fills, cancellations, deterministic rejections, and invariant verification:
```powershell
.\build\marketlab.exe demo
```

### 2. Generate Synthetic Workload
Generates a deterministic synthetic stream of orders from a seed:
```powershell
.\build\marketlab.exe generate --seed 42 --count 10000 --output commands.jsonl
```

### 3. Replay Workload with Full Audit
Replays a saved command stream, exports events and state, displays ASCII book, and verifies all invariants:
```powershell
.\build\marketlab.exe replay --input commands.jsonl --events-out events.jsonl --state-out state.json --ascii --verify
```

### 4. Differential Verification Against Reference Matcher
Executes randomized generated workloads through both the production `OrderBook` and the naive `ReferenceMatcher`, comparing every event and state:
```powershell
.\build\marketlab.exe test-differential --seed 12345 --count 5000
```

---

## 6. Actual Benchmark Results

The benchmark measures the pure in-memory matching core separately from CLI JSON parsing and file I/O.

### Environment
- **Platform**: Windows 11 (x86_64)
- **Compiler**: Clang 23.1.4 (x86_64-w64-windows-gnu)
- **Build Mode**: Release (`-O3`, `NDEBUG`, `-static`)
- **Timer**: `std::chrono::steady_clock` (Windows QPC)

### Benchmark Workload
- **Workload size**: 1,000,000 commands
- **Seed**: 42
- **Command Mix**: 70% Limit orders (5% IOC), 10% Market orders, 20% Cancellations
- **Active Resting Book**: 8,012 orders (489 bid levels, 606 ask levels)

### Measured Results
```text
=========================================================
 MarketLab Milestone 1: In-Memory Matching Benchmark
=========================================================
1. UN-INSTRUMENTED THROUGHPUT MEASUREMENT:
  Execution time:        0.1000 s
  Throughput:            10,000,870 commands/sec (~10.0M cmds/sec)
  Event generation rate: 17,407,744 events/sec (~17.4M events/sec)
  Total events:          1,740,623
  Active book size:      8,012 resting orders (489 bid levels, 606 ask levels)
  Book invariant check:  PASS

2. INSTRUMENTED LATENCY MEASUREMENT:
  Latency statistics (per command):
    Min:         0.0 ns
    Mean:      120.7 ns
    p50:       100.0 ns  (0.100 us)
    p90:       200.0 ns  (0.200 us)
    p99:       300.0 ns  (0.300 us)
    p99.9:     600.0 ns  (0.600 us)
    Max:    7330.6 us    (initial allocator cold page faults)
=========================================================
```

### Measurement Methodology & Limitations
- **Throughput vs Latency**: Measured in distinct runs. The throughput run has zero timing instrumentation inside the hot loop. The latency run records individual command deltas.
- **Clock overhead**: High-resolution clock read on Windows adds ~15–25 ns overhead per sample in the latency run.
- **Tree-map overhead**: The current implementation uses node-allocated `std::map` and `std::list`. Milestone 6 will introduce a bounded tick-indexed flat price ladder with an order pool to eliminate node heap allocations entirely.

---

## 7. Python Virtual Environment

A Python virtual environment is set up at `.venv`:
```powershell
# Activate the virtual environment
.\.venv\Scripts\Activate.ps1

# Verify marketlab python package
python -c "import marketlab; print(marketlab.__version__)"
```

---

## 8. Git Repository & Remote

The repository is tracked with Git and configured with remote:
```text
https://github.com/UjwalSanikam/marketlab.git
```

---

## 9. Next Milestone (Milestone 2)

Milestone 2 will introduce:
1. Virtual clock and deterministic event scheduler.
2. Market maker baseline strategy (inventory-aware).
3. Fill ledger, portfolio cash/inventory tracking, and pre-trade reservation controls.
4. End-to-end simulated trading loop with zero wall-clock dependencies.
