/*
* ===================================================================
*   Scalping Bot (Carver Inspired) - ACSIL Study for Sierra Chart
* ===================================================================
*
*   Inspired by Rob Carver's intraday scalping bot concept, this study
*   implements a mean-reversion scalping strategy.
*
*   "Can I build a scalping bot? A blogpost with numerous double digit SR"
*   https://qoppac.blogspot.com/2025/05/can-i-build-scalping-bot-blogpost-with.html
*
*   Strategy Overview:
*   1.  Dynamic Range 'R': Uses a volatility value from a user-specified study
*       subgraph to determine trade parameters.
*   2.  OCO Bracket Entry: When flat (and optionally within a trading window),
*       places OCO Buy Limit and Sell Limit orders around the current price.
*       These orders have pre-attached Stop-Loss and Take-Profit orders, all
*       calculated as fractions of the dynamic 'R' value.
*   3.  State Management:
*       - Tracks OCO parent limit order IDs.
*       - When one leg of the OCO fills, its ID is stored as the active trade's
*         parent. Exit logic then monitors the children (SL/TP) of this filled parent.
*       - Key states: FLAT & READY, BRACKET_ARMED (OCO working), IN_TRADE.
*   4.  Trading Window (Optional):
*       - A user input allows enabling/disabling a specific trading time window.
*       - If enabled, the bot only initiates trades within "Start Time" and "Stop Time".
*       - If enabled, at "Stop Time", any open position is flattened, and working
*         orders are canceled.
*   5.  Safety: If an active Stop or Take-Profit order (child of the filled
*       entry) is detected as CANCELED or in ERROR state while a position is open,
*       the bot will attempt to flatten the position to prevent it from becoming
*       unprotected.
*
*   Core Parameters:
*   - Number of Contracts
*   - Volatility Subgraph (for 'R' value)
*   - Fractions of 'R' for: Bracket Entry Offset, Stop Loss, Take Profit
*   - Optional Trading Window Enable
*   - Start Time, Stop Time (if window is used)
*   - Master Trading Enable switch
*   - Log Detail Level (dropdown: NONE, ERROR, WARN, INFO, DEBUG, VERBOSE)
*
*   Important: Thorough simulation is crucial before live trading.
*   See README.md for simulation recommendations and risk disclaimers.
*
* ===================================================================
*/

#include "sierrachart.h"

SCDLLName("Scalping Bot")

// Enum for logging levels to control the verbosity of messages.
// Higher values mean more detailed logs.
enum LoggingLevel {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_ERROR = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_INFO = 3,
    LOG_LEVEL_DEBUG = 4,
    LOG_LEVEL_VERBOSE = 5
};

// Enum to represent the bot's trading state (which side it's on, or flat).
enum TradeSide {
    SIDE_FLAT = 0,
    SIDE_LONG = 1,
    SIDE_SHORT = 2
};

// Enum to represent the status of the OCO bracket order.
enum BracketStatus {
    BRACKET_NOT_ARMED = 0,
    BRACKET_ARMED_AND_WORKING = 1
};

// Symbolic constants for persistent variable keys.
#define PID_PARENT_BUY_LIMIT_ORDER_ID 1
#define PID_PARENT_SELL_LIMIT_ORDER_ID 2
#define PID_CURRENT_TRADE_SIDE 3        // Stores the value from the TradeSide enum
#define PID_IS_BRACKET_ARMED 4          // Stores the value from the BracketStatus enum
#define PID_ACTIVE_FILLED_PARENT_ORDER_ID 5 // Stores the ID of the OCO leg that actually filled


// Persistent variable keys for log debouncing (to prevent spamming the log)
#define PID_LAST_LOGGED_DISABLED_BAR 100
#define PID_LAST_LOGGED_BEFORE_WINDOW_BAR 101
#define PID_LAST_LOGGED_INVALID_R_BAR 102
#define PID_LAST_LOGGED_AFTER_WINDOW_BAR 103
#define PID_LAST_LOGGED_OFFSETS_BAR 104


// Forward declaration of helper function for logging.
void LogSCSMessage(SCStudyInterfaceRef& sc, int currentLogLevelSetting, LoggingLevel messageLevel, const SCString& message, bool showInTradeServiceLog = false);
void LogSCSMessage(SCStudyInterfaceRef& sc, int currentLogLevelSetting, LoggingLevel messageLevel, const char* message, bool showInTradeServiceLog = false);


