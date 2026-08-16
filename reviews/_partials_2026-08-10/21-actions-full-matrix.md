# 21 — LFPowerGrid Actions Full Matrix

**Scope:** every `LFPG_Action*.c` + `LFPG_Actions.c` + `LFPG_ActionRegistration.c` + `LFPG_VanillaActionOverrides.c`  
**Source root:** `C:\Users\guill\OneDrive\Documentos\DayZ Projects\LFPowerGrid`  
**Method:** static review of scripts only (grep + Read). No runtime tests.  
**Distance constant:** `LFPG_INTERACT_DIST_M = 5.0` (`scripts\3_Game\LFPG_Defines.c:548`)  
**Ownership:** **none** on any LFPG action class (no territory/group/owner gate in action code).  
**IsServer:** DayZ actions use `OnExecuteServer` / `OnFinishProgressServer` / client RPC; no custom `IsServer()` override on LFPG actions.

**Counts (verified by file inventory):**
| Bucket | Count |
|--------|------:|
| Registered in `ActionConstructor.RegisterActions` | **57** (comment claims 55 — stale) |
| Concrete action classes (incl. port/cut/burner subclasses) | **62** (+ bases/helpers not registered) |
| Base / helper / CB (not registered) | PortBase, CutPortBase, ToggleBurnerBase, PlaceGeneric parent chain, ActionRaycast, *CB |
| Orphan registered-but-never-`AddAction` | **0** |
| Vanilla overrides | 2 (`ActionTurnOn/OffPowerGenerator`) |

---

## 0. Legend

| Col | Meaning |
|-----|---------|
| **Base** | Action base class |
| **CCI / CCT** | Item / target condition components |
| **Cond** | Key `ActionCondition` gates |
| **Exec** | Where mutation happens |
| **Power** | Power gate in action |
| **Own** | Ownership check (always **N**) |
| **Dist** | Distance enforcement |
| **AddAction host** | Class that calls `AddAction(...)` |

**Exec codes:** `Srv` = OnExecuteServer, `FinSrv` = OnFinishProgressServer, `Cli` = OnExecuteClient (+ RPC), `EndSrv` = OnEndServer, `—` = none / empty.

---

## 1. Full matrix

### 1.1 Wiring / cable reel / pliers (`LFPG_Actions.c` + tool SetActions)

| # | Class | File:line | Base | CCI / CCT | Cond (summary) | Exec | Cont. | Power | Own | Dist | AddAction host | Reg |
|---|-------|-----------|------|-----------|----------------|------|-------|-------|-----|------|----------------|-----|
| 1 | `ActionLFPG_PortBase` | `LFPG_Actions.c:463` | SingleUse | CCINonRuined / CCTCursor(5) | CableReel; electric device; port index valid; hide if Sorter UI open (client) | **Cli** Start/Finish wiring session | n/a | N | **N** | CCT | (base) | n/a |
| 2–8 | `ActionLFPG_Port0`…`Port6` | `LFPG_Actions.c:621-654` | PortBase | ↑ | m_PortIndex 0..6 | Cli | n/a | N | N | CCT | `LFPG_CableReel` `LFPG_Items.c:43-49` | Y |
| 9 | `ActionLFPG_PlaceWaypoint` | `LFPG_Actions.c:659` | SingleUse | CCINonRuined / **CCTNone** | CableReel; wiring session active (client); not on device/player | **Cli** AddWaypoint | n/a | N | N | session-only | CableReel:52 | Y |
| 10 | `ActionLFPG_CancelWiring` | `LFPG_Actions.c:742` | SingleUse | CCINonRuined / CCTNone | CableReel; session active | **Cli** Cancel | n/a | N | N | — | CableReel:53 | Y |
| 11 | `ActionLFPG_CutWires` | `LFPG_Actions.c:805` | SingleUse | CCINonRuined / CCTCursor(5) | **Pliers**; energy **source**; DistSq | **Cli** RPC `CUT_WIRES` | n/a | N | **N** | CCT+DistSq | `Pliers` Registration:127 | Y |
| 12 | `ActionLFPG_CutPortBase` | `LFPG_Actions.c:878` | SingleUse | CCINonRuined / CCTCursor(5) | Pliers; port exists; **port occupied**; DistSq | **Cli** RPC `CUT_PORT` | n/a | N | **N** | CCT+DistSq | (base) | n/a |
| 13–19 | `ActionLFPG_CutPort0`…`6` | `LFPG_Actions.c:1048-1080` | CutPortBase | ↑ | port index 0..6 | Cli | n/a | N | N | CCT+DistSq | Pliers:128-134 | Y |
| 20 | `ActionLFPG_ToggleSource` | `LFPG_Actions.c:1086` | Interact | CCINone / CCTCursor(5) | `LFPG_Generator`; DistSq; OFF needs sparkplug | **Srv** `LFPG_ToggleSource` | n/a | sparkplug gate | N | CCT+DistSq | `LFPG_Generator` TestDevices:130 | Y |
| 21 | `ActionLFPG_DebugStatus` | `LFPG_Actions.c:1238` | SingleUse | CCINonRuined / CCTCursor(5) | CableReel; electric device; suppress linked Sorter | **Cli** MessageStatus dump | n/a | read-only | N | CCT | CableReel:56 | Y |
| — | `LFPG_ActionRaycast` | `LFPG_Actions.c:51` | helper | — | ray helpers | — | — | — | — | — | — | **not action** |

