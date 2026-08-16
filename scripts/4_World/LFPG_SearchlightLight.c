#ifndef SERVER
// Client-only compilation boundary
// =========================================================
// LF_PowerGrid - Searchlight light classes (v1.6.0)
//
// 4 light classes for the LFPG_Searchlight device:
//   LFPG_SearchlightBeamCore  — primary spot, 120m, 24°, managed shadows, brightness 15
//   LFPG_SearchlightBeamSpill — secondary spot, 80m, 48°, shadows OFF, brightness 4
//   LFPG_SearchlightHalo      — lens glow, 5m radius point light
//   LFPG_SearchlightSplash    — ground splash, 10m radius point light
//
// All: client-side only, SetVisibleDuringDaylight(false),
//      SetLifetime(1000000), warm white color.
//
// DancingShadows OFF on BeamCore (Audit H1):
//   ScriptedLightBase does RemoveChild/AddChild/Update per EOnFrame
//   when amplitude > 0. Cost unacceptable with SetOrientation rotating parent.
//
// Flicker order: SetFlickerAmplitude BEFORE SetBrightnessTo
//   (ScriptedLightBase applies flicker as multiplier on brightness).
//
// Enforce Script: no ternaries, no ++/--, no foreach.
// =========================================================

class LFPG_SearchlightLightManager
{
    protected static ref array<LFPG_SearchlightBeamCore> s_Cores;
    protected static LFPG_SearchlightBeamCore s_ShadowCore;
    protected static const float LFPG_SEARCHLIGHT_TIER_HYSTERESIS_M = 3.0;

    protected static void Ensure()
    {
        if (!s_Cores)
        {
            s_Cores = new array<LFPG_SearchlightBeamCore>;
        }
    }

    static void Reset()
    {
        Ensure();
        s_Cores.Clear();
        s_ShadowCore = null;
    }

    static void RegisterCore(LFPG_SearchlightBeamCore core)
    {
        Ensure();
        if (core && s_Cores.Find(core) < 0)
        {
            s_Cores.Insert(core);
        }
    }

    static void UnregisterCore(LFPG_SearchlightBeamCore core)
    {
        Ensure();
        int index = s_Cores.Find(core);
        if (index >= 0)
        {
            s_Cores.Remove(index);
        }
        if (s_ShadowCore == core)
        {
            s_ShadowCore = null;
        }
    }

