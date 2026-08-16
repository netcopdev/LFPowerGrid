# 29 — Mission UI lifecycle residual

**Scope:** `scripts/5_Mission/*`, `scripts/3_Game/LFPG_UIScaler.c`, MissionInit-only input paths for sorter/BTC/CCTV; interactions with CableHUD/TankHUD/InventoryMenuMod.  
**No source edits.** Findings: `F-MISS-NNN` with `path:line`.  
**Verified how:** direct Read of listed files + grep of Open/Close/IsOpen/IsAlive/HandleEsc/UIScaler/CreateElecGraph.  
**Not verified:** runtime SP/dedicated repro, heap after reconnect, in-game death while each UI open.

---

## Executive summary

1. **ElecGraph factory only on `MissionServer`** → offline/SP (`MissionGameplay`) falls back to stub base graph (power sim degraded).
2. **Death force-close is incomplete:** only production Sorter + BTC; **TEST sorter / CCTV / Searchlight** lack MissionInit death guards (and CCTV/Searchlight lack any `IsAlive` check).
3. **`LFPG_SorterView_TEST` is orphaned from MissionInit input** (no OnKeyPress/OnKeyRelease/death) while still using `SetDisabled(true)` → stuck-input class of bug.
4. **Shared `LFPG_UIScaler` static capture** is overwritten by V3 vs V4 TEST panels; Apply can scale the wrong tree for the rest of the session.
5. **Cleanup asymmetry** on reconnect/finish: several singletons Init without prior Reset; InventoryMenu cargo signal can outlive the menu.

---

## Init order & ElecGraph factory

### Sequence (client, `MissionGameplay.OnInit`)

| Step | Call | File |
|------|------|------|
| 1 | `super.OnInit()` | `LFPG_MissionInit.c:63` |
| 2 | `Widget.SetLV/SetTextLV(0)` | `LFPG_MissionInit.c:67-68` |
| 3 | Reset CableHUD / CableRenderer / WiringClient | `LFPG_MissionInit.c:73-75` |
| 4 | `DeviceInspector.Init()` | `LFPG_MissionInit.c:76` |
| 5 | Reset CameraViewport / Searchlight* | `LFPG_MissionInit.c:78-80` |
| 6 | `SorterView.Init()` (eager) + comment TEST lazy | `LFPG_MissionInit.c:81-82` |
| 7 | `BTCAtmView.Init()` | `LFPG_MissionInit.c:83` |
| 8 | LaserBeam Reset, ActionRaycast proximity Init, TankHUD.Init, BTC client data Reset | `LFPG_MissionInit.c:84-87` |

### Sequence (server, `MissionServer.OnInit`)

- `NetworkManager.Get()` → ctor builds graph via mission factory → `StartServerScheduler()` also from ctor and again from OnInit (`LFPG_MissionInit.c:17-18`, `LFPG_NetworkManager.c:518-524`, `597`).

### Findings — factory / offline

**F-MISS-001** `scripts/5_Mission/LFPG_MissionInit.c:36`  
`LFPG_CreateElecGraph()` is overridden **only** on `MissionServer`. Base on `MissionBaseWorld` returns `null` (`LFPG_PlayerRPC.c:27`).  
`NetworkManager` ctor (`LFPG_NetworkManager.c:518-523`) does:

```c
if (mw) m_Graph = mw.LFPG_CreateElecGraph();
if (!m_Graph) {
  // fallback base graph (sim degraded)
  m_Graph = new LFPG_ElecGraph();
}
```

Offline/SP and any host whose mission type is `MissionGameplay` never installs `LFPG_ElecGraphImpl`. Base methods only log + no-op (`LFPG_ElecGraph.c:47-98`). **Offline power propagation is structurally dead.**  
**Fix direction:** same override on `MissionGameplay`, or default factory on a 5_Mission mission base used by both.

**F-MISS-002** `scripts/4_World/LFPG_NetworkManager.c:518-524` + first `Get()` race  
Graph is fixed at **first** `NetworkManager` construction. Any early `Get()` while `GetMission()` is null → permanent stub graph for the process lifetime (no re-factory). MissionServer.OnInit is the intended first call, but entity EEInit paths also call `Get()` (`LFPG_Battery.c:221`, actions, etc.).  
**Severity:** medium if mission always exists before entities; high if load order can invert.