Notes:
- Port / Cut / Waypoint / Cancel / Debug execute **client-side** then RPC (wiring/cut). Server authority lives in RPC handlers (out of this partial’s deep scope).
- `AddActionJuncture` overridden to `true` for PlaceWaypoint:708 and CancelWiring:796 (anti soft-target crash).

### 1.2 Placement

| # | Class | File:line | Base | CCI / CCT | Cond | Exec | Cont. | Power | Own | Dist | AddAction host | Reg |
|---|-------|-----------|------|-----------|------|------|-------|-------|-----|------|----------------|-----|
| 22 | `LFPG_ActionPlaceGeneric` | `LFPG_ActionPlaceGeneric.c:18` | ActionPlaceObject | vanilla inherit | vanilla place | **EndSrv** cleanup shim (no `super` — Frontier_Fortifications NULL guard) | vanilla place | N | N | vanilla hologram | `LFPG_KitBase` / `LFPG_KitBaseDeployable` | Y |
| 23 | `LFPG_ActionPlaceLogicGate` | `LFPG_ActionPlaceLogicGate.c:18` | PlaceGeneric | inherit | inherit (no override) | inherit | inherit | N | N | inherit | `LFPG_LogicGate_Kit.LFPG_AddPlaceAction` LogicGate.c:24 | Y |

### 1.3 Continuous tools (Hammer / Screwdriver)

| # | Class | File:line | Base | CCI / CCT | Cond | Exec | Cont. gate | Power | Own | Dist | AddAction host | Reg |
|---|-------|-----------|------|-----------|------|------|------------|-------|-----|------|----------------|-----|
| 24 | `LFPG_ActionUpgradeSolarPanel` | `LFPG_ActionUpgradeSolarPanel.c:36` | Continuous | CCINonRuined / CCTCursor(5) | Hammer; `LFPG_SolarPanel` not T2; plate≥5 + nails≥20 | **FinSrv** spawn T2, consume mats, kill T1 | **CAContinuousTime(8s)** | N | **N** | CCT only (no DistSq; no re-dist on finish) | `Hammer` Registration:144 | Y |
| 25 | `LFPG_ActionUpgradeWaterPump` | `LFPG_ActionUpgradeWaterPump.c:36` | Continuous | CCINonRuined / CCTCursor(5) | Hammer; T1 pump; plates+nails | **FinSrv** spawn T2, filter transfer, kill T1 | **CAContinuousTime(8s)** | N | **N** | CCT only | Hammer:145 | Y |
| 26 | `LFPG_ActionInstallMic` | `LFPG_ActionInstallMic.c:35` | Continuous | CCINonRuined / CCTCursor(5) | Screwdriver; Intercom; !radioInstalled; PersonalRadio in slot | **FinSrv** `LFPG_InstallRadio` | **CAContinuousTime(5s)** | N | **N** | CCT only (no DistSq) | `Screwdriver` Registration:156 | Y |
| 27 | `LFPG_ActionDismantleDevice` | `LFPG_ActionDismantleDevice.c:44` | Continuous | CCINonRuined / CCTCursor(5) | Screwdriver; DeviceBase; kit≠""; **no att/cargo**; DistSq | **FinSrv** re-validate → spawn kit → dmg screw 10% → ObjectDelete | **CAContinuousTime(5s)** | N | **N** | CCT+DistSq **revalidated** | Screwdriver:157 | Y |

### 1.4 Device interact (no tool)

