/*
* ===================================================================
*   Scalping Bot (Carver Inspired) - ACSIL Study for Sierra Chart
* ===================================================================
*
*   INSPIRATION:
*   This automated scalping bot is inspired by the concepts discussed in Rob Carver's blog post:
*   "Can I build a scalping bot? A blogpost with numerous double digit SR"
*   (https://qoppac.blogspot.com/2025/05/can-i-build-scalping-bot-blogpost-with.html)
*   It adapts some of the core ideas for implementation within the Sierra Chart Advanced Custom Study Interface
*   Language (ACSIL) C++ environment.
*
*   STRATEGY OUTLINE:
*   The bot aims to profit from short-term price movements using a mean-reversion scalping approach.
*
*   1.  Dynamic Range 'R':
*       - The strategy relies on a dynamic range value, 'R', which is derived from a user-specified
*         study subgraph (e.g., an "Average Rotation" indicator, Average True Range, or similar volatility measure).
*       - This 'R' value represents the expected short-term trading range or volatility over the chart's
*         bar period 'H' (where H is the period of the chart the study is applied to).
*
*   2.  OCO Bracket Entry:
*       - When the bot is flat (no position) and within a defined trading window, it calculates entry,
*         stop-loss, and take-profit levels based on the current 'R' value.
*       - It then submits an OCO (One-Cancels-Other) bracket order consisting of:
*           a) A Buy Limit order placed (R * BracketFraction) below the current market price.
*           b) A Sell Limit order placed (R * BracketFraction) above the current market price.
*       - Both the Buy Limit and Sell Limit orders have attached Stop-Loss and Take-Profit orders:
*           - Stop-Loss is placed (R * StopLossFraction) away from the potential entry price.
*           - Take-Profit is placed (R * TakeProfitFraction) away from the potential entry price.
*
*   3.  Simplified State Machine:
*       - The use of OCO bracket orders with immediately attached stops and targets simplifies the state machine
*         compared to the blog post's more granular state descriptions.
*       - Key states managed by the bot:
*           - FLAT & READY: No position, no active OCO bracket. Awaiting conditions to place a new bracket.
*           - BRACKET_ARMED (Waiting for Entry): OCO parent limit orders are working.
*           - IN_TRADE (Position Active): An entry order has filled; attached stop/target orders are working.
*           - (Implicitly handles exit and reset to FLAT & READY upon SL/TP fill).
*
*   4.  Trading Window & Flattening:
*       - The bot only attempts to place new OCO bracket orders within a user-defined start and stop time.
*       - At the specified "Stop Time," if the bot has an open position, it will be automatically flattened.
*         Any working OCO parent orders (if not yet filled) will also be canceled. The bot aims to be flat
*         outside the active trading window.
*
*   CORE PARAMETERS:
*   - Volatility Subgraph (Range R): Source of the dynamic range value.
*   - Bracket Entry Offset Fraction of R: Determines how far from current price the initial OCO limits are set.
*   - Stop Loss Offset Fraction of R: Determines the stop-loss distance from the entry price.
*   - Take Profit Offset Fraction of R: Determines the take-profit distance from the entry price.
*   - Number of Contracts, Start/Stop Times, Trading Enable switch, Log Level.
*
*   IMPORTANT CONSIDERATIONS FOR USE:
*   - This is an advanced auto-trading study. Thorough testing in a simulated environment with realistic
*     settings (including "Estimated Position in Queue" and appropriate slippage simulation for stops)
*     is crucial before considering live trading.
*   - The quality and responsiveness of the 'R' value (volatility subgraph) significantly impact performance.
*   - Market conditions, instrument liquidity, and transaction costs will affect profitability.
*
* ===================================================================
*/

#include "sierrachart.h"

SCDLLName("Scalping Bot") // DLL name for Sierra Chart menu

// Define logging levels for clarity
enum LoggingLevel {
    LOG_LEVEL_NONE = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_DEBUG = 2,
    LOG_LEVEL_VERBOSE = 3
};

// Forward declaration of helper function for logging
void LogSCSMessage(SCStudyInterfaceRef& sc, LoggingLevel level, const SCString& message, bool showInTradeServiceLog = false);
void LogSCSMessage(SCStudyInterfaceRef& sc, LoggingLevel level, const char* message, bool showInTradeServiceLog = false);