**F-MISS-003** `scripts/5_Mission/LFPG_MissionInit.c:17-18` vs ctor `StartServerScheduler`  
Scheduler start is duplicated (ctor + OnInit). Guarded by `if (m_ServerScheduler) return` (`LFPG_NetworkManager.c:618-619`) so not a double-timer, but OnInit is not the sole entry — offline never hits MissionServer.OnInit and relies on ctor alone.

---

## Cleanup / leaks

### OnMissionFinish (client)

`LFPG_MissionInit.c:351-369`: WiringClient, CableRenderer, CableHUD, LaserBeam, DeviceInspector, CameraViewport, Searchlight*, SorterView, SorterView_TEST, BTCAtmView, TankHUD.  
**Not called on finish (but reset on init):** `LFPG_BTCAtmClientData.Reset` (`OnInit:87` only).  
**Never cleared:** `LFPG_CargoRefreshSignal` invoker (`LFPG_CargoRefreshSignal.c:16-24`), `LFPG_ActionRaycast` static proximity buffers.

**F-MISS-004** `scripts/5_Mission/LFPG_MissionInit.c:351-369`  
`LFPG_BTCAtmClientData.Reset()` missing on finish. Stale netIds/price/balance survive until next OnInit if finish order is partial or UI reopens mid-transition.

**F-MISS-005** `scripts/5_Mission/LFPG_InventoryMenuMod.c:19-34` + `LFPG_CargoRefreshSignal.c:27-32`  
Subscribe on `InventoryMenu.OnShow`, unsubscribe on `OnHide`. If mission ends while inventory is open (or OnHide skipped), invoker still holds `LFPG_OnCargoRefresh` → Invoke after menu/widget death. No MissionFinish force-clear of invoker.

**F-MISS-006** `scripts/5_Mission/LFPG_MissionInit.c:61-87` reconnect Init vs finish  
OnInit **Resets** CableHUD/Renderer/Wiring/CCTV/Searchlight but only **Init** (no prior Cleanup) for:

| Singleton | OnInit | Early-return if alive? |
|-----------|--------|------------------------|
| DeviceInspector | Init only | Get()+CreateWidgets if root exists may keep stale root |
| SorterView | Init | **yes** `if (s_Instance) return` (`LFPG_SorterView.c:1231-1232`) |
| BTCAtmView | Init | **yes** (`LFPG_BTCAtmView.c:1114-1115`) |
| TankHUD | Init | instance may survive if finish skipped; CreateWidgets bails if `m_Root` (`LFPG_TankHUD.c:67-68`) |

If `OnMissionFinish` is skipped (hard disconnect), next `OnInit` **does not** reconstruct sorter/BTC/tank/inspector → stale widgets / open locks possible.

**F-MISS-007** `scripts/4_World/LFPG_SorterView.c:1328-1341` / `LFPG_BTCAtmView.c:1194-1202`  
Cleanup nulls `s_Instance` without calling `DoClose()` first. Relies on destructor for HIC/`ChangeGameFocus`/`PlayerControlEnable`. Destructor has `g_Game` null guards (`LFPG_BTCAtmView.c:172-197`, sorter ~300+). If GC is delayed while mission tears down, focus lock can linger one frame or longer. MissionFinish path should prefer explicit `Close()` then null.

**F-MISS-008** `scripts/3_Game/LFPG_UIScaler.c:63-99, 295-325` + dual Capture  
Single static capture shared by production sorter and TEST:

- Production: Capture in `SorterView.Init` (`LFPG_SorterView.c:1246-1248`), Reset in Cleanup (`:1331`).
- TEST: Capture in lazy Init (`LFPG_SorterView_TEST.c:1514-1516`), Reset in Cleanup (`:1624`).

Whichever Init runs **last** owns `s_Widgets`. `Open` only `Apply`s (`LFPG_SorterView.c:1406-1407`) — never re-Captures. Opening TEST after V3 (or vice versa in one session) makes the other panel Apply against the wrong widget pointers / design sizes.  
Mission finish order Cleanup V3 then TEST both Reset — OK at quit; **broken mid-session dual use**.

