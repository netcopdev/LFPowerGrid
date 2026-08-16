# 22 — DeviceInspector deep review

**Scope:** `LFPG_DeviceInspector.c` (~1882 LOC, `#ifndef SERVER`), layout `gui/layouts/LFPG_DeviceInspector.layout`, RPC path `INSPECT_DEVICE` / `INSPECT_RESPONSE` (`LFPG_RPCServerHandler.HandleInspectDevice`, `LFPG_RPCClientHandler.HandleInspectResponse`), tick wiring in `LFPG_MissionInit`, raycast helper `GetCursorTargetDeviceWithProximity`.  
**Compare:** Sorter UI (`LFPG_SorterView` / `LFPG_SorterController`, Dabs MVC) + Sorter RPC proximity pattern.  
**Constraints:** findings only (`F-INSP-NNN` + `path:line`). No source edits.

---

## Summary

Inspector is a **client-only floating overlay** (cable reel held, no wiring session): SyncVars paint instantly; topology arrives via RPC. Client distance is soft (`LFPG_INTERACT_DIST_M` via raycast); **server inspect has rate-limit only — no distance, no entity-type, no ruined check**. That is the dominant security finding. Secondary: **port display ignores `LFPG_PortDef.m_Label`** (semantic ports wrong), **wire-row Y offsets drop Link/Battery after F3 height fix**, continuous per-frame cost, and architecture divergence from Sorter (no Dabs MVC, no explicit Open/Close focus lifecycle).

---

## Findings

### Security / multiplayer

#### F-INSP-001 — P0 · Server `INSPECT_DEVICE` has no distance check
**`scripts\4_World\LFPG_RPCServerHandler.c:1966-2120`** (esp. `1971-1972`, `1997-2013`, `2018-2086`, `2088-2110`)

`HandleInspectDevice` only gates on `sender` + `AllowPlayerAction`. It never compares `player.GetPosition()` to the resolved entity. Contrast Sorter config:

- `HandleSorterConfigRequest` → `vector.Distance(...) > LFPG_INTERACT_DIST_M` at **`2170-2175`**

A cheater can spam NetworkIDs (or deviceIds) from map edge and map full base topology.

**Impact:** remote reconnaissance of wire graph (ports, peers, power, overload state).

---

#### F-INSP-002 — P0 · Topology oracle without a live entity (netId 0,0 + clientDeviceId)
**`scripts\4_World\LFPG_RPCServerHandler.c:1997-2013`**, graph query **`2018-2086`**

If `netLow == 0 && netHigh == 0`, resolution is skipped and `serverDeviceId = clientDeviceId`. Graph is queried by that string regardless of whether any entity exists near the player. Client correlation id is echoed back (`rpc.Write(clientDeviceId)` at **2092**).

**Impact:** pure ID-space graph dump; no need to spoof NetworkIDs of spawned objects.

---

#### F-INSP-003 — P0 · Full topology + internal IDs + power leak in response payload
**Server write:** `LFPG_RPCServerHandler.c:2097-2108`  
**Client read:** `LFPG_RPCClientHandler.c:443-472`  
**Struct:** `LFPG_DeviceInspector.c:26-47`

Each wire entry ships:

| Field | Sensitivity |
|--------|-------------|
| `m_LocalPort` / `m_RemotePort` | port graph layout |
| `m_RemoteDeviceId` | stable internal device IDs |
| `m_RemoteTypeName` | peer type (class) |
| `m_AllocatedPower` | live load |
| `m_EdgeState` | overload/brownout |

UI only *displays* type name + local port + power (`PopulateWireData` **1430-1454**); `m_RemoteDeviceId` / `m_RemotePort` stay in memory for any client mod intercepting the RPC. Combined with F-INSP-001/002 this is full base graph intel, not cosmetic HUD data.

---

#### F-INSP-004 — P1 · No server validation that target is electric / not ruined / player has reel
**`LFPG_RPCServerHandler.c:1997-2013`**

Missing (present on Sorter path **2162-2188**):

- type cast / `IsElectricDevice`
- `IsRuined()`
- powered (optional for inspect, but ruined is not optional for interaction parity)
- client condition “holding cable reel” is **client-only** (`DeviceInspector.c:484-488`, `594-608`)