| # | Class | File:line | Base | CCI / CCT | Cond | Exec | Power | Own | Dist | AddAction host | Reg |
|---|-------|-----------|------|-----------|------|------|-------|-----|------|----------------|-----|
| 28 | `LFPG_ActionWatchMonitor` | `LFPG_ActionWatchMonitor.c:36` | Interact | CCINone / CCT(5) | Monitor; powered; DistSq; not in CCTV/searchlight | **Cli** RPC `REQUEST_CAMERA_LIST`; Srv empty | **Y** powered | N | CCT+DistSq | `LFPG_Monitor`:81 | Y |
| 29 | `LFPG_ActionTogglePushButton` | `LFPG_ActionTogglePushButton.c:16` | Interact | CCINone / CCT(5) | PushButton; DistSq; not already pressed | **Srv** `LFPG_ToggleButton` | N | N | CCT+DistSq | `LFPG_PushButton`:67 | Y |
| 30 | `LFPG_ActionToggleSwitchV2` | `LFPG_ActionToggleSwitchV2.c:16` | Interact | CCINone / CCT(5) | SwitchV2; DistSq | **Srv** `LFPG_ToggleSwitch` | N | N | CCT+DistSq | `LFPG_SwitchV2`:90 | Y |
| 31 | `LFPG_ActionToggleSwitchRemote` | `LFPG_ActionToggleSwitchRemote.c:15` | Interact | CCINone / CCT(5) | SwitchRemote; DistSq | **Srv** toggle | N | N | CCT+DistSq | `LFPG_SwitchRemote`:77 | Y |
| 32 | `LFPG_ActionToggleSwitchV2Remote` | `LFPG_ActionToggleSwitchV2Remote.c:16` | Interact | CCINone / CCT(5) | SwitchV2Remote; DistSq | **Srv** toggle | N | N | CCT+DistSq | `LFPG_SwitchV2Remote`:69 | Y |
| 33 | `LFPG_ActionToggleFurnace` | `LFPG_ActionToggleFurnace.c:16` | Interact | CCINone / CCT(5) | Furnace; DistSq; ON needs fuel>0 **or** cargo | **Srv** `LFPG_ToggleFurnace` | fuel/cargo gate | N | CCT+DistSq | `LFPG_Furnace`:97 | Y |
| 34 | `LFPG_ActionFeedFurnace` | `LFPG_ActionFeedFurnace.c:30` | Interact | CCINone / CCT(5) | hands item; Furnace; DistSq; filters kits/reel/size; full-fuel gate | **Srv** add fuel / ObjectDelete item | N | **N** | CCT+DistSq | Furnace:98 | Y |
| 35 | `LFPG_ActionOperateSearchlight` | `LFPG_ActionOperateSearchlight.c:14` | Interact | CCINone / CCT(5) | Searchlight; **powered**; DistSq; not in viewport/controller | **Cli** RPC `SEARCHLIGHT_ENTER`; Srv empty | **Y** | N | CCT+DistSq | `LFPG_Searchlight`:103 | Y |
| 36 | `LFPG_ActionCycleDetectMode` | `LFPG_ActionCycleDetectMode.c:13` | Interact | CCINone / CCT(5) | MotionSensor; DistSq | **Srv** `LFPG_CycleDetectMode` | N | **N** | CCT+DistSq | `LFPG_MotionSensor`:118 | Y |
| 37 | `LFPG_ActionPairSensor` | `LFPG_ActionPairSensor.c:15` | Interact | CCINone / CCT(5) | MotionSensor; DistSq | **Srv** overwrite paired group (LBmaster) | N | **N** | CCT+DistSq | MotionSensor:119 | Y |
| 38 | `LFPG_ActionToggleBatteryOutput` | `LFPG_ActionToggleBatteryOutput.c:18` | Interact | CCINone / CCT(5) | BatteryBase; gate-capable; DistSq | **Srv** toggle output + propagate | N | N | CCT+DistSq | `LFPG_Battery` Base:116 | Y |
| 39 | `LFPG_ActionToggleIntercom` | `LFPG_ActionToggleIntercom.c:16` | Interact | CCINone / CCT(5) | Intercom; **powered**; DistSq | **Srv** `LFPG_ToggleIntercom` | **Y** | N | CCT+DistSq | `LFPG_Intercom`:144 | Y |
| 40 | `LFPG_ActionRFToggle` | `LFPG_ActionRFToggle.c:17` | Interact | CCINone / CCT(5) | Intercom; powered+ON; cooldown 2s; DistSq | **Srv** `LFPG_ExecuteRFToggle` (recheck power) | **Y** | N | CCT+DistSq | Intercom:145 | Y |
| 41 | `LFPG_ActionToggleBroadcast` | `LFPG_ActionToggleBroadcast.c:16` | Interact | CCINone / CCT(5) | Intercom; radio installed; DistSq | **Srv** `LFPG_ToggleBroadcast` | radio T2 | N | CCT+DistSq | Intercom:146 | Y |
| 42 | `LFPG_ActionCycleFrequency` | `LFPG_ActionCycleFrequency.c:17` | Interact | CCINone / CCT(5) | Intercom; radio installed; DistSq | **Srv** `LFPG_CycleFrequency` | radio T2 | N | CCT+DistSq | Intercom:147 | Y |
| 43 | `LFPG_ActionCheckSprinkler` | `LFPG_ActionCheckSprinkler.c:13` | Interact | CCINone / CCT(5) | Sprinkler; DistSq | **Srv** status MessageStatus | read | N | CCT+DistSq | `LFPG_Sprinkler`:86 | Y |
| 44 | `LFPG_ActionToggleFridgeDoor` | `LFPG_ActionToggleFridgeDoor.c:11` | Interact | CCINone / CCT(5) | Fridge; DistSq | **Srv** `LFPG_ToggleDoor` | N | N | CCT+DistSq | `LFPG_Fridge`:57 | Y |
| 45 | `LFPG_ActionToggleBurnerBase` | `LFPG_ActionToggleBurner.c:11` | Interact | CCINone / CCT(5) | ElectricStove; DistSq | **Srv** `LFPG_ToggleBurner(i)` | N in action | N | CCT+DistSq | (base) | n/a |
| 46–49 | `ToggleBurner0`…`3` | `LFPG_ActionToggleBurner.c:112-141` | BurnerBase | ↑ | burner index 0..3 | Srv | N | N | CCT+DistSq | `LFPG_ElectricStove`:649-652 | Y |
| 50 | `LFPG_ActionOpenBTCAtm` | `LFPG_ActionOpenBTCAtm.c:26` | Interact | CCINone / CCT(5) | BTCAtmBase; ATM powered; !ruined | **Cli** RPC `BTC_OPEN_REQUEST` | **Y** ATM | N | CCT | `LFPG_BTCAtm`:296 | Y |
| 51 | `LFPG_ActionSpeakerOn` | `LFPG_ActionSpeakerOn.c:13` | Interact | CCINone / CCT(5) | Speaker; currently OFF; DistSq | **Srv** `LFPG_ToggleSpeaker(true)` | N (knob physical) | N | CCT+DistSq | `LFPG_Speaker`:107 | Y |
| 52 | `LFPG_ActionSpeakerOff` | `LFPG_ActionSpeakerOff.c:12` | Interact | CCINone / CCT(5) | Speaker; currently ON; DistSq | **Srv** toggle false | N | N | CCT+DistSq | Speaker:108 | Y |
| 53 | `LFPG_ActionOpenSorterPanel` | `LFPG_ActionOpenSorterPanel.c:24` | Interact | CCINone / CCT(5) | exact type `LFPG_Sorter`; powered; !ruined; linked; UI closed | **Cli** RPC `SORTER_CONFIG_REQUEST` | **Y** | N | CCT | `LFPG_Sorter`:102 | Y |
| 54 | `LFPG_ActionSyncSorter` | `LFPG_ActionSyncSorter.c:24` | Interact | CCINone / CCT(5) | Sorter IsKindOf; powered; !ruined; UI closed | **Cli** RPC `SORTER_RESYNC` | **Y** | N | CCT | Sorter:103 (+ inherits on TEST) | Y |
| 55 | `LFPG_ActionOpenSorterPanel_TEST` | `test\LFPG_ActionOpenSorterPanel_TEST.c:24` | Interact | CCINone / CCT(5) | Sorter_TEST; powered; linked | **Cli** RPC `SORTER_TEST_CONFIG_REQUEST` | **Y** | N | CCT | `LFPG_Sorter_TEST`:42 | Y |

