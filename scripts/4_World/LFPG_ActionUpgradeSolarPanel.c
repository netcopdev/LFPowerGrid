// =========================================================
// LF_PowerGrid - Upgrade Solar Panel T1 → T2
//
// v0.7.47: ActionContinuousBase with crafting progress bar.
//
// Requirements:
//   - Player holds Hammer (item in hands)
//   - Target: LFPG_SolarPanel (T1, NOT T2)
//   - T1 has MetalPlate in slot LFPG_SolarPlate with qty >= 5
//   - T1 has Nail in slot LFPG_SolarNails with qty >= 20
//
// On completion:
//   1. Capture transform
//   2. Create and validate T2 at same position/orientation
//   3. Consume materials (excess returned to ground)
//   4. DeviceLifecycle.OnDeviceKilled (cuts wires, cleans graph)
//   5. Delete T1
//
// ENFORCE SCRIPT NOTES:
//   - No ternary operators
//   - No ++ / -- operators
//   - Explicit typing on all variables
//   - No foreach loops
// =========================================================

// ---- Callback: controls progress bar duration ----
class LFPG_ActionUpgradeSolarCB : ActionContinuousBaseCB
{
    override void CreateActionComponent()
    {
        m_ActionData.m_ActionComponent = new CAContinuousTime(8.0);
    }
};

// ---- Main action ----
class LFPG_ActionUpgradeSolarPanel : ActionContinuousBase
{
    // Material requirements
    static const int LFPG_SOLAR_PLATE_REQUIRED = 5;
    static const int LFPG_SOLAR_NAILS_REQUIRED = 20;

    void LFPG_ActionUpgradeSolarPanel()
    {
        m_CallbackClass = LFPG_ActionUpgradeSolarCB;
        m_CommandUID = DayZPlayerConstants.CMD_ACTIONFB_CRAFTING;
        m_FullBody = true;
        m_StanceMask = DayZPlayerConstants.STANCEMASK_ERECT | DayZPlayerConstants.STANCEMASK_CROUCH;
        m_Text = "#STR_LFPG_ACTION_UPGRADE_SOLAR";
    }

    override void CreateConditionComponents()
    {
        m_ConditionItem = new CCINonRuined;
        m_ConditionTarget = new CCTCursor(LFPG_INTERACT_DIST_M);
    }

    override bool ActionCondition(PlayerBase player, ActionTarget target, ItemBase item)
    {
        if (!player || !target || !item)
            return false;

        // Item in hands must be a Hammer
        if (!item.IsKindOf("Hammer"))
            return false;

        Object targetObj = target.GetObject();
        if (!targetObj)
            return false;

        // Target must be T1 solar panel
        LFPG_SolarPanel panel = LFPG_SolarPanel.Cast(targetObj);
        if (!panel)
            return false;

        // Must NOT be T2 already
        if (panel.IsKindOf("LFPG_SolarPanel_T2"))
            return false;

        // Check materials in attachment slots
        EntityAI plate = panel.FindAttachmentBySlotName("LFPG_SolarPlate");
        if (!plate)
            return false;

        int plateQty = plate.GetQuantity();
        if (plateQty < LFPG_SOLAR_PLATE_REQUIRED)
            return false;

        EntityAI nails = panel.FindAttachmentBySlotName("LFPG_SolarNails");
        if (!nails)
            return false;

        int nailsQty = nails.GetQuantity();
        if (nailsQty < LFPG_SOLAR_NAILS_REQUIRED)
            return false;

        return true;
    }

