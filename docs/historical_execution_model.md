# MarketLab Historical Execution Model (Conservative Baseline)

## 1. Overview and Design Philosophy

MarketLab's historical replay engine evaluates strategy performance against real historical market data without execution leakage, lookahead bias, or unrealistic fill assumptions.

Because Level 2 (L2) market data provides aggregated price-level depth rather than individual order identifiers or full queue positions, any backtesting execution model must make explicit, documented assumptions. MarketLab adopts a **conservative baseline execution model** designed to avoid over-optimistic fill rates.

---

## 2. Model Mechanics

### A. Aggressive Orders (Market & Crossing Limit Orders)
- **Immediate Execution**:
  Aggressive orders match against visible resting reconstructed order book depth:
  - Aggressive `BUY` matches against Ask levels sorted ascending by price ($\le \text{price}$ for Limit orders, $\le \text{protection\_price}$ for Market orders).
  - Aggressive `SELL` matches against Bid levels sorted descending by price ($\ge \text{price}$ for Limit orders).
- **Hypothetical Liquidity Consumption**:
  When an aggressive order fills $Q$ lots at price $P$, $Q$ is tracked as `consumed_liquidity(P)`.
  Subsequent aggressive orders in the same replay window observe remaining visible depth:
  $$\text{Available}(P) = \max(0, \text{BookQty}(P) - \text{ConsumedQty}(P))$$
- **Liquidity Replenishment**:
  When a new exchange depth update arrives for price level $P$, `consumed_liquidity(P)` is reset, reflecting refreshed state from the exchange.
- **Unfilled Remainder**:
  Any unfilled remainder of an aggressive IOC or Market order is immediately canceled with an `IOC_REMAINDER` or `MARKET_REMAINDER` report; reserved balances are released.

### B. Passive Resting Orders (Limit Orders Posting Behind the Spread)
- **Queue-Ahead Initialization**:
  When a passive limit order is admitted at virtual timestamp $T$, the engine observes the total resting depth $D(P)$ already resting at price $P$ (less any previously consumed liquidity).
  The order's `queue_ahead` is initialized to $D(P)$:
  $$\text{queue\_ahead} = D(P)$$
  This assumes worst-case FIFO queue priority (our order joins the back of the queue).
- **Queue Depletion via Public Trades**:
  When a public trade arrives:
  - For passive `BUY` orders ($P_{\text{order}} \ge P_{\text{trade}}$ and taker is `SELL`):
    - If $P_{\text{trade}} < P_{\text{order}}$, the trade crossed below our price, implying our queue ahead was completely exhausted; $\text{queue\_ahead}$ is set to 0.
    - If $P_{\text{trade}} == P_{\text{order}}$, trade volume depletes $\text{queue\_ahead}$:
      $$\Delta Q = \min(Q_{\text{trade}}, \text{queue\_ahead})$$
      $$\text{queue\_ahead} \leftarrow \text{queue\_ahead} - \Delta Q$$
      $$Q_{\text{rem\_trade}} = Q_{\text{trade}} - \Delta Q$$
- **Trade Volume Fill Capping**:
  A passive order receives fills **only** after its $\text{queue\_ahead}$ reaches 0, and fills are strictly capped by the remaining public trade volume $Q_{\text{rem\_trade}}$:
  $$\text{fill\_qty} = \min(Q_{\text{rem\_trade}}, \text{order.remaining\_qty})$$
  This guarantees that simulated passive fills never exceed actual traded exchange volume.

### C. Self-Trade Prevention (STP)
If a strategy submits an order that would cross against its own resting passive order (e.g. submitting a Market Buy while holding an active resting Limit Sell), the incoming order is atomically rejected with `SELF_TRADE_PROHIBITED`.

---

## 3. Double-Entry Accounting and Reconciliation

Historical fills do not have internal maker-taker counterparty pairs from the same simulation environment. To maintain authoritative accounting:
- **Designated Counterparty**: An external account named `"HISTORICAL_MARKET"` acts as the counterparty for all historical executions.
- **Double-Entry Settlement**:
  Every fill executes a standard `TradeSettlement` recorded in `Ledger`:
  $$\Delta \text{Cash}_{\text{Strategy}} + \Delta \text{Cash}_{\text{Market}} + \Delta \text{Cash}_{\text{Fees}} = 0$$
  $$\Delta \text{Inv}_{\text{Strategy}} + \Delta \text{Inv}_{\text{Market}} = 0$$
- **Conservation Invariants**:
  Total system cash and base inventory remain conserved across every replay step, audited via `ledger.verify_all_invariants()`.

---

## 4. What the Model DOES and Does NOT Capture

### What the Model DOES Capture:
1. **Visible Liquidity Limits**: Aggressive orders cannot take liquidity that does not exist on the reconstructed book.
2. **Public Trade Volume Bounds**: Passive fills are strictly constrained by verified exchange trade prints.
3. **Queue-Ahead Priority**: Strategy orders do not execute until earlier resting volume has traded.
4. **Causality and Delays**: Orders cannot use future data; execution reports and observations respect simulated network latencies.
5. **Exact Ledger Conservation**: No unbacked currency or inventory creation.

### What the Model Does NOT Capture:
1. **Intra-Level FIFO Queue Position**: L2 aggregated updates do not disclose individual order sizes or cancellations ahead of us in the queue.
2. **Cancellations Ahead of Our Order**: If resting orders ahead of us cancel without trading, our real queue position advances faster than this model assumes (making this model strictly conservative).
3. **Hidden / Iceberg Orders**: Exchange orders that are not displayed in public depth feeds cannot be consumed.
4. **Market Impact**: Simulated strategy orders do not alter real historical market trajectories or cause other participants to alter their behavior.
5. **Network / Co-location Advantage**: Sub-millisecond packet racing or exchange gateway queue jumps are outside the scope of L2 replay.
