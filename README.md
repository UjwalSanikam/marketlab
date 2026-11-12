# MarketLab: Deterministic Exchange Simulator & Trading Research Platform

MarketLab is a deterministic exchange simulator and trading research platform engineered to study how execution latency, queue dynamics, market data failures, and pre-trade risk controls alter algorithmic strategy outcomes, fills, and portfolio survival.

This repository implements:
- **Milestone 1**: The Deterministic Matching Engine Core (C++20, price-time priority, checked arithmetic, overflow hardening, strict input parsing, and canonical SHA-256 reproducibility).
- **Milestone 2**: Deterministic Simulated Trading Session (Virtual clock and priority-queue scheduler, strategy interface, inventory-aware market maker, pre-trade atomic risk reservations, and a conservation-audited portfolio ledger).
- **Milestone 3**: Real Market-Data Capture, Reconstructed Order Book, Historical Replay, & Conservative Execution Model (Public Binance Spot BTCUSDT raw capture, exact integer decimal scaling, official snapshot synchronization, queue-ahead depletion, aggressive liquidity consumption, and double-entry conservation).
- **Milestone 4**: Event-Driven Backtester & Execution Model Calibration (Configurable passive queue position, aggressive market impact, gross/net P&L, fees, turnover, fill rate, drawdown, post-fill markouts across configurable horizons, chronological calibration/holdout validation, and bitwise reproducible exports).

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
1/4 Test #1: AllTests .........................   Passed    0.03 sec
    Start 2: RegressionTests
2/4 Test #2: RegressionTests ..................   Passed    0.26 sec
    Start 3: SimTests
3/4 Test #3: SimTests .........................   Passed    0.03 sec
    Start 4: HistTests
4/4 Test #4: HistTests ........................   Passed    0.01 sec