    static void Tick()
    {
        Ensure();
        PlayerBase player = PlayerBase.Cast(g_Game.GetPlayer());
        if (!player)
            return;

        vector camPos = g_Game.GetCurrentCameraPosition();
        int screenW = 0;
        int screenH = 0;
        GetScreenSize(screenW, screenH);
        float shadowEnterM = LFPG_SEARCHLIGHT_SHADOW_MAX_M - LFPG_SEARCHLIGHT_TIER_HYSTERESIS_M;
        float shadowExitM = LFPG_SEARCHLIGHT_SHADOW_MAX_M + LFPG_SEARCHLIGHT_TIER_HYSTERESIS_M;
        float fullEnterM = LFPG_SEARCHLIGHT_FULL_M - LFPG_SEARCHLIGHT_TIER_HYSTERESIS_M;
        float fullExitM = LFPG_SEARCHLIGHT_FULL_M + LFPG_SEARCHLIGHT_TIER_HYSTERESIS_M;
        float shadowEnterSq = shadowEnterM * shadowEnterM;
        float shadowExitSq = shadowExitM * shadowExitM;
        float fullEnterSq = fullEnterM * fullEnterM;
        float fullExitSq = fullExitM * fullExitM;
        float bestDistSq = shadowEnterSq + 1.0;
        LFPG_SearchlightBeamCore chosen = null;
        LFPG_SearchlightBeamCore stickyChosen = null;
        LFPG_SearchlightBeamCore newChosen = null;
        int eligibleCount = 0;
        int visibleCount = 0;

        int i;
        for (i = 0; i < s_Cores.Count(); i = i + 1)
        {
            LFPG_SearchlightBeamCore candidate = s_Cores[i];
            if (!candidate || !candidate.LFPG_GetOwner())
                continue;

            eligibleCount = eligibleCount + 1;
            vector ownerPos = candidate.LFPG_GetOwner().GetPosition();
            float distSq = vector.DistanceSq(camPos, ownerPos);
            vector screenPos = g_Game.GetScreenPos(ownerPos);
            bool visible = (screenPos[2] > 0.0 && screenW > 0 && screenH > 0 && screenPos[0] >= 0.0 && screenPos[0] <= screenW && screenPos[1] >= 0.0 && screenPos[1] <= screenH);
            if (visible)
            {
                visibleCount = visibleCount + 1;
                if (candidate == s_ShadowCore && distSq <= shadowExitSq)
                {
                    stickyChosen = candidate;
                }
                if (distSq <= shadowEnterSq && distSq < bestDistSq)
                {
                    bestDistSq = distSq;
                    newChosen = candidate;
                }
            }
        }

        if (stickyChosen)
            chosen = stickyChosen;
        else
            chosen = newChosen;
        s_ShadowCore = chosen;

        int activeShadowCount = 0;
        for (i = 0; i < s_Cores.Count(); i = i + 1)
        {
            LFPG_SearchlightBeamCore core = s_Cores[i];
            if (!core || !core.LFPG_GetOwner())
                continue;

            float candidateDistSq = vector.DistanceSq(camPos, core.LFPG_GetOwner().GetPosition());
            bool shadowActive = (core == chosen);
            bool fullRigActive = core.LFPG_IsFullRigActive();
            if (fullRigActive)
                fullRigActive = (candidateDistSq <= fullExitSq);
            else
                fullRigActive = (candidateDistSq <= fullEnterSq);
            core.LFPG_ApplyTier(shadowActive, fullRigActive);
            if (shadowActive)
            {
                activeShadowCount = activeShadowCount + 1;
            }

            if (LFPG_PERFDIAG_ENABLED)
            {
                string candidateLog = "LFPG_PERFDIAG searchlight_candidate timestamp=" + g_Game.GetTime().ToString();
                candidateLog = candidateLog + " deviceId=" + core.LFPG_GetOwner().LFPG_GetDeviceId();
                candidateLog = candidateLog + " distance=" + Math.Sqrt(candidateDistSq).ToString();
                candidateLog = candidateLog + " shadow=" + shadowActive.ToString();
                Print(candidateLog);
            }
        }

        if (LFPG_PERFDIAG_ENABLED)
        {
            string chosenId = "";
            if (chosen && chosen.LFPG_GetOwner())
            {
                chosenId = chosen.LFPG_GetOwner().LFPG_GetDeviceId();
            }
            string summary = "LFPG_PERFDIAG searchlight_frame timestamp=" + g_Game.GetTime().ToString();
            summary = summary + " eligible=" + eligibleCount.ToString();
            summary = summary + " visible=" + visibleCount.ToString();
            summary = summary + " chosenDeviceId=" + chosenId;
            summary = summary + " activeShadowCores=" + activeShadowCount.ToString();
            Print(summary);
        }
    }
};

// ---------------------------------------------------------
// BeamCore: primary directional spot
// ---------------------------------------------------------
class LFPG_SearchlightBeamCore : SpotLightBase
{
    protected LFPG_Searchlight m_Owner;
    protected ScriptedLightBase m_Spill;
    protected ScriptedLightBase m_Halo;
    protected bool m_ShadowActive;
    protected bool m_FullRigActive = true;