### 1.5 Water pump continuous

| # | Class | File:line | Base | CCI / CCT | Cond | Exec | Cont. | Power | Own | Dist | Host | Reg |
|---|-------|-----------|------|-----------|------|------|-------|-------|-----|------|------|-----|
| 56 | `LFPG_ActionDrinkPump` | `LFPG_ActionDrinkPump.c:17` | Continuous | CCINone / **CCTCursor(UAMaxDistances.DEFAULT)** | CanEatAndDrink; T1 powered & !sprinkler; T2 powered or tank | **FinSrv** Consume + tank drain | CAContinuousRepeat DRINK_WELL; ContinuousInteractInput | powered / tank | N | **UA DEFAULT** | WaterPump T1:81 T2:407 | Y |
| 57 | `LFPG_ActionWashHandsPump` | `LFPG_ActionWashHandsPump.c:17` | Continuous | CCINone / **CCTObject(UA DEFAULT)** | bloody hands; empty hands; no gloves; same water gates | **FinSrv** clear agents + tank | CAContinuousRepeat WASH_HANDS | powered / tank | N | **UA DEFAULT** | T1:82 T2:408 | Y |
| 58 | `LFPG_ActionFillPump` | `LFPG_ActionFillPump.c:21` | Continuous | CCINonRuined / CCTCursor(UA DEFAULT) | T2 only; **unpowered**; tank>0; container space; CanFillContainer | **FinSrv** transfer liquid; **FinCli** sound | CAContinuousRepeat + ActionConditionContinue | unpowered path | N | **UA DEFAULT** | T2 only:409 | Y |

### 1.6 Remote controller (item-held)