SCSFExport scsf_Scalping_Bot(SCStudyInterfaceRef sc)
{
    //── Study Inputs ──────────────────────────────────────────────────────
    SCInputRef NumContracts = sc.Input[0];
    SCInputRef VolSubgraph = sc.Input[1];       // Input for the 'R' value from another study's subgraph
    SCInputRef BracketFrac = sc.Input[2];       // Fraction of R for distance from current price to OCO limits
    SCInputRef StopFrac = sc.Input[3];          // Fraction of R for stop-loss offset from entry price
    SCInputRef TPFrac = sc.Input[4];            // Fraction of R for take-profit offset from entry price
    SCInputRef StartTimeInput = sc.Input[5];    // Trading start time
    SCInputRef StopTimeInput = sc.Input[6];     // Trading stop time (also flatten time)
    SCInputRef EnableInput = sc.Input[7];       // Master enable/disable for trading
    SCInputRef LogLevelInput = sc.Input[8];     // Controls the verbosity of logging

    //── Persistent State Variables ───────────────────────────────────────
    // These variables retain their values across calls to the study function.
    int& ParentBuyLimitOrderID = sc.GetPersistentInt(1);  // Stores the InternalOrderID of the parent BUY LIMIT of the OCO
    int& ParentSellLimitOrderID = sc.GetPersistentInt(2); // Stores the InternalOrderID of the parent SELL LIMIT of the OCO
    int& CurrentTradeSide = sc.GetPersistentInt(3);       // 0=flat, 1=long, 2=short
    int& IsBracketArmed = sc.GetPersistentInt(4);         // 0=No OCO bracket working, 1=OCO bracket has been submitted and is working

    //── Default Settings ──────────────────────────────────────────────────
    if (sc.SetDefaults)
    {
        sc.GraphName = "Scalping Bot"; // Name displayed in chart
        sc.AutoLoop = 1;  // Process the study function on every tick/update
        sc.UpdateAlways = 1; // Ensure sc.Index is always ArraySize-1 in the main block
        sc.MaintainTradeStatisticsAndTradesData = true; // Necessary for sc.GetTradePosition and order history

        // Initialize Input Parameters
        NumContracts.Name = "Number of Contracts";
        NumContracts.SetInt(1);
        NumContracts.SetIntLimits(1, 1000);

        VolSubgraph.Name = "Volatility Subgraph (Range R)";
        VolSubgraph.SetStudySubgraphValues(0, 0); // Default to Study 0, Subgraph 0 (placeholder)

        BracketFrac.Name = "Bracket Entry Offset Fraction of R";
        BracketFrac.SetFloat(0.25f); // e.g., R * 0.25 distance from close to limit prices
        BracketFrac.SetFloatLimits(0.001f, 5.0f);

        StopFrac.Name = "Stop Loss Offset Fraction of R";
        StopFrac.SetFloat(0.5f);     // e.g., R * 0.5 stop loss distance from entry
        StopFrac.SetFloatLimits(0.001f, 10.0f);

        TPFrac.Name = "Take Profit Offset Fraction of R";
        TPFrac.SetFloat(1.0f);       // e.g., R * 1.0 take profit distance from entry
        TPFrac.SetFloatLimits(0.001f, 20.0f);

        StartTimeInput.Name = "Start Time (HHMMSS)";
        StartTimeInput.SetTime(HMS_TIME(8, 30, 0)); // Example: 08:30:00

        StopTimeInput.Name = "Stop Time (HHMMSS) & Flatten";
        StopTimeInput.SetTime(HMS_TIME(15, 0, 0));  // Example: 15:00:00

        EnableInput.Name = "Enable Trading";
        EnableInput.SetYesNo(false); // Default to disabled for safety

        LogLevelInput.Name = "Log Detail Level";
        LogLevelInput.SetInt(LOG_LEVEL_INFO); // Default to Info level
        LogLevelInput.SetIntLimits(LOG_LEVEL_NONE, LOG_LEVEL_VERBOSE);
        LogLevelInput.AddInputPath("ACSAdvancedCustomStudyExamples.ScalpingBot.LogLevelInput"); // For easier access via Control Bar

        // Critical Unmanaged Auto-trading Settings (User should be aware these are set by the study)
        // These settings control how Sierra Chart's trading system interacts with this study's orders.
        sc.MaximumPositionAllowed = 100000; // Effectively unlimited for this bot's typical small positions
        sc.AllowMultipleEntriesInSameDirection = true; // Allows new entries even if already in a position (though this bot aims for flat before new entry)
        sc.AllowOppositeEntryWithOpposingPositionOrOrders = true; // Allows reversing signals (not directly used by this bot's simple logic)
        sc.CancelAllOrdersOnEntriesAndReversals = false; // Bot manages its own order cancellations explicitly
        sc.CancelAllOrdersOnReversals = false;
        sc.CancelAllOrdersOnEntries = false;
        sc.AllowEntryWithWorkingOrders = true; // Allows entry even if other orders are working (bot manages OCO)
        sc.CancelAllWorkingOrdersOnExit = false; // Bot handles reset explicitly, FlattenPosition handles attached orders
        sc.SupportAttachedOrdersForTrading = true; // Essential for OCO with attached stops/targets

        return; // Exit after setting defaults
    }

    //── Bootstrap Logic (Full Recalculation, First Bar) ──────────────────
    // This section runs ONCE when the study is first applied or fully recalculated.
    // It attempts to determine the bot's state if it was already running or if orders exist.
    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        SCString bootstrapMsg;
        LogSCSMessage(sc, LOG_LEVEL_DEBUG, "Performing bootstrap on full recalculation.");

        // 1. Infer current position from Sierra Chart's trade data
        s_SCPositionData pos;
        sc.GetTradePosition(pos);
        if (pos.PositionQuantity > 0) CurrentTradeSide = 1; // Long
        else if (pos.PositionQuantity < 0) CurrentTradeSide = 2; // Short
        else CurrentTradeSide = 0; // Flat

        bootstrapMsg.Format("Bootstrap: Current Position Qty: %d, Inferred TradeSide: %d", pos.PositionQuantity, CurrentTradeSide);
        LogSCSMessage(sc, LOG_LEVEL_DEBUG, bootstrapMsg);

        // 2. Attempt to re-identify working OCO bracket orders if currently flat.
        // This helps if the study is reloaded while an OCO was placed but not filled.
        // It does NOT rely on persisted order IDs for this scan, but re-scans.
        // If not flat, it assumes the persisted EntryOrderIDs relate to the current trade.
        if (CurrentTradeSide == 0)
        {
            // If flat, reset any assumptions about armed brackets and specific order IDs from persistent memory,
            // then try to find live ones.
            int local_ParentBuyLimitOrderID = 0;
            int local_ParentSellLimitOrderID = 0;
            bool local_IsBracketArmed = false;

            int orderIndex = 0;
            s_SCTradeOrder currentOrder;
            std::vector<int> validParentLimitOrders; // Store potential OCO parent limit orders

            while (sc.GetOrderByIndex(orderIndex++, currentOrder) != SCTRADING_ORDER_ERROR)
            {
                // Look for OPEN, PARENT (not attached) LIMIT orders
                if (currentOrder.OrderStatusCode == SCT_OSC_OPEN &&
                    currentOrder.ParentInternalOrderID == 0 &&
                    currentOrder.OrderTypeAsInt == SCT_ORDERTYPE_LIMIT)
                {
                    // Check if this limit order has 2 attached children (potential stop and target)
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
                        validParentLimitOrders.push_back(currentOrder.InternalOrderID);
                    }
                }
            }

            // If exactly two such parent limit orders are found, assume they form an OCO pair.
            // This is a heuristic. For more robustness, one might tag orders with ClientOrderID.
            if (validParentLimitOrders.size() == 2)
            {
                // Assuming the first is Buy Limit and second is Sell Limit based on typical OCO structure.
                // A more robust check would compare their prices relative to market.
                s_SCTradeOrder orderA, orderB;
                sc.GetOrderByOrderID(validParentLimitOrders[0], orderA);
                sc.GetOrderByOrderID(validParentLimitOrders[1], orderB);

                // Assign based on price: lower price is Buy Limit, higher is Sell Limit.
                if (orderA.Price1 < orderB.Price1) { // orderA.Price1 is the limit price
                    local_ParentBuyLimitOrderID = orderA.InternalOrderID;
                    local_ParentSellLimitOrderID = orderB.InternalOrderID;
                } else {
                    local_ParentBuyLimitOrderID = orderB.InternalOrderID;
                    local_ParentSellLimitOrderID = orderA.InternalOrderID;
                }
                local_IsBracketArmed = true;

                // Update persistent state
                ParentBuyLimitOrderID = local_ParentBuyLimitOrderID;
                ParentSellLimitOrderID = local_ParentSellLimitOrderID;
                IsBracketArmed = local_IsBracketArmed;

                bootstrapMsg.Format("Bootstrap: Found and re-armed OCO bracket. BuyLimitID: %d, SellLimitID: %d", ParentBuyLimitOrderID, ParentSellLimitOrderID);
                LogSCSMessage(sc, LOG_LEVEL_INFO, bootstrapMsg);
            }
            else
            {
                // If not exactly 2 found, assume no valid OCO from this bot is active. Clear persisted IDs.
                ParentBuyLimitOrderID = 0;
                ParentSellLimitOrderID = 0;
                IsBracketArmed = 0;
                if (!validParentLimitOrders.empty()) {
                    bootstrapMsg.Format("Bootstrap: Found %d potential parent orders, but not 2. Not arming OCO.", (int)validParentLimitOrders.size());
                    LogSCSMessage(sc, LOG_LEVEL_DEBUG, bootstrapMsg);
                } else {
                     LogSCSMessage(sc, LOG_LEVEL_DEBUG, "Bootstrap: No active OCO bracket found while flat.");
                }
            }
        } else { // Currently InTrade
             LogSCSMessage(sc, LOG_LEVEL_DEBUG, "Bootstrap: In a trade. Persisted parent OCO IDs will be used for exit detection.");
             // IsBracketArmed should be 0 if InTrade. If not, it's an inconsistent state.
             if (IsBracketArmed) {
                 LogSCSMessage(sc, LOG_LEVEL_WARN, "Bootstrap WARNING: InTrade is true, but IsBracketArmed is also true. Resetting IsBracketArmed to 0.");
                 IsBracketArmed = 0; // Correct inconsistent state
             }
        }
    }

    //── Main Trading Logic (runs only on the last bar of the chart) ─────
    // With AutoLoop=1 and UpdateAlways=1, this ensures the logic runs on each tick/update.
    if (sc.Index != sc.ArraySize - 1)
        return; // Only process on the most recent bar/tick data

    SCString logMsg; // Reusable SCString for log messages

    //── Trading Enabled Check ─────────────────────────────────────────────
    if (!EnableInput.GetYesNo())
    {
        // Log only once per new bar to avoid flooding if trading is disabled
        int& lastLoggedDisabledBar = sc.GetPersistentInt(100); // Unique key for this persistent var
        if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedDisabledBar != sc.CurrentBarIndex) {
            LogSCSMessage(sc, LOG_LEVEL_INFO, "Trading is disabled via 'Enable Trading' input.");
            lastLoggedDisabledBar = sc.CurrentBarIndex;
        }
        return; // Stop further processing if trading is not enabled
    }

    //── Time Gating Logic ─────────────────────────────────────────────────
    int currentTime = sc.BaseDateTimeIn[sc.Index].GetTime(); // Current bar's time
    int tradingStartTime = StartTimeInput.GetTime();
    int tradingStopTime = StopTimeInput.GetTime();

    if (currentTime < tradingStartTime)
    {
        // Log only once per new bar before trading window
        int& lastLoggedBeforeWindowBar = sc.GetPersistentInt(101);
        if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedBeforeWindowBar != sc.CurrentBarIndex) {
             logMsg.Format("Waiting for trading window to start. CurrentTime: %06d, StartTime: %06d", currentTime, tradingStartTime);
             LogSCSMessage(sc, LOG_LEVEL_DEBUG, logMsg);
             lastLoggedBeforeWindowBar = sc.CurrentBarIndex;
        }
        return; // Not yet time to trade
    }

    if (currentTime >= tradingStopTime)
    {
        logMsg.Format("Trading window ended (CurrentTime: %06d, StopTime: %06d). Flattening position and cancelling orders.", currentTime, tradingStopTime);
        LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg, true); // Show in Trade Service Log

        // Cancel any working OCO parent orders if the bracket was armed but not filled
        if (IsBracketArmed)
        {
            if (ParentBuyLimitOrderID != 0) {
                LogSCSMessage(sc, LOG_LEVEL_DEBUG, "End of Day: Cancelling ParentBuyLimitOrderID: " + SCString(ParentBuyLimitOrderID));
                sc.CancelOrder(ParentBuyLimitOrderID);
            }
            if (ParentSellLimitOrderID != 0) {
                LogSCSMessage(sc, LOG_LEVEL_DEBUG, "End of Day: Cancelling ParentSellLimitOrderID: " + SCString(ParentSellLimitOrderID));
                sc.CancelOrder(ParentSellLimitOrderID);
            }
        }

        // Flatten any open position
        s_SCPositionData positionData;
        sc.GetTradePosition(positionData);
        if (positionData.PositionQuantity != 0)
        {
            LogSCSMessage(sc, LOG_LEVEL_INFO, "End of Day: Flattening open position of " + SCString(positionData.PositionQuantity) + " contracts.", true);
            sc.FlattenPosition(); // This will also cancel associated stops/targets for the position
        }

        // Reset all state variables
        ParentBuyLimitOrderID = 0;
        ParentSellLimitOrderID = 0;
        CurrentTradeSide = 0;
        IsBracketArmed = 0;
        LogSCSMessage(sc, LOG_LEVEL_INFO, "End of Day: All states reset. Bot is flat and idle.");
        return; // Stop further processing for the day
    }

    //── Calculate Dynamic Offsets based on 'R' ──────────────────────────
    SCFloatArray volatilityArray;
    sc.GetStudyArrayUsingID(VolSubgraph.GetStudyID(), VolSubgraph.GetSubgraphIndex(), volatilityArray);

    if (volatilityArray.IsArrayEmpty() || sc.Index >= volatilityArray.GetArraySize() || volatilityArray[sc.Index] <= 0.0f)
    {
        // Log only once per new bar if R is invalid
        int& lastLoggedInvalidRBar = sc.GetPersistentInt(102);
         if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED || lastLoggedInvalidRBar != sc.CurrentBarIndex) {
            logMsg.Format("Invalid or zero 'R' (volatility) value from subgraph at Index %d. Value: %f. Cannot calculate offsets.", sc.Index, volatilityArray.IsArrayEmpty() ? 0.0f : volatilityArray[sc.Index]);
            LogSCSMessage(sc, LOG_LEVEL_WARN, logMsg);
            lastLoggedInvalidRBar = sc.CurrentBarIndex;
        }
        return; // Cannot proceed without a valid R value
    }
    float R_value = volatilityArray[sc.Index];

    // Calculate raw offsets based on R and input fractions
    float rawEntryOffset = R_value * BracketFrac.GetFloat();
    float rawStopOffset = R_value * StopFrac.GetFloat();
    float rawTakeProfitOffset = R_value * TPFrac.GetFloat();

    // Round offsets to tick size for order placement
    // Entry offset used to calculate limit prices from current close
    float calculatedEntryOffset = sc.RoundToIncrement(rawEntryOffset, sc.TickSize);
    // Stop and TP offsets are directly used in s_SCNewOrder as price offsets
    float calculatedStopOffset = sc.RoundToIncrement(rawStopOffset, sc.TickSize);
    float calculatedTakeProfitOffset = sc.RoundToIncrement(rawTakeProfitOffset, sc.TickSize);

    if (LogLevelInput.GetInt() >= LOG_LEVEL_VERBOSE) {
        logMsg.Format("VERBOSE: R_Value: %.5f, CalcEntryOff: %.5f, CalcStopOff: %.5f, CalcTPOff: %.5f, TickSize: %.5f",
            R_value, calculatedEntryOffset, calculatedStopOffset, calculatedTakeProfitOffset, sc.TickSize);
        LogSCSMessage(sc, LOG_LEVEL_VERBOSE, logMsg);
    }

    // Ensure offsets are at least one tick size
    if (calculatedEntryOffset < sc.TickSize) calculatedEntryOffset = sc.TickSize;
    if (calculatedStopOffset < sc.TickSize) calculatedStopOffset = sc.TickSize;
    if (calculatedTakeProfitOffset < sc.TickSize) calculatedTakeProfitOffset = sc.TickSize;


    //── State Machine Logic ───────────────────────────────────────────────

    // STATE 1: FLAT and OCO BRACKET NOT ARMED --> Try to place OCO bracket
    if (CurrentTradeSide == 0 && IsBracketArmed == 0)
    {
        float currentClosePrice = sc.Close[sc.Index];
        float buyLimitPrice = sc.RoundToTickSize(currentClosePrice - calculatedEntryOffset, sc.TickSize);
        float sellLimitPrice = sc.RoundToTickSize(currentClosePrice + calculatedEntryOffset, sc.TickSize);

        // Ensure buy limit is below sell limit and there's some spread
        if (buyLimitPrice >= sellLimitPrice) {
            logMsg.Format("Calculated Buy Limit (%.5f) is not below Sell Limit (%.5f). Adjusting or skipping.", buyLimitPrice, sellLimitPrice);
            LogSCSMessage(sc, LOG_LEVEL_WARN, logMsg);
            // Potentially adjust by a tick or skip this attempt
            buyLimitPrice = sc.RoundToTickSize(sellLimitPrice - sc.TickSize, sc.TickSize);
            if (buyLimitPrice >= sellLimitPrice) {
                 LogSCSMessage(sc, LOG_LEVEL_ERROR, "Still unable to set Buy Limit below Sell Limit. Skipping OCO placement.");
                 return;
            }
        }


        logMsg.Format("Attempting to place OCO bracket. R=%.5f. BuyLimit@%.5f, SellLimit@%.5f, StopOffset=%.5f, TPOffset=%.5f",
            R_value, buyLimitPrice, sellLimitPrice, calculatedStopOffset, calculatedTakeProfitOffset);
        LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg);

        s_SCNewOrder ocoOrder;
        ocoOrder.OrderQuantity = NumContracts.GetInt();
        ocoOrder.OrderType = SCT_ORDERTYPE_OCO_BUY_LIMIT_SELL_LIMIT; // OCO: One Buy Limit, One Sell Limit

        // Leg 1: Buy Limit Order (Primary in s_SCNewOrder terms for OCO)
        ocoOrder.Price1 = buyLimitPrice; // Price for the Buy Limit
        ocoOrder.Stop1Offset = calculatedStopOffset;      // Stop offset if Buy Limit fills
        ocoOrder.Target1Offset = calculatedTakeProfitOffset;  // Target offset if Buy Limit fills
        ocoOrder.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT; // Target is a Limit order
        ocoOrder.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP;    // Stop is a Stop order

        // Leg 2: Sell Limit Order (Secondary in s_SCNewOrder terms for OCO)
        ocoOrder.Price2 = sellLimitPrice; // Price for the Sell Limit
        ocoOrder.Stop1Offset_2 = calculatedStopOffset;     // Stop offset if Sell Limit fills
        ocoOrder.Target1Offset_2 = calculatedTakeProfitOffset; // Target offset if Sell Limit fills
        ocoOrder.AttachedOrderTarget2Type = SCT_ORDERTYPE_LIMIT; // Target is a Limit order
        ocoOrder.AttachedOrderStop2Type = SCT_ORDERTYPE_STOP;    // Stop is a Stop order


        int submissionResult = sc.SubmitOCOOrder(ocoOrder);

        if (submissionResult > 0) // Success if quantity > 0 (or other positive indication)
        {
            ParentBuyLimitOrderID = ocoOrder.InternalOrderID;   // ID of the Buy Limit part of OCO
            ParentSellLimitOrderID = ocoOrder.InternalOrderID2; // ID of the Sell Limit part of OCO
            IsBracketArmed = 1; // Mark OCO bracket as active

            logMsg.Format("OCO Bracket submitted successfully. BuyLimitID: %d (Attached StopID: %d, TargetID: %d), SellLimitID: %d (Attached StopID: %d, TargetID: %d)",
                ParentBuyLimitOrderID, ocoOrder.Stop1InternalOrderID, ocoOrder.Target1InternalOrderID,
                ParentSellLimitOrderID, ocoOrder.Stop1InternalOrderID_2, ocoOrder.Target1InternalOrderID_2);
            LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg, true); // Show in Trade Service Log
        }
        else
        {
            logMsg.Format("SubmitOCOOrder FAILED. Result code: %d. Check Trade Service Log for details.", submissionResult);
            LogSCSMessage(sc, LOG_LEVEL_ERROR, logMsg, true); // Show in Trade Service Log
            // Reset any potentially partially set IDs (though SubmitOCOOrder should handle atomicity)
            ParentBuyLimitOrderID = 0;
            ParentSellLimitOrderID = 0;
            IsBracketArmed = 0;
        }
        return; // Finished processing for this tick after attempting OCO placement
    }

    // STATE 2: OCO BRACKET IS ARMED, CURRENTLY FLAT --> Poll for entry fill
    if (CurrentTradeSide == 0 && IsBracketArmed == 1)
    {
        s_SCTradeOrder filledOrderDetails;
        bool entryFilled = false;
        int filledParentOrderID = 0;

        // Check if Buy Limit parent was filled
        if (ParentBuyLimitOrderID != 0 && sc.GetOrderByOrderID(ParentBuyLimitOrderID, filledOrderDetails) == SCTRADING_ORDER_NO_ERROR)
        {
            if (filledOrderDetails.OrderStatusCode == SCT_OSC_FILLED)
            {
                CurrentTradeSide = 1; // Entered Long
                filledParentOrderID = ParentBuyLimitOrderID;
                entryFilled = true;
                logMsg.Format("Entry filled: BUY LIMIT (ParentOrderID: %d) filled. Quantity: %.0f, AvgFillPrice: %.5f",
                    ParentBuyLimitOrderID, filledOrderDetails.FilledQuantity, filledOrderDetails.AvgFillPrice);
                LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg, true);
            }
            else if (filledOrderDetails.OrderStatusCode == SCT_OSC_CANCELED || filledOrderDetails.OrderStatusCode == SCT_OSC_REJECTED) {
                logMsg.Format("Buy Limit ParentOrderID %d is CANCELED/REJECTED. Status: %d", ParentBuyLimitOrderID, filledOrderDetails.OrderStatusCode);
                LogSCSMessage(sc, LOG_LEVEL_WARN, logMsg);
                // If one leg of OCO is unexpectedly canceled/rejected, the other might still be open.
                // Simplest is to reset the bracket state to attempt a new one if the other leg isn't filled.
                // More advanced logic could cancel the other leg. For now, this will be caught if neither fills.
                ParentBuyLimitOrderID = 0; // Invalidate this ID
            }
        }

        // Check if Sell Limit parent was filled (only if not already filled by buy)
        if (!entryFilled && ParentSellLimitOrderID != 0 && sc.GetOrderByOrderID(ParentSellLimitOrderID, filledOrderDetails) == SCTRADING_ORDER_NO_ERROR)
        {
            if (filledOrderDetails.OrderStatusCode == SCT_OSC_FILLED)
            {
                CurrentTradeSide = 2; // Entered Short
                filledParentOrderID = ParentSellLimitOrderID;
                entryFilled = true;
                logMsg.Format("Entry filled: SELL LIMIT (ParentOrderID: %d) filled. Quantity: %.0f, AvgFillPrice: %.5f",
                    ParentSellLimitOrderID, filledOrderDetails.FilledQuantity, filledOrderDetails.AvgFillPrice);
                LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg, true);
            }
            else if (filledOrderDetails.OrderStatusCode == SCT_OSC_CANCELED || filledOrderDetails.OrderStatusCode == SCT_OSC_REJECTED) {
                 logMsg.Format("Sell Limit ParentOrderID %d is CANCELED/REJECTED. Status: %d", ParentSellLimitOrderID, filledOrderDetails.OrderStatusCode);
                 LogSCSMessage(sc, LOG_LEVEL_WARN, logMsg);
                 ParentSellLimitOrderID = 0; // Invalidate this ID
            }
        }

        if (entryFilled)
        {
            IsBracketArmed = 0; // OCO resolved, now in a trade with attached SL/TP
                                // The non-filled OCO parent leg is automatically cancelled by the exchange/broker.
                                // ParentBuyLimitOrderID and ParentSellLimitOrderID now store the original parent IDs,
                                // one of which is filled, the other cancelled. These are used to find children.
            LogSCSMessage(sc, LOG_LEVEL_DEBUG, "Trade entered. IsBracketArmed set to 0. Waiting for SL/TP.");
        } else {
             // If both ParentOrderIDs become 0 due to cancellation/rejection without a fill, reset.
            if (ParentBuyLimitOrderID == 0 && ParentSellLimitOrderID == 0 && IsBracketArmed) {
                LogSCSMessage(sc, LOG_LEVEL_WARN, "Both OCO parent legs seem inactive without a fill. Resetting bracket state.");
                IsBracketArmed = 0; // Reset to allow new bracket placement
            } else if (LogLevelInput.GetInt() >= LOG_LEVEL_VERBOSE) {
                 LogSCSMessage(sc, LOG_LEVEL_VERBOSE, "VERBOSE: OCO Armed, no entry fill detected yet.");
            }
        }
        return; // Finished processing for this tick
    }

    // STATE 3: IN A TRADE --> Poll for exit (SL or TP hit on attached orders)
    if (CurrentTradeSide != 0) // Implies IsBracketArmed is 0
    {
        bool exitDetected = false;
        s_SCTradeOrder childOrderDetails;
        int orderIndex = 0;

        // The attached stop/target orders are children of ONE of the original OCO parent orders.
        // We need to check children of both ParentBuyLimitOrderID and ParentSellLimitOrderID,
        // as we don't explicitly store which one was filled here (though CurrentTradeSide implies it).
        // One of these IDs corresponds to the filled parent, the other to the auto-cancelled one.
        int originalOcoParents[] = { ParentBuyLimitOrderID, ParentSellLimitOrderID };

        for (int parentID : originalOcoParents)
        {
            if (parentID == 0) continue; // Skip if this parent ID was not set or cleared

            orderIndex = 0; // Reset for each parent
            while (sc.GetOrderByIndex(orderIndex++, childOrderDetails) != SCTRADING_ORDER_ERROR)
            {
                if (childOrderDetails.ParentInternalOrderID == parentID) // Is it a child of one of our OCO parents?
                {
                    if (LogLevelInput.GetInt() >= LOG_LEVEL_VERBOSE) {
                        logMsg.Format("VERBOSE: Checking child order ID %d of ParentID %d. Status: %d, Type: %d",
                            childOrderDetails.InternalOrderID, parentID, childOrderDetails.OrderStatusCode, childOrderDetails.OrderTypeAsInt);
                        LogSCSMessage(sc, LOG_LEVEL_VERBOSE, logMsg);
                    }

                    if (childOrderDetails.OrderStatusCode == SCT_OSC_FILLED) // Stop or Target Hit
                    {
                        exitDetected = true;
                        logMsg.Format("Exit detected: Attached Order (ID: %d, ParentID: %d, Type: %s) FILLED. Qty: %.0f, AvgFillPrice: %.5f",
                            childOrderDetails.InternalOrderID,
                            parentID,
                            (childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP || childOrderDetails.OrderTypeAsInt == SCT_ORDERTYPE_STOP_LIMIT) ? "STOP" : "TARGET",
                            childOrderDetails.FilledQuantity,
                            childOrderDetails.AvgFillPrice);
                        LogSCSMessage(sc, LOG_LEVEL_INFO, logMsg, true);
                        break; // Exit loop once an exit fill is found
                    }
                }
            }
            if (exitDetected) break; // Exit outer loop too
        }

        if (exitDetected)
        {
            // Reset all state variables to prepare for a new cycle
            ParentBuyLimitOrderID = 0;
            ParentSellLimitOrderID = 0;
            CurrentTradeSide = 0;
            IsBracketArmed = 0; // Should already be 0
            LogSCSMessage(sc, LOG_LEVEL_INFO, "Trade exited. All states reset. Ready for new OCO bracket.");
        } else if (LogLevelInput.GetInt() >= LOG_LEVEL_VERBOSE) {
             LogSCSMessage(sc, LOG_LEVEL_VERBOSE, "VERBOSE: In trade, no SL/TP fill detected yet.");
        }
        return; // Finished processing for this tick
    }
}