100% tests passed out of 4
```

Direct executables:
- `.\build\marketlab_tests.exe`: Milestone 1 engine tests (FIFO priority, partial fills, cancellations).
- `.\build\marketlab_regressions.exe`: Milestone 1 hardening regression tests (quantity overflow, invalid TIF, malformed inputs).
- `.\build\marketlab_sim_tests.exe`: Milestone 2 simulation tests (tie-breaking, cash competition, short-selling prevention, fees, PnL, latencies, and SHA-256 reproducibility).
- `.\build\marketlab_hist_tests.exe`: Milestone 3 historical replay tests (snapshot sync protocol, sequence gaps, stales, deletes, queue-ahead depletion, liquidity consumption, double-entry ledger reconciliation, and bitwise repeatability).

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

## 6. Simulation Configuration Schema & Validation Rules

Simulation sessions are configured via a strictly validated JSON document (e.g., `data/demo_session.json`).

### 1. Document Structure & Default Values
All fields in the configuration are optional. When omitted, standard defaults are applied. When present, values must strictly adhere to the documented types and constraints:

| Scope | Field Key | Type | Default Value | Constraints / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Root** | `seed` | `uint64` | `42` | RNG seed for synthetic market flow generation. |
| | `run_horizon_ns` | `uint64` | `10,000,000,000` (10s) | Simulation duration horizon in nanoseconds (must be > 0). |
| | `max_events` | `uint64` | `500,000` | Safety ceiling on processed scheduler events (must be > 0). |
| | `strategy_account_id` | `string` | `"MM"` | Strategy participant account identifier. |
| | `strategy_starting_cash` | `int64` | `1,000,000` | Initial capital credited to strategy ledger. |
| | `strategy_starting_inventory` | `int64` | `100` | Initial position lots credited to strategy ledger (>= 0). |
| | `background_account_id` | `string` | `"BACKGROUND"` | External counterparty account identifier. |
| | `background_starting_cash` | `int64` | `10,000,000` | Initial capital credited to background ledger. |
| | `background_starting_inventory` | `int64` | `10,000` | Initial position lots credited to background ledger (>= 0). |
| **`instrument`** | `symbol` | `string` | `"BTC-USDT"` | Symbol identifier. |
| | `tick_size` | `int64` | `1` | Minimum price increment (must be > 0). |
| | `lot_size` | `int64` | `1` | Minimum quantity increment (must be > 0). |
| **`fees`** | `maker_fee_bps` | `int64` | `0` | Maker fee in basis points (1 bps = 0.01%, must be >= 0). |
| | `taker_fee_bps` | `int64` | `0` | Taker fee in basis points (must be >= 0). |
| | `fee_account_id` | `string` | `"FEE_ACCOUNT"` | Exchange account accumulating trading fee deductions. |
| **`latencies`** | `market_data_delay_ns` | `uint64` | `0` | Delay from market event generation to strategy delivery. |
| | `strategy_delay_ns` | `uint64` | `0` | Simulated strategy computation latency. |
| | `order_entry_delay_ns` | `uint64` | `0` | Network latency from order submission to exchange gateway. |
| | `execution_report_delay_ns`| `uint64` | `0` | Network latency for fill / cancel reports back to strategy. |
| **`risk`** | `max_order_qty` | `int64` | `1,000,000` | Hard limit on quantity per order (must be > 0). |
| | `max_order_notional` | `int64` | `1,000,000,000` | Hard cap on notional value per order (must be > 0). |
| | `max_inventory` | `int64` | `5,000,000` | Cap on net inventory exposure (must be > 0). |
| | `max_data_age_ns` | `uint64` | `10,000,000,000` (10s)| Maximum market data age before gating new orders. |
| | `allow_self_trade` | `bool` | `false` | When false, checks full executable depth for self-trade. |
| **`strategy`** | `quote_size` | `int64` | `5` | Lot size per quote order (quoting disabled if <= 0). |
| | `half_spread` | `int64` | `2` | Half-spread offset from calculated reservation price. |
| | `inventory_target` | `int64` | `0` | Target inventory level for reservation price skew. |
| | `skew_per_lot` | `int64` | `1` | Reservation price shift in ticks per unit inventory deviation. |
| | `quote_interval_ns` | `uint64` | `1,000,000,000` (1s) | Minimum interval between quote updates. |

### 2. Strict Validation & Error Handling Rules
Configuration parsing is enforced by an internal RFC 8259-compliant JSON parser (`JsonParser` in `include/marketlab/json.hpp`):
- **Object Root Required**: Root element must be a JSON object (`{...}`). Arrays, strings, numbers, booleans, or null at the root are rejected immediately.
- **Malformed Syntax**: Unterminated strings, invalid literals, unquoted identifiers, or trailing commas fail with line and column diagnostics.
- **No Trailing Garbage**: Extraneous tokens after the closing brace of the root object are rejected.
- **Duplicate Keys Prohibited**: Any duplicate key inside an object produces an immediate parse error.
- **Strict Typing**: Fields must strictly match their expected JSON types (e.g., boolean fields cannot accept strings or numbers; integer fields cannot accept floating-point numbers or strings).
- **No Unknown Keys**: Any unknown key at the root or within nested objects (`instrument`, `fees`, `latencies`, `risk`, `strategy`) is rejected with a descriptive error naming the offending path.
- **Omitted vs. Invalid Distinction**: Defaults apply exclusively to omitted optional fields. An explicitly supplied invalid value (e.g. `tick_size = 0`, `maker_fee_bps = -5`, or `strategy_account_id == background_account_id`) is strictly rejected before the simulation starts and returns a non-zero exit code.

### 3. Example Configuration File (`data/demo_session.json`)
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

## 7. Milestone 3: Real Market-Data Capture, Reconstruction & Historical Replay

Milestone 3 implements end-to-end ingestion of public cryptocurrency market data, deterministic order-book reconstruction, historical strategy replay, and a conservative execution model.

- **Market Data Protocol Specification**: [docs/market_data_protocol.md](docs/market_data_protocol.md)
- **Historical Execution Model Specification**: [docs/historical_execution_model.md](docs/historical_execution_model.md)

### Key Capabilities
1. **Public Binance Spot Capture Collector**:
   - Python-based collector (`python/marketlab/capture.py` / `marketlab capture`) connecting to Binance Spot `BTCUSDT` public endpoints (`wss://stream.binance.com:9443` and `https://api.binance.com/api/v3/depth`).
   - Zero credentials or trading permissions required.
   - Records monotonic receipt timestamps (`recv_mono_ns`), UTC timestamps (`recv_utc`), sequence numbers, connection lifecycle events, and bounded buffer overflow markers.