    override void OnFinishProgressServer(ActionData action_data)
    {
        super.OnFinishProgressServer(action_data);
    
        if (!action_data || !action_data.m_Target)
            return;
    
        Object targetObj = action_data.m_Target.GetObject();
        if (!targetObj)
            return;
    
        LFPG_SolarPanel panel = LFPG_SolarPanel.Cast(targetObj);
        if (!panel)
            return;
    
        // Revalidate the target and material references before claiming the operation.
        if (panel.IsKindOf("LFPG_SolarPanel_T2"))
        {
            LFPG_Util.Warn("[UpgradeSolar] Target is already T2, aborting.");
            return;
        }
    
        EntityAI plate = panel.FindAttachmentBySlotName("LFPG_SolarPlate");
        if (!plate)
        {
            LFPG_Util.Warn("[UpgradeSolar] No MetalPlate found, aborting.");
            return;
        }
    
        int plateQty = plate.GetQuantity();
        if (plateQty < LFPG_SOLAR_PLATE_REQUIRED)
        {
            LFPG_Util.Warn("[UpgradeSolar] Insufficient MetalPlate qty=" + plateQty.ToString());
            return;
        }
    
        EntityAI nails = panel.FindAttachmentBySlotName("LFPG_SolarNails");
        if (!nails)
        {
            LFPG_Util.Warn("[UpgradeSolar] No Nails found, aborting.");
            return;
        }
    
        int nailsQty = nails.GetQuantity();
        if (nailsQty < LFPG_SOLAR_NAILS_REQUIRED)
        {
            LFPG_Util.Warn("[UpgradeSolar] Insufficient Nails qty=" + nailsQty.ToString());
            return;
        }
    
        if (!panel.LFPG_TryBeginExclusiveOp())
        {
            LFPG_Util.Warn("[UpgradeSolar] Another destructive operation already owns the target.");
            return;
        }
    
        vector pos = panel.GetPosition();
        vector ori = panel.GetOrientation();
        string deviceId = panel.LFPG_GetDeviceId();
    
        // The T2 model is rotated 180 degrees relative to T1.
        float correctedYaw = ori[0] + 180.0;
        if (correctedYaw >= 360.0)
            correctedYaw = correctedYaw - 360.0;
        ori = Vector(correctedYaw, ori[1], ori[2]);
    
        EntityAI t2 = EntityAI.Cast(g_Game.CreateObjectEx("LFPG_SolarPanel_T2", pos, ECE_CREATEPHYSICS));
        if (!t2)
        {
            panel.LFPG_EndExclusiveOp();
            LFPG_Util.Error("[UpgradeSolar] Failed to create LFPG_SolarPanel_T2 - aborting upgrade, T1 + materials preserved");
            return;
        }
    
        t2.SetPosition(pos);
        t2.SetOrientation(ori);
        t2.Update();
    
        array<EntityAI> stagedOutputs = new array<EntityAI>();
        stagedOutputs.Insert(t2);
        if (!StageMaterialSurplus(plate, plateQty, LFPG_SOLAR_PLATE_REQUIRED, pos, stagedOutputs))
        {
            AbortStagedUpgradeOutputs(stagedOutputs);
            panel.LFPG_EndExclusiveOp();
            LFPG_Util.Error("[UpgradeSolar] Failed to stage MetalPlate surplus - T1 + materials preserved");
            return;
        }
        if (!StageMaterialSurplus(nails, nailsQty, LFPG_SOLAR_NAILS_REQUIRED, pos, stagedOutputs))
        {
            AbortStagedUpgradeOutputs(stagedOutputs);
            panel.LFPG_EndExclusiveOp();
            LFPG_Util.Error("[UpgradeSolar] Failed to stage Nail surplus - T1 + materials preserved");
            return;
        }
    
        // Commit sources only after T2 and every surplus output exist.
        g_Game.ObjectDelete(plate);
        g_Game.ObjectDelete(nails);
        LFPG_DeviceLifecycle.OnDeviceKilled(panel, deviceId);
        g_Game.ObjectDelete(panel);
    
        // The exclusive flag intentionally remains set until EEDelete completes.
        LFPG_Util.Info("[UpgradeSolar] T2 created at " + pos.ToString() + " ori=" + ori.ToString());
    }

    // ---- Helpers: stage excess without consuming sources; abort staged outputs ----
    protected bool StageMaterialSurplus(EntityAI item, int currentQty, int required, vector dropPos, array<EntityAI> stagedOutputs)
    {
        if (!item || !stagedOutputs)
            return false;
        if (currentQty <= required)
            return true;
    
        int excessQty = currentQty - required;
        string itemType = item.GetType();
        vector spawnPos = dropPos;
        spawnPos[1] = spawnPos[1] + 0.1;
    
        EntityAI excess = EntityAI.Cast(g_Game.CreateObject(itemType, spawnPos, false));
        if (!excess)
            return false;
    
        stagedOutputs.Insert(excess);
        ItemBase excessItem = ItemBase.Cast(excess);
        if (!excessItem)
            return false;
    
        excessItem.SetQuantity(excessQty);
        return true;
    }
    
    protected void AbortStagedUpgradeOutputs(array<EntityAI> stagedOutputs)
    {
        if (!stagedOutputs)
            return;
    
        int i = 0;
        for (i = 0; i < stagedOutputs.Count(); i = i + 1)
        {
            EntityAI output = stagedOutputs[i];
            if (output)
                g_Game.ObjectDelete(output);
        }
        stagedOutputs.Clear();
    }
};
