# MarketLab: Deterministic Exchange Simulator & Trading Research Platform

MarketLab is a deterministic exchange simulator and trading research platform engineered to study how execution latency, queue dynamics, market data failures, and pre-trade risk controls alter algorithmic strategy outcomes, fills, and portfolio survival.

This repository implements:
- **Milestone 1**: The Deterministic Matching Engine Core (C++20, price-time priority, checked arithmetic, overflow hardening, strict input parsing, and canonical SHA-256 reproducibility).
- **Milestone 2**: Deterministic Simulated Trading Session (Virtual clock and priority-queue scheduler, strategy interface, inventory-aware market maker, pre-trade atomic risk reservations, and a conservation-audited portfolio ledger).

---

## 1. Architecture Overview

MarketLab enforces complete separation between the authoritative exchange runtime and external strategy agents. Strategies cannot read or mutate the order book or ledger directly. All interactions occur across explicit, simulated network latency delays using discrete market observations and execution reports.

```text
               +-------------------------------------------------------------+
               |                  Virtual Scheduler (Min-Heap)               |
               |       SimTime (ns), 5 Priority Tiers, FIFO Tie-Breaking     |
               +------------------------------+------------------------------+
                                              |
                +-----------------------------+-----------------------------+
                |                                                           |
                v                                                           v
+-------------------------------+                           +-------------------------------+
|       Market Observations     |                           |    Inventory Market Maker     |
|   (BBO, Mid, Last Trade)      |                           |   (Avellaneda-Stoikov Skew)   |
+---------------+---------------+                           +---------------+---------------+
                | [market_data_delay]                                       | [strategy_delay]
                +------------------------------>+                           v
                                                |             +-----------------------------+
                                                |             |   Order / Cancel Intents    |
                                                |             +--------------+--------------+
                                                |                            | [order_entry_delay]
                                                v                            v
               +----------------------------------------------------------------------------+
               |                       Pre-Trade Risk Engine & Atomic Ledger                |
               |  - Order Qty & Notional Caps          - Conservative Buy Cash Reservation  |
               |  - Strict Short-Selling Prohibition   - Market Buy Protection Price Cap    |
               |  - Stale Data Gating (Age Limit)      - Self-Trade Prevention (STP)        |
               +-------------------------------------+--------------------------------------+
                                                     | (If Approved)
                                                     v
                                    +---------------------------------+
                                    |     C++20 Matching Engine       |
                                    |   (Price-Time FIFO Priority)    |
                                    +----------------+----------------+
                                                     |
                        +----------------------------+----------------------------+
                        |                                                         |
                        v                                                         v
         +-----------------------------+                           +-----------------------------+
         |     Authoritative Ledger    |                           |      Execution Reports      |
         |  - Trade Settlement at Tex  |                           |  (Accepted, Rejected, Trade)|
         |  - Cash/Inventory Conserve  |                           +--------------+--------------+
         |  - Maker/Taker Fee Split    |                                          | [exec_report_delay]
         |  - Avg Cost & Realized PnL  |                                          v
         +-----------------------------+                           +-----------------------------+
                                                                   |      Strategy Updates       |
                                                                   +-----------------------------+
```

---

## 2. Milestone 2 Capabilities

### 1. Virtual Clock & Deterministic Min-Heap Event Scheduler
- **Time Representation**: Pure integer simulated nanoseconds (`SimTime`), with checked arithmetic guarding against overflow. Wall-clock timing is prohibited in canonical simulation data. Time additions use `safe_add_time` and reject overflow rather than defaulting to immediate execution.
- **Priority Queue**: Stable min-heap ordered by `(timestamp, priority_tier, sequence_id)`.
- **Deterministic Tie-Breaking**: Equal-timestamp events are strictly ordered by documented priority tiers, followed by FIFO sequence ID:
  0. `Tier 0 (MarketDataAuthoritative)`: Authoritative market data state updates execute at observation time $T_{\text{obs}}$ before strategy visibility.
  1. `Tier 1 (MarketDataDelivery)`: Market observations delivered to strategy mailboxes at $T_{\text{obs}} + \text{delay}$.
  2. `Tier 2 (StrategyWakeup)`: Strategy periodic wakeups and timer hooks.
  3. `Tier 3 (OrderAdmission)`: Strategy order submissions and cancellations arriving at the runtime.
  4. `Tier 4 (ExecutionReportDelivery)`: Delayed fill reports, cancels, and risk rejections delivered to strategies.
  5. `Tier 5 (BackgroundArrival)`: External counterparty order flow injection.
- **Run Horizon & Loop Guards**: Prevents unbounded same-time loops with explicit run horizons (`run_horizon_ns`), event count limits (`max_events`), and explicit termination reasons (`TimeHorizonReached`, `EventLimitReached`, `AllEventsProcessed`).