| # | Class | File:line | Base | CCI / CCT | Cond | Exec | Power | Own | Dist | Host | Reg |
|---|-------|-----------|------|-----------|------|------|-------|-----|------|------|-----|
| 59 | `LFPG_ActionPairRemote` | `LFPG_ActionPairRemote.c:19` | SingleUse | CCINone / CCT(5) | RemoteController in hands; RF-capable target; DistSq | **Srv** pair/unpair + LED/sound + sync list | N | **N** | CCT+DistSq | `LFPG_RemoteController`:108 | Y |
| 60 | `LFPG_ActionActivateRemote` | `LFPG_ActionActivateRemote.c:20` | SingleUse | CCINone / **CCTNone** | RemoteController in hands | **Srv** `LFPG_ActivateToggle` (range 200m, CD 1s) | N | **N** (item possession) | range on device side | RemoteController:109 | Y |

---

## 2. Registration ↔ SetActions cross-check

### 2.1 `RegisterActions` inventory (`LFPG_ActionRegistration.c:16-119`)

| Group | Inserts | # |
|-------|---------|--:|
| Port0–6 | 21–27 | 7 |
| Waypoint, Cancel, CutWires, CutPort0–6 | 30–39 | 10 |
| ToggleSource, DebugStatus | 40–41 | 2 |
| Upgrade Solar/Pump | 44–45 | 2 |
| PlaceGeneric, PlaceLogicGate | 48–49 | 2 |
| WatchMonitor | 52 | 1 |
| PushButton, SwitchV2, SwitchRemote, SwitchV2Remote | 55–58 | 4 |
| Drink/Wash/Fill Pump | 61–63 | 3 |
| OpenSorter, SyncSorter, OpenSorter_TEST | 66–69 | 3 |
| ToggleFurnace, FeedFurnace | 72–73 | 2 |
| OperateSearchlight | 76 | 1 |
| CycleDetectMode, PairSensor | 79–80 | 2 |
| ToggleBatteryOutput | 83 | 1 |
| Intercom ×5 (Toggle, RF, InstallMic, Broadcast, CycleFreq) | 86–90 | 5 |
| CheckSprinkler | 93 | 1 |
| ToggleFridgeDoor | 96 | 1 |
| Burner0–3 | 99–102 | 4 |
| OpenBTCAtm | 105 | 1 |
| SpeakerOn/Off | 108–109 | 2 |
| PairRemote, ActivateRemote | 112–113 | 2 |
| DismantleDevice | 116 | 1 |
| **Total** | | **57** |

Log string at line 118 claims `(55)` — **stale** (see F-AX-009).

### 2.2 Tool hosts (Registration modded classes)

| Tool | Actions | Lines |
|------|---------|-------|
| `Pliers` | CutWires + CutPort0–6 | 122–135 |
| `Hammer` | UpgradeSolar, UpgradeWaterPump | 139–146 |
| `Screwdriver` | InstallMic, DismantleDevice | 151–158 |

### 2.3 Device / item SetActions hosts

| Host | Path | Actions |
|------|------|---------|
| `LFPG_CableReel` | `LFPG_Items.c:34-56` | Port0–6, PlaceWaypoint, CancelWiring, DebugStatus, TakeItem* |
| `LFPG_Generator` | `LFPG_TestDevices.c:108-130` | ToggleSource; RemoveAction vanilla on/off/plug |
| `LFPG_DeviceBase` | `lfpg_devicebase.c:411-416` | RemoveAction TakeItem* (no LFPG adds) |
| `LFPG_KitBase` | `LFPG_KitBase.c:156-160` | TogglePlaceObject + `LFPG_AddPlaceAction()` |
| `LFPG_KitBaseDeployable` | `LFPG_KitBaseDeployable.c:105-109` | TogglePlaceObject + PlaceGeneric |
| `LFPG_LogicGate_Kit` | `LFPG_LogicGate.c:22-25` | PlaceLogicGate (overrides AddPlaceAction) |
| `LFPG_Monitor` | `LFPG_Monitor.c:78-81` | WatchMonitor |
| `LFPG_PushButton` | `LFPG_PushButton.c:64-67` | TogglePushButton |
| `LFPG_SwitchV2` | `LFPG_SwitchV2.c:87-90` | ToggleSwitchV2 |
| `LFPG_SwitchRemote` | `LFPG_SwitchRemote.c:74-77` | ToggleSwitchRemote |
| `LFPG_SwitchV2Remote` | `LFPG_SwitchV2Remote.c:66-69` | ToggleSwitchV2Remote |
| `LFPG_WaterPump` | `LFPG_WaterPump.c:78-82` | Drink, Wash |
| `LFPG_WaterPump_T2` | `LFPG_WaterPump.c:404-409` | Drink, Wash, Fill |
| `LFPG_Furnace` | `LFPG_Furnace.c:94-98` | ToggleFurnace, FeedFurnace |
| `LFPG_Sorter` | `LFPG_Sorter.c:99-103` | OpenSorterPanel, SyncSorter |
| `LFPG_Sorter_TEST` | `test\LFPG_Sorter_TEST.c:39-42` | super + OpenSorterPanel_TEST |
| `LFPG_Searchlight` | `LFPG_Searchlight.c:100-103` | OperateSearchlight |
| `LFPG_MotionSensor` | `LFPG_MotionSensor.c:115-119` | CycleDetectMode, PairSensor |
| Battery (Medium/Large path) | `LFPG_Battery.c:113-116` | ToggleBatteryOutput |
| `LFPG_Intercom` | `LFPG_Intercom.c:141-147` | ToggleIntercom, RFToggle, ToggleBroadcast, CycleFrequency |
| `LFPG_Sprinkler` | `LFPG_Sprinkler.c:83-86` | CheckSprinkler |
| `LFPG_Fridge` | `LFPG_Fridge.c:54-57` | ToggleFridgeDoor |
| `LFPG_ElectricStove` | `LFPG_ElectricStove.c:645-652` | Burner0–3 |
| `LFPG_BTCAtm` | `LFPG_BTCAtm.c:293-296` | OpenBTCAtm |
| `LFPG_Speaker` | `LFPG_Speaker.c:104-108` | SpeakerOn, SpeakerOff |
| `LFPG_RemoteController` | `LFPG_RemoteController.c:105-109` | PairRemote, ActivateRemote |