2. **Order Book Reconstruction Protocol**:
   - Implements the official Binance snapshot-and-delta synchronization protocol.
   - Detects and drops stale updates ($u \le \text{lastUpdateId}$) and duplicate updates.
   - Detects sequence gaps ($U > \text{lastUpdateId} + 1$) and marks feed status `INVALID`.
   - Reconstructs depth levels (adds, updates, deletions for quantity `0`) without crossed books or negative quantities.
3. **Exact Integer Decimal Arithmetic (Zero Floats)**:
   - String-based exact integer parser (`DecimalParser`) converts prices and quantities directly to 64-bit integer ticks and lots:
     - Price Scale: 2 (1 tick = 0.01 USDT; e.g. `"60000.50"` $\rightarrow$ `6000050`).
     - Quantity Scale: 5 (1 lot = 0.00001 BTC; e.g. `"1.50000"` $\rightarrow$ `150000`).
   - Automatically rejects sub-tick/sub-lot fractional amounts and non-numeric characters.
4. **Conservative Historical Execution Adapter**:
   - **Aggressive Orders**: Take visible resting liquidity from top-of-book depth. Hypothetically consumes liquidity across multiple price levels, resetting consumption when subsequent exchange depth updates arrive.
   - **Passive Orders**: Join the back of the queue at the time of admission with `queue_ahead` equal to visible resting depth. Depletes `queue_ahead` strictly matching public trade prices and volumes.
   - **Fill Capping**: Passive fills cannot exceed remaining public trade volume.
   - **Self-Trade Prevention (STP)**: Strategy orders crossing against own resting passive orders are atomically rejected with `SELF_TRADE_PROHIBITED`.
5. **Authoritative Double-Entry Accounting**:
   - Uses designated counterparty `"HISTORICAL_MARKET"` to balance all trade settlements.
   - Conserves total cash and total inventory across all accounts, verified via `Ledger::verify_all_invariants()`.
6. **Virtual Time Causality & Canonical Exports**:
   - Events are stepped deterministically by monotonic receipt time.
   - Canonical binary exports (UTF-8 bytes with explicit LF `0x0A` line endings) and SHA-256 manifests guarantee bitwise cross-platform reproducibility.

### Milestone 3 CLI Commands

#### 1. Reconstruct Raw Capture Stream
Normalizes raw JSONL capture data into canonical integer ticks/lots:
```powershell
.\build\marketlab.exe reconstruct --input data\fixtures\valid_sync.jsonl --output data\fixtures\valid_sync_norm.jsonl
```
Output:
```text
Reconstructed 5 normalized events.
Output: data\fixtures\valid_sync_norm.jsonl
Normalized SHA-256: aa217ab15454207e484736dc03aed9ece711183bf2297108266c12e85133fcf9
```

#### 2. Deterministic Historical Replay
Replays normalized market data through strategy execution, risk evaluation, conservative fills, and ledger settlement:
```powershell
.\build\marketlab.exe replay-hist --input data\fixtures\valid_sync_norm.jsonl `
  --events-out events.jsonl --fills-out fills.jsonl --state-out state.json --manifest-out manifest.json --verify
```
Output:
```text
=========================================================
  MarketLab Historical Replay Summary
