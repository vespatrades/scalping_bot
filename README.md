# Scalping Bot ACSIL Study for Sierra Chart

## Inspiration

This automated scalping bot is an ACSIL (Advanced Custom Study Interface and Language) implementation for Sierra Chart. It draws inspiration from [Rob Carver's "Can I build a scalping bot?" blog post](https://qoppac.blogspot.com/2025/05/can-i-build-scalping-bot-blogpost-with.html), adapting its core concepts to the Sierra Chart trading environment and leveraging its native order functionalities.

## Strategy Overview

This automated scalping bot employs a mean-reversion approach, aiming to capture small profits from short-term price oscillations using dynamically calculated parameters.

1.  **Dynamic Range (`R`) Calculation**:
    *   The core volatility measure, `R`, is derived dynamically from a user-selected study's subgraph output on the chart (configured via the "Volatility Subgraph (Range R)" input). This external study typically calculates a value like "Average Rotation" based on price rotations over a defined lookback period (e.g., 90 days).
    *   This dynamic `R` value forms the basis for determining trade entry and exit levels.

2.  **Trading Window (Optional)**:
    *   The bot can operate within a user-defined time window, specified by "Start Time" and "Stop Time" inputs, if the "Use Trading Window" input is set to "Yes" (default).
    *   If the trading window is enabled:
        *   Outside these hours, or if trading is globally disabled via the "Enable Trading" input, no new trades are initiated.
        *   At the designated "Stop Time," the bot will automatically attempt to flatten any open positions and cancel all working orders, ensuring it concludes the trading session flat.
    *   If "Use Trading Window" is set to "No", the bot will operate as long as "Enable Trading" is "Yes", without regard to specific start/stop times and without an automated end-of-day flatten based on time.

3.  **Entry Logic - OCO (Order-Cancels-Order) Brackets**:
    *   When the bot is flat (no open position) and allowed to trade (either within the active trading window if enabled, or globally enabled if window is disabled), it seeks to enter the market using OCO bracket orders.
    *   It calculates two initial limit order prices based on the current closing price and the dynamic range `R`:
        *   Buy Limit Price: `Current Close Price - (R * Bracket Width Fraction)`
        *   Sell Limit Price: `Current Close Price + (R * Bracket Width Fraction)`
        *   The `Bracket Width Fraction` is a user-defined input that determines the distance of these initial limit orders from the current price.
    *   These two limit orders are submitted as a single OCO group. If one limit order is filled, Sierra Chart automatically cancels the other.
    *   Attached to *each* of these initial limit orders are its own pre-defined stop-loss and take-profit orders. These are also specified as offsets from the eventual entry price, based on fractions of `R`:
        *   Stop-Loss Offset from Entry: `R * Stop Loss Fraction`
        *   Take-Profit Offset from Entry: `R * Take Profit Fraction`

4.  **Trade Management & Exit Logic**:
    *   Once one of the initial OCO limit orders is filled, the bot is considered "In Trade" (either long or short). The ID of this filled parent order is stored.
    *   The other initial limit order of the OCO group is automatically cancelled by Sierra Chart.
    *   The attached stop-loss and take-profit orders corresponding to the filled entry order become active.
    *   The bot then monitors these active child orders (stop-loss and take-profit) of the filled parent order.
    *   The trade is exited when either the stop-loss or the take-profit level is hit and filled.
    *   **Safety Exit**: If an active stop-loss or take-profit order is detected as CANCELED or in an ERROR state by the system/broker (not due to a fill), the bot will attempt to flatten the current position immediately to avoid an unprotected trade.
    *   Upon exit (either by SL/TP fill or safety flatten), the bot returns to a flat state, ready to look for new OCO bracket opportunities if conditions allow.

5.  **Parameters and Precision**:
    *   **Number of Contracts**: Sets the quantity for each trade.
    *   **Volatility Subgraph (Range R)**: Specifies the Sierra Chart study and its subgraph to use for sourcing the dynamic `R` value.
    *   **Bracket Width Fraction of R**: Determines how far from the current price the initial OCO limit orders are placed.
    *   **Stop Loss Fraction of R**: Determines the stop-loss distance from the entry price, as a multiple of `R`.
    *   **Take Profit Fraction of R**: Determines the take-profit distance from the entry price, as a multiple of `R`.
    *   **Use Trading Window**: A Yes/No input to enable or disable the time-based trading window and end-of-day flattening. Defaults to "Yes".
    *   **Start Time (HHMMSS)**: Trading start time, if the window is enabled.
    *   **Stop Time (HHMMSS) & Flatten**: Trading stop time and flatten time, if the window is enabled.
    *   **Enable Trading**: Master switch to enable or disable all trading actions by the bot.
    *   **Log Detail Level**: A dropdown list (NONE, ERROR, WARN, INFO, DEBUG, VERBOSE) to control the verbosity of log messages for debugging and monitoring. Defaults to "INFO".
    *   All price calculations for orders (entry prices, stop-loss offsets, take-profit offsets) are rounded to the nearest tick size of the traded instrument to ensure order validity. Offsets are also ensured to be at least one tick.

6.  **State Management & Resilience**:
    *   The bot uses Sierra Chart's persistent variables to maintain its operational state (e.g., Flat, BracketArmed, InPosition, ActiveFilledParentOrderID) across study function calls.
    *   A bootstrap mechanism is included, which attempts to re-synchronize the study's internal state with actual open orders and positions if the study is reloaded or the chart undergoes a full recalculation.

This strategy leverages Sierra Chart's robust OCO functionality to manage entries and initial risk, while adapting trade parameters dynamically based on the `R` value derived from an external indicator.

## Prerequisites

- **Sierra Chart** installed.
- An understanding of ACSIL development and Sierra Chart's trading functionalities.

## Installation

1.  Save the `scalping_bot.cpp` file.
2.  Place the `.cpp` file in your `[SierraChartInstallationPath]/ACS_Source` directory.
3.  Open Sierra Chart.
4.  Select **Analysis >> Build Custom Studies DLL** from the main menu.
5.  Follow the on-screen prompts. If successful, the DLL will be built, and the "Scalping Bot" study will be available in `Custom Studies` to add to your charts.

## Live Simulation Recommendation

- **[Enable Estimated Position in Queue Tracking](https://www.sierrachart.com/index.php?page=doc/GlobalTradeSettings.html#ChartTradeSettings_EnableEstimatedPositionInQueueTracking)** (Global Settings >> Chart Trade Settings >> General >> Position in Queue)
- The feature is intended to provide a more realistic live trading simulation, especially for strategies sensitive to order queue dynamics.
- This requires market depth data for the instrument being traded.

## Risk Disclaimer

TRADING INVOLVES SIGNIFICANT RISK OF LOSS AND IS NOT SUITABLE FOR ALL INVESTORS. This scalping bot ACSIL study is provided for educational and informational purposes only. Its use is entirely at your own risk.

-   **Past performance is not indicative of future results.**
-   The authors and contributors of this software accept no liability whatsoever for any financial losses, damages, or other adverse consequences resulting from the use of this software or the information it provides.
-   Market conditions, data feed quality, execution latency, and broker-specific behaviors can all impact the performance of automated trading systems.
-   It is your responsibility to understand the code, its parameters, and its potential behavior in various market scenarios.