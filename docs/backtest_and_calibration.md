# MarketLab Event-Driven Backtester & Execution Model Calibration

## 1. Overview and Architecture

MarketLab Milestone 4 delivers an event-driven backtesting engine and execution model calibration workflow. It operates on reconstructed historical market data with pure integer arithmetic (ticks and lots), a virtual discrete-event scheduler, double-entry portfolio accounting, and zero lookahead bias.

```
       +---------------------------------------------------------+
       |             Normalized Historical Market Data            |
       |       (Snapshots, Depth Updates, Public Trades)         |
       +---------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------+
|                       VirtualScheduler (Min-Heap)                     |
|  - EventPriority::MarketDataAuthoritative                             |
|  - EventPriority::MarketDataDelivery (latencies.market_data_delay)    |
|  - EventPriority::StrategyWakeup / ObservationDelivery                |
|  - EventPriority::OrderAdmission (latencies.order_entry_delay)        |
|  - EventPriority::ExecutionReportDelivery                             |
+-----------------------------------------------------------------------+
        |                                                 ^
        v                                                 |
+--------------------+                           +--------------------+
| Reconstructed Book |                           | Reference Strategy |
| - L2 Depth Levels  |                           | - MarketMaker Mode |
| - Bridge Validator |                           | - Taker Mode       |
+--------------------+                           | - Hybrid Mode      |
        |                                                 |
        +-----------------------+-------------------------+
                                |
                                v
+-----------------------------------------------------------------------+
|                      HistoricalExecutionAdapter                       |
|  - Configurable Queue Position (Conservative, Optimistic, Proport.)  |
|  - Configurable Aggressive Impact (None, FixedSlippage, LinearImpact) |
|  - Pre-Trade Risk Engine & Atomic Margin/Cash Reservations           |
|  - Self-Trade Prevention (STP) & Depth Consumption Limits             |
+-----------------------------------------------------------------------+
                                |
                                v
+-----------------------------------------------------------------------+
|                       Authoritative Ledger & Audit                    |
|  - Double-Entry Settlement against "HISTORICAL_MARKET" Counterparty   |
|  - Fee Pool Allocation & Cash / Inventory Conservation Checks         |
|  - Post-Fill Markout Audit Horizons & Equity Curve Drawdown Tracking  |
+-----------------------------------------------------------------------+
```

---

## 2. Configurable Execution Models

Historical Level 2 (L2) market data provides aggregate depth without queue position visibility. MarketLab provides explicit, parameterizable assumptions while preserving conservative baseline defaults:

### A. Passive Queue Position Models (`--queue-model`, `--queue-pct`)
When a strategy limit order rests at price level $P$ with visible resting depth $D(P)$:
1. **Conservative (Baseline, Default)**:
   $$\text{queue\_ahead} = D(P)$$
   The order joins the back of the queue (100% of visible depth ahead). Public trades at price $P$ must fully exhaust $\text{queue\_ahead}$ before strategy orders receive fills.
2. **Optimistic**:
   $$\text{queue\_ahead} = 0$$
   The order assumes front-of-queue priority (0% queue ahead). It fills immediately on any public trade touching price level $P$, up to the trade volume.
3. **Proportional**:
   $$\text{queue\_ahead} = \left\lfloor \frac{\text{queue\_position\_pct}}{100} \times D(P) \right\rfloor$$
   A user-specified percentage of visible depth (e.g. 50% for middle-of-queue priority).

### B. Aggressive Market Impact & Slippage Models (`--impact-model`, `--slippage-ticks`, `--impact-ticks`)
When an aggressive order takes liquidity across visible book levels:
1. **None (Baseline, Default)**:
   The fill price exactly matches the visible price on the reconstructed book.
2. **Fixed Slippage**:
   $$\text{ExecutionPrice}_{\text{Buy}} = P_{\text{book}} + \text{fixed\_slippage\_ticks}$$
   $$\text{ExecutionPrice}_{\text{Sell}} = P_{\text{book}} - \text{fixed\_slippage\_ticks}$$
   Adds a deterministic fixed-tick penalty representing adverse execution or latency slippage.
3. **Linear Impact**:
   $$\text{ExecutionPrice}_{\text{Buy}} = P_{\text{book}} + \left\lfloor \frac{Q_{\text{fill}}}{100000} \times \text{impact\_ticks\_per\_lot} \right\rfloor$$
   $$\text{ExecutionPrice}_{\text{Sell}} = P_{\text{book}} - \left\lfloor \frac{Q_{\text{fill}}}{100000} \times \text{impact\_ticks\_per\_lot} \right\rfloor$$
   Scales price penalty proportionally with traded quantity.

**Protection Guarantee**: Fill prices are capped by the client's limit price or market-buy protection price cap. Any slippage or impact that would exceed the protection price is rejected or truncated.

---

## 3. Metrics Definitions & Mathematical Formulations

### A. Financial Performance
- **Gross P&L (ticks/cents)**:
  $$\text{Gross P\&L} = \text{Net P\&L} + \text{Total Fees Paid}$$
- **Net P&L (ticks/cents)**:
  $$\text{Net P\&L} = \text{Realized P\&L} + \text{Unrealized P\&L} - \text{Total Fees Paid}$$
- **Starting & Ending Equity**:
  $$\text{Equity}(t) = \text{Cash}(t) + \text{Inventory}(t) \times \text{MidPrice}(t)$$