### 2. Strategy Interface & Inventory-Aware Market Maker
- **Strategy Interface (`IStrategy`, `IStrategyContext`)**:
  - Receives only delivered `MarketObservation` records and `ExecutionReport` events.
  - Submits order and cancellation intents through the runtime context.
  - Has zero direct access to the order book, counterparties, or authoritative balances.
- **Inventory-Aware Market Maker (`InventoryAwareMarketMaker`)**:
  - Calculates reservation price skewed by inventory deviation:
    $$\text{Price}_{\text{reservation}} = \text{Mid} - (\text{Inventory}_{\text{current}} - \text{Inventory}_{\text{target}}) \times \text{SkewPerLot}$$
  - Places symmetric bid and ask quotes around the reservation price:
    $$\text{Bid} = \text{Price}_{\text{reservation}} - \text{HalfSpread}, \quad \text{Ask} = \text{Price}_{\text{reservation}} + \text{HalfSpread}$$
  - Dynamically updates active resting quotes and cancels stale quotes. Quoting cleanly halts if `quote_size <= 0`.

### 3. Pre-Trade Risk Engine & Atomic Reservations
- **Order Size & Notional Limits**: Hard caps on single-order quantity (`max_order_qty`) and notional value (`max_order_notional`).
- **Strict Short-Selling Prohibition**: Sell orders are rejected if requested quantity exceeds available (unreserved) inventory (`available_inventory < qty`).
- **Projected Inventory Limit**: Buy orders verify that settled inventory plus outstanding pending buy orders plus the incoming order quantity does not breach the inventory cap:
  $$\text{Inventory}_{\text{settled}} + \text{Quantity}_{\text{pending\_buys}} + \text{Quantity}_{\text{new}} \le \text{MaxInventory}$$
  Capacity is released atomically upon fills, cancellations, and risk rejections.
- **Conservative Cash Reservation for Buys**: Buy orders atomically reserve cash covering notional plus the maximum applicable maker/taker fee rate using 128-bit checked arithmetic:
  $$\text{FeeRes} = \left\lceil \frac{\text{Notional} \times \max(\text{Fee}_{\text{maker}}, \text{Fee}_{\text{taker}})}{10,000} \right\rceil$$
- **Protected Market-Buy Execution**: Market buys require an explicit protection price cap. During runtime execution, market buys execute through protected IOC limit semantics bounded at the protection cap, preventing unexpected sweeps of deep resting liquidity and ensuring account cash never drops below zero.
- **Market Data Staleness Guard**: Orders are rejected if simulated current time exceeds the latest observation timestamp plus `max_data_age_ns`.
- **Deep-Book Self-Trade Prevention (STP)**: Rejects crossing orders across the entire executable depth and across subsequent orders at the same price level.
- **Cancel Ownership Verification**: Prevents unauthorized cancellation of orders belonging to other accounts.

### 4. Portfolio Ledger & Conservation Accounting
- **Authoritative vs. Observed Separation**: Balances and reservations are authoritatively settled at matching execution time $T_{\text{exec}}$, while the strategy's internal position representation updates only when the delayed execution report arrives at $T_{\text{exec}} + \text{execution\_report\_delay}$.
- **Pure Integer Cost Basis & Capitalized Buy Fees**:
  - Zero floating-point calculations in monetary balances and cost accounting.
  - Buy-side trading fees are capitalized directly into inventory total acquisition cost:
    $$\text{TotalCost}_{\text{new}} = \text{TotalCost}_{\text{prev}} + \text{Notional}_{\text{buy}} + \text{Fee}_{\text{buy}}$$
    $$\text{CostBasisPerLot} = \lfloor \text{TotalCost} / \text{Inventory} \rfloor$$
  - Realized PnL is computed on sales using exact integer cost-of-goods-sold and net proceeds:
    $$\text{RealizedPnL} = (\text{Notional}_{\text{sell}} - \text{Fee}_{\text{sell}}) - \left\lfloor \frac{\text{TotalCost} \times \text{Qty}_{\text{sold}}}{\text{Inventory}_{\text{prev}}} \right\rfloor$$
  - Net cash change matches realized PnL identically over full trading cycles.
- **Comprehensive Ledger Invariant Verification**:
  - Global cash and inventory conservation: $\sum \Delta \text{Cash} = 0, \sum \Delta \text{Inventory} = 0$.
  - Non-negative available cash and available inventory for all participant accounts.
  - Exact order-to-reservation reconciliation: $\text{ReservedCash} \equiv \sum \text{OrderCashRes}$, $\text{ReservedInventory} \equiv \sum \text{OrderInvRes}$.