Any authenticated player with rate-limit budget can inspect.

---

#### F-INSP-005 — P1 · Inspect path mutates registry as side effect
**`LFPG_RPCServerHandler.c:2008-2010`**

```
LFPG_DeviceRegistry.Get().Register(resolvedObj, resolvedId);
```

Read-only inspect re-registers devices. Intended “heal stale refs”, but any remote inspect becomes a write path into registry (ordering/races with lifecycle). Prefer heal only on trust-bound wiring RPCs.

---

#### F-INSP-006 — P2 · Unbounded edge count in response (bandwidth / CPU)
**Server:** inserts all in+out edges, no cap (`2026-2084`).  
**Client clamp:** `wireCount > LFPG_MAX_WIRES_PER_DEVICE` (**64**, `RPCClientHandler.c:438-441`).  
**UI slots:** `LFPG_INSPECT_MAX_WIRES = 8` (`Defines.c:662`).

High-degree node → large RPC every 1s client cooldown while still only showing 8 rows. Cap server-side to UI max (or page) + drop remote IDs if not needed.

---

#### F-INSP-007 — P2 · Client distance is soft only
**`LFPG_Actions.c:249-252`**, proximity fallback **`344-366`** (`LFPG_INTERACT_DIST_M = 5.0`)

Honest clients stay near the device. Server never re-checks (F-INSP-001). Proximity sphere (`proxyRadius = 1.5` at **336**) can also select a *different* nearby electric device than the mesh under the crosshair → wrong panel + inspect of unintended peer (UX + mild confusion, not remote exploit by itself).

---

### Wrong port display

#### F-INSP-008 — P1 · `FormatPortName` invents labels; ignores `LFPG_PortDef.m_Label`
**`LFPG_DeviceInspector.c:1782-1815`** vs labels at device constructors

Heuristic only:

- hardcode `input_main`
- strip `input_*` / `output_*`

Breaks semantic ports:

| Device | Port name | True label | Inspector shows |
|--------|-----------|------------|-----------------|
| Intercom | `input_toggle` | Toggle Signal (`Intercom.c:117-119`) | **Input toggle** (matches `input_`) |
| MemoryCell | `input_0..3` | Power / Toggle / Reset / Set (`MemoryCell.c:59-75`) | Input 0..3 |
| MemoryCell | `output_0/1` | Output / Inverted | Output 0 / Output 1 |
| Monitor | `output_1..4` | Camera 1..4 (`Monitor.c:55-62`) | Output 1..4 |
| Counter / gates | `input_0`, `output_0` | role labels | generic Input/Output N |

Wiring actions already use `LFPG_DeviceAPI.GetPortLabel` (`LFPG_Actions.c:521`, `926`, `1395`). Inspector should resolve label by port name on the local entity (or send `m_Label` in RPC for local+remote).

---

#### F-INSP-009 — P2 · Remote port never shown; peer identity incomplete
**Received:** `RPCClientHandler.c:457-458`  
**Displayed:** only `FormatDeviceName(entry.m_RemoteTypeName)` at **`DeviceInspector.c:1446`**

Two peers of same type are indistinguishable. `m_RemotePort` wasted. Not wrong text, but loses the only way to disambiguate multi-port peers without using labels.

---

### UI layout / offsets

#### F-INSP-010 — P1 · Wire section Y ignores LinkLine + BatteryLine (regresses F3)
**`LFPG_DeviceInspector.c:1385-1398`** vs correct offsets in **`1278-1288`** and height **`1506` / `1515`**

`PopulateClientData` positions separator/header with:

`tank + fuel + reserve + link + battery`

Then, when server data arrives, `PopulateWireData` **overwrites** separator / header / wire slots using only:

`tank + fuel + reserve`

For **Sorter** (LinkLine +20) or **Battery** (BatteryLine +26), wire rows draw over the extra lines while panel height was already fixed in v4.3 F3.

Also position clamp fallback **`1567`** omits `m_LinkLineOffset`.

---