// Helper function for logging with level control and timestamping
void LogSCSMessage(SCStudyInterfaceRef& sc, LoggingLevel level, const SCString& message, bool showInTradeServiceLog) {
    if (sc.Input[8].GetInt() < static_cast<int>(level)) { // Assuming LogLevelInput is Input[8]
        return;
    }

    SCString logLevelStr;
    switch (level) {
        case LOG_LEVEL_INFO:    logLevelStr = "INFO";    break;
        case LOG_LEVEL_DEBUG:   logLevelStr = "DEBUG";   break;
        case LOG_LEVEL_VERBOSE: logLevelStr = "VERBOSE"; break;
        case LOG_LEVEL_NONE:    return; // Should not happen if check above works
        default:                logLevelStr = "LOG";     break;
    }

    SCString finalMessage;
    finalMessage.Format("[%s %s Bar:%d]: %s",
        sc.CurrentSystemDateTimeToString().GetChars(), // System time for high resolution
        logLevelStr.GetChars(),
        sc.CurrentBarIndex, // sc.CurrentBarIndex gives the actual chart bar index
        message.GetChars()
    );
    sc.AddMessageToLog(finalMessage, showInTradeServiceLog ? 1 : 0);
}

// Overload for const char* to easily pass string literals
void LogSCSMessage(SCStudyInterfaceRef& sc, LoggingLevel level, const char* message, bool showInTradeServiceLog) {
    SCString scsMessage(message);
    LogSCSMessage(sc, level, scsMessage, showInTradeServiceLog);
}