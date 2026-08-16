// =========================================================
// LF_PowerGrid - Generic placement action for ALL LFPG Kits
//
// v4.0 (Fase 3): Replaces 20 individual LFPG_ActionPlaceX files
// with a single generic action. All kits share the same placement
// animation pipeline (m_FullBody + PLACING_* commands).
//
// LogicGate kits inherit from this class and override ActionCondition
// only (vanilla validation fails for small items).
//
// ActionTogglePlaceObject enters placement mode (shows hologram).
// This action confirms the placement (fires animation + callback).
//
// IMPORTANT: Must be registered in ActionConstructor.RegisterActions()
//   via actions.Insert(LFPG_ActionPlaceGeneric).
// =========================================================

class LFPG_ActionPlaceGeneric : ActionPlaceObject
{
    void LFPG_ActionPlaceGeneric()
    {
        m_StanceMask = DayZPlayerConstants.STANCEMASK_ERECT | DayZPlayerConstants.STANCEMASK_CROUCH;
        m_FullBody = true;
    }

    override void SetupAnimation(ItemBase item)
    {
        if (!item)
            return;

        if (item.IsHeavyBehaviour())
        {
            m_CommandUID = DayZPlayerConstants.CMD_ACTIONFB_PLACING_HEAVY;
        }
        else if (item.IsOneHandedBehaviour())
        {
            m_CommandUID = DayZPlayerConstants.CMD_ACTIONFB_PLACING_1HD;
        }
        else if (item.IsTwoHandedBehaviour())
        {
            m_CommandUID = DayZPlayerConstants.CMD_ACTIONFB_PLACING_2HD;
        }
        else
        {
            m_CommandUID = DayZPlayerConstants.CMD_ACTIONFB_PLACING_2HD;
        }
    }

    override protected int GetStanceMask(PlayerBase player)
    {
        return DayZPlayerConstants.STANCEMASK_ERECT | DayZPlayerConstants.STANCEMASK_CROUCH;
    }

    // =========================================================
    // OnEndServer — Frontier_Fortifications compatibility shim.
    //
    // Frontier_Fortifications declares a modded class in the
    // ActionDeployObject/ActionPlaceObject chain whose OnEndServer
    // dereferences a NULL pointer when handling non-fortification
    // kits (Frontier_Fortifications/scripts/4_World/classes/actions/
    // actiondeploybasefortificationkits.c:5, "NULL pointer to
    // instance"). Calling super would route through that broken
    // override on every LFPG kit placement.
    //
    // We bypass super and replicate the vanilla cleanup from
    // ActionDeployObject.OnEndServer + AnimatedActionBase.OnEndServer
    // (P:\scripts\4_world\classes\useractionscomponent\actions\
    // continuous\deployactions\actiondeployobject.c:205-238 and
    // P:\scripts\4_world\classes\useractionscomponent\
    // animatedactionbase.c:497-501).
    //
    // The vanilla IsBasebuildingKit Delete() branch is intentionally
    // skipped: LFPG kits dispose of themselves via LFPG_DeferredDelete
    // scheduled in LFPG_KitBase.OnPlacementComplete.
    //
    // Revisit if Frontier_Fortifications ships a fix; reverting is
    // a single deletion of this override.
    // =========================================================
    override void OnEndServer(ActionData action_data)
    {
        if (action_data.m_Player)
            action_data.m_Player.SetPerformedActionID(-1);

        PlaceObjectActionData poActionData = PlaceObjectActionData.Cast(action_data);
        if (!poActionData || !poActionData.m_MainItem)
            return;

        if (!poActionData.m_AlreadyPlaced)
        {
            poActionData.m_MainItem.SetIsBeingPlaced(false);

            if (g_Game.IsMultiplayer())
            {
                poActionData.m_Player.PlacingCancelServer();
            }
            else
            {
                poActionData.m_Player.PlacingCancelLocal();
                poActionData.m_Player.PlacingCancelServer();
            }
        }

        if (poActionData.m_MainItem.GetLoopDeploySoundset() != string.Empty)
            poActionData.m_MainItem.StopItemSoundServer(SoundConstants.ITEM_DEPLOY_LOOP);
    }
};
