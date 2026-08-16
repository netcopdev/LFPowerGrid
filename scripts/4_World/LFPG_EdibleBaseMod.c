// =========================================================
// LF_PowerGrid - Edible_Base mod: pause decay inside powered, closed fridge
//
// Vanilla DayZ only pauses food decay when GetIsFrozen() is true
// (edible_base.c : CanProcessDecay). Setting the temperature low
// does not stop spoilage. This override checks whether the food's
// hierarchy root is a powered, closed LFPG_Fridge; if so decay is
// paused. The fridge still handles temperature via its cool tick.
//
// Why both CanProcessDecay() AND ProcessDecay() are overridden:
//   Many canned/opened food classes (DogFoodCan_Opened, PorkCan_Opened,
//   Lunchmeat_Opened, CrabCan_Opened, UnknownFoodCan_Opened, Pajka_Opened,
//   Pate_Opened, BrisketSpread_Opened, BakedBeansCan_Opened, TunaCan_Opened,
//   SardinesCan_Opened, SpaghettiCan_Opened, PeachesCan_Opened, etc.)
//   override CanProcessDecay() in vanilla WITHOUT calling super, so the
//   modded Edible_Base.CanProcessDecay() never runs for them. Only
//   Edible_Base overrides ProcessDecay() (no subclass does), so guarding
//   ProcessDecay() captures every descendant in a single point.
//
// Compatibility:
//   - CanProcessDecay: calls super first, so any other mod that returns
//     false (e.g., tightens decay rules) keeps precedence. Acts as a
//     cheap short-circuit for base Edible_Base instances.
//   - ProcessDecay: early-returns without touching m_DecayTimer /
//     m_DecayDelta / m_LastDecayStage, so when the fridge opens or
//     powers down, decay resumes from the exact pre-pause state.
//   - Does not add member fields (Enforce restriction on modded class).
// =========================================================

modded class Edible_Base
{
    bool LFPG_IsInSealedPoweredFridge()
    {
        EntityAI root = GetHierarchyRoot();
        if (!root)
            return false;

        LFPG_Fridge fridge = LFPG_Fridge.Cast(root);
        if (!fridge)
            return false;

        if (!fridge.LFPG_IsPowered())
            return false;

        if (fridge.LFPG_IsOpen())
            return false;

        return true;
    }

    override bool CanProcessDecay()
    {
        if (!super.CanProcessDecay())
            return false;

        if (LFPG_IsInSealedPoweredFridge())
            return false;

        return true;
    }

    override void ProcessDecay(float delta, bool hasRootAsPlayer)
    {
        if (LFPG_IsInSealedPoweredFridge())
            return;

        super.ProcessDecay(delta, hasRootAsPlayer);
    }
};
