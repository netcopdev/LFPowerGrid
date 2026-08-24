// =========================================================
// LF_PowerGrid - server-side registry deviceId -> EntityAI
// NOTE: kept minimal; avoids global ticks.
//
// v0.7.43 (Fix 1): Stale ref auto-prune in FindById.
// v0.7.44 (Level 4): Null filter in GetAll (hallazgo 1a),
//                     Count() diagnostic helper.
// =========================================================

class LFPG_DeviceRegistry
{
    protected static ref LFPG_DeviceRegistry s_Instance;

    protected ref map<string, EntityAI> m_ById;
    // Reverse ownership makes the registry a one-to-one identity map.  DayZ
    // calls EEInit before OnStoreLoad, so a persisted LFPG entity briefly owns
    // a generated ID and then restores its saved ID.  Without the reverse map
    // both keys survived and the graph could address one physical device as
    // two electrical nodes.
    protected ref map<EntityAI, string> m_IdByEntity;

    void LFPG_DeviceRegistry()
    {
        m_ById = new map<string, EntityAI>;
        m_IdByEntity = new map<EntityAI, string>;
    }

    static LFPG_DeviceRegistry Get()
    {
        if (!s_Instance)
            s_Instance = new LFPG_DeviceRegistry();
        return s_Instance;
    }

    static void Reset()
    {
        if (s_Instance)
        {
            s_Instance.m_ById.Clear();
            s_Instance.m_IdByEntity.Clear();
            s_Instance = null;
        }
    }

    void Register(EntityAI obj, string deviceId)
    {
        if (!obj || deviceId == "")
            return;

        // Native LFPG IDs live on the entity and are authoritative.  Several
        // RPC/legacy resolution paths pass a correlational ID to Register;
        // accepting that as another key recreates the alias problem even after
        // the persistence lifecycle has been fixed.  Vanilla devices have no
        // LFPG ID and retain the deterministic ID supplied by their caller.
        string authoritativeId = LFPG_DeviceAPI.GetDeviceId(obj);
        if (authoritativeId != "" && authoritativeId != deviceId)
        {
            string aliasMsg = "[DeviceRegistry] Ignored non-authoritative alias ";
            aliasMsg = aliasMsg + deviceId + " canonical=" + authoritativeId;
            aliasMsg = aliasMsg + " type=" + obj.GetType();
            LFPG_Util.Warn(aliasMsg);
            deviceId = authoritativeId;
        }

        // Remove the previous key owned by this same entity.  This is the
        // normal EEInit -> OnStoreLoad transition for persisted devices.
        string previousId;
        if (m_IdByEntity.Find(obj, previousId) && previousId != "" && previousId != deviceId)
        {
            EntityAI previousOwner;
            if (m_ById.Find(previousId, previousOwner) && previousOwner == obj)
            {
                m_ById.Remove(previousId);
            }
        }

        // A DeviceId may never name two live entities.  Preserve the existing
        // registry behaviour (the latest authoritative registration wins), but
        // also detach the displaced entity's reverse entry so it cannot later
        // remove the new owner by mistake.
        EntityAI displaced;
        if (m_ById.Find(deviceId, displaced) && displaced && displaced != obj)
        {
            string displacedId;
            if (m_IdByEntity.Find(displaced, displacedId) && displacedId == deviceId)
            {
                m_IdByEntity.Remove(displaced);
            }

            string collisionMsg = "[DeviceRegistry] DeviceId collision replaced key=";
            collisionMsg = collisionMsg + deviceId + " oldType=" + displaced.GetType();
            collisionMsg = collisionMsg + " newType=" + obj.GetType();
            LFPG_Util.Warn(collisionMsg);
        }

        m_ById.Set(deviceId, obj);
        m_IdByEntity.Set(obj, deviceId);
    }

    void Unregister(string deviceId, EntityAI objExpected = null)
    {
        if (deviceId == "")
            return;

        EntityAI current;
        if (m_ById.Find(deviceId, current))
        {
            if (!objExpected || objExpected == current)
            {
                m_ById.Remove(deviceId);

                string reverseId;
                if (current && m_IdByEntity.Find(current, reverseId) && reverseId == deviceId)
                {
                    m_IdByEntity.Remove(current);
                }
            }
        }
    }