### 2.4 Orphans

| Check | Result |
|-------|--------|
| Registered but never `AddAction` | **None** — every Insert has ≥1 AddAction host |
| `AddAction` without Register | **None found** among LFPG_* action types |
| Registered but intentionally dual-host | InstallMic+Dismantle on Screwdriver; upgrades on Hammer; cuts on Pliers |

### 2.5 Dual AddAction note (Sorter_TEST)

`LFPG_Sorter_TEST.SetActions` calls `super` then adds TEST open panel. V3 `OpenSorterPanel` is also on the list but **hidden** by exact `GetType() == "LFPG_Sorter"` (`LFPG_ActionOpenSorterPanel.c:55-57`). SyncSorter remains shared (IsKindOf). Intentional.

---

## 3. Continuous gates detail

| Action | Component | Duration / unit | Re-validate on finish | ActionConditionContinue |
|--------|-----------|-----------------|----------------------|-------------------------|
| Dismantle | CAContinuousTime | 5.0 s | **Full** `LFPG_ValidateDismantle` | (engine default) |
| InstallMic | CAContinuousTime | 5.0 s | radioInstalled + radio present | no |
| Upgrade Solar | CAContinuousTime | 8.0 s | T2 type + materials qty | no |
| Upgrade Pump | CAContinuousTime | 8.0 s | T2 type + materials qty | no |
| DrinkPump | CAContinuousRepeat | UATimeSpent.DRINK_WELL | power/tank in FinSrv | no explicit |
| WashHandsPump | CAContinuousRepeat | UATimeSpent.WASH_HANDS | power/tank | no |
| FillPump | CAContinuousRepeat | UATimeSpent.WASH_HANDS | tank/space in FinSrv | **yes** (tank + container) |
| PlaceGeneric | vanilla place pipeline | anim | EndSrv cancel path | vanilla |

---

## 4. `LFPG_VanillaActionOverrides.c`

| Modded class | Path | Behavior |
|--------------|------|----------|
| `ActionTurnOnPowerGenerator` | `:23-105` | **ActionCondition:** false if target Cast/IsKindOf `LFPG_Generator`. **OnExecuteServer safety net:** if still LFPG gen → sparkplug gate then `LFPG_ToggleSource` (not super). |
| `ActionTurnOffPowerGenerator` | `:110-177` | Same block pattern; safety net always `LFPG_ToggleSource` for LFPG gen. |

**Defense-in-depth stack:**
1. `LFPG_Generator.SetActions` `RemoveAction` TurnOn/Off + Plug (`LFPG_TestDevices.c:116-124`)
2. Condition block in vanilla overrides
3. Execute redirect with sparkplug gate (TurnOn)
4. `ActionLFPG_ToggleSource` is the intended path

**No constructor overrides** (commented risk: would break all vanilla generators).

---

## 5. Grief / exploit analysis (actions)

### 5.1 High severity