#### F-INSP-011 — P2 · Battery line width 360 > panel width 300
**`DeviceInspector.c:393`** (`SetSize(360, 18)`) vs `LFPG_INSPECT_PANEL_W = 300` (`Defines.c:658`)

Long charge strings clip outside panel / overlap chrome.

---

#### F-INSP-012 — P3 · Layout file is geometry-empty; all positions code-driven
**`gui/layouts/LFPG_DeviceInspector.layout:1-209`**

Every child uses `hexactpos/vexactpos/hexactsize/vexactsize 1` with no real coords; `CreateWidgets` **`277-424`** forces geometry. Fragile for designers; consistent with comment at **277**, but diverges from Sorter layout which is fully authored + `UIScaler`.

---

### UI tick cost

#### F-INSP-013 — P1 · Per-frame work even when panel hidden-by-conditions is cheap, but active path is heavy
**Tick:** `DeviceInspector.c:466-589` from **every** `MissionGameplay` frame (`MissionInit.c:324`) when not in CCTV.

When reel held + target device:

| Work | Frequency | Cost notes |
|------|-----------|------------|
| `IsHoldingCableReel` | every frame | hands inventory |
| `GetCursorTargetDeviceWithProximity` | every frame | shared ray cache; on miss: `GetObjectsAtPosition3D` sphere (`Actions.c:339`) every `PROXIMITY_FALLBACK_TTL_S` (0.1s) |
| `UpdatePanelPosition` | every frame | `GetScreenPos` + **unconditional** `m_Panel.SetPos` (**1631**, no dirty check) |
| `ClientDataChanged` + many `Cast`s | 2 Hz (`LFPG_INSPECT_REFRESH_MS=500`) | MemoryCell, T2 pump, Furnace, Sorter, Battery |
| `RequestServerData` | called every frame while `!m_HasServerData` (**566-568**) | gated by 1000ms cooldown, still call overhead |

Mitigations already present: dirty text/color/show/size maps (**705-758**), client snapshot skip (**610-702**), pos lerp (**1614-1628**). Gaps: no dirty for panel pos; proximity sphere under reel scan; no early-out if reel held but player in inventory UI / map.

---

#### F-INSP-014 — P2 · Retry loop hammering `RequestServerData` signature
**`DeviceInspector.c:563-568`**, **`1685-1714`**

While response pending, every Tick enters Request (early return on cooldown). Fine under normal RTT; under packet loss stays at 1 Hz forever with no timeout UI beyond “Connections loading…”. No max retries / backoff / “server unreachable” state.

---

### Nulls / lifecycle / robustness

#### F-INSP-015 — P1 · `PopulateWireData` does not null-check wire entries
**`DeviceInspector.c:1423-1428`**

```
LFPG_InspectWireEntry entry = m_RespWires[si];
// uses entry.m_Direction without if (!entry)
```

Partial RPC read (`RPCClientHandler.c:455-461` `break` on fail) can leave incomplete array; a null slot would NRE.

`OnInspectResponse` also assumes `wires` non-null (**1336** `wires.Count()`).

---

#### F-INSP-016 — P2 · `HidePanel` clears device id but not dirty caches / offsets / snapshot
**`DeviceInspector.c:1647-1660`**

Clears: visibility, `m_CurrentDeviceId`, wires, `m_HasServerData`.  
Keeps: `m_LastWidgetText/Color/Visible/Pos/Size`, line offsets, `m_ClientSnapshotValid`, `m_Smooth*`.

Next device usually forces full populate via `deviceId != m_CurrentDeviceId` (**523-536**), so mostly OK. Edge: behind-camera hide (**580-587**) intentionally keeps state (good), but ForceHide → HidePanel **clears** id — next look at same device re-RPCs (extra traffic, not a bug).

---

#### F-INSP-017 — P2 · `m_SnapshotEntity` is bare `EntityAI` (weak) compared by pointer
**`DeviceInspector.c:169`, `673-674`, `685`, `701`**