    // v0.7.43 (Fix 1): Validate entity ref is still alive.
    // DayZ can invalidate EntityAI refs when the C++ backing
    // is destroyed (streaming, forced deletion without EEDelete).
    // Stale refs evaluate as non-null in map but null in usage.
    // One null-check per lookup — zero overhead for valid refs.
    EntityAI FindById(string deviceId)
    {
        EntityAI obj;
        if (m_ById.Find(deviceId, obj))
        {
            if (!obj)
            {
                m_ById.Remove(deviceId);
                return null;
            }
            return obj;
        }
        return null;
    }

    // v0.7.44 (Level 4, hallazgo 1a): Filter null refs in GetAll.
    // The registry now enforces one key per entity in Register.  Keep the
    // pointer dedup as a defensive read barrier for state created by an older
    // hot-reloaded script instance; encountering it is an invariant failure.
    void GetAll(array<EntityAI> outArr)
    {
        if (!outArr)
        {
            LFPG_Util.Warn("DeviceRegistry.GetAll called with null outArr");
            return;
        }

        outArr.Clear();

        // Dedup by entity pointer in O(n): a hashed seen-map replaces the former
        // O(n^2) outArr.Find() scan. Object-keyed maps are a vanilla pattern
        // (scripts/4_world/classes/useractionscomponent/actiontargets.c:4
        // map<Object,Object>). Output order and contents are unchanged.
        map<EntityAI, bool> seen = new map<EntityAI, bool>;
        bool alreadySeen;

        int i;
        for (i = 0; i < m_ById.Count(); i = i + 1)
        {
            EntityAI ent = m_ById.GetElement(i);
            if (!ent) continue;

            if (seen.Find(ent, alreadySeen))
            {
                // Log the duplicate key for debugging
                string dupKey = m_ById.GetKey(i);
                string dedupMsg = "[DeviceRegistry] DEDUP: dupKey=";
                dedupMsg = dedupMsg + dupKey;
                dedupMsg = dedupMsg + " type=";
                dedupMsg = dedupMsg + ent.GetType();
                LFPG_Util.Warn(dedupMsg);
                continue;
            }

            seen.Set(ent, true);
            outArr.Insert(ent);
        }
    }

    // v0.7.4: remove entries where the entity reference has been
    // invalidated by the engine (despawn, streaming, forced deletion
    // without EEDelete). Called during self-heal.
    int PruneNullEntries()
    {
        ref array<string> nullKeys = new array<string>;

        int i;
        for (i = 0; i < m_ById.Count(); i = i + 1)
        {
            EntityAI ent = m_ById.GetElement(i);
            if (!ent)
            {
                nullKeys.Insert(m_ById.GetKey(i));
            }
        }

        int k;
        for (k = 0; k < nullKeys.Count(); k = k + 1)
        {
            m_ById.Remove(nullKeys[k]);
        }

        // Entity handles can be invalidated without EEDelete.  Rebuild the
        // reverse ownership table from the surviving authoritative map so it
        // cannot retain dead object keys or drift after a collision repair.
        m_IdByEntity.Clear();
        int reverseIndex;
        for (reverseIndex = 0; reverseIndex < m_ById.Count(); reverseIndex = reverseIndex + 1)
        {
            EntityAI reverseEntity = m_ById.GetElement(reverseIndex);
            if (reverseEntity)
            {
                m_IdByEntity.Set(reverseEntity, m_ById.GetKey(reverseIndex));
            }
        }

        if (nullKeys.Count() > 0)
        {
            LFPG_Util.Info("[DeviceRegistry] Pruned " + nullKeys.Count().ToString() + " null entries");
        }

        return nullKeys.Count();
    }

    // v0.7.44: Diagnostic — count entries (for debug logging).
    int Count()
    {
        return m_ById.Count();
    }
};