| ID | Title | Evidence | Impact |
|----|-------|----------|--------|
| **F-AX-001** | **Zero ownership on all LFPG actions** | Grep `owner|territory|permission` on `LFPG_Action*.c` → no functional checks | Any player in 5 m can operate, reconfigure, cut, dismantle, feed, pair, upgrade foreign devices |
| **F-AX-005** | **Dismantle = kit steal** | `LFPG_ActionDismantleDevice.c:65-116,133-194` — only empty DeviceBase + screwdriver + dist; spawn kit at feet | Raider recovers deployment kit after 5 s if device emptied; **no owner gate** |
| **F-AX-006** | **Wire cut grief (Pliers)** | CutWires `:820-844`, CutPort `:895-1014` — no owner; Cli RPC | Sever base power graph at 5 m with pliers |
| **F-AX-003** | **PairSensor overwrite** | `LFPG_ActionPairSensor.c:59-100` — always `LFPG_SetPairedGroupName` from actor’s group | Hostile re-pairs TEAM/ENEMY sensor to their group or clears pairing |
| **F-AX-002** | **FeedFurnace kit denylist incomplete** | `LFPG_IsLFPGKit` `:259-305` lists ~21 kits; `config.cpp` defines many more | **Burnable kits not filtered:** e.g. `LFPG_Intercom_Kit`, `LFPG_Speaker_Kit`, `LFPG_Fridge_Kit`, `LFPG_Sprinkler_Kit`, `LFPG_BatteryLarge_Kit`, `LFPG_BatteryAdapter_Kit`, `LFPG_DoorController_Kit`, `LFPG_ElectricStove_Kit`, `LFPG_BTCAtm_Kit`, `LFPG_BTCAtmAdmin_Kit`, `LFPG_MemoryCell_Kit`, `LFPG_SwitchRemote_Kit`, `LFPG_SwitchV2Remote_Kit`, `LFPG_Sorter_TEST_Kit` (exact type ≠ `Sorter_Kit`) |

### 5.2 Medium severity

| ID | Title | Evidence | Impact |
|----|-------|----------|--------|
| **F-AX-004** | **PairRemote open** | `LFPG_ActionPairRemote.c:34-80,88-164` | Anyone with a remote can pair RF switches in 5 m (cap 32, 200 m activate) |
| **F-AX-011** | **Open control of switches / furnace / burners / RF** | Toggle* OnExecuteServer with only Cast+dist | Sabotage base power/heat/cooking without ownership |
| **F-AX-010** | **Whitelist furnace burns items for 0 fuel** | FeedFurnace `:128-135,207-240` — non-whitelist → destroy with `fuelToAdd=0` | Grief/disposal of valuables when `FurnaceFuelWhitelistOnly` |
| **F-AX-012** | **ActivateRemote range blast** | RemoteController 200 m (`LFPG_RemoteController.c:31-35,422+`); action CCTNone | Stolen/looted remote toggles all paired RF devices in range (CD 1 s) |
| **F-AX-007** | **Upgrade actions weak distance / finish gates** | Solar/Pump ActionCondition: no DistSq; FinSrv: materials recheck, **no hammer re-kind, no DistSq** | Relies on engine continuous lock + CCTCursor only |
| **F-AX-008** | **InstallMic no DistSq / incomplete finish item check** | InstallMic `:52-86` no DistSq; FinSrv rechecks radio state only | Weaker than Dismantle pattern |
| **F-AX-016** | **Hostile complete of victim’s upgrade** | Upgrade needs hammer + mats already on device | Attacker can finish upgrade / consume victim’s attached plates+nails |

### 5.3 Low / hygiene

| ID | Title | Evidence |
|----|-------|----------|
| **F-AX-009** | Register log says 55, actual **57** | Registration.c:118 vs count §2.1 |
| **F-AX-013** | TEST sorter action registered in production constructor | Registration.c:69 + test class |
| **F-AX-015** | Pump actions use `UAMaxDistances.DEFAULT` not `LFPG_INTERACT_DIST_M` | Drink/Wash/Fill CreateConditionComponents |
| **F-AX-014** | Vanilla TurnOn safety-net toggles (not force-ON) | Overrides.c:84 — if condition leak, behaves as toggle; rare |

### 5.4 Mitigations already present (good)

| Control | Where |
|---------|--------|
| Dismantle requires empty cargo+attachments | DismantleDevice.c:95-107 |
| Dismantle FinSrv full re-validate | `:151-158` |
| T2 Solar/Pump + BatteryAdapter not dismantlable | kit classname `""` |
| Upgrade FinSrv material re-check | Solar:121-148, Pump:116-143 |
| Upgrade spawn-before-mutate | Solar:165-171, Pump:191-197 |
| FeedFurnace re-validate hands item | Feed:192-200 |
| RFToggle server recheck power+switch | RFToggle.c:89-94 |
| Generator sparkplug gate + vanilla block | ToggleSource + VanillaOverrides + RemoveAction |
| Port actions blocked while Sorter UI open | PortBase `#ifndef SERVER` |
| PlaceWaypoint skips device/player soft-target | Raycast helpers + AddActionJuncture |
| Frontier fortification NULL bypass on place EndServer | PlaceGeneric.c:79-105 |

---

## 6. Findings index (F-AX-NNN)

