# Scalping Bot ACSIL Study for Sierra Chart

## Inspiration

This automated scalping bot is an ACSIL (Advanced Custom Study Interface and Language) implementation for Sierra Chart. It draws inspiration from [Rob Carver's "Can I build a scalping bot?" blog post](https://qoppac.blogspot.com/2025/05/can-i-build-scalping-bot-blogpost-with.html), adapting its core concepts to the Sierra Chart trading environment and leveraging its native order functionalities.

## Strategy Overview

This automated scalping bot employs a mean-reversion approach, aiming to capture small profits from short-term price oscillations using dynamically calculated parameters.

1.  **Dynamic Range (`R`) Calculation**:
    *   The core volatility measure, `R`, is not a simple high-low range of a recent period. Instead, it's derived dynamically from an "Average Rotation" value.
    *   This `Average Rotation` is typically sourced from another study's subgraph output on the chart (configured via the "Volatility Subgraph (Range R)" input). This external study usually calculates this value based on price rotations over a defined lookback period (e.g., a 90-day period for average bar rotations).
    *   This dynamic `R` value forms the basis for determining trade entry and exit levels.

2.  **Trading Window**:
    *   The bot operates exclusively within a user-defined time window, specified by "Start Time" and "Stop Time" inputs.
    *   Outside these hours, or if trading is disabled via the "Enable Trading" input, no new trades are initiated.
    *   Crucially, at the designated "Stop Time," the bot will automatically attempt to flatten any open positions and cancel all working orders, ensuring it concludes the trading session flat.

3.  **Entry Logic - OCO (Order-Cancels-Order) Brackets**:
    *   When the bot is flat (no open position) and operating within the active trading window, it seeks to enter the market using OCO bracket orders.
    *   It calculates two initial limit order prices based on the current closing price and the dynamic range `R`:
        *   Buy Limit Price: `Current Close Price - (R * Bracket Width Fraction)`
        *   Sell Limit Price: `Current Close Price + (R * Bracket Width Fraction)`
        *   The `Bracket Width Fraction` is a user-defined input that determines the distance of these initial limit orders from the current price.
    *   These two limit orders are submitted as a single OCO group. If one limit order is filled, Sierra Chart automatically cancels the other.
    *   Attached to *each* of these initial limit orders are its own pre-defined stop-loss and take-profit orders. These are also specified as offsets from the eventual entry price, based on fractions of `R`:
        *   Stop-Loss Offset from Entry: `R * Stop Loss Fraction`
        *   Take-Profit Offset from Entry: `R * Take Profit Fraction`

4.  **Trade Management & Exit Logic**:
    *   Once one of the initial OCO limit orders is filled, the bot is considered "In Trade" (either long or short).
    *   The other initial limit order of the OCO group is automatically cancelled by Sierra Chart.
    *   The attached stop-loss and take-profit orders corresponding to the filled entry order become active.
    *   The bot then monitors these active stop-loss and take-profit orders.
    *   The trade is exited when either the stop-loss or the take-profit level is hit and filled.
    *   Upon exit, the bot returns to a flat state, ready to look for new OCO bracket opportunities if within the trading window. This simplifies the state machine as there is no "unprotected" state; entries are immediately bracketed.

5.  **Parameters and Precision**:
    *   **Number of Contracts**: Sets the quantity for each trade.
    *   **Volatility Subgraph (Range R)**: Specifies the Sierra Chart study and its specific subgraph to use for sourcing the dynamic `R` value (Average Rotation).
    *   **Bracket Width Fraction of R**: Determines how far from the current price the initial buy/sell limit orders of the OCO bracket are placed.
    *   **Stop Loss Fraction of R**: After an entry is filled, this determines the stop-loss distance from the entry price, as a multiple of `R`.
    *   **Take Profit Fraction of R**: After an entry is filled, this determines the take-profit distance from the entry price, as a multiple of `R`.
    *   **Log Detail Level**: Controls the verbosity of log messages output to the Sierra Chart Study Log for debugging and monitoring.
    *   All price calculations for orders (entry prices, stop-loss offsets, take-profit offsets) are rounded to the nearest tick size of the traded instrument to ensure order validity.

6.  **State Management & Resilience**:
    *   The bot uses Sierra Chart's persistent variables to maintain its operational state (e.g., Flat, BracketArmed, InPosition) and track critical order identifiers across study function calls.
    *   A bootstrap mechanism is included, which attempts to re-synchronize the study's internal state with actual open orders and positions if the study is reloaded or the chart undergoes a full recalculation.

This strategy leverages Sierra Chart's robust OCO functionality to manage entries and initial risk, while adapting trade parameters dynamically based on the `R` value derived from an external "Average Rotation" indicator.

## Prerequisites

- **Sierra Chart** installed.
- An understanding of ACSIL development and Sierra Chart's trading functionalities.

## Installation

1.  Save the `scalping_bot.cpp` file.
2.  Place the `.cpp` file in your `[SierraChartInstallationPath]/ACS_Source` directory.
3.  Open Sierra Chart.
4.  Select **Analysis >> Build Custom Studies DLL** from the main menu.
5.  Follow the on-screen prompts. If successful, the DLL will be built, and the "Scalping Bot" study will be available to add to your charts.

## Simulation and Testing Recommendations

For more realistic simulation results in Sierra Chart, especially when backtesting or paper trading this scalping bot, consider the following settings:

1.  **Enable Estimated Position in Queue Tracking**:
    *   Navigate to **Global Settings >> Chart Trade Settings >> General >> Position in Queue**. Enable this option.
    *   When this is enabled (and "Estimated Position (Q)" is also enabled on the order line display), Sierra Chart will estimate your limit order's position in the order book queue at its price level. This provides a more realistic simulation of whether your limit order would get filled based on market depth and subsequent trades at that price.
    *   The estimated position in queue (EP:) is calculated based on the bid/ask quantity at the order's price when entered, plus the order's own quantity. It decreases as trades occur at that price level.
    *   This feature requires market depth data to be available for the instrument.

2.  **Simulating Stop-Loss Slippage**:
    *   While ACSIL uses attached stop orders, which are typically Stop Market orders, their fill price in live trading can differ from the trigger price due to slippage.
    *   Sierra Chart's trade simulation can provide more realistic stop-loss fills if it has access to historical bid/ask prices and market depth.
    *   Ensure your "Trade Simulation Mode" settings (found in **Trade >> Trade Simulation Mode On/Off >> Trade Simulation Mode Settings**) are configured for realistic fill simulation. Options like "Simulate Stop/Stop-Limit Order Fills Using Opposite Side Best Price + Slippage Amount" or using historical bid/ask data for fills will yield more conservative and realistic results.

3.  **Use High-Quality Data**:
    *   For scalping strategies, tick-by-tick historical data with market depth is ideal for the most accurate backtesting.
    *   Ensure your Sierra Chart Data/Trade Service provides this level of detail for the instruments you are testing.

4.  **Consult Sierra Chart Documentation**:
    *   For comprehensive details on configuring and utilizing Sierra Chart's trade simulation capabilities, refer to the official documentation: [Sierra Chart Trade Simulation Documentation](https://www.sierrachart.com/index.php?page=doc/TradeSimulation.php#UsingTradeSimulation)

Thoroughly testing with these more realistic simulation settings can help you better understand the potential performance and risks of the scalping bot before considering live trading.

## Risk Disclaimer

TRADING INVOLVES SIGNIFICANT RISK OF LOSS AND IS NOT SUITABLE FOR ALL INVESTORS. This scalping bot ACSIL study is provided for educational and informational purposes only. Its use is entirely at your own risk.

-   **Past performance is not indicative of future results.**
-   Users should thoroughly test this software in a simulated trading environment with historical data (backtesting) and then with live data in simulation mode before considering deployment with real capital.
-   The authors and contributors of this software accept no liability whatsoever for any financial losses, damages, or other adverse consequences resulting from the use of this software or the information it provides.
-   Always employ proper risk management techniques, including setting appropriate position sizes and stop-losses, and never trade with funds you cannot afford to lose.
-   Market conditions, data feed quality, execution latency, and broker-specific behaviors can all impact the performance of automated trading systems.
-   It is your responsibility to understand the code, its parameters, and its potential behavior in various market scenarios.