=========================================================
Feed Status:          VALID
Final Update ID:      115
Order Book Levels:    Bids=4, Asks=3
Top of Book:          Bid=6000200 | Ask=6000900
Events Processed:     20 in 48 us
Simulated Time:       1.0005 s

--- Strategy Execution (Account: MM) ---
Orders Submitted:     4
Orders Accepted:      2
Risk Rejections:      2
Aggressive Trades:    0
Passive Trades:       1
Total Volume Traded:  5 lots
Realized PnL:         29972508
Total PnL (w/ Mark):  600054969758
Fees Paid:            30002

--- Double-Entry Ledger Conservation ---
Cash Conservation:      [PASS] (100100000000 == 100100000000)
Inventory Conservation: [PASS] (1000100000 == 1000100000)

--- Canonical SHA-256 Hashes ---
Events SHA-256:   aa217ab15454207e484736dc03aed9ece711183bf2297108266c12e85133fcf9  (events.jsonl)
Fills SHA-256:    33290fb5cd5d263c653838e0474dede2b16e6bf9fbc25b2ec2112c3dc06ca5db  (fills.jsonl)
State SHA-256:    cb71799f2d857a088baf975fb3f637092d489a1945308c228d48dd799359e518  (state.json)
Manifest SHA-256: f297172398b7ba528aad2bee9697aef7dc36c1b6c9fe276f8f4a88188e98d8ac  (manifest.json)
=========================================================
Audit verification: ALL CONSERVATION CHECKS PASSED!
```

#### 3. Interactive Historical Execution Demo
Runs the built-in offline demonstration exhibiting book reconstruction, aggressive IOC sweeps across multiple ask levels, passive order placement with queue-ahead, and queue-ahead depletion from public trades:
```powershell
.\build\marketlab.exe demo-hist
```

#### 4. Live Market Data Capture
Captures live public Binance Spot BTCUSDT WebSocket streaming messages and REST depth snapshots into raw JSONL:
```powershell
.\build\marketlab.exe capture --duration 5 --output capture_btc.jsonl
```

---

## 8. Milestone 4 Capabilities: Event-Driven Backtesting & Calibration

Milestone 4 introduces an event-driven backtesting engine (`marketlab backtest`) and a calibration workflow (`marketlab calibrate`) with explicit execution model assumptions, comprehensive reporting, and out-of-sample holdout validation.

### 1. Configurable Execution Model Assumptions
- **Passive Queue Position Models (`--queue-model`, `--queue-pct`)**:
  - `CONSERVATIVE` (100% visible depth ahead): Order joins the back of the queue; fills only after all pre-existing depth trades.
  - `OPTIMISTIC` (0% queue ahead): Order assumes front-of-queue priority; fills immediately on any touching public trade.
  - `PROPORTIONAL` (`--queue-pct <0-100>`): Assumes a configurable fraction of resting depth ahead (e.g. 50% for middle-of-queue priority).
- **Aggressive Impact & Slippage Models (`--impact-model`, `--slippage-ticks`, `--impact-ticks`)**:
  - `NONE`: Baseline execution against visible reconstructed book prices.
  - `FIXED_SLIPPAGE`: Applies a deterministic tick penalty to aggressive fills.
  - `LINEAR_IMPACT`: Penalizes fill price proportional to order lot size.
  - *Protection Price Enforced*: Simulated execution prices cannot exceed the client's limit price or protected market-buy cap.

### 2. Comprehensive Financial & Risk Metrics
- **Financial Performance**: Gross P&L, Net P&L, Realized P&L, Unrealized P&L, Trading Fees, Turnover.
- **Order & Trade Profile**: Orders Submitted/Accepted/Rejected/Canceled, Aggressive/Passive Trades, Filled Volume, Quantity & Order Fill Rates.
- **Risk & Drawdown Profile**: Inventory range (Min, Max, Peak Exposure), Peak Equity, Max Drawdown (ticks and %).
- **Double-Entry Ledger Audit**: Rigorous verification that total system cash and inventory are conserved across all simulation events.

### 3. Post-Fill Markout Analysis
Post-fill markouts evaluate price movement at configurable horizons ($T_{\text{fill}} + H$, default: 100ms, 1s, 5s):
- **Completed**: Target timestamp occurred within the session and a valid mid price was observed.
  - Buy fill: $\text{markout\_ticks} = M - P$
  - Sell fill: $\text{markout\_ticks} = P - M$
  - Basis points: $\text{markout\_bps} = \frac{\text{markout\_ticks}}{P} \times 10{,}000$
- **Unfinished**: Target timestamp extends beyond the session horizon.
- **MissingMark**: Target timestamp was reached, but the feed status was invalid or book was unpopulated.
- **Causality Guarantee**: Markout metrics are computed from chronologically recorded book state without lookahead or feedback into strategy logic.

### 4. Calibration & Holdout Validation Workflow
- **Chronological Split**: Input events are divided at timestamp $T_{\text{split}} = T_{\text{start}} + \lfloor (T_{\text{end}} - T_{\text{start}}) \times R \rfloor$ (default: 60% train / 40% holdout).
- **In-Sample Parameter Selection**: Evaluates candidate grid on calibration data and ranks by objective (`net_pnl`, `gross_pnl`, `fill_rate`, `min_drawdown`).
- **Holdout Validation**: The winning candidate and baseline are evaluated out-of-sample on holdout data. To maintain valid feed state, an authoritative reconstructed snapshot is synthesized at $T_{\text{split}}$ and prepended to holdout events, while strategy balances are reset to initial capital.
- **Sensitivity Analysis Labeling**: If public trade data produces zero fills across all candidates, reports are explicitly labeled as "Execution Model Sensitivity Analysis".

### Milestone 4 CLI Commands

#### 1. Deterministic Strategy Backtest
```powershell
.\build\marketlab.exe backtest --input data\fixtures\calibration_fixture_norm.jsonl `
  --events-out events.jsonl --fills-out fills.jsonl --state-out state.json `
  --report-out report.json --manifest-out manifest.json --verify