**F-MISS-009** `scripts/5_Mission/LFPG_MissionInit.c:230-244` widgets-created flag  
`m_LFPG_WidgetsCreated` is never cleared in OnMissionFinish (instance fields die with MissionGameplay). OK if mission object is always new. If engine reuses MissionGameplay (unusual), CCTV `InitWidgets` would never re-run after viewport Reset destroyed overlay (`CameraViewport.ForceCleanup` → `DestroyWidgets` at `LFPG_CameraViewport.c:736`).

**F-MISS-010** CableHUD frame pairing — OK path  
`OnUpdate` (`LFPG_MissionInit.c:289-307`): `BeginFrame(canvasProducerActive)`; `EndFrame` only if producers active. Inactive path clears canvas once via `m_HadActiveProducers` (`LFPG_CableHUD.c:186-196`). No EndFrame leak. CCTV `ShouldSkipInspector` forces `canvasProducerActive=false` (`:277-287`) — cables/lasers suppressed during CCTV. Intentional.

**F-MISS-011** TankHUD still ticks under CCTV  
TankHUD is **not** gated by `skipCameraOps` (`LFPG_MissionInit.c:317-319` after CCTV overlay). DeviceInspector is. During spectator, cursor ray may be meaningless; risk of stray tank HUD over CCTV is cosmetic/low.

---

## Input paths (MissionInit only)

### OnKeyPress (`LFPG_MissionInit.c:96-128`)

Priority: **CCTV active → swallow all (no super)** → **Sorter.IsOpen → ESC HandleEsc / swallow all** → **BTC.IsOpen → same** → super.

### OnKeyRelease (`LFPG_MissionInit.c:134-157`)

CCTV: swallow all. ESC-only special case for BTC/Sorter open **or** EscCooldown; otherwise super.

### Findings — swallow / ESC

**F-MISS-012** `scripts/5_Mission/LFPG_MissionInit.c:105-126`  
Production sorter/BTC intentionally swallow **all** keys while open (Bug A+D). Chat, hotbar, map keys cannot fire — by design. Document as UX contract, not bug, unless product wants partial passthrough.

**F-MISS-013** `scripts/5_Mission/LFPG_MissionInit.c:143-155` vs `:105-116`  
Asymmetry: OnKeyPress swallows every key; OnKeyRelease only gates **ESC**. Non-ESC key-up still hits `super` while UI open. Usually harmless with HIC disabled / PlayerControlDisable; residual risk with mods that key-up on release.

**F-MISS-014** `scripts/5_Mission/LFPG_MissionInit.c:96-128` — **SorterView_TEST missing**  
Comments on TEST claim ESC via MissionGameplay (`LFPG_SorterView_TEST.c:54, 1575-1578`) but MissionInit never calls `LFPG_SorterView_TEST.IsOpen/HandleEscKey/IsEscCooldown`. While TEST is open:

- ESC goes to `super` → pause menu / engine UI.
- `SetDisabled(true)` still applied on open (`LFPG_SorterView_TEST.c:1748` area) → **movement/actions stuck** if player cannot close via X and pause doesn't restore HIC.
- Death force-close (below) also missing for TEST.

**Severity: high** for any build that ships V4 TEST entities.

**F-MISS-015** EscCooldown 200ms (`LFPG_SorterView.c:1311-1322`, BTC `:1181-1191`)  
Only applied on release path in MissionInit. Good for double-ESC pause suppression. TEST has identical API (`LFPG_SorterView_TEST.c:1605-1616`) **unwired**.

**F-MISS-016** CCTV vs sorter mutual exclusion not enforced in MissionInit  
If both were active, CCTV branch wins and sorter never receives ESC. Actions generally block concurrent open; no MissionInit assert. Low if actions hold.

**F-MISS-017** BTC open action has **no** `IsOpen` guard (`LFPG_ActionOpenBTCAtm.c:41-66`)  
Sorter action does (`LFPG_ActionOpenSorterPanel.c:72-74`). Duplicate BTC open RPC possible; DoOpen early-returns if already open (`LFPG_BTCAtmView.c:1236-1237`). Low (wasted RPC), not MissionInit, noted for lifecycle context.

---

## Death / unconscious force-close matrix

MissionInit guards (`LFPG_MissionInit.c:167-213`): only **SorterView** and **BTCAtmView**. Pattern: null player OR `!IsAlive()` OR `IsUnconscious()` → `Close()`.