SCSFExport scsf_Scalping_Bot(SCStudyInterfaceRef sc)
{
    //── Study Inputs ──────────────────────────────────────────────────────
    SCInputRef NumContracts = sc.Input[0];      // How many contracts/shares to trade.
    SCInputRef VolSubgraph = sc.Input[1];       // Source for the dynamic range 'R' value.
    SCInputRef BracketFrac = sc.Input[2];       // Fraction of R: distance from price to OCO entry limits.
    SCInputRef StopFrac = sc.Input[3];          // Fraction of R: stop-loss distance from entry price.
    SCInputRef TPFrac = sc.Input[4];            // Fraction of R: take-profit distance from entry price.
    SCInputRef UseTradingWindowInput = sc.Input[5]; // Controls whether a time window is used.
    SCInputRef StartTimeInput = sc.Input[6];    // Bot's operational start time.
    SCInputRef StopTimeInput = sc.Input[7];     // Bot's operational stop time (also triggers flattening).
    SCInputRef EnableInput = sc.Input[8];       // Master switch to enable/disable trading.
    SCInputRef LogLevelInput = sc.Input[9];     // Controls logging verbosity.

    //── Persistent State Variables ───────────────────────────────────────
    // These variables retain their values across calls to this study function.

    // IDs of the OCO parent limit orders (the entry orders themselves)
    int& ParentBuyLimitOrderID_Persist = sc.GetPersistentInt(PID_PARENT_BUY_LIMIT_ORDER_ID);
    int& ParentSellLimitOrderID_Persist = sc.GetPersistentInt(PID_PARENT_SELL_LIMIT_ORDER_ID);

    // Current trading state (flat, long, short) using the TradeSide enum
    int& CurrentTradeSide_Persist = sc.GetPersistentInt(PID_CURRENT_TRADE_SIDE);

    // Status of the OCO bracket (armed or not) using the BracketStatus enum
    int& IsBracketArmed_Persist = sc.GetPersistentInt(PID_IS_BRACKET_ARMED);

    // IDs of the active filled Parent order
    int& ActiveFilledParentOrderID_Persist = sc.GetPersistentInt(PID_ACTIVE_FILLED_PARENT_ORDER_ID);

    //── Default Settings Block (sc.SetDefaults) ───────────────────────────
    // This block is executed only once when the study is first added to a chart,
    // or when its settings are reset to default.
    if (sc.SetDefaults)
    {
        sc.GraphName = "Scalping Bot"; // Name displayed in the chart's "Studies" list and on the chart.
        sc.AutoLoop = 1;  // Setting to 1 enables "automatic looping".
                          // This means Sierra Chart will call this study function for each bar
                          // during a full recalculation, and then for each new tick/update.
        sc.UpdateAlways = 1; // Setting to 1 ensures this study function is called on every chart update.
        sc.MaintainTradeStatisticsAndTradesData = true; // Allows access to trade position data (sc.GetTradePosition)
                                                        // and order history (sc.GetOrderByOrderID, sc.GetOrderByIndex).

        // Initialize Input Parameters
        NumContracts.Name = "Number of Contracts";
        NumContracts.SetInt(1); // Default to 1 contract.
        NumContracts.SetIntLimits(1, 1000); // Min 1, Max 1000 contracts.

        VolSubgraph.Name = "Volatility Subgraph (Range R)";
        // Links this input to select a subgraph from another study on the chart.
        // Parameters: (DefaultStudyID, DefaultSubgraphIndex)
        // StudyID 0 usually refers to the main price graph, but here it's a placeholder.
        // User must configure this to point to a valid volatility indicator.
        VolSubgraph.SetStudySubgraphValues(0, 0);

        BracketFrac.Name = "Bracket Entry Offset Fraction of R";
        BracketFrac.SetFloat(0.5f); // e.g., OCO limits are R * 0.5 away from the current price.
        BracketFrac.SetFloatLimits(0.001f, 5.0f);

        StopFrac.Name = "Stop Loss Offset Fraction of R";
        StopFrac.SetFloat(0.5f);     // e.g., Stop-loss is R * 0.5 away from entry.
        StopFrac.SetFloatLimits(0.001f, 10.0f);

        TPFrac.Name = "Take Profit Offset Fraction of R";
        TPFrac.SetFloat(1.0f);       // e.g., Take-profit is R * 1.0 away from entry.
        TPFrac.SetFloatLimits(0.001f, 20.0f);

        UseTradingWindowInput.Name = "Use Trading Window";
        UseTradingWindowInput.SetYesNo(true);

        StartTimeInput.Name = "Start Time (HHMMSS)";
        // HMS_TIME(hour, minute, second) creates a time value Sierra Chart understands.
        StartTimeInput.SetTime(HMS_TIME(8, 30, 0)); // Default: 08:30:00

        StopTimeInput.Name = "Stop Time (HHMMSS) & Flatten";
        StopTimeInput.SetTime(HMS_TIME(15, 0, 0));  // Default: 15:00:00

        EnableInput.Name = "Enable Trading";
        EnableInput.SetYesNo(false); // Default to disabled for safety. User must explicitly enable.

        LogLevelInput.Name = "Log Detail Level";
        // Define the dropdown options for log level. Semicolon separated.
        // The order here MUST match the LoggingLevel enum values.
        LogLevelInput.SetCustomInputStrings("NONE;ERROR;WARN;INFO;DEBUG;VERBOSE");
        // Set the default selection to "INFO" (which is index 3, matching LOG_LEVEL_INFO)
        LogLevelInput.SetCustomInputIndex(LOG_LEVEL_INFO); // Use the enum value directly for clarity

        // Critical Unmanaged Auto-trading Settings (User should be aware these are set by the study)
        // These settings control how Sierra Chart's global trading system interacts with this study's orders.
        // It's good practice to set these explicitly to ensure predictable behavior.
        sc.MaximumPositionAllowed = 100000; // Set high to practically not limit this bot's small positions.
        sc.AllowMultipleEntriesInSameDirection = true; // Not strictly used by this bot's flat-before-new-entry logic.
        sc.AllowOppositeEntryWithOpposingPositionOrOrders = true; // Not strictly used.
        sc.CancelAllOrdersOnEntriesAndReversals = false; // Bot manages its own OCO cancellations.
        sc.CancelAllOrdersOnReversals = false;
        sc.CancelAllOrdersOnEntries = false;
        sc.AllowEntryWithWorkingOrders = true; // Bot uses OCO, so this allows entry while the other OCO leg is working.
        sc.CancelAllWorkingOrdersOnExit = false; // Bot handles its own order cleanup on exit or EOD flatten.
        sc.SupportAttachedOrdersForTrading = true; // ESSENTIAL for OCO with attached stops/targets to function.

        // sc.SendOrdersToTradeService = 1; // THIS IS THE MASTER SWITCH FOR LIVE TRADING.
                                          // Default is 0 (simulated).
                                          // UNCOMMENT THIS LINE CAREFULLY TO ENABLE LIVE TRADING ORDERS.
                                          // Also requires "Automated Trading Enabled - Global"
                                          // and "Automated Trading Enabled - For Chart" in Sierra Chart menus.

        return; // Exit after setting defaults. No further processing in this call.
    }

    //── Bootstrap Logic (Full Recalculation, First Bar) ──────────────────
    // This section runs ONCE when the study is first applied or fully recalculated (e.g., chart reload, study settings change).
    // Its purpose is to try and re-synchronize the bot's internal state with the actual market state
    // (current position, existing orders) if the study was previously running or if orders were placed manually.
    // sc.IsFullRecalculation is true during a full recalculation.
    // sc.Index == 0 means this is the first bar being processed in that full recalculation sequence.
    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        SCString bootstrapMsg; // Reusable string for log messages
        // Get the user-set log level for conditional logging
        int currentLogLevelSetting = LogLevelInput.GetInt();
        LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_DEBUG, "BOOTSTRAP: Performing full recalculation.");

        // 1. Reset all persisted order IDs to ensure a clean state before trying to re-identify.
        ParentBuyLimitOrderID_Persist = 0;
        ParentSellLimitOrderID_Persist = 0;
        ActiveFilledParentOrderID_Persist = 0;
        IsBracketArmed_Persist = BRACKET_NOT_ARMED; // Assuming not armed until proven otherwise

        // 2. Infer current position from Sierra Chart's trade data.
        s_SCPositionData pos; // Structure to hold position data.
        sc.GetTradePosition(pos); // ACSIL function to get current trade position for the chart's symbol/account.

        if (pos.PositionQuantity > 0) CurrentTradeSide_Persist = SIDE_LONG;
        else if (pos.PositionQuantity < 0) CurrentTradeSide_Persist = SIDE_SHORT;
        else CurrentTradeSide_Persist = SIDE_FLAT;

        bootstrapMsg.Format("BOOTSTRAP: Current Position Qty: %.0f, Inferred TradeSide: %d", pos.PositionQuantity, CurrentTradeSide_Persist);
        LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_DEBUG, bootstrapMsg);

        // 3. If currently flat, attempt to re-identify working OCO bracket orders.
        // Iterate through all known orders for the current chart to find potential OCO parents.
        if (static_cast<TradeSide>(CurrentTradeSide_Persist) == SIDE_FLAT)
        {
            int orderIndex = 0;
            s_SCTradeOrder currentOrder;
            std::vector<int> validParentLimitOrderIDs;

            while (sc.GetOrderByIndex(orderIndex++, currentOrder) != SCTRADING_ORDER_ERROR)
            {
                if (currentOrder.OrderStatusCode == SCT_OSC_OPEN &&
                    currentOrder.ParentInternalOrderID == 0 &&
                    currentOrder.OrderTypeAsInt == SCT_ORDERTYPE_LIMIT)
                {
                    int childIndex = 0;
                    int childOrderCount = 0;
                    s_SCTradeOrder childOrder;
                    while (sc.GetOrderByIndex(childIndex++, childOrder) != SCTRADING_ORDER_ERROR)
                    {
                        if (childOrder.ParentInternalOrderID == currentOrder.InternalOrderID)
                        {
                            childOrderCount++;
                        }
                    }
                    if (childOrderCount == 2)
                    {
                        validParentLimitOrderIDs.push_back(currentOrder.InternalOrderID);
                    }
                }
            }

            // If we found exactly two such parent limit orders, assume they form an OCO pair.
            if (validParentLimitOrderIDs.size() == 2)
            {
                s_SCTradeOrder orderA, orderB;
                sc.GetOrderByOrderID(validParentLimitOrderIDs[0], orderA);
                sc.GetOrderByOrderID(validParentLimitOrderIDs[1], orderB);

                if (orderA.Price1 < orderB.Price1) {
                    ParentBuyLimitOrderID_Persist = orderA.InternalOrderID;
                    ParentSellLimitOrderID_Persist = orderB.InternalOrderID;
                } else {
                    ParentBuyLimitOrderID_Persist = orderB.InternalOrderID;
                    ParentSellLimitOrderID_Persist = orderA.InternalOrderID;
                }
                IsBracketArmed_Persist = BRACKET_ARMED_AND_WORKING;
                bootstrapMsg.Format("BOOTSTRAP: Found and re-armed OCO bracket. BuyLimitID: %d, SellLimitID: %d", ParentBuyLimitOrderID_Persist, ParentSellLimitOrderID_Persist);
                LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_INFO, bootstrapMsg);
            }
            else
            {
                if (!validParentLimitOrderIDs.empty()) {
                    bootstrapMsg.Format("BOOTSTRAP: Found %d potential parent orders with 2 children, but not exactly 2. Not arming OCO.", (int)validParentLimitOrderIDs.size());
                    LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_DEBUG, bootstrapMsg);
                } else {
                     LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_DEBUG, "BOOTSTRAP: No active OCO bracket found while flat.");
                }
            }
        } else {
             if (static_cast<BracketStatus>(IsBracketArmed_Persist) == BRACKET_ARMED_AND_WORKING) {
                 LogSCSMessage(sc, currentLogLevelSetting, LOG_LEVEL_WARN, "BOOTSTRAP: InTrade, but IsBracketArmed was true. Resetting IsBracketArmed.");
                 IsBracketArmed_Persist = BRACKET_NOT_ARMED;
             }
        }
    }

    //── Main Trading Logic (runs only on the last bar of the chart if sc.UpdateAlways = 1) ─────
    // sc.Index is the current bar index being processed by Sierra Chart.
    // sc.ArraySize is the total number of bars in the chart.
    // We only want to execute our trading logic on the very latest bar data.
    if (sc.Index != sc.ArraySize - 1)
        return; // Not the last bar, so do nothing in this call.

    SCString logMsg;
    int currentLogLevel = LogLevelInput.GetInt();

    //── Trading Enabled Check ─────────────────────────────────────────────
    // Check the "Enable Trading" input. If not 'Yes', stop all bot activity.
    if (!EnableInput.GetYesNo())
    {
        // Log this disabled state, but not on every tick to avoid spam.
        int& lastLoggedDisabledBar = sc.GetPersistentInt(PID_LAST_LOGGED_DISABLED_BAR);
        // sc.GetBarHasClosedStatus() tells if the current bar (sc.Index) has closed.
        // We log once per closed bar, or if the bar index changes (meaning a new bar formed).
        if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedDisabledBar != sc.CurrentIndex) {
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, "Trading is disabled via 'Enable Trading' input.");
            lastLoggedDisabledBar = sc.CurrentIndex; // Update the last bar index we logged this for.
        }
        return; // Exit if trading is disabled.
    }

    //── TickSize Validity Check ───────────────────────────────────────────
    // sc.TickSize is the minimum price increment for the instrument.
    if (sc.TickSize <= 0.0f) {
        LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, "TickSize is invalid or zero. Halting operations.", true);
        return; // Cannot operate without a valid TickSize.
    }

    //── Optional Time Gating Logic ────────────────────────────────────────
    bool proceedToTradeLogic = true;

    if (UseTradingWindowInput.GetYesNo()) {
        int currentTime = sc.BaseDateTimeIn[sc.Index].GetTime();
        int tradingStartTime = StartTimeInput.GetTime();
        int tradingStopTime = StopTimeInput.GetTime();

        if (currentTime < tradingStartTime) {
            int& lastLoggedBeforeWindowBar = sc.GetPersistentInt(PID_LAST_LOGGED_BEFORE_WINDOW_BAR);
            if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedBeforeWindowBar != sc.CurrentIndex) {
                logMsg.Format("Waiting for trading window to start. CurrentTime: %06d, StartTime: %06d", currentTime, tradingStartTime);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
                lastLoggedBeforeWindowBar = sc.CurrentIndex;
            }
            if (static_cast<BracketStatus>(IsBracketArmed_Persist) == BRACKET_ARMED_AND_WORKING) {
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, "Outside trading window: Cancelling armed OCO bracket.", true);
                if (ParentBuyLimitOrderID_Persist != 0) sc.CancelOrder(ParentBuyLimitOrderID_Persist);
                if (ParentSellLimitOrderID_Persist != 0) sc.CancelOrder(ParentSellLimitOrderID_Persist);
                ParentBuyLimitOrderID_Persist = 0;
                ParentSellLimitOrderID_Persist = 0;
                IsBracketArmed_Persist = BRACKET_NOT_ARMED;
                ActiveFilledParentOrderID_Persist = 0;
            }
            proceedToTradeLogic = false;
        } else if (currentTime >= tradingStopTime) {
            int& lastLoggedAfterWindowBar = sc.GetPersistentInt(PID_LAST_LOGGED_AFTER_WINDOW_BAR);
            bool logThisBar = (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedAfterWindowBar != sc.CurrentIndex);

            if (logThisBar) {
                logMsg.Format("Trading window ended (CurrentTime: %06d, StopTime: %06d). Flattening position and cancelling orders.", currentTime, tradingStopTime);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
            }

            if (static_cast<BracketStatus>(IsBracketArmed_Persist) == BRACKET_ARMED_AND_WORKING) {
                if (ParentBuyLimitOrderID_Persist != 0) {
                    logMsg.Format("End of Day: Cancelling ParentBuyLimitOrderID: %d", ParentBuyLimitOrderID_Persist);
                    LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
                    sc.CancelOrder(ParentBuyLimitOrderID_Persist);
                }
                if (ParentSellLimitOrderID_Persist != 0) {
                    logMsg.Format("End of Day: Cancelling ParentSellLimitOrderID: %d", ParentSellLimitOrderID_Persist);
                    LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
                    sc.CancelOrder(ParentSellLimitOrderID_Persist);
                }
            }

            s_SCPositionData positionData;
            sc.GetTradePosition(positionData);
            if (positionData.PositionQuantity != 0) {
                logMsg.Format("End of Day: Flattening open position of %.0f contracts.", positionData.PositionQuantity);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
                sc.FlattenPosition();
            }

            ParentBuyLimitOrderID_Persist = 0;
            ParentSellLimitOrderID_Persist = 0;
            ActiveFilledParentOrderID_Persist = 0;
            CurrentTradeSide_Persist = SIDE_FLAT;
            IsBracketArmed_Persist = BRACKET_NOT_ARMED;

            if (logThisBar) {
                 LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, "End of Day: All states reset. Bot is flat and idle.");
                 lastLoggedAfterWindowBar = sc.CurrentIndex;
            }
            return;
        }
    }

    if (!proceedToTradeLogic) {
        return;
    }

    //── Calculate Dynamic Offsets based on 'R' ──────────────────────────
    // Get the 'R' value from the external study subgraph specified by the user.
    SCFloatArray volatilityArray; // This will hold the data from the specified subgraph.
    // sc.GetStudyArrayUsingID gets the data. Parameters: (StudyID, SubgraphIndex, OutputArray)
    // StudyID and SubgraphIndex are obtained from the VolSubgraph input.
    sc.GetStudyArrayUsingID(VolSubgraph.GetStudyID(), VolSubgraph.GetSubgraphIndex(), volatilityArray);

    // Validate the 'R' value.
    if (volatilityArray.GetArraySize() == 0 || sc.Index >= volatilityArray.GetArraySize() || volatilityArray[sc.Index] <= 0.0f)
    {
        int& lastLoggedInvalidRBar = sc.GetPersistentInt(PID_LAST_LOGGED_INVALID_R_BAR);
         if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedInvalidRBar != sc.CurrentIndex) {
            logMsg.Format("Invalid or zero 'R' (volatility) value from subgraph at Index %d. Value: %f. Cannot calculate offsets.", sc.Index, (volatilityArray.GetArraySize() == 0 || sc.Index >= volatilityArray.GetArraySize()) ? 0.0f : volatilityArray[sc.Index]);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_WARN, logMsg);
            lastLoggedInvalidRBar = sc.CurrentIndex;
        }
        return; // Cannot proceed without a valid 'R' value.
    }
    float R_value = volatilityArray[sc.Index]; // The dynamic range 'R'.

    // Calculate raw offsets based on 'R' and user-defined fractions.
    float rawEntryOffset = R_value * BracketFrac.GetFloat();
    float rawStopOffset = R_value * StopFrac.GetFloat();
    float rawTakeProfitOffset = R_value * TPFrac.GetFloat();

    // Round these raw offsets to the nearest tick size of the instrument.
    // sc.TickSize is the minimum price increment for the current symbol.
    // sc.RoundToIncrement is an ACSIL helper for this rounding.
    float calculatedEntryOffset = sc.RoundToIncrement(rawEntryOffset, sc.TickSize);
    float calculatedStopOffset = sc.RoundToIncrement(rawStopOffset, sc.TickSize);
    float calculatedTakeProfitOffset = sc.RoundToIncrement(rawTakeProfitOffset, sc.TickSize);

    // Debug logging for calculated offsets if enabled.
    int& lastLoggedOffsetsBar = sc.GetPersistentInt(PID_LAST_LOGGED_OFFSETS_BAR);
    if (currentLogLevel >= LOG_LEVEL_VERBOSE) { // Changed from DEBUG to VERBOSE to match enum
        if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedOffsetsBar != sc.CurrentIndex) {
            logMsg.Format("VERBOSE: R_Value: %.5f, RawEntryOff: %.5f, RawStopOff: %.5f, RawTPOff: %.5f", R_value, rawEntryOffset, rawStopOffset, rawTakeProfitOffset);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_VERBOSE, logMsg);
            logMsg.Format("VERBOSE: CalcEntryOff: %.5f, CalcStopOff: %.5f, CalcTPOff: %.5f, TickSize: %.5f",
                calculatedEntryOffset, calculatedStopOffset, calculatedTakeProfitOffset, sc.TickSize);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_VERBOSE, logMsg);
            lastLoggedOffsetsBar = sc.CurrentIndex;
        }
    }

    // Ensure calculated offsets are at least one tick size.
    // This prevents orders from being placed too close or at invalid prices.
    bool entryOffsetAdjusted = false;
    bool stopOffsetAdjusted = false;
    bool tpOffsetAdjusted = false;

    if (calculatedEntryOffset < sc.TickSize) {
        calculatedEntryOffset = sc.TickSize;
        entryOffsetAdjusted = true;
    }
    if (calculatedStopOffset < sc.TickSize) {
        calculatedStopOffset = sc.TickSize;
        stopOffsetAdjusted = true;
    }
    if (calculatedTakeProfitOffset < sc.TickSize) {
        calculatedTakeProfitOffset = sc.TickSize;
        tpOffsetAdjusted = true;
    }

    // Log adjustments if DEBUG level is met and an adjustment occurred
    if (currentLogLevel >= LOG_LEVEL_DEBUG && (entryOffsetAdjusted || stopOffsetAdjusted || tpOffsetAdjusted)) {
         if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedOffsetsBar != sc.CurrentIndex) {
            if (entryOffsetAdjusted) {
                logMsg.Format("DEBUG: Entry offset was less than TickSize (%.5f), adjusted to TickSize.", sc.TickSize);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
            }
            if (stopOffsetAdjusted) {
                logMsg.Format("DEBUG: Stop offset was less than TickSize (%.5f), adjusted to TickSize.", sc.TickSize);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
            }
            if (tpOffsetAdjusted) {
                logMsg.Format("DEBUG: Take Profit offset was less than TickSize (%.5f), adjusted to TickSize.", sc.TickSize);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, logMsg);
            }
            // Update lastLoggedOffsetsBar if we logged anything here (or above if VERBOSE was on)
            if (currentLogLevel < LOG_LEVEL_VERBOSE) lastLoggedOffsetsBar = sc.CurrentIndex;
         }
    }


    //── State Machine Logic ───────────────────────────────────────────────
    TradeSide currentTradeSide = static_cast<TradeSide>(CurrentTradeSide_Persist);
    BracketStatus currentBracketStatus = static_cast<BracketStatus>(IsBracketArmed_Persist);

    // STATE 1: FLAT and OCO BRACKET NOT ARMED --> Try to place OCO bracket
    // Bot is flat, no orders are out, conditions are met to try and enter.
    if (currentTradeSide == SIDE_FLAT && currentBracketStatus == BRACKET_NOT_ARMED)
    {
        // sc.Close is an array of closing prices for each bar. sc.Close[sc.Index] is the latest close.
        float currentClosePrice = sc.Close[sc.Index];
        // Calculate entry limit prices. sc.RoundToTickSize ensures valid order prices.
        float buyLimitPrice = sc.RoundToTickSize(currentClosePrice - calculatedEntryOffset, sc.TickSize);
        float sellLimitPrice = sc.RoundToTickSize(currentClosePrice + calculatedEntryOffset, sc.TickSize);

        // Sanity check: buy limit must be below sell limit.
        if (buyLimitPrice >= sellLimitPrice) {
            logMsg.Format("Calculated Buy Limit (%.5f) is not below Sell Limit (%.5f). Adjusting buy limit down by one tick.", buyLimitPrice, sellLimitPrice);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_WARN, logMsg);
            buyLimitPrice = sc.RoundToTickSize(sellLimitPrice - sc.TickSize, sc.TickSize);
            if (buyLimitPrice >= sellLimitPrice) { // Still problematic
                 logMsg.Format("Still unable to set Buy Limit (%.5f) below Sell Limit (%.5f) after adjustment. TickSize: %.5f. Skipping OCO placement.", buyLimitPrice, sellLimitPrice, sc.TickSize);
                 LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, logMsg);
                 return; // Skip OCO placement this tick.
            }
        }

        logMsg.Format("Attempting to place OCO bracket. R=%.5f. Close=%.5f. BuyLimit@%.5f, SellLimit@%.5f, StopOffset=%.5f, TPOffset=%.5f",
            R_value, currentClosePrice, buyLimitPrice, sellLimitPrice, calculatedStopOffset, calculatedTakeProfitOffset);
        LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg);

        // s_SCNewOrder is the ACSIL structure used to define parameters for a new order.
        s_SCNewOrder ocoOrder;
        ocoOrder.OrderQuantity = NumContracts.GetInt(); // Get quantity from user input.
        ocoOrder.OrderType = SCT_ORDERTYPE_OCO_BUY_LIMIT_SELL_LIMIT; // Specify OCO order type.

        // Define the BUY leg of the OCO
        ocoOrder.Price1 = buyLimitPrice; // Price for the buy limit order.
        ocoOrder.Stop1Offset = calculatedStopOffset;      // Stop-loss offset for the buy leg.
        ocoOrder.Target1Offset = calculatedTakeProfitOffset;  // Take-profit offset for the buy leg.
        ocoOrder.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT; // Target is a Limit order.
        ocoOrder.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;    // Stop is a Stop Market order.

        // Define the SELL leg of the OCO
        ocoOrder.Price2 = sellLimitPrice; // Price for the sell limit order.
        ocoOrder.Stop1Offset_2 = calculatedStopOffset;     // Stop-loss offset for the sell leg.
        ocoOrder.Target1Offset_2 = calculatedTakeProfitOffset; // Take-profit offset for the sell leg.
        ocoOrder.AttachedOrderTarget2Type = SCT_ORDERTYPE_LIMIT; // Target is a Limit order.
        ocoOrder.AttachedOrderStop2Type = SCT_ORDERTYPE_STOP;    // Stop is a Stop Market order.

        // Submit the OCO order to Sierra Chart's trading system.
        // This function returns an integer. >0 means success, and it's the InternalOrderID of the first OCO leg.
        int submissionResult = sc.SubmitOCOOrder(ocoOrder);

        if (submissionResult > 0) // OCO submission was successful
        {
            // Store the InternalOrderIDs of the parent OCO limit orders and their potential attached orders.
            // These IDs are returned in the ocoOrder structure after sc.SubmitOCOOrder.
            ParentBuyLimitOrderID_Persist = ocoOrder.InternalOrderID;   // ID of the Buy Limit leg
            ParentSellLimitOrderID_Persist = ocoOrder.InternalOrderID2; // ID of the Sell Limit leg

            IsBracketArmed_Persist = BRACKET_ARMED_AND_WORKING; // Update bot state.

            logMsg.Format("OCO Bracket submitted. BuyLimitID: %d (S:%d, T:%d), SellLimitID: %d (S:%d, T:%d)",
                ParentBuyLimitOrderID_Persist, ocoOrder.Stop1InternalOrderID, ocoOrder.Target1InternalOrderID,
                ParentSellLimitOrderID_Persist, ocoOrder.Stop1InternalOrderID_2, ocoOrder.Target1InternalOrderID_2);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
        }
        else // OCO submission failed
        {
            logMsg.Format("SubmitOCOOrder FAILED. Result code: %d. Check Trade Service Log for details.", submissionResult);
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, logMsg, true);
            // Ensure state reflects failure (redundant if already 0, but good practice)
            ParentBuyLimitOrderID_Persist = 0;
            ParentSellLimitOrderID_Persist = 0;
            IsBracketArmed_Persist = BRACKET_NOT_ARMED;
        }
        return; // Finished processing for this tick.
    }

    // STATE 2: OCO BRACKET IS ARMED, CURRENTLY FLAT --> Poll for entry fill
    // OCO entry orders are working, waiting for one of them to be filled.
    if (currentTradeSide == SIDE_FLAT && currentBracketStatus == BRACKET_ARMED_AND_WORKING)
    {
        s_SCTradeOrder filledOrderDetails; // Structure to hold details of a filled order.
        bool entryFilled = false;          // Flag to track if an entry occurred.
        TradeSide sideEntered = SIDE_FLAT; // To store which side got filled.
        int filledParentID = 0;

        // Check status of the BUY LIMIT parent order.
        if (ParentBuyLimitOrderID_Persist != 0 && sc.GetOrderByOrderID(ParentBuyLimitOrderID_Persist, filledOrderDetails) != SCTRADING_ORDER_ERROR)
        {
            if (filledOrderDetails.OrderStatusCode == SCT_OSC_FILLED) // Order filled
            {
                sideEntered = SIDE_LONG;
                filledParentID = ParentBuyLimitOrderID_Persist;
                entryFilled = true;
                logMsg.Format("Entry filled: BUY LIMIT (ParentOrderID: %d) filled. Quantity: %.0f, AvgFillPrice: %.5f",
                    ParentBuyLimitOrderID_Persist, filledOrderDetails.FilledQuantity, filledOrderDetails.AvgFillPrice);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
            }
            else if (filledOrderDetails.OrderStatusCode == SCT_OSC_CANCELED || filledOrderDetails.OrderStatusCode == SCT_OSC_ERROR) {
                logMsg.Format("Buy Limit ParentOrderID %d is now status %d", ParentBuyLimitOrderID_Persist, filledOrderDetails.OrderStatusCode);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_WARN, logMsg);
                ParentBuyLimitOrderID_Persist = 0; // Mark as inactive.
            }
        }

        // If BUY leg wasn't filled, check status of the SELL LIMIT parent order.
        if (!entryFilled && ParentSellLimitOrderID_Persist != 0 && sc.GetOrderByOrderID(ParentSellLimitOrderID_Persist, filledOrderDetails) != SCTRADING_ORDER_ERROR)
        {
            if (filledOrderDetails.OrderStatusCode == SCT_OSC_FILLED) // Order filled
            {
                sideEntered = SIDE_SHORT;
                filledParentID = ParentSellLimitOrderID_Persist;
                entryFilled = true;
                logMsg.Format("Entry filled: SELL LIMIT (ParentOrderID: %d) filled. Quantity: %.0f, AvgFillPrice: %.5f",
                    ParentSellLimitOrderID_Persist, filledOrderDetails.FilledQuantity, filledOrderDetails.AvgFillPrice);
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
            }
            else if (filledOrderDetails.OrderStatusCode == SCT_OSC_CANCELED || filledOrderDetails.OrderStatusCode == SCT_OSC_ERROR) {
                 logMsg.Format("Sell Limit ParentOrderID %d is now status %d", ParentSellLimitOrderID_Persist, filledOrderDetails.OrderStatusCode);
                 LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_WARN, logMsg);
                 ParentSellLimitOrderID_Persist = 0; // Mark as inactive.
            }
        }

        // If an entry was filled:
        if (entryFilled)
        {
            CurrentTradeSide_Persist = sideEntered; // Update trade side.
            ActiveFilledParentOrderID_Persist = filledParentID;
            IsBracketArmed_Persist = BRACKET_NOT_ARMED; // OCO bracket is no longer considered "armed".

            // Set the Active Stop and Target Order IDs based on which leg was filled.
            if (sideEntered == SIDE_LONG) {
                ParentSellLimitOrderID_Persist = 0;
            } else { // SIDE_SHORT
                ParentBuyLimitOrderID_Persist = 0;
            }
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_DEBUG, "Trade entered. Waiting for SL/TP of active trade.");
        } else { // No entry fill yet.
            // If both parent OCO legs became inactive (e.g., user cancelled, or SC cancelled one after the other was rejected),
            // then reset the bracket state.
            if (ParentBuyLimitOrderID_Persist == 0 && ParentSellLimitOrderID_Persist == 0 && currentBracketStatus == BRACKET_ARMED_AND_WORKING) {
                LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_WARN, "Both OCO parent legs seem inactive without a fill. Resetting bracket state.");
                IsBracketArmed_Persist = BRACKET_NOT_ARMED;
                ActiveFilledParentOrderID_Persist = 0;
            } else if (currentLogLevel >= LOG_LEVEL_VERBOSE) {
                 LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_VERBOSE, "VERBOSE: OCO Armed, no entry fill detected yet.");
            }
        }
        return; // Finished processing for this tick.
    }

    // STATE 3: IN A TRADE --> Poll for exit (SL or TP hit on attached orders, or critical cancellation)
    // Bot has an open position (long or short) and is monitoring its active stop-loss and take-profit.
    if (currentTradeSide != SIDE_FLAT)
    {
        bool exitDetected = false;
        s_SCTradeOrder childOrderDetails;
        int orderIndex = 0;

        if (ActiveFilledParentOrderID_Persist == 0) {
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, "In trade, but ActiveFilledParentOrderID is 0. Cannot monitor SL/TP. This is an inconsistent state.", true);
            s_SCPositionData posCheck; sc.GetTradePosition(posCheck);
            if(posCheck.PositionQuantity != 0) sc.FlattenPosition();
            CurrentTradeSide_Persist = SIDE_FLAT;
            return;
        }

        while (sc.GetOrderByIndex(orderIndex++, childOrderDetails) != SCTRADING_ORDER_ERROR)
        {
            if (childOrderDetails.ParentInternalOrderID == ActiveFilledParentOrderID_Persist)
            {
                if (currentLogLevel >= LOG_LEVEL_VERBOSE) {
                    logMsg.Format("VERBOSE: Checking child order ID %d of ActiveFilledParentID %d. Status: %d, Type: %d",
                        childOrderDetails.InternalOrderID, ActiveFilledParentOrderID_Persist, childOrderDetails.OrderStatusCode, childOrderDetails.OrderTypeAsInt);
                    LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_VERBOSE, logMsg);
                }

                if (childOrderDetails.OrderStatusCode == SCT_OSC_FILLED)
                {
                    exitDetected = true;
                    logMsg.Format("Exit detected: Attached Order (ID: %d, ParentID: %d, Type: %s) FILLED. Qty: %.0f, Price: %.5f",
                        childOrderDetails.InternalOrderID,
                        ActiveFilledParentOrderID_Persist,
                        (childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP || childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP_LIMIT) ? "STOP" : "TARGET",
                        childOrderDetails.FilledQuantity,
                        childOrderDetails.AvgFillPrice);
                    LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, logMsg, true);
                    break;
                }
                else if (childOrderDetails.OrderStatusCode == SCT_OSC_CANCELED ||
                         childOrderDetails.OrderStatusCode == SCT_OSC_ERROR)
                {
                    logMsg.Format("CRITICAL SAFETY: Active SL/TP child order (ID: %d, ParentID: %d, Type: %s) is now status %d! Position may be unprotected.",
                        childOrderDetails.InternalOrderID, ActiveFilledParentOrderID_Persist,
                        (childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP || childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP_LIMIT) ? "STOP" : "TARGET",
                        childOrderDetails.OrderStatusCode);
                    LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, logMsg, true);

                    s_SCPositionData currentPos;
                    sc.GetTradePosition(currentPos);
                    if (currentPos.PositionQuantity != 0) {
                        LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_ERROR, "Attempting to flatten position due to unexpected issue with active SL/TP order.", true);
                        sc.FlattenPosition();
                    }
                    exitDetected = true;
                    break;
                }
            }
        }

        if (exitDetected)
        {
            ParentBuyLimitOrderID_Persist = 0;
            ParentSellLimitOrderID_Persist = 0;
            ActiveFilledParentOrderID_Persist = 0;
            CurrentTradeSide_Persist = SIDE_FLAT;
            IsBracketArmed_Persist = BRACKET_NOT_ARMED;
            LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_INFO, "Trade exited/flattened. All states reset. Ready for new OCO bracket.");
        } else if (currentLogLevel >= LOG_LEVEL_VERBOSE) {
             LogSCSMessage(sc, currentLogLevel, LOG_LEVEL_VERBOSE, "VERBOSE: In trade, no SL/TP fill or critical order issue detected yet.");
        }
        return;
    }
}