- **Duplicate Fill & Cancellation Idempotence**: Duplicate trade execution reports and unauthorized or unknown order cancels are rejected without altering ledger state.

---

## 3. Worked Trading Example & Balance Reconciliation

Consider a session with three accounts: Buyer (`ALICE`), Seller (`BOB`), and `EXCHANGE_FEES`. Fee rates are 10 bps maker (0.10%) and 20 bps taker (0.20%).

```text
Initial Balances:
  ALICE:          Cash = 10,000, Inventory =   0
  BOB:            Cash =      0, Inventory = 100
  EXCHANGE_FEES:  Cash =      0, Inventory =   0
  Total System:   Cash = 10,000, Inventory = 100
```

1. **Order 1 (Alice Limit Buy)**: Alice submits Buy 10 @ 100 (GTC).
   - Notional = 1,000. Conservative taker fee = $1,000 \times 20 / 10,000 = 2$.
   - Pre-trade risk reserves 1,002 cash. Alice available cash becomes 8,998. Order rests at 100.
2. **Order 2 (Bob Crossing Sell)**: Bob submits Sell 10 @ 100 (IOC).
   - Pre-trade risk checks Bob inventory ($10 \le 100$); reserves 10 inventory. Bob available inventory becomes 90.
3. **Execution & Settlement ($T_{\text{exec}}$)**:
   - Matcher crosses orders at resting price 100.
   - Trade: 10 lots @ 100 = 1,000 notional.
   - Taker (Bob) fee = $1,000 \times 20 / 10,000 = 2$.
   - Maker (Alice) fee = $1,000 \times 10 / 10,000 = 1$.
   - Alice cash deducted: $1,000 + 1 = 1,001$. Alice inventory: $+10$.
   - Bob cash credited: $1,000 - 2 = 998$. Bob inventory: $-10$.
   - Fee account cash credited: $1 + 2 = 3$.

### Balance Table

| Account | Metric | Before Trade | Reservation | After Trade & Settlement | Net Change |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **ALICE** (Buyer/Maker) | Cash | 10,000 | 1,002 reserved (8,998 avail) | 8,999 (0 reserved) | **-1,001** |
| | Inventory | 0 | 0 | 10 | **+10** |
| | Avg Cost Basis | 0.0 | - | 100.0 | - |
| | Fees Paid | 0 | - | 1 | +1 |
| **BOB** (Seller/Taker) | Cash | 0 | 0 | 998 | **+998** |
| | Inventory | 100 | 10 reserved (90 avail) | 90 (0 reserved) | **-10** |
| | Fees Paid | 0 | - | 2 | +2 |
| **EXCHANGE_FEES** | Cash | 0 | 0 | 3 | **+3** |
| | Inventory | 0 | 0 | 0 | 0 |
| **Total System** | **Cash** | **10,000** | - | **10,000** | **0 (Conserved)** |
| | **Inventory** | **100** | - | **100** | **0 (Conserved)** |

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

# Build all targets (core library, CLI, tests, regressions, benchmark)
cmake --build build
```

### Run Full Test Suite via CTest
```powershell
ctest --test-dir build --output-on-failure
```
Outputs:
```text
Test project D:/finance/build
    Start 1: AllTests
1/3 Test #1: AllTests .........................   Passed    0.02 sec
    Start 2: RegressionTests
2/3 Test #2: RegressionTests ..................   Passed    0.01 sec
    Start 3: SimTests
3/3 Test #3: SimTests .........................   Passed    0.01 sec

100% tests passed out of 3
```

Direct executables:
- `.\build\marketlab_tests.exe`: Milestone 1 engine tests (FIFO priority, partial fills, cancellations).
- `.\build\marketlab_regressions.exe`: Milestone 1 hardening regression tests (quantity overflow, invalid TIF, malformed inputs).
- `.\build\marketlab_sim_tests.exe`: Milestone 2 simulation tests (tie-breaking, cash competition, short-selling prevention, fees, PnL, latencies, and SHA-256 reproducibility).

---

## 5. CLI Usage & Simulated Session Demonstration

### 1. Run Interactive Simulation Demo
Runs an end-to-end simulated trading session with the inventory-aware market maker quoting around a dynamic midpoint:
```powershell
.\build\marketlab.exe sim --demo
```
Example Output:
```text
=========================================================
  MarketLab Milestone 2 Simulation Interactive Demo
=========================================================
Strategy: InventoryAwareMarketMaker (Account: MM)
Starting Balances: 1000000 cash, 100 inventory
Latencies: MD=5ms, Strat=2ms, OE=3ms, ER=10ms
Simulating deterministic trading session...

=========================================================
  MarketLab Simulation Session Summary