| UI | MissionInit death close | Self-tick death/alive | Input lock on open | Stuck risk if die while open |
|----|-------------------------|----------------------|--------------------|------------------------------|
| **Sorter (V3)** | **YES** `:170-189` | Close releases HIC (`DoClose` `:1491-1503`) | HIC `SetDisabled(true)` | Covered |
| **Sorter TEST (V4)** | **NO** | None in MissionInit; DoClose would release if called | HIC `SetDisabled(true)` | **HIGH — SetDisabled stuck** |
| **BTC ATM** | **YES** `:192-212` | Close → `PlayerControlEnable` (`:1312-1317`) | `PlayerControlDisable(ALL)` | Covered |
| **CCTV viewport** | **NO** | No `IsAlive`/`IsUnconscious` in Tick (`LFPG_CameraViewport.c:808+`); timeout 120s (`:52`, `:897`) | HIC disabled + spectator | **HIGH — spectator/orphan until timeout/server** |
| **Searchlight grab** | **NO** | Null player → local `DoCleanup` **without RPC** (`:217-221`); **no** IsAlive | Aim continues while corpse exists | **MED — server grab orphan; aim on corpse** |
| **DeviceInspector** | N/A (passive) | Hides if no player (`:476-480`); ForceHide under CCTV (`MissionInit:328`) | None | Low |
| **TankHUD** | N/A (passive) | Hide if no player (`:114-118`) | None | Low |
| **Wiring session** | Partial | Auto-cancel if reel not in hands (`:331-348`); **not** on death if reel still in corpse hands | Preview only | MED — cancel on death missing |
| **InventoryMenu cargo hook** | N/A | OnHide unsub | None | See F-MISS-005 |

### Findings — death matrix

**F-MISS-018** `scripts/5_Mission/LFPG_MissionInit.c:167-213`  
Coverage gap: **TEST sorter, CCTV, searchlight, wiring** not force-closed on death/unconscious. Comment at `:167-169` documents sorter SetDisabled rationale but was not generalized.

**F-MISS-019** `scripts/4_World/LFPG_CameraViewport.c:808-902`  
No death/unconscious check while `m_Active`. Player may be null in spectator (`:91` comment, `:405-408` stores `m_PlayerRef`). Death of body does not set `m_ExitPhase=1`. Stuck until `LFPG_CCTV_MAX_DURATION_S` (120s) or external SafeAbort (device destroy only, `:245-258`).

**F-MISS-020** `scripts/4_World/LFPG_SearchlightController.c:217-221`  
`!player` → `DoCleanup()` only (no `SEARCHLIGHT_EXIT_V2` RPC). Dead-but-non-null player (`IsAlive()==false`) continues grab Tick (distance/F-key). Server may keep operator state until distance/F/exit.

**F-MISS-021** `scripts/5_Mission/LFPG_MissionInit.c:331-348`  
Wiring auto-cancel only checks reel-in-hands after early path work. Dead player still “holds” reel on corpse → session may remain active (preview/cables) until reel removed. No IsAlive cancel.

**F-MISS-022** Death close does not set EscCloseTime  
Force `Close()`/`DoClose()` from OnUpdate does not stamp `s_EscCloseTime` (only `HandleEscKey` does). Low: pause menu race only if death coincides with ESC release.

---

## Offline vs server / dedicated

| Concern | Dedicated server | Client→server | Offline SP (typical MissionGameplay + IsServer) |
|---------|------------------|---------------|--------------------------------------------------|
| Mission class | `MissionServer` | `MissionGameplay` client | `MissionGameplay` host |
| ElecGraph Impl factory | **yes** (`:36`) | N/A client (no graph) | **no → stub** (F-MISS-001) |
| NetworkManager scheduler | OnInit + ctor | client no SERVER graph block | ctor only if SERVER compiled |
| Client UIs / MissionInit UI | stripped `#ifndef SERVER` | full | full client path runs |
| `g_Game.IsDedicatedServer()` early return in OnUpdate | true → skip client UI ticks | false | false → UI ticks on host |
| CCTV ResetGUI skip | N/A | `m_LFPG_SkipResetGUI` (`:50-58`) | same |