Sorter link change detection uses entity pointer equality. If linked container despawns and a new entity reuses… (DayZ entities don't recycle pointers easily) mostly OK; if link cleared, pointer goes null and `changed` fires. Acceptable but brittle vs NetworkID compare.

---

#### F-INSP-018 — P2 · Open/close lifecycle is implicit show/hide, not explicit Open/Close
**Inspector:** `Init` / `Tick` / `HidePanel` / `ForceHide` / `Cleanup` (`DeviceInspector.c:189-218`, `466-589`, `1638-1660`; `MissionInit.c:76`, `321-328`, `358`)  
**Sorter:** pre-create + `Open`/`Close` + focus lock + input disable (`SorterView.c:1228-1328`, destructor **294-326**)

Inspector never touches `ChangeGameFocus` / cursor / `SetDisabled` (correct for overlay). Gaps vs Sorter patterns:

- no `IsOpen()` API
- no single `DoClose` that resets **all** widget state
- CCTV integration is ad-hoc ForceHide (good enough)
- no UIScaler (`SorterView` Capture/Apply)

Not wrong by design (overlay vs modal), but **inconsistent lifecycle discipline** for future panels.

---

#### F-INSP-019 — P3 · Dabs MVC not used
**Inspector:** manual `CreateWidgets` + `FindAnyWidget` + static singleton (`DeviceInspector.c:248-275`).  
**Sorter:** `extends ScriptView`, `GetControllerType → LFPG_SorterController` (`SorterView.c:55-56`, `217-225`).

No Controller, no WidgetEventHandler, no data binding. Acceptable for read-only HUD; cost is duplicated color palette, no shared UIScaler, harder to test.

---

### Multiplayer correctness (non-security)

#### F-INSP-020 — P2 · Stale-response guard is client-id only
**`DeviceInspector.c:1319-1328`**, server echoes **client** id (`RPCServerHandler.c:2088-2092`)

If client SyncVar deviceId races but NetworkID resolved to different server id, response still applies under client id. Wire list is for **server** graph node (`serverDeviceId`) while correlation is client id — intentional for race heal, but if clientId wrong and netId right, client shows server topology under wrong “current” label until next switch. NetId-primary correlation would be safer.

---

#### F-INSP-021 — P2 · Topology generation invalidates client data but wires re-request depends on cooldown
**`DeviceInspector.c:540-568`**

On `LFPG_GetWireGeneration()` change: clears `m_HasServerData`, marks dirty, re-populates client SyncVars, then `RequestServerData` subject to **1000 ms** cooldown. Player can see “loading” / empty connections for up to 1s after a local wire gen bump. Acceptable; document or lower cooldown on topology change only.

---

#### F-INSP-022 — P3 · Total Load for passthrough uses allocated power, not demand
**`DeviceInspector.c:923-948`**

Sums `m_AllocatedPower` on OUT edges. Edge state **2** is computed server-side from `allocated < eps && demand > eps` (`RPCServerHandler.c:2041-2047`) but demand is **not** sent. Overloaded downstream may show `0 u/s` total load while status wires go orange — confusing, not corrupt.

---

### i18n / polish

#### F-INSP-023 — P3 · Hardcoded English on tank / fuel / battery / link lines
**`DeviceInspector.c:980-1005`, `1074-1121`, `1161-1169`, `1208-1261`**

Core status/type strings use `Loc("#STR_LFPG_INSPECT_*")`; specialty lines are English literals. Inconsistent with stringtable for rest of inspector.

---

#### F-INSP-024 — P3 · MemoryCell status uses raw `" — ON"` / `" — OFF"`
**`DeviceInspector.c:862-869`**

Not localized; also not using cell-specific port labels elsewhere (ties to F-INSP-008).

---

## Comparison matrix — Inspector vs Sorter UI

| Concern | DeviceInspector | Sorter (Dabs MVC) |
|---------|-----------------|-------------------|
| Framework | Raw widgets + singleton | `ScriptView` + Controller |
| Lifecycle | Tick show/hide | Explicit Open/Close + focus/input |
| Pre-create at mission init | Yes (`Init`) | Yes (`Init` hidden) |
| Server proximity | **Missing** | `LFPG_INTERACT_DIST_M` |
| Server type/ruined checks | **Missing** | Present |
| Rate limit | `AllowPlayerAction` | Same + client msg on throttle |
| Per-frame Update | Always when reel+target | Only when `m_IsOpen` |
| Resolution scaling | None | `LFPG_UIScaler` |
| Dirty widget updates | Yes (text/color/show/size) | Hover O(1) UserData |
| Sort / z-order | `SetSort(10001)` | `SetSort(50000)` |

---

## Positive notes (not findings)

- Client/server `#ifndef SERVER` boundary for UI class; `LFPG_InspectWireEntry` shared.
- Stale RPC drop by deviceId (**1319-1328**).
- NetworkID + clientDeviceId correlation pattern (same family as wiring).
- Dirty helpers cut SetText spam; snapshot avoids 2 Hz full rebuilds when SyncVars quiet.
- ForceHide on CCTV (`MissionInit.c:326-328`) avoids frozen panel.
- Behind-camera hide keeps state (**580-587**) — good UX.
- H2 retry when cooldown blocked first inspect (**563-568**).
- F3 panel height includes all dynamic offsets (**1502-1506**) — wire **position** still wrong (F-INSP-010).

---

## Recommended fix order (guidance only)

1. **F-INSP-001 + 002 + 004** — server: resolve entity required; distance ≤ `LFPG_INTERACT_DIST_M`; electric + not ruined; reject empty netId without entity.
2. **F-INSP-003 + 006** — strip `m_RemoteDeviceId` from payload (or hash); cap edges to UI max; optional omit remote id entirely.
3. **F-INSP-008** — resolve `m_Label` via port name / DeviceAPI instead of `FormatPortName` heuristics.
4. **F-INSP-010** — one helper `ExtraLineOffset()` used by PopulateClientData, PopulateWireData, and clamp fallback.
5. **F-INSP-013/014** — dirty `SetPos`; backoff on inspect retry; optional skip Tick when inventory open.

---

## Verification performed

| What | How |
|------|-----|
| Full `LFPG_DeviceInspector.c` | Read 1–1882 |
| Layout | Full read |
| Server/client inspect RPC | Read HandleInspectDevice / HandleInspectResponse |
| Sorter proximity pattern | Read HandleSorterConfigRequest 2136–2188 |
| Port labels vs FormatPortName | Grep AddPort + MemoryCell/Intercom/Monitor |
| Raycast distance | Read GetCursorTargetDevice* in LFPG_Actions.c |
| Mission lifecycle | Read MissionInit Init/Tick/Cleanup |

**Not verified:** live MP repro of remote inspect; frame-time profiling; stringtable completeness for missing keys.

---

## Index

| ID | Sev | Topic |
|----|-----|--------|
| F-INSP-001 | P0 | No server distance |
| F-INSP-002 | P0 | Graph query by bare deviceId |
| F-INSP-003 | P0 | Topology / ID / power leak |
| F-INSP-004 | P1 | No type/ruined/reel server checks |
| F-INSP-005 | P1 | Registry Register side effect |
| F-INSP-006 | P2 | Unbounded wire payload |
| F-INSP-007 | P2 | Soft client distance only |
| F-INSP-008 | P1 | Wrong port labels |
| F-INSP-009 | P2 | Remote port unused in UI |
| F-INSP-010 | P1 | Wire Y missing link/battery offsets |
| F-INSP-011 | P2 | Battery line wider than panel |
| F-INSP-012 | P3 | Empty layout geometry |
| F-INSP-013 | P1 | Per-frame tick cost |
| F-INSP-014 | P2 | Request retry hammer |
| F-INSP-015 | P1 | Null entry / null wires |
| F-INSP-016 | P2 | HidePanel partial state clear |
| F-INSP-017 | P2 | Snapshot entity pointer |
| F-INSP-018 | P2 | Implicit lifecycle vs Sorter Open/Close |
| F-INSP-019 | P3 | No Dabs MVC |
| F-INSP-020 | P2 | Correlation by client id only |
| F-INSP-021 | P2 | Topology change vs RPC cooldown |
| F-INSP-022 | P3 | Total Load = allocated not demand |
| F-INSP-023 | P3 | Hardcoded English specialty lines |
| F-INSP-024 | P3 | Hardcoded MemoryCell ON/OFF tags |

**P0 count:** 3 · **P1 count:** 6 · **P2 count:** 11 · **P3 count:** 4
