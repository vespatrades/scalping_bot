#include "sierrachart.h"

SCDLLName("Scalping Bot")

SCSFExport scsf_Scalping_Bot(SCStudyInterfaceRef sc)
{
    //── Inputs ────────────────────────────────────────────────────────────
    SCInputRef NumContracts = sc.Input[0];
    SCInputRef VolSubgraph = sc.Input[1];
    SCInputRef BracketFrac = sc.Input[2];
    SCInputRef StopFrac = sc.Input[3];
    SCInputRef TPFrac = sc.Input[4];
    SCInputRef StartTimeInput = sc.Input[5];
    SCInputRef StopTimeInput = sc.Input[6];
    SCInputRef EnableInput = sc.Input[7];

    //── Persistent State ─────────────────────────────────────────────────
    int& EntryOrderID1 = sc.GetPersistentInt(1);  // first limit parent
    int& EntryOrderID2 = sc.GetPersistentInt(2);  // second limit parent
    int& InTrade = sc.GetPersistentInt(3);  // 0=flat,1=long,2=short
    int& BracketArmed = sc.GetPersistentInt(4);  // 0=none,1=armed

    if (sc.SetDefaults)
    {
        sc.GraphName = "Scalping Bot";
        sc.AutoLoop = 1;
        sc.UpdateAlways = 1;
        sc.MaintainTradeStatisticsAndTradesData = true;

        NumContracts.Name = "Number of Contracts";           NumContracts.SetInt(1);
        VolSubgraph.Name = "Volatility Subgraph (Range R)"; VolSubgraph.SetStudySubgraphValues(0, 0);
        BracketFrac.Name = "Half Bracket Width Fraction of R";   BracketFrac.SetFloat(0.5f);
        StopFrac.Name = "Stop Loss Fraction of R";       StopFrac.SetFloat(0.5f);
        TPFrac.Name = "Take Profit Fraction of R";     TPFrac.SetFloat(1.0f);

        StartTimeInput.Name = "Start Time";   StartTimeInput.SetTime(HMS_TIME(8, 30, 0));
        StopTimeInput.Name = "Stop Time";    StopTimeInput.SetTime(HMS_TIME(15, 0, 0));
        EnableInput.Name = "Enable Trading"; EnableInput.SetYesNo(false);

        // Unmanaged auto-trading settings
        sc.MaximumPositionAllowed = 100000;
        sc.AllowMultipleEntriesInSameDirection = 1;
        sc.AllowOppositeEntryWithOpposingPositionOrOrders = 1;
        sc.CancelAllOrdersOnEntriesAndReversals = 0;
        sc.CancelAllOrdersOnReversals = 0;
        sc.CancelAllOrdersOnEntries = 0;
        sc.AllowEntryWithWorkingOrders = 1;
        sc.CancelAllWorkingOrdersOnExit = 0;
        sc.SupportAttachedOrdersForTrading = 1;
        //uncomment for live trading
        //sc.SendOrdersToTradeService = 1;
        return;
    }

    //── 1) BOOTSTRAP on full recalculation at bar 0 ──────────────────────
    if (sc.IsFullRecalculation && sc.Index == 0)
    {
        // Infer current position
        {
            s_SCPositionData pos;
            sc.GetTradePosition(pos);
            InTrade = (pos.PositionQuantity > 0 ? 1
                : pos.PositionQuantity < 0 ? 2 : 0);
        }

        // Find two working LIMIT parents each with 2 attached children
        EntryOrderID1 = EntryOrderID2 = 0;
        BracketArmed = 0;

        int idx = 0;
        s_SCTradeOrder od;
        std::vector<int> validParents;
        while (sc.GetOrderByIndex(idx++, od) != SCTRADING_ORDER_ERROR)
        {
            if (od.OrderStatusCode != SCT_OSC_OPEN)              continue;
            if (od.ParentInternalOrderID != 0)                      continue;
            if (od.OrderTypeAsInt != SCT_ORDERTYPE_LIMIT)        continue;

            int cidx = 0, childCount = 0;
            s_SCTradeOrder child;
            while (sc.GetOrderByIndex(cidx++, child) != SCTRADING_ORDER_ERROR)
                if (child.ParentInternalOrderID == od.InternalOrderID)
                    childCount++;

            if (childCount == 2)
                validParents.push_back(od.InternalOrderID);
        }

        if (validParents.size() == 2)
        {
            EntryOrderID1 = validParents[0];
            EntryOrderID2 = validParents[1];
            BracketArmed = 1;
        }
    }

    //── 2) Only run trading + logging on last bar ─────────────────────────
    if (sc.Index != sc.ArraySize - 1)
        return;

    SCString msg;

    // Time gate
    if (!EnableInput.GetYesNo())
    {
        sc.AddMessageToLog("Trading disabled", 0);
        return;
    }
    int now = sc.BaseDateTimeIn[sc.Index].GetTime(),
        start = StartTimeInput.GetTime(),
        stop = StopTimeInput.GetTime();
    if (now < start)
    {
        msg.Format("Before trading window (now=%04d)", now);
        sc.AddMessageToLog(msg, 0);
        return;
    }
    if (now >= stop)
    {
        msg.Format("After trading window (now=%04d); cancelling parents", now);
        sc.AddMessageToLog(msg, 0);
        if (EntryOrderID1) sc.CancelOrder(EntryOrderID1);
        if (EntryOrderID2) sc.CancelOrder(EntryOrderID2);
        EntryOrderID1 = EntryOrderID2 = InTrade = BracketArmed = 0;
        return;
    }

    // Compute offsets
    SCFloatArray volArray;
    sc.GetStudyArrayUsingID(
        VolSubgraph.GetStudyID(),
        VolSubgraph.GetSubgraphIndex(),
        volArray);
    float R = volArray[sc.Index];
    if (R <= 0.0f)
    {
        sc.AddMessageToLog("No volatility data", 0);
        return;
    }
    float entryOff = R * BracketFrac.GetFloat();
    float stopOff = R * StopFrac.GetFloat();
    float tpOff = R * TPFrac.GetFloat();

    // 3) Place bracket if flat & not armed
    if (InTrade == 0 && !BracketArmed)
    {
        msg.Format("Placing bracket: entryOff=%.5f stopOff=%.5f tpOff=%.5f",
            entryOff, stopOff, tpOff);
        sc.AddMessageToLog(msg, 0);

        s_SCNewOrder o;
        o.Reset();
        o.OrderQuantity = NumContracts.GetInt();
        o.OrderType = SCT_ORDERTYPE_OCO_BUY_LIMIT_SELL_LIMIT;
        o.Price1 = sc.Close[sc.Index] - entryOff;
        o.Price2 = sc.Close[sc.Index] + entryOff;
        o.Stop1Offset = stopOff;
        o.Target1Offset = tpOff;
        o.Stop1Offset_2 = stopOff;
        o.Target1Offset_2 = tpOff;

        int result = sc.SubmitOCOOrder(o);
        if (result > 0)
        {
            EntryOrderID1 = o.InternalOrderID;
            EntryOrderID2 = o.InternalOrderID2;
            BracketArmed = 1;
            msg.Format("Bracket submitted: p1=%d p2=%d",
                EntryOrderID1, EntryOrderID2);
        }
        else
        {
            msg.Format("SubmitOCOOrder failed: %d", result);
        }
        sc.AddMessageToLog(msg, result > 0 ? 0 : 1);
        return;
    }

    // 4) Poll for entry fill (DO NOT clear parents here!)
    if (BracketArmed && InTrade == 0)
    {
        s_SCTradeOrder ord;
        if (EntryOrderID1 &&
            sc.GetOrderByOrderID(EntryOrderID1, ord) &&
            ord.OrderStatusCode == SCT_OSC_FILLED)
        {
            InTrade = (ord.BuySell == BSE_BUY ? 1 : 2);
            msg.Format("Entry filled on parent1=%d side=%s",
                EntryOrderID1,
                InTrade == 1 ? "BUY" : "SELL");
            sc.AddMessageToLog(msg, 0);
            BracketArmed = 0;  // now in-trade
            return;
        }
        if (EntryOrderID2 &&
            sc.GetOrderByOrderID(EntryOrderID2, ord) &&
            ord.OrderStatusCode == SCT_OSC_FILLED)
        {
            InTrade = (ord.BuySell == BSE_BUY ? 1 : 2);
            msg.Format("Entry filled on parent2=%d side=%s",
                EntryOrderID2,
                InTrade == 1 ? "BUY" : "SELL");
            sc.AddMessageToLog(msg, 0);
            BracketArmed = 0;
            return;
        }
    }

    // 5) Exit detection: scan children for filled stop/TP
    if (InTrade != 0)
    {
        bool exitHit = false;
        int parents[2] = { EntryOrderID1, EntryOrderID2 };
        s_SCTradeOrder od;
        for (int i = 0; i < 2 && !exitHit; ++i)
        {
            int pid = parents[i];
            if (pid == 0) continue;
            int idx2 = 0;
            while (!exitHit && sc.GetOrderByIndex(idx2++, od) != SCTRADING_ORDER_ERROR)
            {
                if (od.ParentInternalOrderID == pid &&
                    od.OrderStatusCode == SCT_OSC_FILLED)
                {
                    exitHit = true;
                    msg.Format("Exit detected: child %d of parent %d filled",
                        od.InternalOrderID, pid);
                    sc.AddMessageToLog(msg, 0);
                }
            }
        }
        if (exitHit)
        {
            EntryOrderID1 = EntryOrderID2 = InTrade = BracketArmed = 0;
            sc.AddMessageToLog("Bracket reset after exit", 0);
        }
    }
}