| ID | Sev | One-liner | Primary path:line |
|----|-----|-----------|-------------------|
| F-AX-001 | **High** | No ownership/territory gate on any action | all `LFPG_Action*.c` (absence) |
| F-AX-002 | **High** | FeedFurnace kit denylist missing many kits → burnable kits | `LFPG_ActionFeedFurnace.c:259-305` |
| F-AX-003 | **High** | PairSensor overwritable by any player | `LFPG_ActionPairSensor.c:99` |
| F-AX-004 | **Med** | PairRemote open to any RF-capable device | `LFPG_ActionPairRemote.c:59-61,120-164` |
| F-AX-005 | **High** | Dismantle recovers kit without ownership | `LFPG_ActionDismantleDevice.c:133-194` |
| F-AX-006 | **High** | Cut wires/ports without ownership | `LFPG_Actions.c:805-1045` |
| F-AX-007 | **Med** | Upgrade Solar/Pump: no DistSq / weak finish auth | `LFPG_ActionUpgradeSolarPanel.c:57-97,99-190` |
| F-AX-008 | **Med** | InstallMic: no DistSq; finish skips screwdriver recheck | `LFPG_ActionInstallMic.c:52-138` |
| F-AX-009 | **Low** | Registered count comment 55 ≠ 57 | `LFPG_ActionRegistration.c:118` |
| F-AX-010 | **Med** | Whitelist mode destroys non-fuel items | `LFPG_ActionFeedFurnace.c:128-240` |
| F-AX-011 | **Med** | Unowned toggles (switches, furnace, burners, speaker, RF) | e.g. `LFPG_ActionToggleSwitchV2.c:64-96` |
| F-AX-012 | **Med** | Remote activate 200 m multi-toggle | `LFPG_ActionActivateRemote.c:51-98` + RemoteController range |
| F-AX-013 | **Low** | TEST action in production RegisterActions | `LFPG_ActionRegistration.c:69` |
| F-AX-014 | **Low** | Vanilla TurnOn safety-net is toggle semantics | `LFPG_VanillaActionOverrides.c:84` |
| F-AX-015 | **Low** | Pump CCT distance ≠ LFPG_INTERACT_DIST_M | Drink/Wash/Fill CreateConditionComponents |
| F-AX-016 | **Med** | Hostile completion of upgrades | Upgrade* ActionCondition anyone+Hammer |

---

## 7. OnExecute* / OnEnd* summary map

| Pattern | Actions |
|---------|---------|
| **OnExecuteServer only** | ToggleSource, ToggleFurnace, FeedFurnace, all Switch/Button/Burner/Fridge/Speaker/Intercom/RF/Broadcast/CycleFreq/Battery/CycleDetect/PairSensor/PairRemote/ActivateRemote, CheckSprinkler |
| **OnExecuteClient only** (+ empty Srv) | Port*, PlaceWaypoint, CancelWiring, Cut*, DebugStatus, WatchMonitor, OperateSearchlight, OpenSorter*, SyncSorter, OpenBTCAtm |
| **OnFinishProgressServer** | Dismantle, InstallMic, Upgrade Solar/Pump, Drink/Wash/Fill Pump |
| **OnFinishProgressClient** | FillPump (sound), WashHandsPump (sound) |
| **OnEndServer** | PlaceGeneric (compat shim; no super) |
| **OnStart/OnEnd** | DrinkPump (hide hands + loop sound) |

---

## 8. Not verified (honest scope)

- Runtime ActionManager binding / menu order.
- RPC server handlers for cut/wire/sorter/BTC/searchlight (distance re-auth there may exist — see `LFPG_RPCServerHandler.c` for some `LFPG_INTERACT_DIST_M` checks — **not fully re-audited here**).
- Whether continuous actions cancel if player walks out of CCT mid-bar (engine default).
- Territory mods (e.g. Expansion/BaseBuildingPlus) may add external gates not present in LFPG sources.
- Exact `UAMaxDistances.DEFAULT` numeric value on current DayZ build.

---

## 9. File inventory (action sources)

| File | Classes |
|------|---------|
| `scripts\4_World\LFPG_Actions.c` | Raycast helper; PortBase+0–6; PlaceWaypoint; CancelWiring; CutWires; CutPortBase+0–6; ToggleSource; DebugStatus |
| `scripts\4_World\LFPG_ActionRegistration.c` | RegisterActions + Pliers/Hammer/Screwdriver SetActions |
| `scripts\4_World\LFPG_VanillaActionOverrides.c` | modded TurnOn/Off PowerGenerator |
| `scripts\4_World\LFPG_Action*.c` (× ~35 files) | per-device/tool actions listed in matrix |
| `scripts\4_World\test\LFPG_ActionOpenSorterPanel_TEST.c` | TEST open panel |

**End of partial 21.**