=========================================================
Simulated Time:      9.945 s (9945000000 ns)
Termination Reason:  ALL_EVENTS_PROCESSED
Total Events:        803 events in 250 us service time

--- Invariant & Conservation Auditing ---
Cash Conservation:      [PASS] (Initial: 11000000, Final: 11000000)
Inventory Conservation: [PASS] (Initial: 10100, Final: 10100)

--- Strategy Performance (Account: MM) ---
Orders Submitted:    69
Orders Accepted:     69
Risk Rejections:     0
Trades Executed:     13
Final Cash:          984534
Final Inventory:     202 lots
Realized PnL:        0
Unrealized PnL:      5542
Total PnL:           5542
Trading Fees Paid:   11

--- Exchange & Ledger Totals ---
Total Trades:        13
Total Volume Traded: 102 lots
Total Notional:      15455
Total Fees Accrued:  36 (Account: EXCHANGE_FEES)

--- Canonical SHA-256 Hashes ---
Events SHA-256:   072eabb6a54f3ecc7ea3d652a714d92332261b5e809a387009f42c9c529c2c94
Fills SHA-256:    0ac8b44b4a2f40ba5e4be94fb46f2ea7e7d27d5bad55823ba7b12ef26f38d464
State SHA-256:    3ff7eadcb1792931e2ec00125efd5eda203d5b772084146b9f36bfe47a0826bb
Manifest SHA-256: 3e2dbd3ec95802708f0d0edad56216186d182d7988e03a69c104526011fc12f2
=========================================================
```

### 2. Export Canonical Logs & Verify with SHA-256
Run a session exporting canonical binary logs (written in strict UTF-8 with LF `0x0A` bytes) and verify against Windows `Get-FileHash`:
```powershell
.\build\marketlab.exe sim --config data/demo_session.json `
    --events-out sim_events.jsonl `
    --fills-out sim_fills.jsonl `
    --state-out sim_state.json `
    --manifest-out sim_manifest.json `
    --verify
```
Verify matching digests via PowerShell:
```powershell
Get-FileHash sim_events.jsonl, sim_fills.jsonl, sim_state.json, sim_manifest.json | Format-List
```
All digests match the printed simulation output byte-for-byte.

### 3. Milestone 1 Core Commands (Preserved)
- **Interactive Matcher Demo**: `.\build\marketlab.exe demo`
- **Workload Generator**: `.\build\marketlab.exe generate --seed 42 --count 10000 --output commands.jsonl`
- **Deterministic Replay**: `.\build\marketlab.exe replay --input scratch/review/overflow.jsonl --verify`
- **Differential Fuzzing**: `.\build\marketlab.exe test-differential --seed 12345 --count 5000`
- **In-Memory Benchmark**: `.\build\marketlab_benchmark.exe 100000 42`

---

## 6. Simulation Configuration Format (`data/demo_session.json`)

```json
{
  "seed": 42,
  "run_horizon_ns": 10000000000,
  "max_events": 500000,
  "instrument": {
    "symbol": "BTC-USDT",
    "tick_size": 1,
    "lot_size": 1
  },
  "fees": {
    "maker_fee_bps": 10,
    "taker_fee_bps": 20,
    "fee_account_id": "EXCHANGE_FEES"
  },
  "latencies": {
    "market_data_delay_ns": 5000000,
    "strategy_delay_ns": 2000000,
    "order_entry_delay_ns": 3000000,
    "execution_report_delay_ns": 10000000
  },
  "risk": {
    "max_order_qty": 1000,
    "max_order_notional": 1000000,
    "max_inventory": 5000,
    "max_data_age_ns": 10000000000,
    "allow_self_trade": false
  },
  "strategy": {
    "quote_size": 10,
    "half_spread": 2,
    "inventory_target": 100,
    "skew_per_lot": 1,
    "quote_interval_ns": 100000000
  },
  "strategy_account_id": "MM",
  "strategy_starting_cash": 1000000,
  "strategy_starting_inventory": 100,
  "background_account_id": "BACKGROUND",
  "background_starting_cash": 10000000,
  "background_starting_inventory": 10000
}
```

---

## 7. Next Milestones (Roadmap)

- **Milestone 3**: Historical Market Data Replay & Data Ingestion Pipeline (Parquet / L2 books, synthetic noise injection, clock synchronization).
- **Milestone 4**: Event-Driven Backtester & Execution Model Calibration (fill simulation models, adverse selection, market impact modeling).
- **Milestone 5**: Real-Time Streaming & Fault Injection (WebSocket / IPC feeds, dropped packet emulation, network partitions, queue delay distributions).
- **Milestone 6**: High-Performance Architecture Optimization (flat price-ladder, memory pools, zero-allocation ring buffers, SIMD-accelerated book ops).