**F-MISS-023** `#ifndef SERVER` client boundary  
TankHUD, InventoryMenuMod, UIScaler, full CameraViewport live only on client build. Server uses CameraViewport stub (`LFPG_CameraViewport.c:1159-1190`). Hybrid SP compilation depends on DayZ dual-script packaging; residual risk if a call site lacks `#ifndef SERVER` and hits stub Get()=null — MissionInit client block is correctly gated.

**F-MISS-024** `MissionServer.OnMissionFinish` (`:22-34`) vs client  
Server flushes NM + Native balance only. No client singleton cleanup on dedicated (correct). Offline host that only runs MissionGameplay finish path must clean client UIs — it does (`:351+`). Server-only pieces on offline host: NM exists under SERVER; balance flush only if MissionServer.OnMissionFinish runs — **offline may skip Native balance flush** if that override never executes. Related to F-MISS-001 host class split.

---

## CableHUD / TankHUD / InventoryMenu / UIScaler interactions (MissionInit)

| Interaction | Status | Note |
|-------------|--------|------|
| CableHUD Begin/End with producers | OK | F-MISS-010 |
| Skip cables during CCTV | OK | `skipCameraOps` |
| Inspector ForceHide during CCTV | OK | `:321-328` |
| TankHUD vs CCTV | Weak | F-MISS-011 |
| TankHUD shares ActionRaycast with Inspector | OK | proximity Init at `:85` |
| UIScaler only sorter(s) | Conflict | F-MISS-008 |
| Inventory cargo refresh | Leak if no OnHide | F-MISS-005 |
| super.OnUpdate always first | OK | `:162` — camera-safe per file header |

---

## Severity rollup

| ID | Sev | One-liner |
|----|-----|-----------|
| F-MISS-001 | **Critical** (offline) | ElecGraph factory missing on MissionGameplay |
| F-MISS-014 | **High** | TEST sorter input/ESC not wired in MissionInit |
| F-MISS-018 | **High** | Death matrix incomplete (TEST/CCTV/SL/wiring) |
| F-MISS-019 | **High** | CCTV no death exit |
| F-MISS-008 | **High** | UIScaler shared Capture V3/V4 |
| F-MISS-006 | **Med** | Init without Reset on reconnect |
| F-MISS-007 | **Med** | Cleanup without DoClose |
| F-MISS-005 | **Med** | Cargo invoker stale callback |
| F-MISS-020 | **Med** | Searchlight death/orphan RPC |
| F-MISS-021 | **Med** | Wiring not cancelled on death |
| F-MISS-002 | **Med** | Graph factory race on first Get() |
| F-MISS-004 | **Low** | BTCAtmClientData not reset on finish |
| F-MISS-011 | **Low** | TankHUD under CCTV |
| F-MISS-013 | **Low** | KeyRelease super while UI open |
| F-MISS-003/009/010/012/016/017/022/023/024 | Info/Low | Documented residual |

---

## Recommended fix order (no code in this review)

1. **Factory:** `override LFPG_ElecGraph LFPG_CreateElecGraph()` on `MissionGameplay` (same as MissionServer) — unblocks offline.
2. **MissionInit input + death:** mirror sorter/BTC blocks for `LFPG_SorterView_TEST`; add death/unconscious exit for CCTV (`m_ExitPhase=1`), Searchlight (`RequestExit`), wiring cancel, TEST Close.
3. **UIScaler:** per-view capture store, or re-Capture on every Open, or separate scaler instances for V3/TEST.
4. **Lifecycle harden:** OnInit always Cleanup/Reset then Init for sorter/BTC/tank/inspector; OnMissionFinish Close-then-Cleanup; clear cargo invoker; Reset BTCAtmClientData.

---

## Evidence index (primary)

- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\5_Mission\LFPG_MissionInit.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\5_Mission\LFPG_TankHUD.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\5_Mission\LFPG_InventoryMenuMod.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\3_Game\LFPG_UIScaler.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_CableHUD.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_SorterView.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\test\LFPG_SorterView_TEST.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_BTCAtmView.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_CameraViewport.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_SearchlightController.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_DeviceInspector.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_NetworkManager.c`
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_PlayerRPC.c` (MissionBaseWorld factory)
- `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid\scripts\4_World\LFPG_ElecGraph.c` (stub)