```
Output:
```text
=========================================================
  MarketLab Event-Driven Backtest Summary Report
=========================================================
Strategy:             ReferenceStrategy (Account: MM)
Instrument:           BTCUSDT (tick=1, lot=1)
Execution Model:      Queue=CONSERVATIVE (100%) | Impact=NONE
Simulated Duration:   15.00 s (117 events processed)
Feed Final Status:    VALID

--- Financial Performance ---
Starting Equity:      610010000000
Ending Equity:        609564980000
Gross P&L:            599564980000 ticks
Net P&L:              599084960000 ticks
Realized P&L:         113757980000
Unrealized P&L:       485807000000
Trading Fees Paid:    480020000
Traded Turnover:      240010000000 notional

--- Order & Execution Activity ---
Orders Submitted:     30
Orders Accepted:      11
Orders Rejected:      19
Orders Canceled:      6
Total Trades:         4 (Aggressive=4, Passive=0)
Total Lots Filled:    40000 lots / 300000 submitted
Quantity Fill Rate:   13.33 %
Order Fill Rate:      13.33 %

--- Risk & Drawdown Profile ---
Inventory Range:      Min=90000 | Max=100000 | Peak Exposure=100000 lots
Final Inventory:      100000 lots
Peak Equity:          610020000000
Max Drawdown:         455020000 ticks (0.07 %)

--- Double-Entry Ledger Conservation ---
Cash Conservation:      [PASS] (Initial: 110000000000, Final: 110000000000)
Inventory Conservation: [PASS] (Initial: 1000100000, Final: 1000100000)

--- Post-Fill Markout Analysis ---
Horizon   Completed   Unfinished  MissingMark Mean (ticks)  Mean (bps)  VWAP (ticks)  VWAP (bps)  
----------------------------------------------------------------------------------------
100ms     3           1           0           250.00        0.42        250.00        0.42        
1s        3           1           0           233.33        0.39        233.33        0.39        
5s        3           1           0           216.67        0.36        216.67        0.36        
=========================================================

