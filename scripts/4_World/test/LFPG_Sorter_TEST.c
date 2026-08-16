// =========================================================
// LF_PowerGrid - Sorter TEST entity (Sprint 0, 2026-04-26)
//
// V4 test variant of LFPG_Sorter. Inherits ALL behaviour from V3
// (NetworkManager registration, sort tick, wire ports, persistence).
// The ONLY runtime differences:
//   1. Different config classname (LFPG_Sorter_TEST) → admin can spawn
//      both V3 and V4 sorters side-by-side on the same server.
//   2. Different action (LFPG_ActionOpenSorterPanel_TEST) → opens the
//      LFPG_SorterView_TEST UI singleton instead of the V3 view.
//
// Everything else (ticks, RPC server-side handling, persistence,
// CanConnectTo, electrical graph integration) is inherited from V3.
//
// V3's LFPG_ActionOpenSorterPanel uses an exact GetType() guard so
// it is invisible on V4 entities — no double-action confusion.
//
// Sprint 0 milestone: V4 looks/behaves identically to V3, but uses a
// separate UI singleton (own RPC chain, own view instance). Visual
// rework happens in Sprint 1+.
// =========================================================

class LFPG_Sorter_TEST_Kit : LFPG_KitBase
{
    override string LFPG_GetSpawnClassname()
    {
        return "LFPG_Sorter_TEST";
    }
};

class LFPG_Sorter_TEST : LFPG_Sorter
{
    // SetActions: keep V3 chain (LFPG_ActionSyncSorter etc. still work
    // on V4 via inheritance) but swap the open-panel action for the V4
    // variant. The V3 LFPG_ActionOpenSorterPanel action is technically
    // also added via super.SetActions(), but its ActionCondition uses
    // an exact GetType() check that returns false on V4 instances —
    // so it never appears in the cursor on a V4 sorter.
    override void SetActions()
    {
        super.SetActions();
        AddAction(LFPG_ActionOpenSorterPanel_TEST);
    }
};