- **Turnover**:
  $$\text{Turnover} = \sum_{i} \text{Price}_i \times \text{Quantity}_i$$
- **Quantity Fill Rate**:
  $$\text{Fill Rate}_{\text{Qty}} = \frac{\text{Filled Quantity}}{\text{Total Order Quantity Submitted}} \times 100\%$$
- **Order Fill Rate**:
  $$\text{Fill Rate}_{\text{Orders}} = \frac{\text{Total Trades Executed}}{\text{Total Orders Submitted}} \times 100\%$$
- **Max Drawdown**:
  $$\text{Drawdown}(t) = \text{Peak Equity}(t) - \text{Equity}(t)$$
  $$\text{Max Drawdown} = \max_{t} \text{Drawdown}(t)$$

### B. Post-Fill Markouts
A post-fill markout measures the change in mid price $H$ nanoseconds after an execution at time $T_{\text{fill}}$:
$$T_{\text{target}} = T_{\text{fill}} + H$$

1. **Markout Statuses**:
   - `COMPLETED`: $T_{\text{target}}$ reached within session duration and a valid reconstructed mid price $M$ is available.
   - `UNFINISHED`: $T_{\text{target}} > T_{\text{session\_end}}$ (the horizon extends beyond the end of the simulation).
   - `MISSING_MARK`: $T_{\text{target}}$ occurred within the session, but the reconstructed book had an invalid feed status or empty bid/ask.

2. **Formulas for Completed Markouts**:
   - For a **Buy Fill** at execution price $P$:
     $$\text{markout\_ticks} = M - P$$
     $$\text{markout\_bps} = \frac{M - P}{P} \times 10{,}000$$
     (Positive markout indicates price rose after buy: favorable execution).
   - For a **Sell Fill** at execution price $P$:
     $$\text{markout\_ticks} = P - M$$
     $$\text{markout\_bps} = \frac{P - M}{P} \times 10{,}000$$
     (Positive markout indicates price fell after sell: favorable execution).
   - **Volume-Weighted Average Markout (VWAP Markout)**:
     $$\text{VWAP Markout} = \frac{\sum_{i \in \text{completed}} Q_i \times \text{markout}_i}{\sum_{i \in \text{completed}} Q_i}$$

### C. Causality & Lookahead Prevention
Markouts are post-simulation audit metrics. Strategy code never receives future mid prices or un-delivered execution reports. Mid-price history is sampled strictly at virtual timestamp $T_{\text{target}}$ from historical book states.

---

## 4. Calibration & Holdout Validation (`marketlab calibrate`)

### A. Chronological Splitting
Given an input normalized event stream $[T_{\text{start}}, T_{\text{end}}]$ and split ratio $R \in (0, 1)$ (default: $0.6$):
$$T_{\text{split}} = T_{\text{start}} + \lfloor (T_{\text{end}} - T_{\text{start}}) \times R \rfloor$$

1. **Calibration Period (In-Sample)**:
   Includes events strictly where $T_{\text{event}} < T_{\text{split}}$.
2. **Holdout Period (Out-of-Sample)**:
   - To prevent feed invalidation at $T_{\text{split}}$, the calibration engine replays in-sample events through a `ReconstructedOrderBook` up to $T_{\text{split}}$.
   - It captures the authoritative order book state at $T_{\text{split}}$ and synthesizes a `NormalizedSnapshot` at $T_{\text{split}}$.
   - This snapshot is prepended to holdout events ($T_{\text{event}} \ge T_{\text{split}}$).
   - Strategy accounts are reset to initial starting capital and zero inventory to ensure independent evaluation.

### B. Parameter Selection & Holdout Comparison
1. All candidates in the parameter grid are evaluated on the calibration period and ranked by the chosen objective (`net_pnl`, `gross_pnl`, `fill_rate`, `min_drawdown`).
2. The highest-ranked candidate is selected as the winner.
3. Both the **Baseline** candidate and the **Selected Winner** are evaluated on the out-of-sample holdout dataset.
4. If public trade data produces zero strategy fills across all candidates, the report is explicitly labeled as **"Execution Model Sensitivity Analysis"**.

---

## 5. CLI Usage & Examples

### A. Backtesting (`marketlab backtest`)
```bash
# Basic backtest against normalized stream
./build/marketlab.exe backtest --input data/fixtures/calibration_fixture_norm.jsonl --verify

# Backtest with custom strategy and execution model
./build/marketlab.exe backtest \
  --input data/fixtures/calibration_fixture_norm.jsonl \
  --strategy-mode HYBRID \
  --quote-size 10000 \
  --half-spread 100 \
  --queue-model PROPORTIONAL \
  --queue-pct 50 \
  --impact-model FIXED_SLIPPAGE \
  --slippage-ticks 2 \
  --events-out events.jsonl \
  --fills-out fills.jsonl \
  --state-out state.json \
  --report-out report.json \
  --manifest-out manifest.json \
  --verify
```

### B. Calibration Workflow (`marketlab calibrate`)
```bash
# Run calibration grid search with 60% train / 40% holdout split
./build/marketlab.exe calibrate \
  --input data/fixtures/calibration_fixture_norm.jsonl \
  --split 0.6 \
  --objective net_pnl \
  --report-out calibration_report.json \
  --verify
```