--- Canonical SHA-256 Hashes ---
Events SHA-256:   066c84f4fd15f43bd2fac741fc725200ae6834ba95add1d6fb6d17c21f981ff9  (events.jsonl)
Fills SHA-256:    ab44c929065a4323865ad6f70593c1a3041c7677e3703e0eaf5b896fd2e8d7ad  (fills.jsonl)
State SHA-256:    7c3ec302ad926d636552d107da2451cfe5c25acb5b2cc83f0b08641c6b4d46ba  (state.json)
Report SHA-256:   25083464cd4c0d75cb9c3549a07fbd075e84f0cda811671b1aedba32305eda03  (report.json)
Manifest SHA-256: 9135473339def5593ca49b66f40ec68dbc23a1b22d196d2ee63c0aa1b312cdc4  (manifest.json)
Runtime:          105 us
=========================================================
Audit verification: ALL CONSERVATION CHECKS PASSED!
```

#### 2. Parameter Calibration & Out-of-Sample Holdout Validation
```powershell
.\build\marketlab.exe calibrate --input data\fixtures\calibration_fixture_norm.jsonl `
  --split 0.6 --objective net_pnl --report-out calibration_report.json --verify
```
Output:
```text
================================================================================
  Calibration & Holdout Validation
================================================================================
Objective:              NET_PNL
Chronological Split:    60.0% Train / 40.0% Holdout
Split Timestamp (ns):   10000000000
Events:                 Total: 20 | Calibration: 17 | Holdout: 4
--------------------------------------------------------------------------------
  Calibration Period Rankings (In-Sample)
--------------------------------------------------------------------------------
Rank  Candidate                                       Score           Net PnL         Trades    Fill Rate   Max DD      
1.    Candidate 2 (Wide Spread: 300 ticks)            600025000000    600025000000    0         0%          10000000    
2.    Candidate 6 (Fixed Slippage: 2 ticks)           599787460080    599787460080    1         4%          117529960   
3.    Baseline (Conservative, 200 spread, 10k quote)  599548984000    599548984000    2         8%          231008000   
4.    Candidate 4 (Optimistic Queue: 0% ahead)        599548984000    599548984000    2         8%          231008000   
5.    Candidate 5 (Proportional Queue: 50% ahead)     599548984000    599548984000    2         8%          231008000   
6.    Candidate 3 (Large Size: 20k quote)             599072968000    599072968000    2         8%          467016000   
7.    Candidate 1 (Tight Spread: 100 ticks)           598473458000    598473458000    7         28%         756521000   
--------------------------------------------------------------------------------
Selected Parameter Set: Candidate 2 (Wide Spread: 300 ticks)
--------------------------------------------------------------------------------
  Out-of-Sample Holdout Performance Comparison
--------------------------------------------------------------------------------
Metric                      Baseline                Selected Winner         
Net P&L (cents):            599560976000            599560976000            
Gross P&L (cents):          599800988000            599800988000            
Fees Paid (cents):          240012000               240012000               
Trades Executed:            2                       2                       
Volume Traded (lots):       20000                   20000                   
Fill Rate (Qty %):          33%                     33%                     
Max Drawdown (cents):       229012000               229012000               
================================================================================

Calibration Time:       407 us
Report exported to:     calibration_report.json
Audit verification: ALL CONSERVATION CHECKS PASSED!
```

---

## 9. Next Milestones (Roadmap)

- **Milestone 5**: Real-Time Streaming & Fault Injection (WebSocket / IPC feeds, dropped packet emulation, network partitions, queue delay distributions).
- **Milestone 6**: High-Performance Architecture Optimization (flat price-ladder, memory pools, zero-allocation ring buffers, SIMD-accelerated book ops).