    void LFPG_SearchlightBeamCore()
    {
        SetVisibleDuringDaylight(false);
        SetRadiusTo(120.0);
        SetSpotLightAngle(24.0);
        SetCastShadow(false);

        // DancingShadows OFF (Audit H1)
        SetDancingShadowsMovementSpeed(0.0);
        SetDancingShadowsAmplitude(0.0);

        // Flicker: subtle realism. Amplitude BEFORE Brightness.
        SetFlickerAmplitude(0.03);
        SetFlickerSpeed(0.4);
        SetBrightnessTo(15.0);

        SetFadeOutTime(0.3);
        SetLifetime(1000000.0);

        // Warm white
        SetDiffuseColor(1.0, 0.97, 0.90);
        SetAmbientColor(1.0, 0.97, 0.90);
    }

    void LFPG_SetRig(LFPG_Searchlight owner, ScriptedLightBase spill, ScriptedLightBase halo)
    {
        m_Owner = owner;
        m_Spill = spill;
        m_Halo = halo;
        LFPG_SearchlightLightManager.RegisterCore(this);
    }

    void LFPG_ClearRig()
    {
        LFPG_SearchlightLightManager.UnregisterCore(this);
        m_Owner = null;
        m_Spill = null;
        m_Halo = null;
    }

    LFPG_Searchlight LFPG_GetOwner()
    {
        return m_Owner;
    }

    bool LFPG_IsFullRigActive()
    {
        return m_FullRigActive;
    }

    void LFPG_ApplyTier(bool shadowActive, bool fullRigActive)
    {
        if (shadowActive != m_ShadowActive)
        {
            m_ShadowActive = shadowActive;
            SetCastShadow(shadowActive);
        }
        if (fullRigActive == m_FullRigActive)
            return;

        m_FullRigActive = fullRigActive;
        if (m_Spill)
        {
            if (fullRigActive)
            {
                m_Spill.SetBrightnessTo(4.0);
            }
            else
            {
                m_Spill.SetBrightnessTo(0.0);
            }
        }
        if (m_Halo)
        {
            if (fullRigActive)
            {
                m_Halo.SetBrightnessTo(12.0);
            }
            else
            {
                m_Halo.SetBrightnessTo(0.0);
            }
        }
    }
};

// ---------------------------------------------------------
// BeamSpill: secondary wide cone (volumetric fill)
// ---------------------------------------------------------
class LFPG_SearchlightBeamSpill : SpotLightBase
{
    void LFPG_SearchlightBeamSpill()
    {
        SetVisibleDuringDaylight(false);
        SetRadiusTo(80.0);
        SetSpotLightAngle(48.0);
        SetCastShadow(false);

        // Minimal flicker
        SetFlickerAmplitude(0.001);
        SetFlickerSpeed(0.2);
        SetBrightnessTo(4.0);

        SetFadeOutTime(0.3);
        SetLifetime(1000000.0);

        SetDiffuseColor(1.0, 0.97, 0.90);
        SetAmbientColor(1.0, 0.97, 0.90);
    }
};

// ---------------------------------------------------------
// Halo: bloom glow around the lens
// ---------------------------------------------------------
class LFPG_SearchlightHalo : PointLightBase
{
    void LFPG_SearchlightHalo()
    {
        SetVisibleDuringDaylight(false);
        SetRadiusTo(5.0);
        SetCastShadow(false);
        SetBrightnessTo(12.0);
        SetFadeOutTime(0.2);
        SetLifetime(1000000.0);

        SetDiffuseColor(1.0, 0.98, 0.92);
        SetAmbientColor(1.0, 0.98, 0.92);
    }
};

// ---------------------------------------------------------
// Splash: ground impact light (repositioned via SyncVars)
// ---------------------------------------------------------
class LFPG_SearchlightSplash : PointLightBase
{
    void LFPG_SearchlightSplash()
    {
        SetVisibleDuringDaylight(false);
        SetRadiusTo(10.0);
        SetCastShadow(false);
        SetBrightnessTo(10.0);
        SetFadeOutTime(0.2);
        SetLifetime(1000000.0);

        SetDiffuseColor(1.0, 0.97, 0.90);
        SetAmbientColor(1.0, 0.97, 0.90);
    }
};
#endif