// Helper function for logging messages to the Sierra Chart Message Log.
void LogSCSMessage(SCStudyInterfaceRef& sc, int currentLogLevelSetting, LoggingLevel messageLevel, const SCString& message, bool showInTradeServiceLog) {
    if (currentLogLevelSetting < static_cast<int>(messageLevel)) {
        return;
    }
    SCString logLevelStr;
    switch (messageLevel) {
        case LOG_LEVEL_ERROR:   logLevelStr = "ERROR";   break;
        case LOG_LEVEL_WARN:    logLevelStr = "WARN";    break;
        case LOG_LEVEL_INFO:    logLevelStr = "INFO";    break;
        case LOG_LEVEL_DEBUG:   logLevelStr = "DEBUG";   break;
        case LOG_LEVEL_VERBOSE: logLevelStr = "VERBOSE"; break;
        default:                logLevelStr = "LOG";     break;
    }
    SCString finalMessage;
    finalMessage.Format("%s [%s Bar:%d]: %s",
        sc.FormatDateTime(sc.CurrentSystemDateTime).GetChars(),
        logLevelStr.GetChars(),
        sc.CurrentIndex,
        message.GetChars()
    );
    sc.AddMessageToLog(finalMessage, showInTradeServiceLog);
}

void LogSCSMessage(SCStudyInterfaceRef& sc, int currentLogLevelSetting, LoggingLevel messageLevel, const char* message, bool showInTradeServiceLog) {
    SCString scsMessage(message);
    LogSCSMessage(sc, currentLogLevelSetting, messageLevel, scsMessage, showInTradeServiceLog);
}