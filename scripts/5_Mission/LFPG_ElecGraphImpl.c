// =========================================================
// LF_PowerGrid - Electrical Graph (v1.0)
//
// In-memory directed graph of the electrical network.
// Nodes = devices, edges = wires. Rebuilt from wire data at
// startup, maintained incrementally during runtime.
//
// NOT persisted — wires are the source of truth.
// Server-only: all public methods are guarded by #ifdef SERVER.
//
// === POWER ALLOCATION (v1.0) ===
// Binary all-off policy: if totalDemand > availableOutput on any
// distributor node, ALL downstream of that node receives 0.
// AllocateOutput (~90 lines) is the sole allocation function.
// Overload state is per-node (m_Overloaded bool), not per-edge.
// PASSTHROUGH always reports real demand (self + downstream) via
// m_LastStableOutput, even when unpowered — prevents oscillation.
//
// === KEY SUBSYSTEMS ===
// - ProcessDirtyQueue: BFS propagation with node+edge budgets,
//   requeue limits, and deferred requeue for deep chains.
// - AllocateOutput: binary demand/allocation on outgoing edges.
//   Multi-source split via stable potential-supply capacity.
// - SyncNodeToEntity: syncs LoadRatio+Overloaded (SOURCE),
//   Powered+Overloaded (PASSTHROUGH), Powered (CONSUMER/CAMERA).
// - ValidateConsumerStates: bidirectional zombie/dark detection.
// - PostBulkRebuild: type-aware orphan SyncVar reset.
//
// === ANTI-OSCILLATION ===
// PASSTHROUGH demand signal = downstreamDemand + selfConsumption,
// always written to m_LastStableOutput regardless of power state.
// Demand is a topology property, not a power-flow property.
// Cold-start fallback: m_MaxOutput when m_LastStableOutput=0.
//
// === SAFETY NETS ===
// - Component Watchdog: per-subnet node limit (v0.7.31)
// - Atomic Graph Mutations: deferred cleanup (v0.7.34)
// - Topology-aware downstream propagation (v0.7.38 B1)
// - Carryover requeue count reset (v0.7.38 RC-09)
// - Deferred requeue for requeue-limit orphans (v0.8.3)
// =========================================================

class LFPG_ElecGraphImpl : LFPG_ElecGraph
{
    // --- Nodes ---
    protected ref map<string, ref LFPG_ElecNode> m_Nodes;

    // --- Dual adjacency ---
    protected ref map<string, ref array<ref LFPG_ElecEdge>> m_Outgoing;
    protected ref map<string, ref array<ref LFPG_ElecEdge>> m_Incoming;

    // --- Connected components ---
    protected int m_NextComponentId;
    protected bool m_ComponentsDirty;

    // --- Propagation (Sprint 4.2 active) ---
    protected ref array<string> m_DirtyQueue;
    protected int m_DirtyQueueHead;      // H4: head index for O(1) dequeue without array copy
    protected int m_CurrentEpoch;
    // T5 W1-F08: epoch stamp per node for lazy requeue-count reset.
    // Avoids walking the full carryover queue at the start of every epoch.
    protected ref map<string, int> m_RequeueEpoch;

    // --- Telemetry ---
    protected int m_NodeCount;
    protected int m_EdgeCount;
    protected int m_LastRebuildMs;
    protected int m_LastProcessMs;        // Sprint 4.2 S2: time spent in ProcessDirtyQueue

    // --- Sprint 4.3: Edge budget tracking ---
    protected int m_EdgesVisitedThisEpoch;
    // T5 R21-T5-001: port-aware devices synchronously query graph edges
    // from SyncNodeToEntity. Charge those visits only while PDQ owns the
    // budget; external gameplay/UI queries must not mutate scheduler state.
    protected bool m_PropagationEdgeAccountingActive;

    // v0.7.46: Flag set by AllocateOutput when any edge's
    // m_AllocatedPower changed. ProcessDirtyQueue Step 3 uses this to
    // re-enqueue downstream even when total output is unchanged.
    protected bool m_AllocChanged;

    // v2.0: Soft demand total from last AllocateOutput call.
    // Set by AllocateOutput, read by PDQ demand signal section.
    // Avoids redundant outgoing edge iteration in demand signal.
    // Reset to 0.0 per-node in PDQ (alongside m_AllocChanged).
    protected float m_LastAllocSoftDemand;

    // Stable potential-supply snapshot for multi-source demand sharing.
    // Values are derived only from topology, source state, gate command,
    // passthrough limits, self-consumption, and virtual generation. Current
    // input/allocation state is deliberately excluded to prevent feedback.
    // The cache is cleared once per ProcessDirtyQueue epoch so every supplier
    // evaluated in a batch sees the same upstream-capability snapshot.
    protected ref map<string, float> m_PotentialSupplyCache;
    protected bool m_PotentialSupplySnapshotReady;
    // Active root sources (and virtual generators) reachable at each node.
    // These prevent a split-then-recombined path from multiplying the same
    // generator's contribution merely because it reaches a target twice.
    protected ref map<string, ref array<string>> m_PotentialContributorCache;
    protected ref map<string, float> m_PotentialContributorCapacity;
    // A failed topological snapshot requests a completed-adjacency audit on the
    // next safe scheduler boundary. This heals cycles introduced by
    // legacy/import code without mutating adjacency while solving.
    protected bool m_RuntimeCycleAuditRequested;

    // --- v0.7.31 (Bloque B): Component Watchdog ---
    // m_ComponentSizes: populated in RebuildComponents(), keyed by componentId.
    // m_WdgQueue/m_WdgVisited: reusable BFS buffers for CountComponentLimited().
    // Max 256 entries → Clear() is negligible cost.
    protected ref map<int, int>     m_ComponentSizes;
    protected ref array<string>     m_WdgQueue;
    protected ref map<string, bool> m_WdgVisited;

    // --- v0.7.32 (Bloque C): Consumer Zombie Validation ---
    // Periodic sweep of consumer nodes to detect and fix "zombie" powered state.
    // m_ValidateTickCount: counts ProcessDirtyQueue calls (advances even when idle).
    // m_LastValidateTick: tick count when last validation batch ran.
    // m_ValidateNodeIdx: round-robin index into m_Nodes for budgeted batching.
    // m_ValidateFixCount: telemetry — total zombies fixed since startup.
    protected int m_ValidateTickCount;
    protected int m_LastValidateTick;
    protected int m_ValidateNodeIdx;
    protected int m_ValidateFixCount;

    // v5.0: BatteryCharger delta-time charging — tracks last charge timestamp
    // per node so charge rate is consistent regardless of visit frequency.
    protected ref map<string, float> m_ChargerLastChargeSec;

    // --- v0.7.34 (Bloque E): Atomic Graph Mutations ---
    // When m_MutationActive is true, CleanupOrphanNode is deferred to
    // EndGraphMutation. Prevents premature node deletion during multi-op
    // sequences (e.g. replace wire = remove old + add new).
    // m_MutationDepth supports safe nesting (Begin can be called N times,
    // only the outermost End triggers cleanup).
    // m_DeferredOrphanCleanup: reusable buffer, cleared on End.
    protected bool m_MutationActive;
    protected int  m_MutationDepth;
    protected ref array<string> m_DeferredOrphanCleanup;

    // --- v0.8.3: Deferred requeue for requeue-limit orphans ---
    // When a node hits LFPG_MAX_REQUEUE_PER_EPOCH and is skipped, its dirty
    // state is preserved and the nodeId is added here. At the end of the epoch,
    // these nodes are re-inserted into the dirty queue for next-epoch processing
    // with reset requeue counts. Prevents permanent orphaning when downstream
    // converges while the node is limit-skipped.
    protected ref array<string> m_DeferredRequeue;

    // True only while a complete graph solution is being calculated and
    // committed. Entity callbacks caused by applying that solution must not
    // feed the just-committed state back into the scheduler as a new event.
    protected bool m_DeterministicSolveActive;

    // --- v0.7.43 (Fix 3): NetworkID backup for entity re-resolution ---
    // When DeviceRegistry ref goes stale (entity streamed/recreated),
    // SyncNodeToEntity can re-resolve via GetObjectByNetworkId.
    // Populated in EnsureNode when entity is available. Session-local
    // (NetworkIDs change on restart, graph is rebuilt anyway).
    protected ref map<string, int> m_NodeNetLow;
    protected ref map<string, int> m_NodeNetHigh;

    void LFPG_ElecGraphImpl()
    {
        m_Nodes = new map<string, ref LFPG_ElecNode>;
        m_Outgoing = new map<string, ref array<ref LFPG_ElecEdge>>;
        m_Incoming = new map<string, ref array<ref LFPG_ElecEdge>>;
        m_DirtyQueue = new array<string>;
        m_DirtyQueueHead = 0;
        m_NextComponentId = 0;
        m_ComponentsDirty = true;
        m_CurrentEpoch = 0;
        m_RequeueEpoch = new map<string, int>;
        m_NodeCount = 0;
        m_EdgeCount = 0;
        m_LastRebuildMs = 0;
        m_LastProcessMs = 0;
        m_EdgesVisitedThisEpoch = 0;
        m_PropagationEdgeAccountingActive = false;
        m_AllocChanged = false;
        m_LastAllocSoftDemand = 0.0;
        m_PotentialSupplyCache = new map<string, float>;
        m_PotentialSupplySnapshotReady = false;
        m_PotentialContributorCache = new map<string, ref array<string>>;
        m_PotentialContributorCapacity = new map<string, float>;
        m_RuntimeCycleAuditRequested = false;

        // v0.7.31 (Bloque B): Component Watchdog buffers
        m_ComponentSizes = new map<int, int>;
        m_WdgQueue = new array<string>;
        m_WdgVisited = new map<string, bool>;

        // v0.7.32 (Bloque C): Consumer Zombie Validation
        m_ValidateTickCount = 0;
        m_LastValidateTick = 0;
        m_ValidateNodeIdx = 0;
        m_ValidateFixCount = 0;
        m_ChargerLastChargeSec = new map<string, float>;

        // v0.7.34 (Bloque E): Atomic Graph Mutations
        m_MutationActive = false;
        m_MutationDepth = 0;
        m_DeferredOrphanCleanup = new array<string>;

        // v0.8.3: Deferred requeue
        m_DeferredRequeue = new array<string>;
        m_DeterministicSolveActive = false;

        // v0.7.43 (Fix 3): NetworkID backup maps
        m_NodeNetLow = new map<string, int>;
        m_NodeNetHigh = new map<string, int>;
    }

    // ===========================
    // Full rebuild from wires
    // ===========================

    // Reconstructs the entire graph from existing wire data.
    // Called once at server startup after all loads complete.
    // Does NOT modify the wire data — read only. Invalid persisted cycle-closing
    // edges are omitted from the runtime graph so propagation remains a DAG.
    override void RebuildFromWires(LFPG_NetworkManager mgr)
    {
        #ifdef SERVER
        if (!mgr)
            return;

        int startMs = g_Game.GetTime();

        // Clear everything
        m_Nodes.Clear();
        m_Outgoing.Clear();
        m_Incoming.Clear();
        m_DirtyQueue.Clear();
        m_DirtyQueueHead = 0;
        m_NodeCount = 0;
        m_EdgeCount = 0;
        m_NodeNetLow.Clear();
        m_NodeNetHigh.Clear();
        m_RequeueEpoch.Clear();
        m_PotentialSupplyCache.Clear();
        m_PotentialSupplySnapshotReady = false;
        m_PotentialContributorCache.Clear();
        m_PotentialContributorCapacity.Clear();
        m_RuntimeCycleAuditRequested = false;

        // v0.7.34 (Bloque E): Full rebuild invalidates any active mutation
        if (m_MutationActive)
        {
            m_MutationActive = false;
            m_MutationDepth = 0;
            m_DeferredOrphanCleanup.Clear();
        }
        m_DeferredRequeue.Clear();

        // Step 1: Iterate all registered devices to create nodes
        ref array<EntityAI> allDevices = new array<EntityAI>;
        LFPG_DeviceRegistry.Get().GetAll(allDevices);

        int di;
        for (di = 0; di < allDevices.Count(); di = di + 1)
        {
            EntityAI devObj = allDevices[di];
            if (!devObj)
                continue;

            string devId = LFPG_DeviceAPI.GetOrCreateDeviceId(devObj);
            if (devId == "")
                continue;

            EnsureNode(devId, devObj);
        }

        // Step 2: Iterate wire sources — LFPG devices with wire stores
        for (di = 0; di < allDevices.Count(); di = di + 1)
        {
            EntityAI srcObj = allDevices[di];
            if (!srcObj)
                continue;

            if (!LFPG_DeviceAPI.HasWireStore(srcObj))
                continue;

            string srcId = LFPG_DeviceAPI.GetOrCreateDeviceId(srcObj);
            if (srcId == "")
                continue;

            ref array<ref LFPG_WireData> wires = LFPG_DeviceAPI.GetDeviceWires(srcObj);
            if (!wires)
                continue;

            int wi;
            for (wi = 0; wi < wires.Count(); wi = wi + 1)
            {
                LFPG_WireData wd = wires[wi];
                if (!wd)
                    continue;

                // Runtime wiring rejects directed cycles before persistence, but
                // old saves and legacy/import paths can contain them. Feeding a
                // cycle into the demand/allocation solver has no fixed topological
                // order: overload walks around the loop and clients see the same
                // branch alternate CRITICAL (orange) and POWERED (green).
                //
                // Rebuild is intentionally read-only, so omit only the edge that
                // closes the cycle. The remaining edges form a deterministic DAG
                // and keep the maximum useful portion of the persisted network.
                if (DetectCycleIfAdded(srcId, wd.m_TargetDeviceId))
                {
                    string lfCycleMsg = "[ElecGraph] Rebuild omitted cycle-closing LFPG edge ";
                    lfCycleMsg = lfCycleMsg + srcId + " -> " + wd.m_TargetDeviceId;
                    lfCycleMsg = lfCycleMsg + " port=" + wd.m_SourcePort + "->" + wd.m_TargetPort;
                    LFPG_Util.Warn(lfCycleMsg);
                    continue;
                }

                AddEdgeInternal(srcId, wd.m_TargetDeviceId, wd.m_SourcePort, wd.m_TargetPort, wd);
            }
        }

        // Step 3: Iterate vanilla wires
        int vCount = mgr.GetVanillaWireOwnerCount();
        int vi;
        for (vi = 0; vi < vCount; vi = vi + 1)
        {
            string vOwnerId = mgr.GetVanillaWireOwnerKey(vi);
            ref array<ref LFPG_WireData> vWires = mgr.GetVanillaWires(vOwnerId);
            if (!vWires)
                continue;

            int vwi;
            for (vwi = 0; vwi < vWires.Count(); vwi = vwi + 1)
            {
                LFPG_WireData vwd = vWires[vwi];
                if (!vwd)
                    continue;

                string srcPort = vwd.m_SourcePort;
                if (srcPort == "")
                    srcPort = LFPG_PORT_OUTPUT_1;

                if (DetectCycleIfAdded(vOwnerId, vwd.m_TargetDeviceId))
                {
                    string vanCycleMsg = "[ElecGraph] Rebuild omitted cycle-closing vanilla edge ";
                    vanCycleMsg = vanCycleMsg + vOwnerId + " -> " + vwd.m_TargetDeviceId;
                    vanCycleMsg = vanCycleMsg + " port=" + srcPort + "->" + vwd.m_TargetPort;
                    LFPG_Util.Warn(vanCycleMsg);
                    continue;
                }

                AddEdgeInternal(vOwnerId, vwd.m_TargetDeviceId, srcPort, vwd.m_TargetPort, vwd);
            }
        }

        // m_Outgoing owns the canonical runtime edge objects. Rebuild the
        // secondary upstream index from those same objects before any solver
        // or audit consumes it, so both traversal directions see one graph.
        CanonicalizeIncomingAdjacency();

        // The incremental check above should make this graph a DAG.  Audit the
        // completed adjacency anyway: identity aliases, imported/legacy stores,
        // or an asymmetric mutation can make an edge-by-edge assumption differ
        // from the graph the capacity solver will actually traverse.  Legal
        // split/recombine diamonds remain untouched because they have no path
        // from an edge target back to its source.
        int auditedCycleEdges = SanitizeCompletedGraphCycles();
        if (auditedCycleEdges > 0)
        {
            string auditSummary = "[ElecGraph] Rebuild runtime DAG audit omitted ";
            auditSummary = auditSummary + auditedCycleEdges.ToString();
            auditSummary = auditSummary + " cycle-closing edge(s)";
            LFPG_Util.Warn(auditSummary);
        }

        // Step 4: Prune nodes with no edges
        ref array<string> emptyNodes = new array<string>;
        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            string nid = m_Nodes.GetKey(ni);
            bool hasOut = false;
            bool hasIn = false;

            ref array<ref LFPG_ElecEdge> outEdges;
            if (m_Outgoing.Find(nid, outEdges) && outEdges && outEdges.Count() > 0)
                hasOut = true;

            ref array<ref LFPG_ElecEdge> inEdges;
            if (m_Incoming.Find(nid, inEdges) && inEdges && inEdges.Count() > 0)
                hasIn = true;

            if (!hasOut && !hasIn)
                emptyNodes.Insert(nid);
        }

        int ei;
        for (ei = 0; ei < emptyNodes.Count(); ei = ei + 1)
        {
            m_Nodes.Remove(emptyNodes[ei]);
            m_Outgoing.Remove(emptyNodes[ei]);
            m_Incoming.Remove(emptyNodes[ei]);
            // v0.7.45 (H5): Consistent with OnDeviceRemoved and CleanupOrphanNode.
            m_NodeNetLow.Remove(emptyNodes[ei]);
            m_NodeNetHigh.Remove(emptyNodes[ei]);
            // v5.1: Clean up charger delta-time timestamp for removed node
            m_ChargerLastChargeSec.Remove(emptyNodes[ei]);
        }
        m_NodeCount = m_Nodes.Count();

        // Step 5: Rebuild component IDs
        m_ComponentsDirty = true;
        RebuildComponents();

        int elapsed = g_Game.GetTime() - startMs;
        m_LastRebuildMs = elapsed;

        string rbMsg = "[ElecGraph] Rebuilt: " + m_NodeCount.ToString() + " nodes, ";
        rbMsg = rbMsg + m_EdgeCount.ToString() + " edges, ";
        rbMsg = rbMsg + m_ComponentSizes.Count().ToString() + " components in ";
        rbMsg = rbMsg + elapsed.ToString() + "ms";
        LFPG_Util.Info(rbMsg);
        #endif
    }

    // ===========================
    // Incremental operations
    // ===========================

    // v0.7.31 (Bloque B): BFS acotada para watchdog por componente.
    // Counts nodes in the connected component containing startId.
    // Early exits when count exceeds limit (returns limit+1).
    // Uses reusable buffers m_WdgQueue/m_WdgVisited — zero alloc per call.
    // Undirected traversal: walks both m_Outgoing and m_Incoming.
    protected int CountComponentLimited(string startId, int limit)
    {
        #ifdef SERVER
        if (startId == "" || limit <= 0)
            return 0;

        m_WdgQueue.Clear();
        m_WdgVisited.Clear();

        bool bTrue = true;
        m_WdgQueue.Insert(startId);
        m_WdgVisited.Set(startId, bTrue);

        int count = 0;
        int headIdx = 0;

        while (headIdx < m_WdgQueue.Count())
        {
            string currId = m_WdgQueue[headIdx];
            headIdx = headIdx + 1;
            count = count + 1;

            // Early exit: component exceeds limit
            if (count > limit)
                return count;

            // Explore outgoing neighbors
            ref array<ref LFPG_ElecEdge> outEdges;
            if (m_Outgoing.Find(currId, outEdges) && outEdges)
            {
                int oi;
                for (oi = 0; oi < outEdges.Count(); oi = oi + 1)
                {
                    ref LFPG_ElecEdge oEdge = outEdges[oi];
                    if (oEdge && oEdge.m_TargetNodeId != "")
                    {
                        bool oVisited = false;
                        m_WdgVisited.Find(oEdge.m_TargetNodeId, oVisited);
                        if (!oVisited)
                        {
                            m_WdgVisited.Set(oEdge.m_TargetNodeId, bTrue);
                            m_WdgQueue.Insert(oEdge.m_TargetNodeId);
                        }
                    }
                }
            }

            // Explore incoming neighbors (undirected traversal)
            ref array<ref LFPG_ElecEdge> inEdges;
            if (m_Incoming.Find(currId, inEdges) && inEdges)
            {
                int ii;
                for (ii = 0; ii < inEdges.Count(); ii = ii + 1)
                {
                    ref LFPG_ElecEdge iEdge = inEdges[ii];
                    if (iEdge && iEdge.m_SourceNodeId != "")
                    {
                        bool iVisited = false;
                        m_WdgVisited.Find(iEdge.m_SourceNodeId, iVisited);
                        if (!iVisited)
                        {
                            m_WdgVisited.Set(iEdge.m_SourceNodeId, bTrue);
                            m_WdgQueue.Insert(iEdge.m_SourceNodeId);
                        }
                    }
                }
            }
        }

        return count;
        #else
        return 0;
        #endif
    }

    override bool OnWireAdded(string sourceId, string targetId, string sourcePort, string targetPort, LFPG_WireData wireRef)
    {
        #ifdef SERVER
        // ==========================================
        // PASO 0: Null guards + self-loop
        // ==========================================
        if (sourceId == "" || targetId == "")
            return false;

        if (sourceId == targetId)
            return false;

        // Keep the graph invariant at the mutation boundary. RPC wiring already
        // performs this pre-check for a friendly player-facing error, but other
        // callers (addons, repair paths, future imports) can invoke OnWireAdded
        // directly. A directed cycle makes downstream demand feed back into its
        // own upstream allocation and produces persistent orange/green flicker.
        if (DetectCycleIfAdded(sourceId, targetId))
        {
            string cycleMsg = "[ElecGraph] OnWireAdded REJECTED: directed cycle ";
            cycleMsg = cycleMsg + sourceId + " -> " + targetId;
            LFPG_Util.Warn(cycleMsg);
            return false;
        }

        // ==========================================
        // PASO 2: Component Watchdog (v0.7.31)
        // ==========================================
        ref LFPG_ElecNode nodeA;
        ref LFPG_ElecNode nodeB;
        bool hasA = m_Nodes.Find(sourceId, nodeA);
        bool hasB = m_Nodes.Find(targetId, nodeB);

        // Global cap applies to nodes introduced by this edge, not to every
        // edge added after the graph happens to reach the cap. Replacements
        // between existing nodes must remain possible at full capacity.
        int newNodeCount = 0;
        if (!hasA || !nodeA)
            newNodeCount = newNodeCount + 1;
        if (!hasB || !nodeB)
            newNodeCount = newNodeCount + 1;
        if (m_NodeCount + newNodeCount > LFPG_MAX_NODES_GLOBAL)
        {
            string capMsg = "[ElecGraph] OnWireAdded REJECTED: global cap (" + m_NodeCount.ToString() + "+" + newNodeCount.ToString() + "/" + LFPG_MAX_NODES_GLOBAL.ToString() + ")";
            LFPG_Util.Warn(capMsg);
            return false;
        }

        // Fast-paths: only when components are clean (already rebuilt)
        int limit = LFPG_MAX_NODES_PER_COMPONENT;
        int sizeA = 0;
        int sizeB = 0;
        int remaining = 0;
        int totalSize = 0;
        if (!m_ComponentsDirty)
        {
            int compA = -1;
            int compB = -1;
            if (hasA && nodeA)
                compA = nodeA.m_ComponentId;
            if (hasB && nodeB)
                compB = nodeB.m_ComponentId;

            // 2a: Same component — internal cable, size unchanged → ALLOW
            if (compA >= 0 && compA == compB)
            {
                // Skip watchdog, proceed directly to insert
            }
            // 2b: Different known components — O(1) size lookup
            else if (compA >= 0 && compB >= 0)
            {
                sizeA = 0;
                sizeB = 0;
                m_ComponentSizes.Find(compA, sizeA);
                m_ComponentSizes.Find(compB, sizeB);

                int mergedSize = sizeA + sizeB;
                if (mergedSize > LFPG_MAX_NODES_PER_COMPONENT)
                {
                    string mergeMsg = "[ElecGraph] OnWireAdded REJECTED: merge exceeds component limit (" + mergedSize.ToString() + "/" + LFPG_MAX_NODES_PER_COMPONENT.ToString() + ")";
                    LFPG_Util.Warn(mergeMsg);
                    return false;
                }
                // Merged size OK, proceed to insert
            }
            // 2c: One or both nodes are new (compId == -1) — BFS fallback
            else
            {
                limit = LFPG_MAX_NODES_PER_COMPONENT;
                int bfsSizeA = 1;
                int bfsSizeB = 1;

                if (hasA && nodeA)
                {
                    if (compA >= 0)
                    {
                        m_ComponentSizes.Find(compA, bfsSizeA);
                    }
                    else
                    {
                        bfsSizeA = CountComponentLimited(sourceId, limit);
                    }
                }

                if (bfsSizeA > limit)
                {
                    string wMsg = "[ElecGraph] OnWireAdded REJECTED: source component exceeds limit";
                    LFPG_Util.Warn(wMsg);
                    return false;
                }

                remaining = limit - bfsSizeA;
                if (remaining <= 0)
                {
                    string wMsg2 = "[ElecGraph] OnWireAdded REJECTED: no budget for target";
                    LFPG_Util.Warn(wMsg2);
                    return false;
                }

                if (hasB && nodeB)
                {
                    if (compB >= 0)
                    {
                        m_ComponentSizes.Find(compB, bfsSizeB);
                    }
                    else
                    {
                        bfsSizeB = CountComponentLimited(targetId, remaining);
                    }
                }

                totalSize = bfsSizeA + bfsSizeB;
                if (totalSize > limit)
                {
                    string szMsg = "[ElecGraph] OnWireAdded REJECTED: merged size (" + totalSize.ToString() + "/" + limit.ToString() + ")";
                    LFPG_Util.Warn(szMsg);
                    return false;
                }
            }
        }
        else
        {
            // ==========================================
            // PASO 3: Components dirty — BFS fallback
            // ==========================================
            limit = LFPG_MAX_NODES_PER_COMPONENT;
            sizeA = 1;
            bool ranBfsA = false;

            if (hasA && nodeA)
            {
                sizeA = CountComponentLimited(sourceId, limit);
                ranBfsA = true;
            }

            if (sizeA > limit)
            {
                string wMsgD = "[ElecGraph] OnWireAdded REJECTED: source exceeds limit (dirty)";
                LFPG_Util.Warn(wMsgD);
                return false;
            }

            // If we ran BFS for A, check if B was already visited (= same component).
            // This avoids double-counting that would cause false rejections.
            // Only safe to check m_WdgVisited when it was freshly populated by BFS-A.
            bool bInA = false;
            if (ranBfsA && hasB && nodeB)
            {
                m_WdgVisited.Find(targetId, bInA);
            }

            if (!bInA)
            {
                // Different components (or A was new) — count B with remaining budget
                remaining = limit - sizeA;
                if (remaining <= 0)
                {
                    string wMsgD2 = "[ElecGraph] OnWireAdded REJECTED: no budget for target (dirty)";
                    LFPG_Util.Warn(wMsgD2);
                    return false;
                }

                sizeB = 1;
                if (hasB && nodeB)
                {
                    sizeB = CountComponentLimited(targetId, remaining);
                }

                totalSize = sizeA + sizeB;
                if (totalSize > limit)
                {
                    string szMsgD = "[ElecGraph] OnWireAdded REJECTED: merged size (" + totalSize.ToString() + "/" + limit.ToString() + ") (dirty)";
                    LFPG_Util.Warn(szMsgD);
                    return false;
                }
            }
            // else: bInA — same component, size doesn't grow, sizeA <= limit already checked
        }

        // ==========================================
        // PASO 4: Insert edge (original logic preserved)
        // ==========================================
        EntityAI srcObj = LFPG_DeviceRegistry.Get().FindById(sourceId);
        EntityAI tgtObj = LFPG_DeviceRegistry.Get().FindById(targetId);

        EnsureNode(sourceId, srcObj);
        EnsureNode(targetId, tgtObj);

        bool inserted = AddEdgeInternal(sourceId, targetId, sourcePort, targetPort, wireRef);
        if (!inserted)
        {
            string wInsMsg = "[ElecGraph] OnWireAdded: edge not inserted " + sourceId + " -> " + targetId;
            LFPG_Util.Warn(wInsMsg);
            return false;
        }

        // [DIAG PT-CHAIN] Punto 1: Post-edge insert verification
        if (LFPG_DIAG_PT_CHAIN)
        {
            ref LFPG_ElecNode diagSrc;
            ref LFPG_ElecNode diagTgt;
            string dSrcType = "?";
            string dTgtType = "?";
            string dSrcOut = "0";
            string dSrcPow = "0";
            string dTgtOut = "0";
            string dTgtPow = "0";
            if (m_Nodes.Find(sourceId, diagSrc) && diagSrc)
            {
                dSrcType = diagSrc.m_DeviceType.ToString();
                dSrcOut = diagSrc.m_OutputPower.ToString();
                dSrcPow = diagSrc.m_Powered.ToString();
            }
            if (m_Nodes.Find(targetId, diagTgt) && diagTgt)
            {
                dTgtType = diagTgt.m_DeviceType.ToString();
                dTgtOut = diagTgt.m_OutputPower.ToString();
                dTgtPow = diagTgt.m_Powered.ToString();
            }
            string ptLog1 = "[PT-CHAIN] OnWireAdded OK: ";
            ptLog1 = ptLog1 + sourceId;
            ptLog1 = ptLog1 + "(type=" + dSrcType;
            ptLog1 = ptLog1 + " out=" + dSrcOut;
            ptLog1 = ptLog1 + " pow=" + dSrcPow + ")";
            ptLog1 = ptLog1 + " -> " + targetId;
            ptLog1 = ptLog1 + "(type=" + dTgtType;
            ptLog1 = ptLog1 + " out=" + dTgtOut;
            ptLog1 = ptLog1 + " pow=" + dTgtPow + ")";
            ptLog1 = ptLog1 + " port=" + sourcePort + "->" + targetPort;
            LFPG_Util.Info(ptLog1);
        }

        // T5 W1-F06: union only the two affected components. The traversal
        // below is filtered by the old component ID, so it cannot cross the
        // newly inserted edge into the component whose ID is retained.
        bool addComponentsUpdated = false;
        if (!m_ComponentsDirty)
        {
            ref LFPG_ElecNode addSourceNode;
            ref LFPG_ElecNode addTargetNode;
            if (m_Nodes.Find(sourceId, addSourceNode) && addSourceNode && m_Nodes.Find(targetId, addTargetNode) && addTargetNode)
            {
                int addSourceComponent = addSourceNode.m_ComponentId;
                int addTargetComponent = addTargetNode.m_ComponentId;
                int addSourceSize = 0;
                int addTargetSize = 0;
                bool addSizesValid = true;
                if (addSourceComponent >= 0 && !m_ComponentSizes.Find(addSourceComponent, addSourceSize))
                    addSizesValid = false;
                if (addTargetComponent >= 0 && !m_ComponentSizes.Find(addTargetComponent, addTargetSize))
                    addSizesValid = false;

                if (addSizesValid && addSourceComponent >= 0 && addSourceComponent == addTargetComponent)
                {
                    addComponentsUpdated = true;
                }
                else if (addSizesValid && addSourceComponent < 0 && addTargetComponent < 0)
                {
                    int addNewComponent = m_NextComponentId;
                    m_NextComponentId = m_NextComponentId + 1;
                    addSourceNode.m_ComponentId = addNewComponent;
                    addTargetNode.m_ComponentId = addNewComponent;
                    m_ComponentSizes.Set(addNewComponent, 2);
                    addComponentsUpdated = true;
                }
                else if (addSizesValid && addSourceComponent < 0)
                {
                    addSourceNode.m_ComponentId = addTargetComponent;
                    m_ComponentSizes.Set(addTargetComponent, addTargetSize + 1);
                    addComponentsUpdated = true;
                }
                else if (addSizesValid && addTargetComponent < 0)
                {
                    addTargetNode.m_ComponentId = addSourceComponent;
                    m_ComponentSizes.Set(addSourceComponent, addSourceSize + 1);
                    addComponentsUpdated = true;
                }
                else if (addSizesValid)
                {
                    int addFromComponent = addTargetComponent;
                    int addToComponent = addSourceComponent;
                    string addRelabelStart = targetId;
                    int addExpectedRelabel = addTargetSize;
                    if (addSourceSize < addTargetSize)
                    {
                        addFromComponent = addSourceComponent;
                        addToComponent = addTargetComponent;
                        addRelabelStart = sourceId;
                        addExpectedRelabel = addSourceSize;
                    }

                    m_WdgQueue.Clear();
                    m_WdgVisited.Clear();
                    m_WdgQueue.Insert(addRelabelStart);
                    int addRelabelHead = 0;
                    int addRelabeled = 0;
                    while (addRelabelHead < m_WdgQueue.Count())
                    {
                        string addCurrentId = m_WdgQueue[addRelabelHead];
                        addRelabelHead = addRelabelHead + 1;
                        bool addWasVisited = false;
                        m_WdgVisited.Find(addCurrentId, addWasVisited);
                        if (addWasVisited)
                            continue;
                        m_WdgVisited.Set(addCurrentId, true);

                        ref LFPG_ElecNode addCurrentNode;
                        if (!m_Nodes.Find(addCurrentId, addCurrentNode) || !addCurrentNode || addCurrentNode.m_ComponentId != addFromComponent)
                            continue;

                        addCurrentNode.m_ComponentId = addToComponent;
                        addRelabeled = addRelabeled + 1;

                        ref array<ref LFPG_ElecEdge> addOutgoing;
                        if (m_Outgoing.Find(addCurrentId, addOutgoing) && addOutgoing)
                        {
                            int addOutIndex;
                            for (addOutIndex = 0; addOutIndex < addOutgoing.Count(); addOutIndex = addOutIndex + 1)
                            {
                                LFPG_ElecEdge addOutEdge = addOutgoing[addOutIndex];
                                ref LFPG_ElecNode addOutNode;
                                if (addOutEdge && m_Nodes.Find(addOutEdge.m_TargetNodeId, addOutNode) && addOutNode && addOutNode.m_ComponentId == addFromComponent)
                                    m_WdgQueue.Insert(addOutEdge.m_TargetNodeId);
                            }
                        }

                        ref array<ref LFPG_ElecEdge> addIncoming;
                        if (m_Incoming.Find(addCurrentId, addIncoming) && addIncoming)
                        {
                            int addInIndex;
                            for (addInIndex = 0; addInIndex < addIncoming.Count(); addInIndex = addInIndex + 1)
                            {
                                LFPG_ElecEdge addInEdge = addIncoming[addInIndex];
                                ref LFPG_ElecNode addInNode;
                                if (addInEdge && m_Nodes.Find(addInEdge.m_SourceNodeId, addInNode) && addInNode && addInNode.m_ComponentId == addFromComponent)
                                    m_WdgQueue.Insert(addInEdge.m_SourceNodeId);
                            }
                        }
                    }

                    if (addRelabeled == addExpectedRelabel)
                    {
                        m_ComponentSizes.Remove(addFromComponent);
                        m_ComponentSizes.Set(addToComponent, addSourceSize + addTargetSize);
                        addComponentsUpdated = true;
                    }
                }
            }
        }
        if (!addComponentsUpdated)
            m_ComponentsDirty = true;

        MarkNodeDirty(sourceId, LFPG_DIRTY_TOPOLOGY);
        MarkNodeDirty(targetId, LFPG_DIRTY_TOPOLOGY);
        MarkIncomingSuppliersDirty(targetId);

        return true;
        #else
        return false;
        #endif
    }

    override void OnWireRemoved(string sourceId, string targetId, string sourcePort, string targetPort)
    {
        #ifdef SERVER
        bool componentsWereClean = !m_ComponentsDirty;
        int oldComponentId = -1;
        ref LFPG_ElecNode oldSourceNode;
        if (componentsWereClean && m_Nodes.Find(sourceId, oldSourceNode) && oldSourceNode)
            oldComponentId = oldSourceNode.m_ComponentId;

        RemoveEdgeInternal(sourceId, targetId, sourcePort, targetPort);

        // v0.7.34 (Bloque E): Defer orphan cleanup during atomic mutations.
        // In a replace sequence (remove old + add new), the target node
        // temporarily has no incoming edges after remove. Immediate cleanup
        // would delete it, losing m_Consumption/m_MaxOutput state. The new
        // OnWireAdded would recreate it via EnsureNode, but with default
        // values — causing a stale-state propagation bug.
        if (m_MutationActive)
        {
            m_DeferredOrphanCleanup.Insert(sourceId);
            m_DeferredOrphanCleanup.Insert(targetId);
        }
        else
        {
            // v0.7.37 (Audit 6, H3): Force target powered=false BEFORE orphan cleanup.
            // CleanupOrphanNode may remove the target from the graph if it has
            // no remaining edges. Once removed, MarkNodeDirty below is a no-op
            // and the entity's m_PoweredNet stays stale (true). Forcing false
            // here ensures it goes dark. Propagation re-enables it next tick
            // if an alternate power path exists.
            // Only needed outside mutations: during atomic ops, cleanup is
            // deferred so the node survives and MarkNodeDirty works normally.
            // Skipping here also avoids an unnecessary entity resolve + RPC
            // and prevents visible powered→unpowered→powered flicker during
            // replace wire sequences.
            EntityAI tgtObj = LFPG_DeviceRegistry.Get().FindById(targetId);
            if (!tgtObj)
            {
                tgtObj = LFPG_DeviceAPI.ResolveVanillaDevice(targetId);
            }
            if (tgtObj)
            {
                LFPG_DeviceAPI.SetPowered(tgtObj, false);
            }
            CleanupOrphanNode(sourceId);
            CleanupOrphanNode(targetId);
        }

        // T5 W1-F06: only the old component can split. Count the source
        // side, then relabel the target side only when it is no longer
        // reachable. Unrelated component IDs and size entries stay untouched.
        bool removeComponentsUpdated = false;
        if (componentsWereClean && oldComponentId >= 0)
        {
            m_ComponentSizes.Remove(oldComponentId);
            m_WdgQueue.Clear();
            m_WdgVisited.Clear();

            int removeSourceSize = 0;
            ref LFPG_ElecNode removeSourceNode;
            if (m_Nodes.Find(sourceId, removeSourceNode) && removeSourceNode && removeSourceNode.m_ComponentId == oldComponentId)
                removeSourceSize = CountComponentLimited(sourceId, m_NodeCount + 1);

            bool removeTargetInSource = false;
            m_WdgVisited.Find(targetId, removeTargetInSource);
            int removeTargetSize = 0;
            ref LFPG_ElecNode removeTargetNode;
            if (!removeTargetInSource && m_Nodes.Find(targetId, removeTargetNode) && removeTargetNode && removeTargetNode.m_ComponentId == oldComponentId)
            {
                removeTargetSize = CountComponentLimited(targetId, m_NodeCount + 1);
                if (removeSourceSize > 0)
                {
                    int removeSplitComponent = m_NextComponentId;
                    m_NextComponentId = m_NextComponentId + 1;
                    int removeVisitedIndex;
                    for (removeVisitedIndex = 0; removeVisitedIndex < m_WdgVisited.Count(); removeVisitedIndex = removeVisitedIndex + 1)
                    {
                        string removeVisitedId = m_WdgVisited.GetKey(removeVisitedIndex);
                        ref LFPG_ElecNode removeVisitedNode;
                        if (m_Nodes.Find(removeVisitedId, removeVisitedNode) && removeVisitedNode)
                            removeVisitedNode.m_ComponentId = removeSplitComponent;
                    }
                    if (removeTargetSize > 0)
                        m_ComponentSizes.Set(removeSplitComponent, removeTargetSize);
                }
            }

            if (removeSourceSize > 0)
                m_ComponentSizes.Set(oldComponentId, removeSourceSize);
            else if (removeTargetSize > 0)
                m_ComponentSizes.Set(oldComponentId, removeTargetSize);

            removeComponentsUpdated = true;
        }
        if (!removeComponentsUpdated)
            m_ComponentsDirty = true;

        MarkNodeDirty(sourceId, LFPG_DIRTY_TOPOLOGY);
        MarkNodeDirty(targetId, LFPG_DIRTY_TOPOLOGY);
        MarkIncomingSuppliersDirty(targetId);
        #endif
    }

    override void OnDeviceRemoved(string deviceId)
    {
        #ifdef SERVER
        if (deviceId == "")
            return;

        ref LFPG_ElecNode removedNode;
        ref array<ref LFPG_ElecEdge> outEdges;
        ref array<ref LFPG_ElecEdge> inEdges;
        bool hadNode = m_Nodes.Find(deviceId, removedNode);
        bool hadOutgoing = m_Outgoing.Find(deviceId, outEdges);
        bool hadIncoming = m_Incoming.Find(deviceId, inEdges);
        if (!hadNode && !hadOutgoing && !hadIncoming)
            return;

        ref array<string> affectedNeighbors = new array<string>;
        ref array<string> affectedSupplierTargets = new array<string>;

        if (hadOutgoing && outEdges)
        {
            int oi = outEdges.Count() - 1;
            while (oi >= 0)
            {
                ref LFPG_ElecEdge oEdge = outEdges[oi];
                if (oEdge)
                {
                    RemoveFromIncoming(oEdge.m_TargetNodeId, deviceId, oEdge.m_SourcePort, oEdge.m_TargetPort);
                    affectedNeighbors.Insert(oEdge.m_TargetNodeId);
                    affectedSupplierTargets.Insert(oEdge.m_TargetNodeId);
                    m_EdgeCount = m_EdgeCount - 1;
                }
                oi = oi - 1;
            }
        }

        if (hadIncoming && inEdges)
        {
            int ii = inEdges.Count() - 1;
            while (ii >= 0)
            {
                ref LFPG_ElecEdge iEdge = inEdges[ii];
                if (iEdge)
                {
                    RemoveFromOutgoing(iEdge.m_SourceNodeId, deviceId, iEdge.m_SourcePort, iEdge.m_TargetPort);
                    affectedNeighbors.Insert(iEdge.m_SourceNodeId);
                    m_EdgeCount = m_EdgeCount - 1;
                }
                ii = ii - 1;
            }
        }

        m_Nodes.Remove(deviceId);
        m_Outgoing.Remove(deviceId);
        m_Incoming.Remove(deviceId);
        // v0.7.45 (H5): Clean up cached NetworkIDs for the removed node.
        // Without this, m_NodeNetLow/High grow unbounded on servers with
        // device turnover. CleanupOrphanNode handles neighbors, but the
        // primary removed node never passes through that path.
        m_NodeNetLow.Remove(deviceId);
        m_NodeNetHigh.Remove(deviceId);
        m_RequeueEpoch.Remove(deviceId);
        // v5.1: Clean up charger delta-time timestamp for removed node
        m_ChargerLastChargeSec.Remove(deviceId);
        m_NodeCount = m_Nodes.Count();

        int ai;
        for (ai = 0; ai < affectedNeighbors.Count(); ai = ai + 1)
        {
            // v0.7.34 (Bloque E): Defer orphan cleanup during atomic mutations.
            // Neighbors may be targets of subsequent operations in the same batch.
            if (m_MutationActive)
            {
                m_DeferredOrphanCleanup.Insert(affectedNeighbors[ai]);
            }
            else
            {
                CleanupOrphanNode(affectedNeighbors[ai]);
            }
            MarkNodeDirty(affectedNeighbors[ai], LFPG_DIRTY_TOPOLOGY);
        }

        int asti;
        for (asti = 0; asti < affectedSupplierTargets.Count(); asti = asti + 1)
        {
            MarkIncomingSuppliersDirty(affectedSupplierTargets[asti]);
        }

        m_ComponentsDirty = true;
        #endif
    }

    // ===========================
    // v0.7.34 (Bloque E): Atomic Graph Mutations
    // ===========================

    // Begin a batch of graph mutations. While active, CleanupOrphanNode
    // is deferred to EndGraphMutation to prevent premature node deletion
    // during multi-op sequences (replace wire, multi-port cut, etc.).
    //
    // Nesting-safe: Begin can be called N times; only the outermost End
    // triggers deferred cleanups. This allows helper methods to wrap
    // their own Begin/End without conflicting with caller batches.
    //
    // Usage (caller in NetworkManager or PlayerRPC):
    //   m_Graph.BeginGraphMutation();
    //   m_Graph.OnWireRemoved(oldSrc, oldTgt, srcP, tgtP);
    //   m_Graph.OnWireAdded(newSrc, newTgt, srcP, tgtP, wireRef);
    //   m_Graph.EndGraphMutation();
    //
    // Cost: zero when not in mutation (single bool check in OnWireRemoved).
    override void BeginGraphMutation()
    {
        #ifdef SERVER
        m_MutationDepth = m_MutationDepth + 1;
        m_MutationActive = true;
        #endif
    }

    // End a batch of graph mutations. When the outermost batch closes
    // (depth reaches 0), processes all deferred orphan cleanups.
    //
    // Dirty marks were already accumulated during the mutation via
    // MarkNodeDirty (idempotent — m_InQueue prevents duplicates).
    // Component membership is maintained by each OnWireAdded/Removed.
    override void EndGraphMutation()
    {
        #ifdef SERVER
        if (m_MutationDepth <= 0)
        {
            string wMutMsg = "[ElecGraph] EndGraphMutation called without matching Begin";
            LFPG_Util.Warn(wMutMsg);
            m_MutationActive = false;
            m_MutationDepth = 0;
            // v0.7.34: Safety clear — prevent stale deferred entries from leaking
            m_DeferredOrphanCleanup.Clear();
            return;
        }

        m_MutationDepth = m_MutationDepth - 1;

        if (m_MutationDepth > 0)
            return;  // Still inside nested mutation

        m_MutationActive = false;

        // Process deferred orphan cleanups.
        // Some nodes may have gained new edges during the mutation,
        // so CleanupOrphanNode correctly skips them (checks hasOut/hasIn).
        int deferredCount = m_DeferredOrphanCleanup.Count();
        int ci;
        for (ci = 0; ci < deferredCount; ci = ci + 1)
        {
            CleanupOrphanNode(m_DeferredOrphanCleanup[ci]);
        }

        if (deferredCount > 0)
        {
            string dbgFlush = "[ElecGraph] EndGraphMutation: flushed " + deferredCount.ToString() + " deferred orphan checks";
            LFPG_Util.Debug(dbgFlush);
        }

        m_DeferredOrphanCleanup.Clear();
        #endif
    }

    // ===========================
    // Cycle detection
    // ===========================

    // m_Outgoing is the ownership index: every edge is created under its
    // source and persisted by that source. m_Incoming is a derived acceleration
    // index used by allocation and telemetry. Rebuild it from the canonical
    // edge objects so an interrupted/legacy mutation cannot give upstream and
    // downstream solvers different topologies.
    protected bool CanonicalizeIncomingAdjacency()
    {
        #ifdef SERVER
        bool mismatch = false;
        int oldIncomingCount = 0;
        int incomingOwnerIndex;

        for (incomingOwnerIndex = 0; incomingOwnerIndex < m_Incoming.Count(); incomingOwnerIndex = incomingOwnerIndex + 1)
        {
            string incomingOwnerId = m_Incoming.GetKey(incomingOwnerIndex);
            ref array<ref LFPG_ElecEdge> indexedIncoming = m_Incoming.GetElement(incomingOwnerIndex);
            if (!indexedIncoming)
                continue;

            oldIncomingCount = oldIncomingCount + indexedIncoming.Count();
            int indexedIncomingIndex;
            for (indexedIncomingIndex = 0; indexedIncomingIndex < indexedIncoming.Count(); indexedIncomingIndex = indexedIncomingIndex + 1)
            {
                LFPG_ElecEdge incomingEdge = indexedIncoming[indexedIncomingIndex];
                if (!incomingEdge || incomingEdge.m_TargetNodeId != incomingOwnerId)
                {
                    mismatch = true;
                    continue;
                }

                ref array<ref LFPG_ElecEdge> matchingOutgoing;
                if (!m_Outgoing.Find(incomingEdge.m_SourceNodeId, matchingOutgoing) || !matchingOutgoing || matchingOutgoing.Find(incomingEdge) < 0)
                    mismatch = true;
            }
        }

        int outgoingCount = 0;
        int outgoingOwnerIndex;
        for (outgoingOwnerIndex = 0; outgoingOwnerIndex < m_Outgoing.Count(); outgoingOwnerIndex = outgoingOwnerIndex + 1)
        {
            string outgoingOwnerId = m_Outgoing.GetKey(outgoingOwnerIndex);
            ref array<ref LFPG_ElecEdge> indexedOutgoing = m_Outgoing.GetElement(outgoingOwnerIndex);
            if (!indexedOutgoing)
                continue;

            int indexedOutgoingIndex;
            for (indexedOutgoingIndex = 0; indexedOutgoingIndex < indexedOutgoing.Count(); indexedOutgoingIndex = indexedOutgoingIndex + 1)
            {
                LFPG_ElecEdge outgoingEdge = indexedOutgoing[indexedOutgoingIndex];
                if (!outgoingEdge)
                {
                    mismatch = true;
                    continue;
                }

                outgoingCount = outgoingCount + 1;
                if (outgoingEdge.m_SourceNodeId != outgoingOwnerId)
                    mismatch = true;

                ref array<ref LFPG_ElecEdge> matchingIncoming;
                if (!m_Incoming.Find(outgoingEdge.m_TargetNodeId, matchingIncoming) || !matchingIncoming || matchingIncoming.Find(outgoingEdge) < 0)
                    mismatch = true;
            }
        }

        if (oldIncomingCount != outgoingCount)
            mismatch = true;

        m_Incoming.Clear();
        for (outgoingOwnerIndex = 0; outgoingOwnerIndex < m_Outgoing.Count(); outgoingOwnerIndex = outgoingOwnerIndex + 1)
        {
            string canonicalSourceId = m_Outgoing.GetKey(outgoingOwnerIndex);
            ref array<ref LFPG_ElecEdge> canonicalOutgoing = m_Outgoing.GetElement(outgoingOwnerIndex);
            if (!canonicalOutgoing)
                continue;

            int canonicalIndex;
            for (canonicalIndex = 0; canonicalIndex < canonicalOutgoing.Count(); canonicalIndex = canonicalIndex + 1)
            {
                ref LFPG_ElecEdge canonicalEdge = canonicalOutgoing[canonicalIndex];
                if (!canonicalEdge || canonicalEdge.m_TargetNodeId == "")
                    continue;

                canonicalEdge.m_SourceNodeId = canonicalSourceId;
                ref array<ref LFPG_ElecEdge> canonicalIncoming;
                if (!m_Incoming.Find(canonicalEdge.m_TargetNodeId, canonicalIncoming) || !canonicalIncoming)
                {
                    canonicalIncoming = new array<ref LFPG_ElecEdge>;
                    m_Incoming.Set(canonicalEdge.m_TargetNodeId, canonicalIncoming);
                }
                canonicalIncoming.Insert(canonicalEdge);
            }
        }

        m_EdgeCount = outgoingCount;
        if (mismatch)
        {
            string canonicalMsg = "[ElecGraph] Canonicalized asymmetric adjacency incoming=";
            canonicalMsg = canonicalMsg + oldIncomingCount.ToString();
            canonicalMsg = canonicalMsg + " outgoing=" + outgoingCount.ToString();
            LFPG_Util.Warn(canonicalMsg);
        }
        return mismatch;
        #else
        return false;
        #endif
    }

    // O(V+E) Kahn audit used as the normal fast path.  A legal large graph is
    // validated once without running a path search per edge.  The more detailed
    // path finder below is invoked only when this proves a cycle exists.
    protected bool CompletedGraphHasDirectedCycle()
    {
        #ifdef SERVER
        ref map<string, int> indegree = new map<string, int>;
        ref array<string> zeroQueue = new array<string>;

        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            indegree.Set(m_Nodes.GetKey(nodeIndex), 0);
        }

        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            string ownerId = m_Outgoing.GetKey(ownerIndex);
            int ownerDegree;
            if (!indegree.Find(ownerId, ownerDegree))
            {
                indegree.Set(ownerId, 0);
            }

            ref array<ref LFPG_ElecEdge> ownerEdges = m_Outgoing.GetElement(ownerIndex);
            if (!ownerEdges)
                continue;

            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < ownerEdges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = ownerEdges[edgeIndex];
                if (!edge)
                    continue;
                if ((edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                if (edge.m_TargetNodeId == "")
                    continue;

                int targetDegree = 0;
                indegree.Find(edge.m_TargetNodeId, targetDegree);
                indegree.Set(edge.m_TargetNodeId, targetDegree + 1);
            }
        }

        int degreeIndex;
        for (degreeIndex = 0; degreeIndex < indegree.Count(); degreeIndex = degreeIndex + 1)
        {
            if (indegree.GetElement(degreeIndex) == 0)
            {
                zeroQueue.Insert(indegree.GetKey(degreeIndex));
            }
        }

        int queueHead = 0;
        int processedCount = 0;
        while (queueHead < zeroQueue.Count())
        {
            string currentId = zeroQueue[queueHead];
            queueHead = queueHead + 1;
            processedCount = processedCount + 1;

            ref array<ref LFPG_ElecEdge> currentEdges;
            if (!m_Outgoing.Find(currentId, currentEdges) || !currentEdges)
                continue;

            int currentEdgeIndex;
            for (currentEdgeIndex = 0; currentEdgeIndex < currentEdges.Count(); currentEdgeIndex = currentEdgeIndex + 1)
            {
                LFPG_ElecEdge currentEdge = currentEdges[currentEdgeIndex];
                if (!currentEdge)
                    continue;
                if ((currentEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                if (currentEdge.m_TargetNodeId == "")
                    continue;

                int remainingDegree = 0;
                if (!indegree.Find(currentEdge.m_TargetNodeId, remainingDegree))
                    continue;
                remainingDegree = remainingDegree - 1;
                indegree.Set(currentEdge.m_TargetNodeId, remainingDegree);
                if (remainingDegree == 0)
                {
                    zeroQueue.Insert(currentEdge.m_TargetNodeId);
                }
            }
        }

        return processedCount < indegree.Count();
        #else
        return false;
        #endif
    }

    // Search the actual enabled adjacency for startId -> targetId while
    // ignoring one candidate edge.  When found, outPath contains both path
    // endpoints in traversal order.  This is intentionally independent from
    // DetectCycleIfAdded so the rebuild audit does not share its assumptions.
    protected bool FindDirectedPathExcludingEdge(string startId, string targetId, LFPG_ElecEdge excludedEdge, array<string> outPath)
    {
        #ifdef SERVER
        if (!outPath)
            return false;

        outPath.Clear();
        if (startId == "" || targetId == "")
            return false;

        if (startId == targetId)
        {
            outPath.Insert(startId);
            return true;
        }

        ref array<string> stack = new array<string>;
        ref map<string, bool> visited = new map<string, bool>;
        ref map<string, string> parent = new map<string, string>;
        stack.Insert(startId);

        int visitedCount = 0;
        while (stack.Count() > 0)
        {
            if (visitedCount >= LFPG_DFS_MAX_VISITED)
            {
                string limitMsg = "[ElecGraph] Completed DAG audit reached DFS limit from ";
                limitMsg = limitMsg + startId + " to " + targetId;
                LFPG_Util.Warn(limitMsg);
                return false;
            }

            int topIndex = stack.Count() - 1;
            string currentId = stack[topIndex];
            stack.Remove(topIndex);

            bool wasVisited = false;
            visited.Find(currentId, wasVisited);
            if (wasVisited)
                continue;

            visited.Set(currentId, true);
            visitedCount = visitedCount + 1;

            if (currentId == targetId)
            {
                ref array<string> reversePath = new array<string>;
                string cursor = targetId;
                reversePath.Insert(cursor);

                int parentSteps = 0;
                while (cursor != startId && parentSteps <= LFPG_DFS_MAX_VISITED)
                {
                    string parentId;
                    if (!parent.Find(cursor, parentId) || parentId == "")
                        return false;
                    cursor = parentId;
                    reversePath.Insert(cursor);
                    parentSteps = parentSteps + 1;
                }

                if (cursor != startId)
                    return false;

                int reverseIndex;
                for (reverseIndex = reversePath.Count() - 1; reverseIndex >= 0; reverseIndex = reverseIndex - 1)
                {
                    outPath.Insert(reversePath[reverseIndex]);
                }
                return true;
            }

            ref array<ref LFPG_ElecEdge> outgoingEdges;
            if (!m_Outgoing.Find(currentId, outgoingEdges) || !outgoingEdges)
                continue;

            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < outgoingEdges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge walkEdge = outgoingEdges[edgeIndex];
                if (!walkEdge || walkEdge == excludedEdge)
                    continue;
                if ((walkEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                if (walkEdge.m_TargetNodeId == "")
                    continue;

                bool targetVisited = false;
                visited.Find(walkEdge.m_TargetNodeId, targetVisited);
                if (targetVisited)
                    continue;

                string existingParent;
                if (!parent.Find(walkEdge.m_TargetNodeId, existingParent))
                {
                    parent.Set(walkEdge.m_TargetNodeId, currentId);
                    stack.Insert(walkEdge.m_TargetNodeId);
                }
            }
        }

        return false;
        #else
        return false;
        #endif
    }

    // Validate the graph after every persisted edge source has been merged.
    // For each edge U->V, a path V->U (excluding that edge) proves the edge is
    // part of a directed cycle.  Remove one runtime edge, then restart the scan
    // until no such path remains.  Wire persistence stays read-only; the full
    // path in the warning identifies the data that an admin or repair pass can
    // remove later.  Complexity is acceptable because rebuild is infrequent
    // and graph size is bounded.
    protected int SanitizeCompletedGraphCycles()
    {
        #ifdef SERVER
        int removedCount = 0;
        bool removedOne = true;

        while (removedOne && removedCount <= LFPG_DFS_MAX_VISITED)
        {
            removedOne = false;
            if (!CompletedGraphHasDirectedCycle())
                break;

            LFPG_ElecEdge cycleEdge = null;
            ref array<string> cycleReturnPath = new array<string>;

            int ownerIndex;
            for (ownerIndex = 0; ownerIndex < m_Outgoing.Count() && !cycleEdge; ownerIndex = ownerIndex + 1)
            {
                ref array<ref LFPG_ElecEdge> ownerEdges = m_Outgoing.GetElement(ownerIndex);
                if (!ownerEdges)
                    continue;

                int candidateIndex;
                for (candidateIndex = 0; candidateIndex < ownerEdges.Count(); candidateIndex = candidateIndex + 1)
                {
                    LFPG_ElecEdge candidate = ownerEdges[candidateIndex];
                    if (!candidate)
                        continue;
                    if ((candidate.m_Flags & LFPG_EDGE_ENABLED) == 0)
                        continue;

                    ref array<string> returnPath = new array<string>;
                    if (FindDirectedPathExcludingEdge(candidate.m_TargetNodeId, candidate.m_SourceNodeId, candidate, returnPath))
                    {
                        cycleEdge = candidate;
                        cycleReturnPath = returnPath;
                        break;
                    }
                }
            }

            if (cycleEdge)
            {
                string cyclePath = cycleEdge.m_SourceNodeId;
                int pathIndex;
                for (pathIndex = 0; pathIndex < cycleReturnPath.Count(); pathIndex = pathIndex + 1)
                {
                    cyclePath = cyclePath + " -> " + cycleReturnPath[pathIndex];
                }

                string cycleMsg = "[ElecGraph] Completed DAG audit omitted runtime edge ";
                cycleMsg = cycleMsg + cycleEdge.m_SourceNodeId + " -> " + cycleEdge.m_TargetNodeId;
                cycleMsg = cycleMsg + " port=" + cycleEdge.m_SourcePort + "->" + cycleEdge.m_TargetPort;
                cycleMsg = cycleMsg + " cycle=" + cyclePath;
                LFPG_Util.Warn(cycleMsg);

                string removeSource = cycleEdge.m_SourceNodeId;
                string removeTarget = cycleEdge.m_TargetNodeId;
                string removeSourcePort = cycleEdge.m_SourcePort;
                string removeTargetPort = cycleEdge.m_TargetPort;
                RemoveEdgeInternal(removeSource, removeTarget, removeSourcePort, removeTargetPort);
                MarkNodeDirty(removeSource, LFPG_DIRTY_TOPOLOGY);
                MarkNodeDirty(removeTarget, LFPG_DIRTY_TOPOLOGY);
                MarkIncomingSuppliersDirty(removeTarget);
                removedCount = removedCount + 1;
                removedOne = true;
            }
            else
            {
                LFPG_Util.Error("[ElecGraph] Completed DAG audit found a cycle but could not isolate an edge");
            }
        }

        if (CompletedGraphHasDirectedCycle())
        {
            LFPG_Util.Error("[ElecGraph] Completed DAG audit did not converge within its safety limit");
        }

        if (removedCount > 0)
        {
            m_PotentialSupplyCache.Clear();
            m_PotentialSupplySnapshotReady = false;
            m_PotentialContributorCache.Clear();
            m_PotentialContributorCapacity.Clear();
            m_ComponentsDirty = true;
        }

        return removedCount;
        #else
        return 0;
        #endif
    }

    override bool DetectCycleIfAdded(string sourceId, string targetId)
    {
        #ifdef SERVER
        if (sourceId == targetId)
            return true;

        ref array<string> stack = new array<string>;
        ref map<string, bool> visited = new map<string, bool>;

        stack.Insert(targetId);
        int visitedCount = 0;
        bool bVisTrue = true;

        while (stack.Count() > 0)
        {
            // v0.7.26 (Audit 4): Depth limit guard for very dense graphs.
            // Conservatively assumes cycle if limit reached (safe: rejects wire).
            if (visitedCount >= LFPG_DFS_MAX_VISITED)
            {
                string wDfsMsg = "[ElecGraph] DetectCycle: visited limit reached (" + visitedCount.ToString() + "), assuming cycle";
                LFPG_Util.Warn(wDfsMsg);
                return true;
            }

            int topIdx = stack.Count() - 1;
            string current = stack[topIdx];
            stack.Remove(topIdx);

            if (current == sourceId)
                return true;

            bool alreadyVisited = false;
            visited.Find(current, alreadyVisited);
            if (alreadyVisited)
                continue;

            visited.Set(current, bVisTrue);
            visitedCount = visitedCount + 1;

            ref array<ref LFPG_ElecEdge> edges;
            if (m_Outgoing.Find(current, edges) && edges)
            {
                int edgeI;
                for (edgeI = 0; edgeI < edges.Count(); edgeI = edgeI + 1)
                {
                    ref LFPG_ElecEdge edge = edges[edgeI];
                    if (edge && edge.m_TargetNodeId != "")
                    {
                        bool tgtVisited = false;
                        visited.Find(edge.m_TargetNodeId, tgtVisited);
                        if (!tgtVisited)
                        {
                            stack.Insert(edge.m_TargetNodeId);
                        }
                    }
                }
            }
        }

        return false;
        #else
        return false;
        #endif
    }

    // ===========================
    // Connected components
    // ===========================

    void RebuildComponents()
    {
        #ifdef SERVER
        if (!m_ComponentsDirty)
            return;

        int ri;
        for (ri = 0; ri < m_Nodes.Count(); ri = ri + 1)
        {
            ref LFPG_ElecNode rNode = m_Nodes.GetElement(ri);
            if (rNode)
                rNode.m_ComponentId = -1;
        }

        // v0.7.31: Clear component sizes for rebuild
        m_ComponentSizes.Clear();

        int nextId = 0;

        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            ref LFPG_ElecNode startNode = m_Nodes.GetElement(ni);
            if (!startNode)
                continue;
            if (startNode.m_ComponentId != -1)
                continue;

            // v0.7.31: Count nodes per component during BFS
            int compSize = 0;

            ref array<string> queue = new array<string>;
            queue.Insert(m_Nodes.GetKey(ni));
            int head = 0;

            while (head < queue.Count())
            {
                string curId = queue[head];
                head = head + 1;

                ref LFPG_ElecNode curNode;
                if (!m_Nodes.Find(curId, curNode) || !curNode)
                    continue;
                if (curNode.m_ComponentId != -1)
                    continue;

                curNode.m_ComponentId = nextId;
                compSize = compSize + 1;

                ref array<ref LFPG_ElecEdge> outE;
                if (m_Outgoing.Find(curId, outE) && outE)
                {
                    int oi;
                    for (oi = 0; oi < outE.Count(); oi = oi + 1)
                    {
                        ref LFPG_ElecEdge oEdge = outE[oi];
                        if (oEdge)
                        {
                            ref LFPG_ElecNode tgtNode;
                            if (m_Nodes.Find(oEdge.m_TargetNodeId, tgtNode) && tgtNode)
                            {
                                if (tgtNode.m_ComponentId == -1)
                                    queue.Insert(oEdge.m_TargetNodeId);
                            }
                        }
                    }
                }

                ref array<ref LFPG_ElecEdge> inE;
                if (m_Incoming.Find(curId, inE) && inE)
                {
                    int ii;
                    for (ii = 0; ii < inE.Count(); ii = ii + 1)
                    {
                        ref LFPG_ElecEdge iEdge = inE[ii];
                        if (iEdge)
                        {
                            ref LFPG_ElecNode srcNode;
                            if (m_Nodes.Find(iEdge.m_SourceNodeId, srcNode) && srcNode)
                            {
                                if (srcNode.m_ComponentId == -1)
                                    queue.Insert(iEdge.m_SourceNodeId);
                            }
                        }
                    }
                }
            }

            // v0.7.31: Store component size for O(1) watchdog lookups
            m_ComponentSizes.Set(nextId, compSize);

            nextId = nextId + 1;
        }

        m_NextComponentId = nextId;
        m_ComponentsDirty = false;
        #endif
    }

    // ===========================
    // Internal helpers
    // ===========================

    protected void EnsureNode(string deviceId, EntityAI obj)
    {
        #ifdef SERVER
        if (deviceId == "")
            return;

        ref LFPG_ElecNode existing;
        if (m_Nodes.Find(deviceId, existing))
            return;

        ref LFPG_ElecNode node = new LFPG_ElecNode();
        node.m_DeviceId = deviceId;

        if (obj)
        {
            node.m_DeviceType = LFPG_DeviceAPI.GetDeviceType(obj);

            // v0.7.43 (Fix 3): Cache NetworkID for fallback resolution.
            // If DeviceRegistry ref goes stale, SyncNodeToEntity can
            // re-resolve via GetObjectByNetworkId.
            int nLow = 0;
            int nHigh = 0;
            obj.GetNetworkID(nLow, nHigh);
            if (nLow != 0 || nHigh != 0)
            {
                m_NodeNetLow.Set(deviceId, nLow);
                m_NodeNetHigh.Set(deviceId, nHigh);
            }

            // v0.7.38 (BugFix): Populate electrical properties on creation.
            // Previously only set by PopulateAllNodeElecStates (bulk rebuild).
            // Without this, runtime wire-adds created nodes with consumption=0
            // and maxOutput=0 — causing no-overload and always-powered bugs.
            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                node.m_MaxOutput = LFPG_DeviceAPI.GetCapacity(obj);
                bool sourceOn = false;
                if (LFPG_DeviceAPI.IsSource(obj))
                {
                    sourceOn = LFPG_DeviceAPI.GetSourceOn(obj);
                }
                else
                {
                    ComponentEnergyManager emSrc = obj.GetCompEM();
                    if (emSrc)
                    {
                        sourceOn = emSrc.IsWorking();
                    }
                }
                node.m_Powered = sourceOn;
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                node.m_MaxOutput = LFPG_DeviceAPI.GetCapacity(obj);
                if (node.m_MaxOutput < LFPG_PROPAGATION_EPSILON)
                {
                    node.m_MaxOutput = LFPG_DEFAULT_PASSTHROUGH_CAPACITY;
                }
                // v0.7.47: PASSTHROUGH self-consumption (CeilingLight pattern).
                // Splitter returns 0.0 explicitly → no regression.
                node.m_Consumption = LFPG_DeviceAPI.GetConsumption(obj);
                // P1: Cache gate capability to avoid entity lookup every tick.
                node.m_IsGated = LFPG_DeviceAPI.IsGateCapable(obj);

                // v2.1: Initialize gate-closed state at rebuild time so the
                // first AllocateOutput pass uses probe demand instead of
                // m_MaxOutput for closed gates. Without this, the first epoch
                // has a 1-pass inflation before converging.
                if (node.m_IsGated)
                {
                    bool initGateOpen = LFPG_DeviceAPI.IsGateOpen(obj);
                    if (!initGateOpen)
                    {
                        node.m_GateClosed = true;
                    }
                }
            }
            else if (node.m_DeviceType == LFPG_DeviceType.CONSUMER || node.m_DeviceType == LFPG_DeviceType.CAMERA)
            {
                node.m_Consumption = LFPG_DeviceAPI.GetConsumption(obj);
            }
        }

        m_Nodes.Set(deviceId, node);
        m_NodeCount = m_Nodes.Count();
        #endif
    }

    protected bool AddEdgeInternal(string sourceId, string targetId, string srcPort, string tgtPort, LFPG_WireData wireRef)
    {
        #ifdef SERVER
        if (sourceId == "" || targetId == "")
            return false;

        ref LFPG_ElecNode srcNode;
        if (!m_Nodes.Find(sourceId, srcNode))
        {
            EntityAI srcObj = LFPG_DeviceRegistry.Get().FindById(sourceId);
            if (!srcObj)
            {
                string wSrcReg = "[ElecGraph] AddEdge rejected: source " + sourceId + " not in registry";
                LFPG_Util.Warn(wSrcReg);
                return false;
            }
            EnsureNode(sourceId, srcObj);
        }

        ref LFPG_ElecNode tgtNode;
        if (!m_Nodes.Find(targetId, tgtNode))
        {
            EntityAI tgtObj = LFPG_DeviceRegistry.Get().FindById(targetId);
            if (!tgtObj)
            {
                string wTgtReg = "[ElecGraph] AddEdge rejected: target " + targetId + " not in registry";
                LFPG_Util.Warn(wTgtReg);
                return false;
            }
            EnsureNode(targetId, tgtObj);
        }

        ref array<ref LFPG_ElecEdge> existOut;
        if (m_Outgoing.Find(sourceId, existOut) && existOut)
        {
            if (existOut.Count() >= LFPG_MAX_EDGES_PER_NODE)
            {
                string wOutLim = "[ElecGraph] AddEdge rejected: source limit " + sourceId + " (out=" + existOut.Count().ToString() + ")";
                LFPG_Util.Warn(wOutLim);
                return false;
            }
        }

        ref array<ref LFPG_ElecEdge> existIn;
        if (m_Incoming.Find(targetId, existIn) && existIn)
        {
            if (existIn.Count() >= LFPG_MAX_EDGES_PER_NODE)
            {
                string wInLim = "[ElecGraph] AddEdge rejected: target limit " + targetId + " (in=" + existIn.Count().ToString() + ")";
                LFPG_Util.Warn(wInLim);
                return false;
            }
        }

        // v0.9.3: Duplicate edge guard — skip if edge with same src+tgt+ports exists.
        // Can happen if DeviceRegistry returns same entity under multiple keys,
        // causing RebuildFromWires to iterate the same wire store twice.
        if (existOut)
        {
            int dupCheck;
            for (dupCheck = 0; dupCheck < existOut.Count(); dupCheck = dupCheck + 1)
            {
                LFPG_ElecEdge dupE = existOut[dupCheck];
                if (!dupE) continue;
                if (dupE.m_TargetNodeId == targetId && dupE.m_SourcePort == srcPort && dupE.m_TargetPort == tgtPort)
                {
                    return false;
                }
            }
        }

        // Create edge
        ref LFPG_ElecEdge edge = new LFPG_ElecEdge();
        edge.m_SourceNodeId = sourceId;
        edge.m_TargetNodeId = targetId;
        edge.m_SourcePort = srcPort;
        edge.m_TargetPort = tgtPort;
        edge.m_WireRef = wireRef;
        // Edges must start ENABLED. Without this, every check that
        // filters by LFPG_EDGE_ENABLED skips the edge entirely.
        edge.m_Flags = LFPG_EDGE_ENABLED;

        // Insert into outgoing
        ref array<ref LFPG_ElecEdge> outArr;
        if (!m_Outgoing.Find(sourceId, outArr) || !outArr)
        {
            outArr = new array<ref LFPG_ElecEdge>;
            m_Outgoing.Set(sourceId, outArr);
        }
        outArr.Insert(edge);

        // Insert into incoming
        ref array<ref LFPG_ElecEdge> inArr;
        if (!m_Incoming.Find(targetId, inArr) || !inArr)
        {
            inArr = new array<ref LFPG_ElecEdge>;
            m_Incoming.Set(targetId, inArr);
        }
        inArr.Insert(edge);

        m_EdgeCount = m_EdgeCount + 1;
        return true;
        #else
        return false;
        #endif
    }

    protected void RemoveEdgeInternal(string sourceId, string targetId, string srcPort, string tgtPort)
    {
        #ifdef SERVER
        bool removedOut = RemoveFromOutgoing(sourceId, targetId, srcPort, tgtPort);
        bool removedIn = RemoveFromIncoming(targetId, sourceId, srcPort, tgtPort);

        if (removedOut || removedIn)
        {
            m_EdgeCount = m_EdgeCount - 1;
            if (m_EdgeCount < 0)
                m_EdgeCount = 0;

            // B6 fix: Detect asymmetric edge state (present in one list but
            // not the other). This indicates a prior bug that left the graph
            // inconsistent. Log it for diagnosis.
            if (removedOut != removedIn)
            {
                string asymMsg = "[ElecGraph] WARN: asymmetric edge removal ";
                asymMsg = asymMsg + sourceId + " -> " + targetId;
                asymMsg = asymMsg + " out=" + removedOut.ToString();
                asymMsg = asymMsg + " in=" + removedIn.ToString();
                LFPG_Util.Warn(asymMsg);
            }
        }
        #endif
    }

    protected bool RemoveFromOutgoing(string ownerId, string targetId, string srcPort, string tgtPort)
    {
        #ifdef SERVER
        ref array<ref LFPG_ElecEdge> arr;
        if (!m_Outgoing.Find(ownerId, arr) || !arr)
            return false;

        int i = arr.Count() - 1;
        while (i >= 0)
        {
            ref LFPG_ElecEdge e = arr[i];
            if (e && e.m_TargetNodeId == targetId && e.m_SourcePort == srcPort && e.m_TargetPort == tgtPort)
            {
                arr.Remove(i);
                return true;
            }
            i = i - 1;
        }
        return false;
        #else
        return false;
        #endif
    }

    protected bool RemoveFromIncoming(string targetId, string sourceId, string srcPort, string tgtPort)
    {
        #ifdef SERVER
        ref array<ref LFPG_ElecEdge> arr;
        if (!m_Incoming.Find(targetId, arr) || !arr)
            return false;

        int i = arr.Count() - 1;
        while (i >= 0)
        {
            ref LFPG_ElecEdge e = arr[i];
            if (e && e.m_SourceNodeId == sourceId && e.m_SourcePort == srcPort && e.m_TargetPort == tgtPort)
            {
                arr.Remove(i);
                return true;
            }
            i = i - 1;
        }
        return false;
        #else
        return false;
        #endif
    }

     protected void CleanupOrphanNode(string deviceId)
    {
        #ifdef SERVER
        if (deviceId == "")
            return;

        ref LFPG_ElecNode node;
        if (!m_Nodes.Find(deviceId, node))
            return;

        bool hasOut = false;
        ref array<ref LFPG_ElecEdge> outE;
        if (m_Outgoing.Find(deviceId, outE) && outE && outE.Count() > 0)
            hasOut = true;

        bool hasIn = false;
        ref array<ref LFPG_ElecEdge> inE;
        if (m_Incoming.Find(deviceId, inE) && inE && inE.Count() > 0)
            hasIn = true;

        if (!hasOut && !hasIn)
        {
            // v0.7.49: Reset entity SyncVars BEFORE deleting node.
            // After removal, ProcessDirtyQueue skips the missing nodeId
            // so SyncNodeToEntity never fires. The entity retains stale
            // m_LoadRatio / m_PoweredNet / mask SyncVars forever.
            // Node still in m_Nodes here, so m_DeviceType is available.
            int orphanType = LFPG_DeviceType.CONSUMER;
            if (node)
            {
                orphanType = node.m_DeviceType;
            }
            ResetOrphanSyncVars(deviceId, orphanType);

            int orphanComponent = -1;
            int orphanComponentSize = 0;
            if (node)
                orphanComponent = node.m_ComponentId;
            if (orphanComponent >= 0 && m_ComponentSizes.Find(orphanComponent, orphanComponentSize))
            {
                orphanComponentSize = orphanComponentSize - 1;
                if (orphanComponentSize > 0)
                    m_ComponentSizes.Set(orphanComponent, orphanComponentSize);
                else
                    m_ComponentSizes.Remove(orphanComponent);
            }

            m_Nodes.Remove(deviceId);
            m_Outgoing.Remove(deviceId);
            m_Incoming.Remove(deviceId);
            m_NodeNetLow.Remove(deviceId);
            m_NodeNetHigh.Remove(deviceId);
            m_RequeueEpoch.Remove(deviceId);
            // v5.1: Clean up charger delta-time timestamp for removed node
            m_ChargerLastChargeSec.Remove(deviceId);
            m_NodeCount = m_Nodes.Count();
        }
        #endif
    }

	// v0.7.49: Shared helper for resetting entity SyncVars when a graph
    // node becomes orphaned (zero edges). Called from:
    //   - CleanupOrphanNode (incremental path, type from live node)
    //   - PostBulkRebuild   (bulk path, type from pre-rebuild snapshot)
    //
    // Entity resolution: 3-tier (Registry -> Vanilla -> NetworkID).
    // NOTE: NetworkID fallback only effective in CleanupOrphanNode path.
    // PostBulkRebuild clears m_NodeNetLow/High during RebuildFromWires
    // (line ~300), so the map lookups return false in that context.
    // This is harmless (2 map misses) and correct: all devices are
    // resolved via Registry or Vanilla after a full rebuild.
    //
    // Returns true if entity was resolved and SyncVars were reset.
    protected bool ResetOrphanSyncVars(string deviceId, int deviceType)
    {
        #ifdef SERVER
        EntityAI orphanObj = LFPG_DeviceRegistry.Get().FindById(deviceId);
        if (!orphanObj)
        {
            orphanObj = LFPG_DeviceAPI.ResolveVanillaDevice(deviceId);
        }
        if (!orphanObj)
        {
            // NetworkID fallback (same pattern as SyncNodeToEntity).
            // Only effective in CleanupOrphanNode path where
            // m_NodeNetLow/High still exist pre-deletion.
            int cachedNetLow = 0;
            int cachedNetHigh = 0;
            bool hasNetLow = m_NodeNetLow.Find(deviceId, cachedNetLow);
            bool hasNetHigh = m_NodeNetHigh.Find(deviceId, cachedNetHigh);
            if (hasNetLow && hasNetHigh)
            {
                if (cachedNetLow != 0 || cachedNetHigh != 0)
                {
                    Object rawObj = g_Game.GetObjectByNetworkId(cachedNetLow, cachedNetHigh);
                    orphanObj = EntityAI.Cast(rawObj);
                }
            }
        }
        if (!orphanObj)
        {
            string missMsg = "[CleanupOrphan] Entity not found for " + deviceId;
            LFPG_Util.Debug(missMsg);
            return false;
        }

        if (deviceType == LFPG_DeviceType.SOURCE)
        {
            // Reset load state. m_SourceOn NOT reset (sun/fuel independent).
            LFPG_DeviceAPI.SetLoadRatio(orphanObj, 0.0);
            LFPG_DeviceAPI.SetOverloaded(orphanObj, false);
        }
        else if (deviceType == LFPG_DeviceType.CONSUMER || deviceType == LFPG_DeviceType.CAMERA)
        {
            LFPG_DeviceAPI.SetPowered(orphanObj, false);
        }
        else if (deviceType == LFPG_DeviceType.PASSTHROUGH)
        {
            LFPG_DeviceAPI.SetPowered(orphanObj, false);
            LFPG_DeviceAPI.SetOverloaded(orphanObj, false);
        }

        string resetMsg = "[CleanupOrphan] Reset SyncVars type=" + deviceType.ToString() + " id=" + deviceId;
        LFPG_Util.Info(resetMsg);
        return true;
        #else
        return false;
        #endif
    }

	
	
	
	
	
    // ===========================
    // Public accessors
    // ===========================

    override LFPG_ElecNode GetNode(string deviceId)
    {
        ref LFPG_ElecNode node;
        if (m_Nodes.Find(deviceId, node))
            return node;
        return null;
    }

    override array<ref LFPG_ElecEdge> GetOutgoing(string deviceId)
    {
        ref array<ref LFPG_ElecEdge> arr;
        if (m_Outgoing.Find(deviceId, arr))
            return arr;
        return null;
    }

    // v2.0: Sum allocated power on all outgoing edges for a node.
    // Used by battery timer to compute actual downstream energy consumption.
    // O(K) where K = outgoing edges (typically 1-3 for batteries).
    override float SumOutgoingAllocations(string nodeId)
    {
        float total = 0.0;
        ref array<ref LFPG_ElecEdge> outEdges;
        if (!m_Outgoing.Find(nodeId, outEdges) || !outEdges)
            return 0.0;

        int oi;
        for (oi = 0; oi < outEdges.Count(); oi = oi + 1)
        {
            ref LFPG_ElecEdge edge = outEdges[oi];
            if (!edge)
                continue;
            if ((edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            total = total + edge.m_AllocatedPower;
        }
        return total;
    }

    // v0.7.36 (Audit Feb2026): Pre-check component size before wire storage.
    // Returns true if adding a wire between sourceId and targetId would
    // cause the merged component to exceed LFPG_MAX_NODES_PER_COMPONENT.
    // Called from FinishWiring BEFORE the replacement phase so the player
    // gets clear feedback without any data mutation.
    // Logic mirrors OnWireAdded watchdog but is read-only.
    override bool WouldExceedComponentLimit(string sourceId, string targetId)
    {
        #ifdef SERVER
        if (sourceId == "" || targetId == "")
            return false;

        // Global hard-cap
        if (m_NodeCount >= LFPG_MAX_NODES_GLOBAL)
            return true;

        int limit = LFPG_MAX_NODES_PER_COMPONENT;

        ref LFPG_ElecNode nodeA;
        ref LFPG_ElecNode nodeB;
        bool hasA = m_Nodes.Find(sourceId, nodeA);
        bool hasB = m_Nodes.Find(targetId, nodeB);

        // Both nodes are new (not in graph yet) → merged size = 2, always OK
        if (!hasA && !hasB)
            return false;

        if (!m_ComponentsDirty)
        {
            int compA = -1;
            int compB = -1;
            if (hasA && nodeA)
                compA = nodeA.m_ComponentId;
            if (hasB && nodeB)
                compB = nodeB.m_ComponentId;

            // Same component → no size growth
            if (compA >= 0 && compA == compB)
                return false;

            // Different known components → O(1) size lookup
            if (compA >= 0 && compB >= 0)
            {
                int sizeA = 0;
                int sizeB = 0;
                m_ComponentSizes.Find(compA, sizeA);
                m_ComponentSizes.Find(compB, sizeB);
                int mergedSize = sizeA + sizeB;
                if (mergedSize > limit)
                    return true;
                return false;
            }
        }

        // Fallback: BFS count (handles dirty components or new nodes)
        int bfsSizeA = 1;
        if (hasA && nodeA)
        {
            bfsSizeA = CountComponentLimited(sourceId, limit);
        }
        if (bfsSizeA > limit)
            return true;

        // Check if B is already in A's component (same component, no growth)
        bool bInA = false;
        if (hasA && nodeA && hasB && nodeB)
        {
            m_WdgVisited.Find(targetId, bInA);
        }
        if (bInA)
            return false;

        int remaining = limit - bfsSizeA;
        if (remaining <= 0)
            return true;

        int bfsSizeB = 1;
        if (hasB && nodeB)
        {
            bfsSizeB = CountComponentLimited(targetId, remaining);
        }

        int totalSize = bfsSizeA + bfsSizeB;
        if (totalSize > limit)
            return true;

        return false;
        #else
        return false;
        #endif
    }

    override array<ref LFPG_ElecEdge> GetIncoming(string deviceId)
    {
        ref array<ref LFPG_ElecEdge> arr;
        if (m_Incoming.Find(deviceId, arr))
            return arr;
        return null;
    }

    override int GetNodeCount()
    {
        return m_NodeCount;
    }

    override int GetEdgeCount()
    {
        return m_EdgeCount;
    }

    override int GetComponentCount()
    {
        if (m_ComponentsDirty)
            RebuildComponents();
        return m_ComponentSizes.Count();
    }

    override int GetLastRebuildMs()
    {
        return m_LastRebuildMs;
    }

    override int GetLastProcessMs()
    {
        return m_LastProcessMs;
    }

    override int GetCurrentEpoch()
    {
        return m_CurrentEpoch;
    }

    override int GetDirtyQueueSize()
    {
        return m_DirtyQueue.Count() - m_DirtyQueueHead;
    }

    // Sprint 4.3: Get count of sources currently in overload state.
    override int GetOverloadedSourceCount()
    {
        #ifdef SERVER
        int count = 0;
        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            ref LFPG_ElecNode node = m_Nodes.GetElement(ni);
            if (node && node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                if (node.m_Overloaded)
                {
                    count = count + 1;
                }
            }
        }
        return count;
        #else
        return 0;
        #endif
    }

    // Sprint 4.3: Get edges visited in last ProcessDirtyQueue call.
    override int GetLastEdgesVisited()
    {
        return m_EdgesVisitedThisEpoch;
    }

    // v1.1.0: Independent verification of PASSTHROUGH power state.
    // Recalculates inputSum from edge allocations, not from cached m_Powered.
    // Used by water pump actions to guard against stale SyncVar state.
    override bool VerifyPassthroughPowered(string nodeId)
    {
        #ifdef SERVER
        ref LFPG_ElecNode node;
        if (!m_Nodes.Find(nodeId, node) || !node)
            return false;

        if (node.m_DeviceType != LFPG_DeviceType.PASSTHROUGH)
            return node.m_Powered;

        float inputSum = 0.0;
        ref array<ref LFPG_ElecEdge> inEdges;
        if (m_Incoming.Find(nodeId, inEdges) && inEdges)
        {
            int ei;
            for (ei = 0; ei < inEdges.Count(); ei = ei + 1)
            {
                ref LFPG_ElecEdge edge = inEdges[ei];
                if (!edge)
                    continue;
                if ((edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                inputSum = inputSum + edge.m_AllocatedPower;
            }
        }

        if (node.m_Consumption > LFPG_PROPAGATION_EPSILON)
        {
            return (inputSum + LFPG_PROPAGATION_EPSILON >= node.m_Consumption);
        }

        return (inputSum > LFPG_PROPAGATION_EPSILON);
        #else
        return false;
        #endif
    }

    // ===========================
    // Bulk rebuild helpers
    // ===========================

    override void PostBulkRebuild(LFPG_NetworkManager mgr)
    {
        #ifdef SERVER
        if (!mgr)
            return;

        // v0.7.49: Snapshot old node IDs AND types BEFORE rebuild.
        // After RebuildFromWires, disconnected devices are pruned from graph.
        // Propagation is additive (source->down) so orphans never get visited
        // and their entity SyncVars stay stale. We detect them here.
        // Types are snapshotted in parallel array so the orphan loop can do
        // type-aware reset (SOURCE needs LoadRatio+overloaded, CONSUMER needs
        // powered, PASSTHROUGH needs powered+overloaded).
        ref array<string> oldNodeIds = new array<string>;
        ref array<int> oldNodeTypes = new array<int>;
        int sni;
        for (sni = 0; sni < m_Nodes.Count(); sni = sni + 1)
        {
            oldNodeIds.Insert(m_Nodes.GetKey(sni));
            ref LFPG_ElecNode snapNode = m_Nodes.GetElement(sni);
            int snapType = LFPG_DeviceType.CONSUMER;
            if (snapNode)
            {
                snapType = snapNode.m_DeviceType;
            }
            oldNodeTypes.Insert(snapType);
        }

        RebuildFromWires(mgr);
        PopulateAllNodeElecStates();
        MarkSourcesDirty();

        // v0.7.49: Full SyncVar reset on orphaned devices.
        // v0.7.41 only called SetPowered(false), which is a no-op for SOURCE
        // (LFPG_SetPowered is empty on sources). Left SOURCE m_LoadRatio and
        // masks stale. Now uses type-aware ResetOrphanSyncVars.
        int orphanCount = 0;
        int oni;
        for (oni = 0; oni < oldNodeIds.Count(); oni = oni + 1)
        {
            string orphanId = oldNodeIds[oni];
            ref LFPG_ElecNode testNode;
            if (!m_Nodes.Find(orphanId, testNode))
            {
                int orphanType = oldNodeTypes[oni];
                bool resolved = ResetOrphanSyncVars(orphanId, orphanType);
                if (resolved)
                {
                    orphanCount = orphanCount + 1;
                }
            }
        }


        string infoRebuild = "[ElecGraph] PostBulkRebuild: rebuilt + populated + sources dirty";
        if (orphanCount > 0)
        {
            infoRebuild = infoRebuild + " orphans=" + orphanCount.ToString();
        }
        LFPG_Util.Info(infoRebuild);
        #endif
    }

    // ===========================
    // Sprint 4.2+4.3: Dirty marking
    // ===========================

    // Sprint 4.3: Now tracks enqueued nodes for targeted requeue reset.
    override void MarkNodeDirty(string nodeId, int mask)
    {
        #ifdef SERVER
        if (m_DeterministicSolveActive)
            return;

        if (nodeId == "")
            return;

        ref LFPG_ElecNode node;
        if (!m_Nodes.Find(nodeId, node) || !node)
            return;

        node.m_DirtyMask = node.m_DirtyMask | mask;
        node.m_Dirty = true;

        if (!node.m_InQueue)
        {
            node.m_InQueue = true;
            m_DirtyQueue.Insert(nodeId);
        }
        #endif
    }

    void MarkComponentDirty(int componentId, int mask)
    {
        #ifdef SERVER
        if (componentId < 0)
            return;

        if (m_ComponentsDirty)
            RebuildComponents();

        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            ref LFPG_ElecNode node = m_Nodes.GetElement(ni);
            if (node && node.m_ComponentId == componentId)
            {
                MarkNodeDirty(m_Nodes.GetKey(ni), mask);
            }
        }
        #endif
    }

    override void MarkSourcesDirty()
    {
        #ifdef SERVER
        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            ref LFPG_ElecNode node = m_Nodes.GetElement(ni);
            if (!node)
                continue;

            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                // v0.7.37 (Audit 6, M4): Use DIRTY_INTERNAL, not DIRTY_INPUT.
                // Sources manage their own powered state — they don't need input
                // re-evaluation on startup. DIRTY_INTERNAL skips the incoming edge
                // loop and directly computes output from m_Powered + m_MaxOutput.
                MarkNodeDirty(m_Nodes.GetKey(ni), LFPG_DIRTY_INTERNAL);
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH && node.m_VirtualGeneration > LFPG_PROPAGATION_EPSILON)
            {
                // v2.0: Battery PASSTHROUGH with stored energy can produce power
                // without upstream input. Must be marked dirty on startup so
                // downstream consumers wake up. Uses DIRTY_INPUT (not INTERNAL)
                // because PASSTHROUGH needs to evaluate incoming edges to combine
                // inputSum + virtualGeneration.
                MarkNodeDirty(m_Nodes.GetKey(ni), LFPG_DIRTY_INPUT);
            }
        }

        string infoSrcDirty = "[ElecGraph] MarkSourcesDirty: queued " + m_DirtyQueue.Count().ToString() + " sources";
        LFPG_Util.Info(infoSrcDirty);
        #endif
    }

    // ===========================
    // Deterministic transactional propagation
    // ===========================

    // A dirty event schedules one complete graph solve. Demand and allocation
    // are never derived from partially updated live state:
    //   1. reverse topological pass freezes requested demand;
    //   2. forward topological pass allocates real power once;
    //   3. every node/edge is committed before any entity is synchronized.
    //
    // The previous asynchronous node solver was removed. Its per-node requeue
    // guard could bound a single epoch without bounding the number of epochs,
    // allowing one allocation feedback loop to flicker forever at 10 Hz.
    override int ProcessDirtyQueue(int nodeBudget, int edgeBudget)
    {
        #ifdef SERVER
        m_PropagationEdgeAccountingActive = true;
        m_EdgesVisitedThisEpoch = 0;
        m_ValidateTickCount = m_ValidateTickCount + 1;

        if (m_MutationActive)
        {
            string solveMutationMsg = "[ElecGraph] Deterministic solve found an open mutation (depth=";
            solveMutationMsg = solveMutationMsg + m_MutationDepth.ToString() + "), force-closing";
            LFPG_Util.Warn(solveMutationMsg);
            m_MutationActive = false;
            m_MutationDepth = 0;
            int solveCleanupIndex;
            for (solveCleanupIndex = 0; solveCleanupIndex < m_DeferredOrphanCleanup.Count(); solveCleanupIndex = solveCleanupIndex + 1)
            {
                CleanupOrphanNode(m_DeferredOrphanCleanup[solveCleanupIndex]);
            }
            m_DeferredOrphanCleanup.Clear();
        }

        if (m_RuntimeCycleAuditRequested)
        {
            m_RuntimeCycleAuditRequested = false;
            CanonicalizeIncomingAdjacency();
            int pendingCycleEdges = SanitizeCompletedGraphCycles();
            if (pendingCycleEdges > 0)
            {
                string pendingCycleMsg = "[ElecGraph] Deterministic pre-solve audit healed ";
                pendingCycleMsg = pendingCycleMsg + pendingCycleEdges.ToString() + " cycle-closing edge(s)";
                LFPG_Util.Warn(pendingCycleMsg);
            }
        }

        int pendingCount = m_DirtyQueue.Count() - m_DirtyQueueHead;
        if (pendingCount <= 0)
        {
            if (m_DirtyQueue.Count() > 0)
            {
                m_DirtyQueue.Clear();
                m_DirtyQueueHead = 0;
            }
            ValidateConsumerStates(edgeBudget);
            m_PropagationEdgeAccountingActive = false;
            return m_DirtyQueue.Count() - m_DirtyQueueHead;
        }

        int solveStartMs = g_Game.GetTime();
        if (m_ComponentsDirty)
            RebuildComponents();

        m_CurrentEpoch = m_CurrentEpoch + 1;
        m_PotentialSupplyCache.Clear();
        m_PotentialSupplySnapshotReady = false;
        m_PotentialContributorCache.Clear();
        m_PotentialContributorCapacity.Clear();

        ref array<string> solveOrder = new array<string>;
        bool orderComplete = BuildSolveTopologicalOrder(solveOrder);
        if (!orderComplete)
        {
            // Adjacency is allowed to change only at this scheduler boundary.
            // Canonicalize and quarantine cycle-closing edges, then try once
            // more. A still-invalid graph is committed safe-off below.
            CanonicalizeIncomingAdjacency();
            int healedCycleEdges = SanitizeCompletedGraphCycles();
            if (healedCycleEdges > 0)
            {
                RebuildComponents();
                string healedCycleMsg = "[ElecGraph] Deterministic solve quarantined ";
                healedCycleMsg = healedCycleMsg + healedCycleEdges.ToString() + " cycle-closing edge(s)";
                LFPG_Util.Warn(healedCycleMsg);
            }
            orderComplete = BuildSolveTopologicalOrder(solveOrder);
        }

        m_DeterministicSolveActive = true;
        if (orderComplete)
        {
            ResetSolveStaging(solveOrder);
            BuildFrozenDemand(solveOrder);
            BuildFrozenDelivery(solveOrder);
            if (ValidateFrozenSolution(solveOrder))
            {
                CommitSolveState(solveOrder);
            }
            else
            {
                CommitSolveFailSafe();
                LFPG_Util.Error("[ElecGraph] Deterministic solution violated an electrical invariant; graph committed safe-off");
            }
        }
        else
        {
            CommitSolveFailSafe();
            LFPG_Util.Error("[ElecGraph] Deterministic solve could not produce a complete order; graph committed safe-off");
        }
        m_DeterministicSolveActive = false;

        ClearSolvedDirtyState();
        m_LastProcessMs = g_Game.GetTime() - solveStartMs;

        string solveMsg = "[ElecGraph] ProcessDirtyQueue: deterministic processed=";
        solveMsg = solveMsg + solveOrder.Count().ToString();
        solveMsg = solveMsg + " edges=" + m_EdgesVisitedThisEpoch.ToString();
        solveMsg = solveMsg + " remaining=0 epoch=" + m_CurrentEpoch.ToString();
        solveMsg = solveMsg + " ms=" + m_LastProcessMs.ToString();
        LFPG_Util.Debug(solveMsg);

        m_PropagationEdgeAccountingActive = false;
        return 0;
        #else
        return 0;
        #endif
    }

    protected bool BuildSolveTopologicalOrder(array<string> solveOrder)
    {
        #ifdef SERVER
        if (!solveOrder)
            return false;

        solveOrder.Clear();
        ref map<string, int> indegree = new map<string, int>;
        ref array<string> ready = new array<string>;

        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            indegree.Set(m_Nodes.GetKey(nodeIndex), 0);
        }

        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            ref array<ref LFPG_ElecEdge> ownerEdges = m_Outgoing.GetElement(ownerIndex);
            if (!ownerEdges)
                continue;

            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < ownerEdges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = ownerEdges[edgeIndex];
                if (!edge || (edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;

                int targetDegree = 0;
                if (!indegree.Find(edge.m_TargetNodeId, targetDegree))
                    continue;
                indegree.Set(edge.m_TargetNodeId, targetDegree + 1);
            }
        }

        int degreeIndex;
        for (degreeIndex = 0; degreeIndex < indegree.Count(); degreeIndex = degreeIndex + 1)
        {
            if (indegree.GetElement(degreeIndex) == 0)
                ready.Insert(indegree.GetKey(degreeIndex));
        }

        int readyHead = 0;
        while (readyHead < ready.Count())
        {
            string currentId = ready[readyHead];
            readyHead = readyHead + 1;
            solveOrder.Insert(currentId);

            ref array<ref LFPG_ElecEdge> currentEdges;
            if (!m_Outgoing.Find(currentId, currentEdges) || !currentEdges)
                continue;

            int currentEdgeIndex;
            for (currentEdgeIndex = 0; currentEdgeIndex < currentEdges.Count(); currentEdgeIndex = currentEdgeIndex + 1)
            {
                LFPG_ElecEdge currentEdge = currentEdges[currentEdgeIndex];
                if (!currentEdge || (currentEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;

                int remainingDegree = 0;
                if (!indegree.Find(currentEdge.m_TargetNodeId, remainingDegree))
                    continue;
                remainingDegree = remainingDegree - 1;
                indegree.Set(currentEdge.m_TargetNodeId, remainingDegree);
                if (remainingDegree == 0)
                    ready.Insert(currentEdge.m_TargetNodeId);
            }
        }

        return solveOrder.Count() == m_Nodes.Count();
        #else
        return false;
        #endif
    }

    protected void ResetSolveStaging(array<string> solveOrder)
    {
        #ifdef SERVER
        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < solveOrder.Count(); nodeIndex = nodeIndex + 1)
        {
            ref LFPG_ElecNode node;
            if (!m_Nodes.Find(solveOrder[nodeIndex], node) || !node)
                continue;

            node.m_SolveDemand = 0.0;
            node.m_SolveSoftRatio = 0.0;
            node.m_SolveInputPower = 0.0;
            node.m_SolveOutputPower = 0.0;
            node.m_SolveRealOutput = 0.0;
            node.m_SolveLoadRatio = 0.0;
            node.m_SolvePowered = false;
            node.m_SolveOverloaded = false;
            node.m_SolveGateClosed = node.m_GateClosed;
        }

        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            ref array<ref LFPG_ElecEdge> edges = m_Outgoing.GetElement(ownerIndex);
            if (!edges)
                continue;
            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < edges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = edges[edgeIndex];
                if (!edge)
                    continue;
                edge.m_SolveDemand = 0.0;
                edge.m_SolveSoftDemand = 0.0;
                edge.m_SolveAllocatedPower = 0.0;
            }
        }
        #endif
    }

    protected bool ResolveSolveGateClosed(string nodeId, LFPG_ElecNode node)
    {
        #ifdef SERVER
        if (!node || !node.m_IsGated)
            return false;

        bool gateClosed = node.m_GateClosed;
        EntityAI gateEntity = LFPG_DeviceRegistry.Get().FindById(nodeId);
        if (!gateEntity)
            gateEntity = LFPG_DeviceAPI.ResolveVanillaDevice(nodeId);

        if (!gateEntity)
            return gateClosed;

        gateClosed = !LFPG_DeviceAPI.IsGateOpen(gateEntity);
        if (gateClosed && !LFPG_DeviceAPI.IsGateControlPowerIndependent(gateEntity) && !node.m_GateClosed)
        {
            // A power-dependent gate may report closed only because the last
            // committed solution browned it out. Preserve its commanded-open
            // demand while an upstream overload exists.
            if (HasUpstreamOverload(nodeId))
                gateClosed = false;
        }
        return gateClosed;
        #else
        return false;
        #endif
    }

    protected void BuildFrozenDemand(array<string> solveOrder)
    {
        #ifdef SERVER
        // Resolve every gate command before the potential-supply snapshot is
        // built. The snapshot and both solve passes must observe the same gate
        // state, including a gate changed by the event that triggered this solve.
        int gateIndex;
        for (gateIndex = 0; gateIndex < solveOrder.Count(); gateIndex = gateIndex + 1)
        {
            string gateId = solveOrder[gateIndex];
            ref LFPG_ElecNode gateNode;
            if (m_Nodes.Find(gateId, gateNode) && gateNode && gateNode.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
                gateNode.m_SolveGateClosed = ResolveSolveGateClosed(gateId, gateNode);
        }

        // Supplier weights are topology/capacity-only and immutable for the
        // entire solve. No allocation produced below can change them.
        BuildPotentialSupplySnapshot();

        int reverseIndex;
        for (reverseIndex = solveOrder.Count() - 1; reverseIndex >= 0; reverseIndex = reverseIndex - 1)
        {
            string nodeId = solveOrder[reverseIndex];
            ref LFPG_ElecNode node;
            if (!m_Nodes.Find(nodeId, node) || !node)
                continue;

            float requestedDemand = 0.0;
            float requestedSoft = 0.0;

            if (node.m_DeviceType == LFPG_DeviceType.CONSUMER || node.m_DeviceType == LFPG_DeviceType.CAMERA)
            {
                requestedDemand = node.m_Consumption;
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                float downstreamDemand = 0.0;
                float downstreamSoft = 0.0;
                ref array<ref LFPG_ElecEdge> outEdges;
                if (m_Outgoing.Find(nodeId, outEdges) && outEdges)
                {
                    int outIndex;
                    for (outIndex = 0; outIndex < outEdges.Count(); outIndex = outIndex + 1)
                    {
                        LFPG_ElecEdge outEdge = outEdges[outIndex];
                        if (!outEdge || (outEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                            continue;
                        m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                        downstreamDemand = downstreamDemand + outEdge.m_SolveDemand;
                        downstreamSoft = downstreamSoft + outEdge.m_SolveSoftDemand;
                    }
                }

                if (node.m_SolveGateClosed)
                {
                    float gateBaseDemand = LFPG_GATE_PROBE_DEMAND;
                    if (node.m_Consumption > gateBaseDemand)
                        gateBaseDemand = node.m_Consumption;
                    requestedSoft = node.m_SoftDemand;
                    requestedDemand = gateBaseDemand + requestedSoft;
                }
                else
                {
                    float downstreamHard = downstreamDemand - downstreamSoft;
                    if (downstreamHard < 0.0)
                        downstreamHard = 0.0;

                    float hardDemand = downstreamHard + node.m_Consumption;
                    float virtualOffset = node.m_VirtualGeneration;
                    if (virtualOffset > hardDemand)
                        virtualOffset = hardDemand;

                    requestedSoft = downstreamSoft + node.m_SoftDemand;
                    requestedDemand = hardDemand - virtualOffset + requestedSoft;

                    // Throughput capacity is a demand ceiling. When a battery
                    // contributes soft charging demand, shed soft demand first.
                    if (node.m_MaxOutput > LFPG_PROPAGATION_EPSILON && requestedDemand > node.m_MaxOutput)
                    {
                        float excess = requestedDemand - node.m_MaxOutput;
                        requestedDemand = node.m_MaxOutput;
                        requestedSoft = requestedSoft - excess;
                        if (requestedSoft < 0.0)
                            requestedSoft = 0.0;
                    }
                }
            }

            if (requestedDemand < 0.0)
                requestedDemand = 0.0;
            node.m_SolveDemand = requestedDemand;

            if (requestedDemand > LFPG_PROPAGATION_EPSILON && requestedSoft > LFPG_PROPAGATION_EPSILON)
            {
                node.m_SolveSoftRatio = requestedSoft / requestedDemand;
                if (node.m_SolveSoftRatio > 1.0)
                    node.m_SolveSoftRatio = 1.0;
            }

            ref array<ref LFPG_ElecEdge> inEdges;
            if (!m_Incoming.Find(nodeId, inEdges) || !inEdges)
                continue;

            int inIndex;
            for (inIndex = 0; inIndex < inEdges.Count(); inIndex = inIndex + 1)
            {
                LFPG_ElecEdge inEdge = inEdges[inIndex];
                if (!inEdge || (inEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;

                m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                float supplyShare = GetIncomingSupplyShare(nodeId, inEdge);
                inEdge.m_SolveDemand = requestedDemand * supplyShare;
                inEdge.m_SolveSoftDemand = inEdge.m_SolveDemand * node.m_SolveSoftRatio;
            }
        }
        #endif
    }

    protected void AllocateFrozenOutput(string nodeId, LFPG_ElecNode node, float availableOutput)
    {
        #ifdef SERVER
        if (!node)
            return;
        if (availableOutput < 0.0)
            availableOutput = 0.0;

        ref array<ref LFPG_ElecEdge> outEdges;
        if (!m_Outgoing.Find(nodeId, outEdges) || !outEdges)
        {
            node.m_SolveLoadRatio = 0.0;
            node.m_SolveOverloaded = false;
            return;
        }

        float totalDemand = 0.0;
        float totalSoft = 0.0;
        int edgeIndex;
        for (edgeIndex = 0; edgeIndex < outEdges.Count(); edgeIndex = edgeIndex + 1)
        {
            LFPG_ElecEdge demandEdge = outEdges[edgeIndex];
            if (!demandEdge || (demandEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            totalDemand = totalDemand + demandEdge.m_SolveDemand;
            totalSoft = totalSoft + demandEdge.m_SolveSoftDemand;
        }

        float totalHard = totalDemand - totalSoft;
        if (totalHard < 0.0)
            totalHard = 0.0;
        bool overloaded = totalHard > availableOutput + LFPG_PROPAGATION_EPSILON;

        float softSurplus = 0.0;
        if (!overloaded && totalSoft > LFPG_PROPAGATION_EPSILON)
        {
            softSurplus = availableOutput - totalHard;
            if (softSurplus < 0.0)
                softSurplus = 0.0;
            if (softSurplus > totalSoft)
                softSurplus = totalSoft;
        }

        float totalAllocated = 0.0;
        for (edgeIndex = 0; edgeIndex < outEdges.Count(); edgeIndex = edgeIndex + 1)
        {
            LFPG_ElecEdge allocEdge = outEdges[edgeIndex];
            if (!allocEdge || (allocEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;

            float allocation = 0.0;
            if (!overloaded)
            {
                float edgeHard = allocEdge.m_SolveDemand - allocEdge.m_SolveSoftDemand;
                if (edgeHard < 0.0)
                    edgeHard = 0.0;
                allocation = edgeHard;
                if (allocEdge.m_SolveSoftDemand > LFPG_PROPAGATION_EPSILON && totalSoft > LFPG_PROPAGATION_EPSILON)
                {
                    allocation = allocation + (softSurplus * allocEdge.m_SolveSoftDemand / totalSoft);
                }
            }
            allocEdge.m_SolveAllocatedPower = allocation;
            totalAllocated = totalAllocated + allocation;
        }

        node.m_SolveOverloaded = overloaded;
        if (availableOutput > LFPG_PROPAGATION_EPSILON)
        {
            node.m_SolveLoadRatio = totalAllocated / availableOutput;
            if (node.m_SolveLoadRatio > 100.0)
                node.m_SolveLoadRatio = 100.0;
        }
        else
        {
            node.m_SolveLoadRatio = 0.0;
        }
        #endif
    }

    protected void BuildFrozenDelivery(array<string> solveOrder)
    {
        #ifdef SERVER
        int orderIndex;
        for (orderIndex = 0; orderIndex < solveOrder.Count(); orderIndex = orderIndex + 1)
        {
            string nodeId = solveOrder[orderIndex];
            ref LFPG_ElecNode node;
            if (!m_Nodes.Find(nodeId, node) || !node)
                continue;

            float inputPower = 0.0;
            ref array<ref LFPG_ElecEdge> inEdges;
            if (m_Incoming.Find(nodeId, inEdges) && inEdges)
            {
                int inIndex;
                for (inIndex = 0; inIndex < inEdges.Count(); inIndex = inIndex + 1)
                {
                    LFPG_ElecEdge inEdge = inEdges[inIndex];
                    if (!inEdge || (inEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                        continue;
                    m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                    inputPower = inputPower + inEdge.m_SolveAllocatedPower;
                }
            }
            node.m_SolveInputPower = inputPower;

            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                node.m_SolvePowered = node.m_Powered;
                if (node.m_SolvePowered)
                    node.m_SolveRealOutput = node.m_MaxOutput;
                node.m_SolveOutputPower = node.m_SolveRealOutput;
                AllocateFrozenOutput(nodeId, node, node.m_SolveRealOutput);
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                float effectiveInput = inputPower + node.m_VirtualGeneration;
                if (effectiveInput > LFPG_PROPAGATION_EPSILON)
                {
                    if (node.m_Consumption > LFPG_PROPAGATION_EPSILON)
                    {
                        if (effectiveInput + LFPG_PROPAGATION_EPSILON >= node.m_Consumption)
                        {
                            node.m_SolvePowered = true;
                            node.m_SolveRealOutput = effectiveInput - node.m_Consumption;
                            if (node.m_SolveRealOutput < 0.0)
                                node.m_SolveRealOutput = 0.0;
                        }
                    }
                    else
                    {
                        node.m_SolvePowered = true;
                        node.m_SolveRealOutput = effectiveInput;
                    }
                }

                if (node.m_MaxOutput > LFPG_PROPAGATION_EPSILON && node.m_SolveRealOutput > node.m_MaxOutput)
                    node.m_SolveRealOutput = node.m_MaxOutput;

                float availableForDownstream = node.m_SolveRealOutput;
                if (node.m_SolveGateClosed)
                    availableForDownstream = 0.0;
                AllocateFrozenOutput(nodeId, node, availableForDownstream);

                // Preserve the public graph contract: PASSTHROUGH output fields
                // carry advertised demand upstream; edge allocations carry real
                // delivered power downstream.
                node.m_SolveOutputPower = node.m_SolveDemand;

                // Off and intentionally closed devices are not overloads.
                if (node.m_SolveRealOutput < LFPG_PROPAGATION_EPSILON || node.m_SolveGateClosed)
                {
                    node.m_SolveOverloaded = false;
                    node.m_SolveLoadRatio = 0.0;
                }
            }
            else if (node.m_DeviceType == LFPG_DeviceType.CONSUMER || node.m_DeviceType == LFPG_DeviceType.CAMERA)
            {
                if (node.m_Consumption > LFPG_PROPAGATION_EPSILON)
                    node.m_SolvePowered = inputPower + LFPG_PROPAGATION_EPSILON >= node.m_Consumption;
                else
                    node.m_SolvePowered = inputPower > LFPG_PROPAGATION_EPSILON;
            }
            else
            {
                node.m_SolvePowered = inputPower > LFPG_PROPAGATION_EPSILON;
            }
        }
        #endif
    }

    protected void CommitSolveState(array<string> solveOrder)
    {
        #ifdef SERVER
        // Commit all graph data first. Entity callbacks therefore observe one
        // internally consistent solution even though entity sync is sequential.
        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            ref array<ref LFPG_ElecEdge> edges = m_Outgoing.GetElement(ownerIndex);
            if (!edges)
                continue;
            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < edges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = edges[edgeIndex];
                if (!edge)
                    continue;
                edge.m_Demand = edge.m_SolveDemand;
                edge.m_AllocatedPower = edge.m_SolveAllocatedPower;
            }
        }

        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < solveOrder.Count(); nodeIndex = nodeIndex + 1)
        {
            ref LFPG_ElecNode node;
            if (!m_Nodes.Find(solveOrder[nodeIndex], node) || !node)
                continue;

            node.m_InputPower = node.m_SolveInputPower;
            node.m_PrevInputPower = node.m_SolveInputPower;
            node.m_OutputPower = node.m_SolveOutputPower;
            node.m_LastStableOutput = node.m_SolveOutputPower;
            node.m_Powered = node.m_SolvePowered;
            node.m_Overloaded = node.m_SolveOverloaded;
            node.m_LoadRatio = node.m_SolveLoadRatio;
            node.m_SoftDemandRatio = node.m_SolveSoftRatio;
            node.m_GateClosed = node.m_SolveGateClosed;
            node.m_LastEpoch = m_CurrentEpoch;
        }

        for (nodeIndex = 0; nodeIndex < solveOrder.Count(); nodeIndex = nodeIndex + 1)
        {
            string nodeId = solveOrder[nodeIndex];
            ref LFPG_ElecNode syncNode;
            if (m_Nodes.Find(nodeId, syncNode) && syncNode)
                SyncNodeToEntity(nodeId, syncNode);
        }
        #endif
    }

    protected bool ValidateFrozenSolution(array<string> solveOrder)
    {
        #ifdef SERVER
        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < solveOrder.Count(); nodeIndex = nodeIndex + 1)
        {
            string nodeId = solveOrder[nodeIndex];
            ref LFPG_ElecNode node;
            if (!m_Nodes.Find(nodeId, node) || !node)
                return false;

            // NaN is the only floating-point value that is not equal to itself.
            if (node.m_SolveDemand != node.m_SolveDemand || node.m_SolveInputPower != node.m_SolveInputPower || node.m_SolveRealOutput != node.m_SolveRealOutput)
                return false;
            if (node.m_SolveDemand < 0.0 || node.m_SolveInputPower < 0.0 || node.m_SolveRealOutput < 0.0)
                return false;

            float outgoingAllocated = 0.0;
            ref array<ref LFPG_ElecEdge> outEdges;
            if (m_Outgoing.Find(nodeId, outEdges) && outEdges)
            {
                int edgeIndex;
                for (edgeIndex = 0; edgeIndex < outEdges.Count(); edgeIndex = edgeIndex + 1)
                {
                    LFPG_ElecEdge edge = outEdges[edgeIndex];
                    if (!edge || (edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                        continue;
                    if (edge.m_SolveDemand != edge.m_SolveDemand || edge.m_SolveAllocatedPower != edge.m_SolveAllocatedPower)
                        return false;
                    if (edge.m_SolveDemand < 0.0 || edge.m_SolveAllocatedPower < 0.0)
                        return false;
                    if (edge.m_SolveAllocatedPower > edge.m_SolveDemand + LFPG_PROPAGATION_EPSILON)
                        return false;
                    outgoingAllocated = outgoingAllocated + edge.m_SolveAllocatedPower;
                }
            }

            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                float sourceLimit = 0.0;
                if (node.m_SolvePowered)
                    sourceLimit = node.m_MaxOutput;
                if (outgoingAllocated > sourceLimit + LFPG_PROPAGATION_EPSILON)
                    return false;
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                float passthroughLimit = node.m_SolveRealOutput;
                if (node.m_SolveGateClosed)
                    passthroughLimit = 0.0;
                if (outgoingAllocated > passthroughLimit + LFPG_PROPAGATION_EPSILON)
                    return false;
            }
            else if (node.m_DeviceType == LFPG_DeviceType.CONSUMER || node.m_DeviceType == LFPG_DeviceType.CAMERA)
            {
                bool shouldBePowered = false;
                if (node.m_Consumption > LFPG_PROPAGATION_EPSILON)
                    shouldBePowered = node.m_SolveInputPower + LFPG_PROPAGATION_EPSILON >= node.m_Consumption;
                else
                    shouldBePowered = node.m_SolveInputPower > LFPG_PROPAGATION_EPSILON;
                if (node.m_SolvePowered != shouldBePowered)
                    return false;
            }
        }
        return true;
        #else
        return false;
        #endif
    }

    protected void CommitSolveFailSafe()
    {
        #ifdef SERVER
        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            ref array<ref LFPG_ElecEdge> edges = m_Outgoing.GetElement(ownerIndex);
            if (!edges)
                continue;
            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < edges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = edges[edgeIndex];
                if (!edge)
                    continue;
                edge.m_Demand = 0.0;
                edge.m_AllocatedPower = 0.0;
            }
        }

        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            string nodeId = m_Nodes.GetKey(nodeIndex);
            ref LFPG_ElecNode node = m_Nodes.GetElement(nodeIndex);
            if (!node)
                continue;

            node.m_InputPower = 0.0;
            node.m_PrevInputPower = 0.0;
            node.m_Overloaded = false;
            node.m_LoadRatio = 0.0;
            node.m_SoftDemandRatio = 0.0;
            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                if (node.m_Powered)
                    node.m_OutputPower = node.m_MaxOutput;
                else
                    node.m_OutputPower = 0.0;
                node.m_LastStableOutput = node.m_OutputPower;
            }
            else
            {
                node.m_Powered = false;
                node.m_OutputPower = 0.0;
                node.m_LastStableOutput = 0.0;
            }
            node.m_LastEpoch = m_CurrentEpoch;
        }

        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            string syncId = m_Nodes.GetKey(nodeIndex);
            ref LFPG_ElecNode syncNode = m_Nodes.GetElement(nodeIndex);
            if (syncNode)
                SyncNodeToEntity(syncId, syncNode);
        }
        #endif
    }

    protected void ClearSolvedDirtyState()
    {
        #ifdef SERVER
        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            ref LFPG_ElecNode node = m_Nodes.GetElement(nodeIndex);
            if (!node)
                continue;
            node.m_Dirty = false;
            node.m_InQueue = false;
            node.m_DirtyMask = 0;
            node.m_RequeueCount = 0;
        }
        m_DirtyQueue.Clear();
        m_DirtyQueueHead = 0;
        m_DeferredRequeue.Clear();
        m_RequeueEpoch.Clear();
        #endif
    }

    // ===========================
    // Sprint 4.3: Entity sync
    // ===========================

    protected void SyncNodeToEntity(string nodeId, LFPG_ElecNode node)
    {
        #ifdef SERVER
        if (!node)
            return;

        EntityAI entObj = LFPG_DeviceRegistry.Get().FindById(nodeId);
        if (!entObj)
        {
            entObj = LFPG_DeviceAPI.ResolveVanillaDevice(nodeId);
        }

        // v0.7.43 (Fix 3): NetworkID fallback when registry ref is stale.
        // DeviceRegistry may lose valid refs when DayZ recreates the C++
        // backing of an entity (streaming, initialization race).
        // NetworkID (engine identity) survives this. If re-resolved,
        // auto-register to prevent future misses.
        if (!entObj)
        {
            int cachedNetLow = 0;
            int cachedNetHigh = 0;
            bool hasNetLow = m_NodeNetLow.Find(nodeId, cachedNetLow);
            bool hasNetHigh = m_NodeNetHigh.Find(nodeId, cachedNetHigh);
            if (hasNetLow && hasNetHigh)
            {
                if (cachedNetLow != 0 || cachedNetHigh != 0)
                {
                    Object rawObj = g_Game.GetObjectByNetworkId(cachedNetLow, cachedNetHigh);
                    entObj = EntityAI.Cast(rawObj);
                    if (entObj)
                    {
                        LFPG_DeviceRegistry.Get().Register(entObj, nodeId);
                    }
                }
            }
        }

        if (!entObj)
        {
            // [DIAG PT-CHAIN] Punto 5a: Entity resolution failed
            if (LFPG_DIAG_PT_CHAIN && node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                string ptLog5a = "[PT-CHAIN] SyncToEntity FAILED: entity NULL for ";
                ptLog5a = ptLog5a + nodeId;
                ptLog5a = ptLog5a + " type=PASSTHROUGH";
                ptLog5a = ptLog5a + " powered=" + node.m_Powered.ToString();
                ptLog5a = ptLog5a + " input=" + node.m_InputPower.ToString();
                ptLog5a = ptLog5a + " output=" + node.m_OutputPower.ToString();
                LFPG_Util.Warn(ptLog5a);
            }
            return;
        }

        if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
        {
            // v1.0: Sync load ratio + overloaded bool to source entity
            float loadDelta = node.m_LoadRatio - node.m_LastSyncedLoadRatio;
            if (loadDelta < 0.0)
            {
                loadDelta = -loadDelta;
            }
            if (loadDelta > 0.01)
            {
                LFPG_DeviceAPI.SetLoadRatio(entObj, node.m_LoadRatio);

                if (loadDelta > LFPG_LOAD_TELEM_DELTA)
                {
                    string loadState = "NORMAL";
                    if (node.m_LoadRatio >= LFPG_LOAD_CRITICAL_THRESHOLD)
                    {
                        loadState = "OVERLOADED";
                    }
                    string telemMsg = "[LoadTelem] " + nodeId;
                    telemMsg = telemMsg + " load=" + node.m_LoadRatio.ToString();
                    telemMsg = telemMsg + " prev=" + node.m_LastSyncedLoadRatio.ToString();
                    telemMsg = telemMsg + " cap=" + node.m_MaxOutput.ToString();
                    telemMsg = telemMsg + " state=" + loadState;
                    LFPG_Util.Info(telemMsg);
                }

                node.m_LastSyncedLoadRatio = node.m_LoadRatio;
            }
            LFPG_DeviceAPI.SetOverloaded(entObj, node.m_Overloaded);
            return;
        }

        // v1.0: PASSTHROUGH nodes sync powered + overloaded for cable visuals.
        if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
        {
            // [DIAG PT-CHAIN] Punto 5b: PASSTHROUGH entity sync
            if (LFPG_DIAG_PT_CHAIN)
            {
                string ptLog5b = "[PT-CHAIN] SyncToEntity: ";
                ptLog5b = ptLog5b + nodeId;
                ptLog5b = ptLog5b + " powered=" + node.m_Powered.ToString();
                ptLog5b = ptLog5b + " input=" + node.m_InputPower.ToString();
                ptLog5b = ptLog5b + " output=" + node.m_OutputPower.ToString();
                ptLog5b = ptLog5b + " entity=" + entObj.GetType();
                LFPG_Util.Info(ptLog5b);
            }
            LFPG_DeviceAPI.SetPowered(entObj, node.m_Powered);
            LFPG_DeviceAPI.SetOverloaded(entObj, node.m_Overloaded);
            return;
        }

        LFPG_DeviceAPI.SetPowered(entObj, node.m_Powered);
        #endif
    }

    // ===========================
    // v0.7.32 (Bloque C): Consumer Zombie Validation
    // ===========================

    // Periodic sweep to detect consumers claiming m_Powered=true without
    // sufficient incoming power. This catches edge cases where the graph
    // changes without propagating to a downstream consumer (timing gaps,
    // entity deletion races, or hypothetical propagation bugs).
    //
    // Design:
    //   - Runs when queue is empty (either post-drain or idle).
    //   - Throttled by LFPG_CONSUMER_VALIDATE_TICK_INTERVAL ticks.
    //     Uses m_ValidateTickCount (increments on every ProcessDirtyQueue call,
    //     including early-returns) so validation fires even during idle periods.
    //   - Budgeted: checks LFPG_VALIDATE_BATCH_SIZE (32) nodes per call.
    //   - Round-robin via m_ValidateNodeIdx — full sweep of N nodes takes
    //     ceil(N/32) invocations × interval each = predictable spread.
    //   - Leaf consumers are repaired directly. PASSTHROUGH mismatches are
    //     re-enqueued because their output, allocations, gates, and neighbours
    //     must converge together; changing only m_Powered leaves stale branches.
    //
    // Power source: reads inEdge.m_AllocatedPower directly (NOT via
    //   GetEdgeAllocatedPower). Intentional: the helper's equal-split
    //   fallback can mask brownout edges (m_AllocatedPower=0 but fallback
    //   returns non-zero). Direct read gives ground truth in steady-state.
    //
    // Cost: O(batch * avg_incoming_edges). At 32 nodes/tick with avg 2
    //       incoming edges = ~64 edge checks per tick. Negligible.
    //
    // Returns number of zombies fixed in this batch.
    protected int ValidateConsumerStates(int edgeBudget)
    {
        #ifdef SERVER
        int nodeTotal = m_Nodes.Count();
        if (nodeTotal <= 0)
            return 0;

        // Tick interval gate (advances even when queue is idle)
        int tickDelta = m_ValidateTickCount - m_LastValidateTick;
        if (tickDelta < LFPG_CONSUMER_VALIDATE_TICK_INTERVAL)
            return 0;

        // Clamp round-robin index if graph shrank
        if (m_ValidateNodeIdx >= nodeTotal)
            m_ValidateNodeIdx = 0;

        int batchSize = LFPG_VALIDATE_BATCH_SIZE;
        if (batchSize > nodeTotal)
            batchSize = nodeTotal;

        int checked = 0;
        int fixed = 0;

        while (checked < batchSize)
        {
            // Preserve the round-robin cursor and stop between nodes. As with
            // AllocateOutput, one node may finish after crossing the budget.
            if (m_EdgesVisitedThisEpoch >= edgeBudget)
                break;

            // Bounds check before access
            if (m_ValidateNodeIdx >= nodeTotal)
                m_ValidateNodeIdx = 0;

            string nodeId = m_Nodes.GetKey(m_ValidateNodeIdx);
            ref LFPG_ElecNode node = m_Nodes.GetElement(m_ValidateNodeIdx);

            m_ValidateNodeIdx = m_ValidateNodeIdx + 1;
            checked = checked + 1;

            if (!node)
                continue;

            // Only validate non-source nodes (CONSUMER, CAMERA, PASSTHROUGH)
            // B5 fix: Previously excluded PASSTHROUGH — a zombie passthrough
            // (m_Powered=true without sufficient input) was never autocorrected.
            if (node.m_DeviceType == LFPG_DeviceType.SOURCE || node.m_DeviceType == LFPG_DeviceType.UNKNOWN)
                continue;

            // Skip nodes currently in the dirty queue — they have pending updates
            if (node.m_InQueue || node.m_Dirty)
                continue;

            // Sum actual incoming power from enabled edges.
            // Reads m_AllocatedPower directly — see method doc above for rationale.
            float incomingPower = 0.0;
            bool hasAnyIncoming = false;

            ref array<ref LFPG_ElecEdge> inEdges;
            if (m_Incoming.Find(nodeId, inEdges) && inEdges)
            {
                int ii;
                for (ii = 0; ii < inEdges.Count(); ii = ii + 1)
                {
                    m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                    ref LFPG_ElecEdge inEdge = inEdges[ii];
                    if (!inEdge)
                        continue;

                    if ((inEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                        continue;

                    hasAnyIncoming = true;
                    float edgePower = inEdge.m_AllocatedPower;
                    if (edgePower < 0.0)
                    {
                        edgePower = 0.0;
                    }
                    incomingPower = incomingPower + edgePower;
                }
            }

            // v0.7.32: Final NaN/negative guard on accumulated sum.
            // Matches ProcessDirtyQueue pattern (v0.7.27 Audit 5).
            if (incomingPower < 0.0)
            {
                incomingPower = 0.0;
            }

            // Determine if this consumer should actually be powered. Batteries
            // and other storage-backed PASSTHROUGH nodes can power themselves
            // from virtual generation even with no incoming allocation.
            bool shouldBePowered = false;
            float effectivePower = incomingPower;
            bool hasUsablePower = hasAnyIncoming;
            if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH && node.m_VirtualGeneration > LFPG_PROPAGATION_EPSILON)
            {
                effectivePower = effectivePower + node.m_VirtualGeneration;
                hasUsablePower = true;
            }

            if (hasUsablePower)
            {
                if (node.m_Consumption > LFPG_PROPAGATION_EPSILON)
                {
                    // Declared consumption: needs enough power to meet demand
                    if (effectivePower + LFPG_PROPAGATION_EPSILON >= node.m_Consumption)
                    {
                        shouldBePowered = true;
                    }
                }
                else
                {
                    // Legacy consumer (consumption=0): any power suffices
                    if (effectivePower > LFPG_PROPAGATION_EPSILON)
                    {
                        shouldBePowered = true;
                    }
                }
            }

            bool powerMismatch = false;
            if (node.m_Powered != shouldBePowered)
            {
                powerMismatch = true;
            }

            // PASSTHROUGH state cannot be repaired by changing m_Powered alone:
            // output power, edge allocations, gate state, and both neighbours may
            // also need updates. Queue a normal propagation pass so the complete
            // electrical state converges atomically.
            if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH && powerMismatch)
            {
                MarkNodeDirty(nodeId, LFPG_DIRTY_INPUT);

                fixed = fixed + 1;
                m_ValidateFixCount = m_ValidateFixCount + 1;

                string ptRepairMsg = "[ElecGraph] Passthrough repair queued: " + nodeId;
                ptRepairMsg = ptRepairMsg + " powered=" + node.m_Powered.ToString();
                ptRepairMsg = ptRepairMsg + " shouldBe=" + shouldBePowered.ToString();
                ptRepairMsg = ptRepairMsg + " incoming=" + incomingPower.ToString();
                ptRepairMsg = ptRepairMsg + " virtual=" + node.m_VirtualGeneration.ToString();
                LFPG_Util.Warn(ptRepairMsg);
                continue;
            }

            // v5.0 debug: trace BatteryCharger node state on each visit
            if (nodeId.IndexOf("BatteryCharger") >= 0)
            {
                if (LFPG_LOG_LEVEL >= 1)
                {
                    string dbgMsg = "[Charger] Validate " + nodeId;
                    dbgMsg = dbgMsg + " powered=" + node.m_Powered.ToString();
                    dbgMsg = dbgMsg + " shouldBe=" + shouldBePowered.ToString();
                    dbgMsg = dbgMsg + " inPower=" + incomingPower.ToString();
                    dbgMsg = dbgMsg + " consumption=" + node.m_Consumption.ToString();
                    dbgMsg = dbgMsg + " hasIncoming=" + hasAnyIncoming.ToString();
                    LFPG_Util.Info(dbgMsg);
                }
            }

            // v0.7.38 (RC-09 safety net): Bidirectional zombie detection.
            // Original: only caught powered=true when shouldBePowered=false (zombie).
            // Added: also catch powered=false when shouldBePowered=true (dark consumer).
            // Dark consumers arise from race conditions like B1 (topology change
            // with stale per-edge allocations during same-epoch processing).
            if (node.m_Powered && !shouldBePowered)
            {
                // Classic zombie: powered but shouldn't be
                node.m_Powered = false;
                node.m_InputPower = incomingPower;

                SyncNodeToEntity(nodeId, node);

                fixed = fixed + 1;
                m_ValidateFixCount = m_ValidateFixCount + 1;

                string zombMsg = "[ElecGraph] Zombie node fixed: " + nodeId;
                zombMsg = zombMsg + " type=" + node.m_DeviceType.ToString();
                zombMsg = zombMsg + " inPower=" + incomingPower.ToString();
                zombMsg = zombMsg + " consumption=" + node.m_Consumption.ToString();
                zombMsg = zombMsg + " totalFixes=" + m_ValidateFixCount.ToString();
                LFPG_Util.Warn(zombMsg);
            }
            else if (!node.m_Powered && shouldBePowered)
            {
                // Inverse zombie (dark consumer): should be powered but isn't.
                // Caused by topology race conditions (B1) or stale allocation reads.
                node.m_Powered = true;
                node.m_InputPower = incomingPower;

                SyncNodeToEntity(nodeId, node);

                fixed = fixed + 1;
                m_ValidateFixCount = m_ValidateFixCount + 1;

                string darkMsg = "[ElecGraph] Dark node fixed: " + nodeId;
                darkMsg = darkMsg + " type=" + node.m_DeviceType.ToString();
                darkMsg = darkMsg + " inPower=" + incomingPower.ToString();
                darkMsg = darkMsg + " consumption=" + node.m_Consumption.ToString();
                darkMsg = darkMsg + " totalFixes=" + m_ValidateFixCount.ToString();
                LFPG_Util.Warn(darkMsg);
            }
            else if (node.m_Powered && shouldBePowered)
            {
                // v4.7: Vanilla consumer energy maintenance.
                // Vanilla CompEM drains energy over time. If not refilled,
                // CanWork() fails and the device stops working even though
                // the LFPG graph still considers it powered.
                // Only applies to vanilla devices (ID starts with "vp:").
                if (nodeId.IndexOf("vp:") == 0)
                {
                    EntityAI vanEnt = LFPG_DeviceRegistry.Get().FindById(nodeId);
                    if (!vanEnt)
                    {
                        vanEnt = LFPG_DeviceAPI.ResolveVanillaDevice(nodeId);
                    }
                    if (vanEnt)
                    {
                        ComponentEnergyManager vanEm = vanEnt.GetCompEM();
                        if (vanEm)
                        {
                            // Keep LFPG's synthetic pool consumable by vanilla
                            // zero-storage Spotlight appliances. Without this,
                            // their 50s DeviceUpdate clamps the pool to zero and
                            // briefly turns the client light off.
                            LFPG_DeviceAPI.EnsureSyntheticVanillaCapacity(vanEnt, vanEm);

                            // v4.9 (BugFix): Top up energy regardless of IsSwitchedOn.
                            // Matches SetPowered v4.9 — energy injection is required
                            // for CanSwitchOn() to return true. Without this, vanilla
                            // CompEM drain depletes the pool and the "Turn On" action
                            // disappears even though LFPG considers it powered.
                            // Residual energy is harmless while IsSwitchedOn=false:
                            // the work cycle is stopped, and we never drain via
                            // SetEnergy(0) (which would cause BatteryCharger to pull
                            // from car battery).
                            float vanEnergy = vanEm.GetEnergy();
                            if (vanEnergy < LFPG_VANILLA_ENERGY_POOL * 0.5)
                            {
                                vanEm.SetEnergy(LFPG_VANILLA_ENERGY_POOL);
                            }

                            // v5.3: BatteryCharger direct charging with delta-time.
                            // Vanilla charging requires HasElectricitySource()
                            // (PlugThisInto crashes). LFPG bypasses vanilla and
                            // charges the attached CarBattery/TruckBattery directly.
                            // Slot is "LargeBattery" (both battery types register it).
                            // Uses AddEnergy (not SetEnergy) so the full vanilla
                            // event chain fires: OnEnergyAdded → ConvertEnergyToQuantity
                            // → SetQuantityNormalized → SetVariableMask(VARIABLE_QUANTITY).
                            // The inventory bar reads m_VarQuantity, not m_EM.m_Energy.
                            // Delta-time via m_ChargerLastChargeSec for consistent rate.
                            string battChargerCls = "BatteryCharger";
                            if (vanEnt.IsKindOf(battChargerCls))
                            {
                                bool chargerOn = vanEm.IsSwitchedOn();
                                EntityAI carBat = vanEnt.FindAttachmentBySlotName("LargeBattery");

                                if (chargerOn && carBat)
                                {
                                    ComponentEnergyManager batEm = carBat.GetCompEM();
                                    if (batEm)
                                    {
                                        float batEnergy = batEm.GetEnergy();
                                        float batMax = batEm.GetEnergyMax();
                                        if (batEnergy < batMax)
                                        {
                                            // Delta-time: seconds since last charge visit
                                            float nowSec = g_Game.GetTime() * 0.001;
                                            float lastSec = 0.0;
                                            bool hasLast = m_ChargerLastChargeSec.Find(nodeId, lastSec);
                                            float deltaSec = nowSec - lastSec;

                                            // First visit or too soon: seed timestamp only
                                            if (!hasLast || deltaSec < 0.1)
                                            {
                                                m_ChargerLastChargeSec.Set(nodeId, nowSec);
                                            }
                                            else
                                            {
                                                // Cap at 10s to prevent burst after server lag
                                                if (deltaSec > 10.0)
                                                    deltaSec = 10.0;

                                                float chargeAmount = LFPG_CHARGER_ENERGY_PER_SEC * deltaSec;
                                                // v5.3: Use AddEnergy instead of SetEnergy.
                                                // SetEnergy() only writes m_Energy — it does
                                                // NOT trigger OnEnergyAdded(), so vanilla's
                                                // full sync chain never fires:
                                                //   AddEnergy(delta)
                                                //     → CompEM.OnEnergyAdded()
                                                //     → VehicleBattery.OnEnergyAdded()
                                                //       → super → ItemBase.OnEnergyAdded()
                                                //         → ConvertEnergyToQuantity()
                                                //           → SetQuantityNormalized()
                                                //             → SetQuantity()
                                                //               → SetVariableMask(VARIABLE_QUANTITY)
                                                //       → SetSynchDirty()  [syncs m_EM.m_Energy]
                                                //
                                                // The inventory bar reads m_VarQuantity, which
                                                // is a SEPARATE SyncVar from m_EM.m_Energy.
                                                // Only ConvertEnergyToQuantity updates it.
                                                // AddEnergy auto-clamps to [0, energyMax].
                                                batEm.AddEnergy(chargeAmount);

                                                // Safety net: force m_VarQuantity sync.
                                                // ConvertEnergyToQuantity (inside AddEnergy
                                                // chain) only fires if vanilla config has
                                                // convertEnergyToQuantity=1. If that flag
                                                // is absent, m_VarQuantity stays stale.
                                                // Explicit SetQuantityNormalized guarantees
                                                // the inventory bar updates in ALL cases.
                                                ItemBase batItem = ItemBase.Cast(carBat);
                                                if (batItem)
                                                {
                                                    float eNorm = batEm.GetEnergy0To1();
                                                    batItem.SetQuantityNormalized(eNorm);
                                                }

                                                m_ChargerLastChargeSec.Set(nodeId, nowSec);

                                                float afterEnergy = batEm.GetEnergy();
                                                string chgLog = "[Charger] Charged ";
                                                chgLog = chgLog + nodeId;
                                                chgLog = chgLog + ": ";
                                                chgLog = chgLog + batEnergy.ToString();
                                                chgLog = chgLog + " -> ";
                                                chgLog = chgLog + afterEnergy.ToString();
                                                chgLog = chgLog + " / ";
                                                chgLog = chgLog + batMax.ToString();
                                                chgLog = chgLog + " dt=";
                                                chgLog = chgLog + deltaSec.ToString();
                                                LFPG_Util.Info(chgLog);
                                            }
                                        }
                                    }
                                }
                                else
                                {
                                    // Clean up timestamp when charger is off or battery removed
                                    m_ChargerLastChargeSec.Remove(nodeId);
                                    string skipLog = "[Charger] Skip ";
                                    skipLog = skipLog + nodeId;
                                    skipLog = skipLog + " switchedOn=";
                                    skipLog = skipLog + chargerOn.ToString();
                                    bool hasBat = carBat != null;
                                    skipLog = skipLog + " hasBat=";
                                    skipLog = skipLog + hasBat.ToString();
                                    LFPG_Util.Info(skipLog);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Wrap index for next invocation
        if (m_ValidateNodeIdx >= nodeTotal)
            m_ValidateNodeIdx = 0;

        // Update the interval only when this invocation actually examined a
        // node; an exhausted propagation budget must not starve validation.
        if (checked > 0)
            m_LastValidateTick = m_ValidateTickCount;

        if (fixed > 0)
        {
            string valMsg = "[ElecGraph] ValidateConsumers: ";
            valMsg = valMsg + fixed.ToString() + " zombies fixed this batch, tick=" + m_ValidateTickCount.ToString();
            LFPG_Util.Info(valMsg);
        }

        return fixed;
        #else
        return 0;
        #endif
    }

    // ===========================
    // Sprint 4.2+4.3: Warmup helpers
    // ===========================

    override void PopulateAllNodeElecStates()
    {
        #ifdef SERVER
        int ni;
        for (ni = 0; ni < m_Nodes.Count(); ni = ni + 1)
        {
            string nid = m_Nodes.GetKey(ni);
            ref LFPG_ElecNode node = m_Nodes.GetElement(ni);
            if (!node)
                continue;

            EntityAI obj = LFPG_DeviceRegistry.Get().FindById(nid);
            if (!obj)
            {
                obj = LFPG_DeviceAPI.ResolveVanillaDevice(nid);
            }
            if (!obj)
                continue;

            if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
            {
                node.m_MaxOutput = LFPG_DeviceAPI.GetCapacity(obj);
                bool sourceOn = false;
                if (LFPG_DeviceAPI.IsSource(obj))
                {
                    sourceOn = LFPG_DeviceAPI.GetSourceOn(obj);
                }
                else
                {
                    ComponentEnergyManager em = obj.GetCompEM();
                    if (em)
                        sourceOn = em.IsWorking();
                }
                node.m_Powered = sourceOn;
            }
            else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                // v0.7.33 (Fix #22): Read max throughput capacity from device.
                // Previously hardcoded to 0.0 (infinite passthrough).
                // Now uses LFPG_GetCapacity if available, else default constant.
                node.m_MaxOutput = LFPG_DeviceAPI.GetCapacity(obj);
                if (node.m_MaxOutput < LFPG_PROPAGATION_EPSILON)
                {
                    node.m_MaxOutput = LFPG_DEFAULT_PASSTHROUGH_CAPACITY;
                }
                // v0.7.47: PASSTHROUGH self-consumption (CeilingLight pattern).
                // Splitter returns 0.0 explicitly → no regression.
                node.m_Consumption = LFPG_DeviceAPI.GetConsumption(obj);
                // P1: Cache gate capability for bulk warmup path.
                node.m_IsGated = LFPG_DeviceAPI.IsGateCapable(obj);
                // v2.0: Battery fields (m_VirtualGeneration, m_SoftDemand) are
                // set by NetworkManager battery timer on first tick (~5s).
                // Warmup gap is acceptable (same pattern as solar panels).
                // Sprint 2 timer calls RefreshBatteryNodeState() which reads
                // stored energy from entity and computes virtualGen + softDemand.
                // For non-battery PASSTHROUGH, fields remain 0.0 (default).
            }
            else if (node.m_DeviceType == LFPG_DeviceType.CONSUMER || node.m_DeviceType == LFPG_DeviceType.CAMERA)
            {
                node.m_Consumption = LFPG_DeviceAPI.GetConsumption(obj);
            }
        }

        string infoPopulate = "[ElecGraph] PopulateAllNodeElecStates: " + m_Nodes.Count().ToString() + " nodes";
        LFPG_Util.Info(infoPopulate);
        #endif
    }

    override void RefreshSourceState(string nodeId)
    {
        #ifdef SERVER
        LFPG_ElecNode node = GetNode(nodeId);
        if (!node)
            return;

        if (node.m_DeviceType != LFPG_DeviceType.SOURCE && node.m_DeviceType != LFPG_DeviceType.CONSUMER && node.m_DeviceType != LFPG_DeviceType.CAMERA)
            return;

        EntityAI obj = LFPG_DeviceRegistry.Get().FindById(nodeId);
        if (!obj)
        {
            obj = LFPG_DeviceAPI.ResolveVanillaDevice(nodeId);
        }
        if (!obj)
            return;

        if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
        {
            bool sourceOn = false;
            ComponentEnergyManager em;
            if (LFPG_DeviceAPI.IsSource(obj))
            {
                sourceOn = LFPG_DeviceAPI.GetSourceOn(obj);
            }
            else
            {
                em = obj.GetCompEM();
                if (em)
                    sourceOn = em.IsWorking();
            }

            node.m_Powered = sourceOn;
            node.m_MaxOutput = LFPG_DeviceAPI.GetCapacity(obj);
            MarkNodeDirty(nodeId, LFPG_DIRTY_INTERNAL);
            return;
        }

        node.m_Consumption = LFPG_DeviceAPI.GetConsumption(obj);
        MarkNodeDirty(nodeId, LFPG_DIRTY_INTERNAL);
        MarkUpstreamNodesDirty(nodeId);
        #endif
    }

    // ===========================
    // Sprint 4.2+4.3: Internal helpers
    // ===========================

    protected void MarkUpstreamNodesDirty(string nodeId)
    {
        #ifdef SERVER
        array<string> upstreamQueue = new array<string>;
        map<string, bool> upstreamVisited = new map<string, bool>;
        int queueHead = 0;
        string currentId;
        array<ref LFPG_ElecEdge> incomingEdges;
        int edgeIndex;
        LFPG_ElecEdge incomingEdge;
        string upstreamId;
        bool alreadyVisited;
        LFPG_ElecNode upstreamNode;

        upstreamQueue.Insert(nodeId);
        upstreamVisited.Set(nodeId, true);

        while (queueHead < upstreamQueue.Count())
        {
            currentId = upstreamQueue[queueHead];
            queueHead = queueHead + 1;
            incomingEdges = GetIncoming(currentId);
            if (!incomingEdges)
                continue;

            for (edgeIndex = 0; edgeIndex < incomingEdges.Count(); edgeIndex = edgeIndex + 1)
            {
                incomingEdge = incomingEdges[edgeIndex];
                if (!incomingEdge || incomingEdge.m_SourceNodeId == "")
                    continue;

                upstreamId = incomingEdge.m_SourceNodeId;
                alreadyVisited = false;
                upstreamVisited.Find(upstreamId, alreadyVisited);
                if (alreadyVisited)
                    continue;

                upstreamVisited.Set(upstreamId, true);
                upstreamNode = GetNode(upstreamId);
                if (!upstreamNode)
                    continue;

                MarkNodeDirty(upstreamId, LFPG_DIRTY_INPUT);
                if (upstreamNode.m_DeviceType != LFPG_DeviceType.SOURCE)
                    upstreamQueue.Insert(upstreamId);
            }
        }
        #endif
    }

    // Reset a target's cached supplier snapshot and re-evaluate every enabled
    // incoming supplier. This is required for topology mutations: after an
    // input edge is removed/disabled, that former supplier can no longer run
    // GetIncomingSupplyShare and notify the siblings that their fractions grew.
    protected void MarkIncomingSuppliersDirty(string targetId)
    {
        #ifdef SERVER
        ref LFPG_ElecNode targetNode;
        if (m_Nodes.Find(targetId, targetNode) && targetNode)
        {
            targetNode.m_ActiveSupplierCount = -1;
            targetNode.m_ActiveSupplierCapacity = -1.0;
        }

        ref array<ref LFPG_ElecEdge> supplierEdges;
        if (!m_Incoming.Find(targetId, supplierEdges) || !supplierEdges)
            return;

        int isi;
        for (isi = 0; isi < supplierEdges.Count(); isi = isi + 1)
        {
            ref LFPG_ElecEdge supplierEdge = supplierEdges[isi];
            if (!supplierEdge)
                continue;
            if ((supplierEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            if (supplierEdge.m_SourceNodeId == "")
                continue;

            MarkNodeDirty(supplierEdge.m_SourceNodeId, LFPG_DIRTY_TOPOLOGY);
        }
        #endif
    }

    protected int CountEnabledOutgoing(string nodeId)
    {
        ref array<ref LFPG_ElecEdge> outEdges;
        if (!m_Outgoing.Find(nodeId, outEdges) || !outEdges)
            return 0;

        int count = 0;
        int oi;
        for (oi = 0; oi < outEdges.Count(); oi = oi + 1)
        {
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            ref LFPG_ElecEdge edge = outEdges[oi];
            if (edge && (edge.m_Flags & LFPG_EDGE_ENABLED) != 0)
            {
                count = count + 1;
            }
        }
        return count;
    }

    // Compact state dump emitted only when the cycle guard fires. It provides
    // enough information to identify the unstable node and direction of the
    // feedback without enabling per-edge hot-path logging on production servers.
    protected void LogRequeueLimit(string nodeId, LFPG_ElecNode node)
    {
        if (!node)
            return;

        string entityType = "unresolved";
        EntityAI entity = LFPG_DeviceRegistry.Get().FindById(nodeId);
        if (!entity)
        {
            entity = LFPG_DeviceAPI.ResolveVanillaDevice(nodeId);
        }
        if (entity)
        {
            entityType = entity.GetType();
        }

        int incomingCount = 0;
        float incomingAllocated = 0.0;
        float incomingDemand = 0.0;
        ref array<ref LFPG_ElecEdge> diagInEdges;
        if (m_Incoming.Find(nodeId, diagInEdges) && diagInEdges)
        {
            int dii;
            for (dii = 0; dii < diagInEdges.Count(); dii = dii + 1)
            {
                ref LFPG_ElecEdge diagIn = diagInEdges[dii];
                if (!diagIn || (diagIn.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                incomingCount = incomingCount + 1;
                incomingAllocated = incomingAllocated + diagIn.m_AllocatedPower;
                incomingDemand = incomingDemand + diagIn.m_Demand;
            }
        }

        int outgoingCount = 0;
        float outgoingAllocated = 0.0;
        float outgoingDemand = 0.0;
        ref array<ref LFPG_ElecEdge> diagOutEdges;
        if (m_Outgoing.Find(nodeId, diagOutEdges) && diagOutEdges)
        {
            int doi;
            for (doi = 0; doi < diagOutEdges.Count(); doi = doi + 1)
            {
                ref LFPG_ElecEdge diagOut = diagOutEdges[doi];
                if (!diagOut || (diagOut.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                outgoingCount = outgoingCount + 1;
                outgoingAllocated = outgoingAllocated + diagOut.m_AllocatedPower;
                outgoingDemand = outgoingDemand + diagOut.m_Demand;
            }
        }

        string msg = "[ElecGraph] Requeue limit reached id=" + nodeId;
        msg = msg + " class=" + entityType;
        msg = msg + " epoch=" + m_CurrentEpoch.ToString();
        msg = msg + " rq=" + node.m_RequeueCount.ToString();
        msg = msg + " mask=" + node.m_DirtyMask.ToString();
        msg = msg + " powered=" + node.m_Powered.ToString();
        msg = msg + " overloaded=" + node.m_Overloaded.ToString();
        msg = msg + " gateClosed=" + node.m_GateClosed.ToString();
        msg = msg + " input=" + node.m_InputPower.ToString();
        msg = msg + " demandSignal=" + node.m_LastStableOutput.ToString();
        msg = msg + " suppliers=" + node.m_ActiveSupplierCount.ToString();
        msg = msg + " supplierCap=" + node.m_ActiveSupplierCapacity.ToString();
        msg = msg + " in=" + incomingCount.ToString();
        msg = msg + "/" + incomingAllocated.ToString();
        msg = msg + "/" + incomingDemand.ToString();
        msg = msg + " out=" + outgoingCount.ToString();
        msg = msg + "/" + outgoingAllocated.ToString();
        msg = msg + "/" + outgoingDemand.ToString();
        LFPG_Util.Warn(msg);
    }

    // Gate state used by potential-supply analysis. Power-independent control
    // commands can be read live; dependent gates use the graph's last accepted
    // state so a transient allocation result cannot alter this epoch's weights.
    protected bool IsPotentialSupplyGateClosed(string nodeId, LFPG_ElecNode node)
    {
        if (!node || !node.m_IsGated)
            return false;

        if (m_DeterministicSolveActive)
            return node.m_SolveGateClosed;

        bool gateClosed = node.m_GateClosed;
        EntityAI gateEntity = LFPG_DeviceRegistry.Get().FindById(nodeId);
        if (gateEntity && LFPG_DeviceAPI.IsGateControlPowerIndependent(gateEntity))
        {
            gateClosed = !LFPG_DeviceAPI.IsGateOpen(gateEntity);
        }
        return gateClosed;
    }

    // Build one immutable source-to-load snapshot for the current propagation
    // epoch. Kahn order guarantees that every enabled predecessor is finalized
    // before its target. This handles arbitrary legal DAG shapes (chains,
    // branches, reconvergence, multiple sources, split/combine/split) without
    // recursive call state or an order-dependent visiting guard.
    protected void BuildPotentialSupplySnapshot()
    {
        if (m_PotentialSupplySnapshotReady)
            return;

        m_PotentialSupplyCache.Clear();
        m_PotentialContributorCache.Clear();
        m_PotentialContributorCapacity.Clear();

        ref map<string, int> indegree = new map<string, int>;
        ref array<string> readyQueue = new array<string>;

        int nodeIndex;
        for (nodeIndex = 0; nodeIndex < m_Nodes.Count(); nodeIndex = nodeIndex + 1)
        {
            indegree.Set(m_Nodes.GetKey(nodeIndex), 0);
        }

        int ownerIndex;
        for (ownerIndex = 0; ownerIndex < m_Outgoing.Count(); ownerIndex = ownerIndex + 1)
        {
            string ownerId = m_Outgoing.GetKey(ownerIndex);
            int ignoredOwnerDegree = 0;
            if (!indegree.Find(ownerId, ignoredOwnerDegree))
                indegree.Set(ownerId, 0);

            ref array<ref LFPG_ElecEdge> ownerEdges = m_Outgoing.GetElement(ownerIndex);
            if (!ownerEdges)
                continue;

            int edgeIndex;
            for (edgeIndex = 0; edgeIndex < ownerEdges.Count(); edgeIndex = edgeIndex + 1)
            {
                LFPG_ElecEdge edge = ownerEdges[edgeIndex];
                if (!edge || (edge.m_Flags & LFPG_EDGE_ENABLED) == 0 || edge.m_TargetNodeId == "")
                    continue;

                int targetDegree = 0;
                indegree.Find(edge.m_TargetNodeId, targetDegree);
                indegree.Set(edge.m_TargetNodeId, targetDegree + 1);
            }
        }

        int degreeIndex;
        for (degreeIndex = 0; degreeIndex < indegree.Count(); degreeIndex = degreeIndex + 1)
        {
            if (indegree.GetElement(degreeIndex) == 0)
                readyQueue.Insert(indegree.GetKey(degreeIndex));
        }

        int queueHead = 0;
        int processedCount = 0;
        while (queueHead < readyQueue.Count())
        {
            string currentId = readyQueue[queueHead];
            queueHead = queueHead + 1;
            processedCount = processedCount + 1;

            float result = 0.0;
            ref array<string> contributors = new array<string>;
            ref LFPG_ElecNode node;
            if (m_Nodes.Find(currentId, node) && node)
            {
                if (node.m_DeviceType == LFPG_DeviceType.SOURCE)
                {
                    if (node.m_Powered && node.m_MaxOutput > LFPG_PROPAGATION_EPSILON)
                    {
                        result = node.m_MaxOutput;
                        string sourceKey = "S:" + currentId;
                        contributors.Insert(sourceKey);
                        m_PotentialContributorCapacity.Set(sourceKey, node.m_MaxOutput);
                    }
                }
                else if (node.m_DeviceType == LFPG_DeviceType.PASSTHROUGH && !IsPotentialSupplyGateClosed(currentId, node))
                {
                    result = node.m_VirtualGeneration;
                    ref array<ref LFPG_ElecEdge> supplyInEdges;
                    if (m_Incoming.Find(currentId, supplyInEdges) && supplyInEdges)
                    {
                        int supplyIndex;
                        for (supplyIndex = 0; supplyIndex < supplyInEdges.Count(); supplyIndex = supplyIndex + 1)
                        {
                            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                            LFPG_ElecEdge supplyEdge = supplyInEdges[supplyIndex];
                            if (!supplyEdge || (supplyEdge.m_Flags & LFPG_EDGE_ENABLED) == 0 || supplyEdge.m_SourceNodeId == "")
                                continue;

                            float upstreamCapacity = 0.0;
                            m_PotentialSupplyCache.Find(supplyEdge.m_SourceNodeId, upstreamCapacity);
                            result = result + upstreamCapacity;
                        }
                    }

                    result = result - node.m_Consumption;
                    if (result < 0.0)
                        result = 0.0;
                    if (node.m_MaxOutput > LFPG_PROPAGATION_EPSILON && result > node.m_MaxOutput)
                        result = node.m_MaxOutput;

                    // Preserve the previous contributor semantics: a node whose
                    // own consumption absorbs all potential has no deliverable
                    // roots, while every live root is retained once across a
                    // reconvergent graph.
                    if (result > LFPG_PROPAGATION_EPSILON)
                    {
                        if (node.m_VirtualGeneration > LFPG_PROPAGATION_EPSILON)
                        {
                            string virtualKey = "V:" + currentId;
                            contributors.Insert(virtualKey);
                            m_PotentialContributorCapacity.Set(virtualKey, node.m_VirtualGeneration);
                        }

                        if (supplyInEdges)
                        {
                            int contributorEdgeIndex;
                            for (contributorEdgeIndex = 0; contributorEdgeIndex < supplyInEdges.Count(); contributorEdgeIndex = contributorEdgeIndex + 1)
                            {
                                LFPG_ElecEdge contributorEdge = supplyInEdges[contributorEdgeIndex];
                                if (!contributorEdge || (contributorEdge.m_Flags & LFPG_EDGE_ENABLED) == 0 || contributorEdge.m_SourceNodeId == "")
                                    continue;

                                float contributorUpstreamCapacity = 0.0;
                                m_PotentialSupplyCache.Find(contributorEdge.m_SourceNodeId, contributorUpstreamCapacity);
                                if (contributorUpstreamCapacity <= LFPG_PROPAGATION_EPSILON)
                                    continue;

                                ref array<string> upstreamContributors;
                                if (!m_PotentialContributorCache.Find(contributorEdge.m_SourceNodeId, upstreamContributors) || !upstreamContributors)
                                    continue;

                                int upstreamIndex;
                                for (upstreamIndex = 0; upstreamIndex < upstreamContributors.Count(); upstreamIndex = upstreamIndex + 1)
                                {
                                    string contributorKey = upstreamContributors[upstreamIndex];
                                    if (contributorKey != "" && contributors.Find(contributorKey) < 0)
                                        contributors.Insert(contributorKey);
                                }
                            }
                        }
                    }
                }
            }

            m_PotentialSupplyCache.Set(currentId, result);
            m_PotentialContributorCache.Set(currentId, contributors);

            ref array<ref LFPG_ElecEdge> currentEdges;
            if (!m_Outgoing.Find(currentId, currentEdges) || !currentEdges)
                continue;

            int currentEdgeIndex;
            for (currentEdgeIndex = 0; currentEdgeIndex < currentEdges.Count(); currentEdgeIndex = currentEdgeIndex + 1)
            {
                LFPG_ElecEdge currentEdge = currentEdges[currentEdgeIndex];
                if (!currentEdge || (currentEdge.m_Flags & LFPG_EDGE_ENABLED) == 0 || currentEdge.m_TargetNodeId == "")
                    continue;

                int remainingDegree = 0;
                if (!indegree.Find(currentEdge.m_TargetNodeId, remainingDegree))
                    continue;
                remainingDegree = remainingDegree - 1;
                indegree.Set(currentEdge.m_TargetNodeId, remainingDegree);
                if (remainingDegree == 0)
                    readyQueue.Insert(currentEdge.m_TargetNodeId);
            }
        }

        if (processedCount < indegree.Count())
        {
            m_RuntimeCycleAuditRequested = true;
            string snapshotMsg = "[ElecGraph] Potential supply snapshot incomplete processed=";
            snapshotMsg = snapshotMsg + processedCount.ToString();
            snapshotMsg = snapshotMsg + " nodes=" + indegree.Count().ToString();
            LFPG_Util.Warn(snapshotMsg);

            // Keep the current epoch conservative and total: unresolved nodes
            // expose zero capacity until the scheduler-boundary audit repairs
            // the corrupt topology on the next tick.
            for (degreeIndex = 0; degreeIndex < indegree.Count(); degreeIndex = degreeIndex + 1)
            {
                string unresolvedId = indegree.GetKey(degreeIndex);
                float resolvedCapacity = 0.0;
                if (!m_PotentialSupplyCache.Find(unresolvedId, resolvedCapacity))
                {
                    m_PotentialSupplyCache.Set(unresolvedId, 0.0);
                    ref array<string> noContributors = new array<string>;
                    m_PotentialContributorCache.Set(unresolvedId, noContributors);
                }
            }
        }

        m_PotentialSupplySnapshotReady = true;
    }

    // Return the maximum deliverable capacity from the immutable current-epoch
    // snapshot, excluding mutable demand/allocation/output state.
    protected float GetPotentialSupplyCapacity(string nodeId)
    {
        BuildPotentialSupplySnapshot();
        float cachedCapacity = 0.0;
        m_PotentialSupplyCache.Find(nodeId, cachedCapacity);
        return cachedCapacity;
    }

    // Return unique active generation roots that can contribute to this node.
    // Source and virtual-generation keys remain distinct.
    protected array<string> GetPotentialSupplyContributors(string nodeId)
    {
        BuildPotentialSupplySnapshot();
        ref array<string> cachedContributors;
        if (m_PotentialContributorCache.Find(nodeId, cachedContributors) && cachedContributors)
            return cachedContributors;

        ref array<string> emptyContributors = new array<string>;
        m_PotentialContributorCache.Set(nodeId, emptyContributors);
        return emptyContributors;
    }

    // Capacity-weighted share for an incoming edge of a multi-source target.
    // The weights are stable for the whole epoch and independent of allocations,
    // so binary overload cannot remove a supplier and then add it back on the
    // following evaluation. Unequal generators/batteries contribute according
    // to their deliverable branch capacity rather than an incorrect equal split.
    // A generation root reachable through multiple reconvergent branches is
    // divided across those branches first, so a splitter cannot duplicate it.
    protected float GetIncomingSupplyShare(string targetId, LFPG_ElecEdge evaluatingEdge)
    {
        ref array<ref LFPG_ElecEdge> inEdges;
        if (!m_Incoming.Find(targetId, inEdges) || !inEdges || !evaluatingEdge)
            return 1.0;

        float totalCapacity = 0.0;
        float evaluatingCapacity = 0.0;
        int activeCount = 0;

        // Sum the deliverable potential of all live target inputs each unique
        // generation root can reach. Its capacity is later divided in that
        // proportion, so a narrow and a wide reconvergent path receive useful
        // weights without multiplying the root. Virtual generation has its own key.
        ref map<string, float> contributorReachCapacity = new map<string, float>;
        int sci;
        for (sci = 0; sci < inEdges.Count(); sci = sci + 1)
        {
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            ref LFPG_ElecEdge countEdge = inEdges[sci];
            if (!countEdge)
                continue;
            if ((countEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            if (countEdge.m_SourceNodeId == "")
                continue;

            float countCapacity = GetPotentialSupplyCapacity(countEdge.m_SourceNodeId);
            if (countCapacity <= LFPG_PROPAGATION_EPSILON)
                continue;

            activeCount = activeCount + 1;
            ref array<string> countContributors = GetPotentialSupplyContributors(countEdge.m_SourceNodeId);
            if (!countContributors)
                continue;

            int cci;
            for (cci = 0; cci < countContributors.Count(); cci = cci + 1)
            {
                string countKey = countContributors[cci];
                if (countKey == "")
                    continue;

                float existingReach = 0.0;
                contributorReachCapacity.Find(countKey, existingReach);
                contributorReachCapacity.Set(countKey, existingReach + countCapacity);
            }
        }

        // Build an effective weight for each incoming branch. The unique-source
        // contribution is capped by that branch's own deliverable potential so
        // a narrow passthrough/gate cannot advertise upstream capacity it cannot
        // physically relay.
        int swi;
        for (swi = 0; swi < inEdges.Count(); swi = swi + 1)
        {
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            ref LFPG_ElecEdge weightEdge = inEdges[swi];
            if (!weightEdge)
                continue;
            if ((weightEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;
            if (weightEdge.m_SourceNodeId == "")
                continue;

            float branchPotential = GetPotentialSupplyCapacity(weightEdge.m_SourceNodeId);
            if (branchPotential <= LFPG_PROPAGATION_EPSILON)
                continue;

            float branchWeight = 0.0;
            ref array<string> weightContributors = GetPotentialSupplyContributors(weightEdge.m_SourceNodeId);
            if (weightContributors)
            {
                int wci;
                for (wci = 0; wci < weightContributors.Count(); wci = wci + 1)
                {
                    string weightKey = weightContributors[wci];
                    float contributorCapacity = 0.0;
                    float contributorReach = 0.0;
                    if (!m_PotentialContributorCapacity.Find(weightKey, contributorCapacity))
                        continue;
                    if (!contributorReachCapacity.Find(weightKey, contributorReach))
                        continue;
                    if (contributorReach <= LFPG_PROPAGATION_EPSILON)
                        continue;

                    branchWeight = branchWeight + (contributorCapacity * branchPotential / contributorReach);
                }
            }

            // Corrupt/legacy nodes without contributor metadata retain the
            // stable branch-potential fallback rather than losing all demand.
            if (branchWeight <= LFPG_PROPAGATION_EPSILON)
            {
                branchWeight = branchPotential;
            }
            if (branchWeight > branchPotential)
            {
                branchWeight = branchPotential;
            }

            totalCapacity = totalCapacity + branchWeight;
            if (weightEdge == evaluatingEdge)
            {
                evaluatingCapacity = branchWeight;
            }
        }

        // A stable capability transition may not alter target demand, so make
        // sibling suppliers recompute their shares. Unlike the previous logic,
        // this signal cannot change as a consequence of those recomputations.
        ref LFPG_ElecNode targetNode;
        if (m_Nodes.Find(targetId, targetNode) && targetNode)
        {
            int previousCount = targetNode.m_ActiveSupplierCount;
            float previousCapacity = targetNode.m_ActiveSupplierCapacity;
            targetNode.m_ActiveSupplierCount = activeCount;
            targetNode.m_ActiveSupplierCapacity = totalCapacity;

            float capacityDelta = totalCapacity - previousCapacity;
            if (capacityDelta < 0.0)
            {
                capacityDelta = 0.0 - capacityDelta;
            }

            bool supplierStateChanged = false;
            if (previousCount >= 0 && previousCount != activeCount)
            {
                supplierStateChanged = true;
            }
            if (previousCapacity >= 0.0 && capacityDelta > LFPG_PROPAGATION_EPSILON)
            {
                supplierStateChanged = true;
            }

            if (supplierStateChanged)
            {
                int sri;
                for (sri = 0; sri < inEdges.Count(); sri = sri + 1)
                {
                    ref LFPG_ElecEdge siblingEdge = inEdges[sri];
                    if (!siblingEdge)
                        continue;
                    if ((siblingEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                        continue;
                    if (siblingEdge.m_SourceNodeId == "" || siblingEdge.m_SourceNodeId == evaluatingEdge.m_SourceNodeId)
                        continue;

                    MarkNodeDirty(siblingEdge.m_SourceNodeId, LFPG_DIRTY_INPUT);
                }
            }
        }

        if (totalCapacity <= LFPG_PROPAGATION_EPSILON)
        {
            // Preserve full dormant demand on every path. Once any source is
            // enabled its positive potential capacity produces a real share.
            return 1.0;
        }
        if (evaluatingCapacity <= LFPG_PROPAGATION_EPSILON)
            return 0.0;

        return evaluatingCapacity / totalCapacity;
    }

    // True only when an enabled upstream path currently contains a node in
    // binary overload. Used to distinguish induced fail-safe gate closure from
    // ordinary source/gate shutdown without depending on current input power.
    protected bool HasUpstreamOverload(string nodeId)
    {
        ref array<string> overloadQueue = new array<string>;
        ref map<string, bool> overloadVisited = new map<string, bool>;
        overloadQueue.Insert(nodeId);
        overloadVisited.Set(nodeId, true);

        int overloadHead = 0;
        while (overloadHead < overloadQueue.Count())
        {
            string currentId = overloadQueue[overloadHead];
            overloadHead = overloadHead + 1;

            ref array<ref LFPG_ElecEdge> overloadInEdges;
            if (!m_Incoming.Find(currentId, overloadInEdges) || !overloadInEdges)
                continue;

            int oui;
            for (oui = 0; oui < overloadInEdges.Count(); oui = oui + 1)
            {
                m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                ref LFPG_ElecEdge overloadEdge = overloadInEdges[oui];
                if (!overloadEdge)
                    continue;
                if ((overloadEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                    continue;
                if (overloadEdge.m_SourceNodeId == "")
                    continue;

                ref LFPG_ElecNode upstreamNode;
                if (!m_Nodes.Find(overloadEdge.m_SourceNodeId, upstreamNode) || !upstreamNode)
                    continue;
                if (upstreamNode.m_Overloaded)
                {
                    // Ignore stale overload flags on branches that can no longer
                    // supply at all (for example a source switched off just
                    // before its queued graph evaluation clears telemetry).
                    float upstreamPotential = GetPotentialSupplyCapacity(overloadEdge.m_SourceNodeId);
                    if (upstreamPotential > LFPG_PROPAGATION_EPSILON)
                        return true;
                }

                bool alreadyVisited = false;
                overloadVisited.Find(overloadEdge.m_SourceNodeId, alreadyVisited);
                if (!alreadyVisited)
                {
                    overloadVisited.Set(overloadEdge.m_SourceNodeId, true);
                    overloadQueue.Insert(overloadEdge.m_SourceNodeId);
                }
            }
        }

        return false;
    }

    // v1.0: Binary power allocation (all-off policy).
    // If totalDemand > availableOutput → ALL edges get 0 (overloaded).
    // If totalDemand <= availableOutput → each edge gets its full demand.
    // Returns totalDemand (always — even when overloaded, for upstream demand signaling).
    protected float AllocateOutput(string nodeId, float availableOutput)
    {
        #ifdef SERVER
        ref array<ref LFPG_ElecEdge> outEdges;
        if (!m_Outgoing.Find(nodeId, outEdges) || !outEdges)
            return 0.0;

        int edgeCount = outEdges.Count();
        if (edgeCount <= 0)
            return 0.0;

        // Pass 1: Collect demands and compute total.
        // Store per-edge demand in edge.m_Demand for pass 2.
        // v2.0: Also track totalSoftDemand via target node m_SoftDemandRatio.
        // For non-battery networks, all ratios are 0.0 → totalSoftDemand stays 0.0.
        float totalDemand = 0.0;
        float totalSoftDemand = 0.0;
        float edgeDemand = 0.0;
        float edgeSoftPortion = 0.0;
        int ei;
        for (ei = 0; ei < edgeCount; ei = ei + 1)
        {
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            ref LFPG_ElecEdge edge = outEdges[ei];
            if (!edge)
                continue;
            if ((edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;

            edgeDemand = 0.0;
            edgeSoftPortion = 0.0;
            ref LFPG_ElecNode targetNode;
            if (m_Nodes.Find(edge.m_TargetNodeId, targetNode) && targetNode)
            {
                if (targetNode.m_DeviceType == LFPG_DeviceType.CONSUMER || targetNode.m_DeviceType == LFPG_DeviceType.CAMERA)
                {
                    edgeDemand = targetNode.m_Consumption;
                }
                else if (targetNode.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
                {
                    edgeDemand = targetNode.m_LastStableOutput;
                    if (edgeDemand < LFPG_PROPAGATION_EPSILON)
                    {
                        // Cold-start fallback: bootstrap demand estimate.
                        // Only use m_MaxOutput if the passthrough has downstream
                        // consumers to serve. A passthrough with no outgoing edges
                        // demands only its self-consumption (0 for Splitter/Combiner,
                        // N for CeilingLight). Without this check, an empty Combiner
                        // (cap=500) causes permanent false overload on a 50 u/s source.
                        // B3 fix: Only count ENABLED outgoing edges.
                        // Without this, disabled edges make ptHasDown=true
                        // and the fallback uses m_MaxOutput (200) as demand
                        // instead of consumption (0), inflating upstream load.

                        // v2.1: Gated PASSTHROUGH with gate closed cannot
                        // serve downstream, so cold-start must NOT inflate
                        // demand to m_MaxOutput OR to selfConsumption.
                        // Using consumption (e.g. 5.0 for PressurePad) creates
                        // a feedback loop: upstream allocates 5.0 → device
                        // powers on → cable green → wrong. Instead, use only
                        // the small probe trickle. The device stays unpowered
                        // (probe < consumption) but can re-evaluate its gate
                        // when toggled. On gate open, demand signal jumps to
                        // real value → upstream re-allocates → converges in
                        // 2-3 requeue cycles. Non-gated PASSTHROUGH (Splitter,
                        // Combiner, CeilingLight, Monitor) has m_GateClosed=
                        // false always → zero regression.
                        if (targetNode.m_GateClosed)
                        {
                            // v2.3: Gated PASSTHROUGH with self-consumption
                            // must demand at least its consumption so it powers
                            // up and can run its detection logic (raycast, step).
                            // Without this, probe=1.0 < consumption=5.0 → device
                            // stays unpowered → gate never opens → deadlock.
                            float gateSelf = targetNode.m_Consumption;
                            if (gateSelf > LFPG_GATE_PROBE_DEMAND)
                            {
                                edgeDemand = gateSelf;
                            }
                            else
                            {
                                edgeDemand = LFPG_GATE_PROBE_DEMAND;
                            }
                        }
                        else
                        {
                            bool ptHasDown = false;
                            ref array<ref LFPG_ElecEdge> ptOutEdges;
                            if (m_Outgoing.Find(edge.m_TargetNodeId, ptOutEdges) && ptOutEdges)
                            {
                                int pti;
                                for (pti = 0; pti < ptOutEdges.Count(); pti = pti + 1)
                                {
                                    m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
                                    if (!ptHasDown)
                                    {
                                        ref LFPG_ElecEdge ptEdge = ptOutEdges[pti];
                                        if (ptEdge && (ptEdge.m_Flags & LFPG_EDGE_ENABLED) != 0)
                                        {
                                            ptHasDown = true;
                                        }
                                    }
                                }
                            }

                            if (ptHasDown && targetNode.m_MaxOutput > LFPG_PROPAGATION_EPSILON)
                            {
                                edgeDemand = targetNode.m_MaxOutput;
                                // v2.4 (Battery oscillation fix): Cap cold-start
                                // estimate to what the source can actually provide.
                                // Without this, a battery with m_MaxOutput=120
                                // connected to a 50 u/s generator triggers overload
                                // on epoch 1 → allocation 0 → cold-start again → loop.
                                // Capping to availableOutput lets the first epoch
                                // converge without false overload. Real demand via
                                // m_LastStableOutput takes over from epoch 2 onward.
                                if (edgeDemand > availableOutput)
                                {
                                    edgeDemand = availableOutput;
                                }
                            }
                            else
                            {
                                edgeDemand = targetNode.m_Consumption;
                            }
                        }
                    }

                    // Stable, capacity-weighted multi-source demand sharing.
                    // A single input returns 1.0; inactive inputs return 0.0.
                    float supplyShare = GetIncomingSupplyShare(edge.m_TargetNodeId, edge);
                    edgeDemand = edgeDemand * supplyShare;

                    // v2.0: Track soft portion of this edge's demand.
                    // SoftDemandRatio is 0.0 for all non-battery PASSTHROUGH
                    // (Splitter, Combiner, CeilingLight, etc.) → no regression.
                    if (targetNode.m_SoftDemandRatio > LFPG_PROPAGATION_EPSILON)
                    {
                        edgeSoftPortion = edgeDemand * targetNode.m_SoftDemandRatio;
                    }
                }
            }

            edge.m_Demand = edgeDemand;
            totalDemand = totalDemand + edgeDemand;
            totalSoftDemand = totalSoftDemand + edgeSoftPortion;
        }

        // v2.0: Cache soft demand total for PDQ demand signal section.
        // Eliminates redundant outgoing edge iteration in PDQ.
        m_LastAllocSoftDemand = totalSoftDemand;

        // v2.0: Overload decision uses hard demand only.
        // Soft demand (battery charging) NEVER causes overload.
        // For non-battery networks: totalSoftDemand=0 → identical to before.
        float totalHardDemand = totalDemand - totalSoftDemand;
        if (totalHardDemand < 0.0)
        {
            totalHardDemand = 0.0;
        }
        bool overloaded = false;
        if (totalHardDemand > availableOutput + LFPG_PROPAGATION_EPSILON)
        {
            overloaded = true;
        }

        // Pass 2: Compute the FINAL allocation for every edge and compare it
        // with the previous final allocation exactly once.
        //
        // Previously this was split into a hard-allocation pass followed by a
        // soft-surplus pass. A stable soft-demand edge therefore changed from
        // (hard + soft) -> hard -> (hard + soft) on every evaluation. The
        // intermediate comparison permanently set m_AllocChanged even though
        // the final allocation was identical. Battery polling then kept every
        // upstream Combiner/Splitter dirty forever.
        //
        // totalHardDemand is the amount allocated before soft demand. Any
        // remaining capacity is distributed proportionally to soft demand in
        // this same pass, so change detection observes only final state.
        float softSurplus = 0.0;
        if (!overloaded && totalSoftDemand > LFPG_PROPAGATION_EPSILON)
        {
            softSurplus = availableOutput - totalHardDemand;
            if (softSurplus < 0.0)
            {
                softSurplus = 0.0;
            }
            if (softSurplus > totalSoftDemand)
            {
                softSurplus = totalSoftDemand;
            }
        }

        float totalAllocated = 0.0;
        int ai;
        for (ai = 0; ai < edgeCount; ai = ai + 1)
        {
            m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            ref LFPG_ElecEdge allocEdge = outEdges[ai];
            if (!allocEdge)
                continue;
            if ((allocEdge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;

            float oldAlloc = allocEdge.m_AllocatedPower;
            float newAlloc = 0.0;
            if (!overloaded)
            {
                ref LFPG_ElecNode allocTarget;
                float allocTargetRatio = 0.0;
                if (m_Nodes.Find(allocEdge.m_TargetNodeId, allocTarget) && allocTarget)
                {
                    allocTargetRatio = allocTarget.m_SoftDemandRatio;
                }

                float edgeSoft = allocEdge.m_Demand * allocTargetRatio;
                float edgeHard = allocEdge.m_Demand - edgeSoft;
                if (edgeHard < 0.0)
                {
                    edgeHard = 0.0;
                }

                newAlloc = edgeHard;
                if (edgeSoft > LFPG_PROPAGATION_EPSILON && softSurplus > LFPG_PROPAGATION_EPSILON && totalSoftDemand > LFPG_PROPAGATION_EPSILON)
                {
                    newAlloc = newAlloc + (softSurplus * edgeSoft / totalSoftDemand);
                }
            }
            allocEdge.m_AllocatedPower = newAlloc;
            totalAllocated = totalAllocated + newAlloc;

            // Track allocation change for Step 3 downstream re-enqueue.
            if (!m_AllocChanged)
            {
                float allocDelta = newAlloc - oldAlloc;
                if (allocDelta < 0.0)
                {
                    allocDelta = -allocDelta;
                }
                if (allocDelta > LFPG_PROPAGATION_EPSILON)
                {
                    m_AllocChanged = true;
                }
            }
        }

        // Update node load metrics.
        // v2.0: LoadRatio uses totalAllocated (hard+soft) for accurate display.
        // Overloaded flag uses totalHardDemand (soft never causes overload).
        ref LFPG_ElecNode srcNode;
        if (m_Nodes.Find(nodeId, srcNode) && srcNode)
        {
            if (srcNode.m_DeviceType == LFPG_DeviceType.SOURCE || srcNode.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            {
                // LoadRatio: actual usage / capacity (for inspector display + cable color)
                float capacity = srcNode.m_MaxOutput;
                if (srcNode.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
                {
                    capacity = availableOutput;
                }

                if (capacity > LFPG_PROPAGATION_EPSILON)
                {
                    float rawRatio = totalAllocated / capacity;
                    if (rawRatio < 0.0)
                    {
                        rawRatio = 0.0;
                    }
                    if (rawRatio > 100.0)
                    {
                        rawRatio = 100.0;
                    }
                    srcNode.m_LoadRatio = rawRatio;
                }
                else
                {
                    if (totalHardDemand > LFPG_PROPAGATION_EPSILON)
                    {
                        srcNode.m_LoadRatio = 100.0;
                    }
                    else
                    {
                        srcNode.m_LoadRatio = 0.0;
                    }
                }
                srcNode.m_Overloaded = overloaded;
            }
        }

        // v2.0: Return totalDemand (hard + soft) for upstream demand signaling.
        // The demand signal carries the full picture; the ratio separates them.
        return totalDemand;
        #else
        return 0.0;
        #endif
    }

    // v1.0: Get allocated power for a specific incoming edge.
    // If source is overloaded (all-off), returns 0 immediately.
    // Otherwise returns per-edge allocation, with equal-split fallback for cold-start.
    protected float GetEdgeAllocatedPower(LFPG_ElecEdge inEdge)
    {
        #ifdef SERVER
        if (!inEdge)
            return 0.0;

        ref LFPG_ElecNode srcNode;
        if (!m_Nodes.Find(inEdge.m_SourceNodeId, srcNode) || !srcNode)
            return 0.0;

        // v1.0: Source in overload → all downstream gets 0.
        if (srcNode.m_Overloaded)
            return 0.0;

        if (inEdge.m_AllocatedPower > LFPG_PROPAGATION_EPSILON)
            return inEdge.m_AllocatedPower;

        // v2.0.1: Gated PASSTHROUGH nodes (PushButton, PressurePad, Laser,
        // Counter) use m_OutputPower as demand signal, NOT real power.
        // When gate closes or upstream has no power, AllocateOutput zeroes
        // edge allocations. The fallback below would read the demand signal
        // from m_OutputPower and leak phantom power to downstream consumers,
        // causing a 1-frame flash. For gated devices, AllocateOutput always
        // runs (they are PASSTHROUGH), so m_AllocatedPower=0 is authoritative.
        // v2.2 (Fix Bug #4): Extended to ALL PASSTHROUGH, not just gated.
        // Any PASSTHROUGH sets m_OutputPower = demandSignal (not real power)
        // in the PDQ demand signal section. AllocateOutput always runs for
        // PASSTHROUGH (Step 2b), so m_AllocatedPower is authoritative.
        // The equal-split fallback would leak phantom power (demand signal)
        // to downstream consumers, showing them as powered when upstream
        // is dead. Cold-start cost: 1 extra convergence cycle (consumer
        // processes before PASSTHROUGH → sees 0 → PASSTHROUGH processes →
        // sets real allocation → consumer re-evaluates with correct data).
        if (srcNode.m_DeviceType == LFPG_DeviceType.PASSTHROUGH)
            return 0.0;

        // Fallback: equal split (cold-start / first pass before AllocateOutput runs)
        float srcOutput = srcNode.m_OutputPower;
        if (srcOutput < LFPG_PROPAGATION_EPSILON)
            return 0.0;

        int enabledOutCount = CountEnabledOutgoing(inEdge.m_SourceNodeId);
        if (enabledOutCount <= 0)
            return 0.0;

        return srcOutput / enabledOutCount;
        #else
        return 0.0;
        #endif
    }

    // T5 W1-F08: Lazy reset keyed by the epoch in which a node is touched.
    protected void EnsureRequeueEpoch(string nodeId, LFPG_ElecNode node)
    {
        #ifdef SERVER
        if (!node)
            return;

        int nodeEpoch = -1;
        bool hasEpoch = m_RequeueEpoch.Find(nodeId, nodeEpoch);
        if (!hasEpoch || nodeEpoch != m_CurrentEpoch)
        {
            node.m_RequeueCount = 0;
            m_RequeueEpoch.Set(nodeId, m_CurrentEpoch);
        }
        #endif
    }

    // =========================================================
    // Port-level power query (v1.3.1)
    //
    // Returns true if any incoming edge targeting the given port
    // on the given device has allocated power > 0 this epoch.
    //
    // Used by devices that need per-port awareness (e.g., RaidAlarm
    // Station uses input_2 as a trigger port distinct from the
    // always-on power feed on input_1).
    //
    // Safe to call from LFPG_SetPowered or any server-side context
    // after ProcessDirtyQueue has run for the current epoch.
    // =========================================================
    override bool IsPortReceivingPower(string deviceId, string portName)
    {
        ref array<ref LFPG_ElecEdge> inEdges;
        if (!m_Incoming.Find(deviceId, inEdges))
            return false;

        if (!inEdges)
            return false;

        int i;
        int count = inEdges.Count();
        for (i = 0; i < count; i = i + 1)
        {
            if (m_PropagationEdgeAccountingActive)
            {
                m_EdgesVisitedThisEpoch = m_EdgesVisitedThisEpoch + 1;
            }
            ref LFPG_ElecEdge edge = inEdges[i];
            if (!edge)
                continue;

            // B4 fix: Skip disabled edges. Without this, a stale
            // m_AllocatedPower from a previous epoch on a disabled
            // edge returns a false positive.
            if ((edge.m_Flags & LFPG_EDGE_ENABLED) == 0)
                continue;

            if (edge.m_TargetPort != portName)
                continue;

            // B4 fix: Use EPSILON for consistency with rest of graph.
            if (edge.m_AllocatedPower > LFPG_PROPAGATION_EPSILON)
                return true;
        }

        return false;
    }

    // v3.1: Enable/disable outgoing edges for a specific output port.
    // Used by MemoryCell to route power to output_0 or output_1.
    override void SetOutputPortEnabled(string deviceId, string portName, bool enabled)
    {
        #ifdef SERVER
        ref array<ref LFPG_ElecEdge> outEdges;
        if (!m_Outgoing.Find(deviceId, outEdges))
            return;

        if (!outEdges)
            return;

        int i;
        int count = outEdges.Count();
        bool anyChanged = false;
        for (i = 0; i < count; i = i + 1)
        {
            ref LFPG_ElecEdge edge = outEdges[i];
            if (!edge)
                continue;

            if (edge.m_SourcePort != portName)
                continue;

            bool wasEnabled = ((edge.m_Flags & LFPG_EDGE_ENABLED) != 0);
            if (enabled && !wasEnabled)
            {
                edge.m_Flags = edge.m_Flags | LFPG_EDGE_ENABLED;
                anyChanged = true;
            }
            else if (!enabled && wasEnabled)
            {
                edge.m_Flags = 0;
                // v3.1: Do NOT zero m_AllocatedPower here.
                // All readers (AllocateOutput, IsPortReceivingPower,
                // ValidateConsumerStates, PDQ input eval) check
                // LFPG_EDGE_ENABLED before reading allocation.
                // Zeroing here disrupts the demand signal mid-epoch,
                // causing upstream reallocation oscillation.
                // DIRTY_TOPOLOGY + forceDownstream ensures downstream
                // re-evaluates and sees the disabled edge via flag check.
                anyChanged = true;
            }

            if (wasEnabled != enabled)
            {
                MarkIncomingSuppliersDirty(edge.m_TargetNodeId);
            }
        }

        if (anyChanged)
        {
            MarkNodeDirty(deviceId, LFPG_DIRTY_TOPOLOGY);

            string dbg = "[ElecGraph] SetOutputPortEnabled: ";
            dbg = dbg + deviceId;
            dbg = dbg + " port=" + portName;
            dbg = dbg + " enabled=" + enabled.ToString();
            LFPG_Util.Debug(dbg);
        }
        #endif
    }
};
