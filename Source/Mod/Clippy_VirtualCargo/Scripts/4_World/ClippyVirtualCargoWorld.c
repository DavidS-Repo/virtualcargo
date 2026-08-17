class CVCWorldItemAdapter
{
    string GetID() { return ""; }
    int GetVersion() { return 1; }
    bool CanHandle(ItemBase item) { return false; }
    void Serialize(ItemBase item, CVCItemNode node) {}
    void Restore(ItemBase item, CVCItemNode node) {}
}

class CVCGenericItemAdapter: CVCWorldItemAdapter
{
    override string GetID() { return "dayz.state-v2"; }
    override bool CanHandle(ItemBase item) { return true; }

    override void Serialize(ItemBase item, CVCItemNode node)
    {
        node.quantity = item.GetQuantity();
        node.health = item.GetHealth01("", "Health");
        node.state.has_quantity = item.HasQuantity();
        node.state.wetness = item.GetWet();
        node.state.temperature = item.GetTemperature();
        node.state.liquid_type = item.GetLiquidType();
        node.state.agents = item.GetAgents();

        TStringArray zones = new TStringArray;
        item.GetDamageZones(zones);
        foreach (string zone : zones)
        {
            CVCHealthZoneState zoneState = new CVCHealthZoneState;
            zoneState.name = zone;
            zoneState.health = item.GetHealth01(zone, "Health");
            node.state.health_zones.Insert(zoneState);
        }

        if (item.HasEnergyManager() && item.GetCompEM())
        {
            node.state.has_energy = true;
            node.state.energy = item.GetCompEM().GetEnergy();
            node.state.switched_on = item.GetCompEM().IsSwitchedOn();
        }

        FoodStage foodStage = item.GetFoodStage();
        if (foodStage)
            node.state.food_stage = foodStage.GetFoodStageType();

        Magazine magazine = Magazine.Cast(item);
        if (magazine)
        {
            int magazineCount = magazine.GetAmmoCount();
            for (int roundIndex = 0; roundIndex < magazineCount; roundIndex++)
            {
                float roundDamage;
                string roundType;
                if (!magazine.GetCartridgeAtIndex(roundIndex, roundDamage, roundType))
                    continue;
                CVCCartridgeState round = new CVCCartridgeState;
                round.damage = roundDamage;
                round.ammo_type = roundType;
                node.state.magazine_cartridges.Insert(round);
            }
        }

        Weapon_Base weapon = Weapon_Base.Cast(item);
        if (weapon)
        {
            node.state.current_muzzle = weapon.GetCurrentMuzzle();
            for (int muzzle = 0; muzzle < weapon.GetMuzzleCount(); muzzle++)
            {
                CVCWeaponMuzzleState muzzleState = new CVCWeaponMuzzleState;
                muzzleState.mode = weapon.GetCurrentMode(muzzle);
                muzzleState.jammed = weapon.IsChamberJammed(muzzle);
                float chamberDamage;
                string chamberType;
                if (weapon.GetCartridgeInfo(muzzle, chamberDamage, chamberType))
                {
                    muzzleState.chamber = new CVCCartridgeState;
                    muzzleState.chamber.damage = chamberDamage;
                    muzzleState.chamber.ammo_type = chamberType;
                }
                int internalCount = weapon.GetInternalMagazineCartridgeCount(muzzle);
                for (int internalIndex = 0; internalIndex < internalCount; internalIndex++)
                {
                    float internalDamage;
                    string internalType;
                    if (weapon.GetInternalMagazineCartridgeInfo(muzzle, internalIndex, internalDamage, internalType))
                    {
                        CVCCartridgeState internalRound = new CVCCartridgeState;
                        internalRound.damage = internalDamage;
                        internalRound.ammo_type = internalType;
                        muzzleState.internal_magazine.Insert(internalRound);
                    }
                }
                node.state.weapon_muzzles.Insert(muzzleState);
            }
        }
    }

    override void Restore(ItemBase item, CVCItemNode node)
    {
        item.SetHealth01("", "Health", Math.Clamp(node.health, 0.0, 1.0));
        if (node.state)
        {
            if (node.state.has_quantity)
                item.SetQuantity(node.quantity, false, false, false, false);
            item.SetWet(node.state.wetness);
            item.SetTemperatureDirect(node.state.temperature);
            item.SetLiquidType(node.state.liquid_type);
            foreach (CVCHealthZoneState zoneState : node.state.health_zones)
            {
                if (zoneState)
                    item.SetHealth01(zoneState.name, "Health", Math.Clamp(zoneState.health, 0.0, 1.0));
            }
            if (node.state.has_energy && item.HasEnergyManager() && item.GetCompEM())
            {
                item.GetCompEM().SetEnergy(node.state.energy);
                if (node.state.switched_on && item.GetCompEM().CanSwitchOn())
                    item.GetCompEM().SwitchOn();
            }
            FoodStage foodStage = item.GetFoodStage();
            if (foodStage && node.state.food_stage >= 0)
                foodStage.ChangeFoodStage(node.state.food_stage);

            Magazine magazine = Magazine.Cast(item);
            if (magazine && node.state.magazine_cartridges)
            {
                magazine.ServerSetAmmoCount(0);
                for (int cartridgeIndex = node.state.magazine_cartridges.Count() - 1; cartridgeIndex >= 0; cartridgeIndex--)
                {
                    CVCCartridgeState cartridge = node.state.magazine_cartridges[cartridgeIndex];
                    magazine.ServerStoreCartridge(cartridge.damage, cartridge.ammo_type);
                }
            }

            Weapon_Base weapon = Weapon_Base.Cast(item);
            if (weapon && node.state.weapon_muzzles)
            {
                for (int muzzle = 0; muzzle < weapon.GetMuzzleCount() && muzzle < node.state.weapon_muzzles.Count(); muzzle++)
                {
                    float removedDamage;
                    string removedType;
                    while (weapon.PopCartridgeFromChamber(muzzle, removedDamage, removedType)) {}
                    while (weapon.PopCartridgeFromInternalMagazine(muzzle, removedDamage, removedType)) {}
                    CVCWeaponMuzzleState muzzleState = node.state.weapon_muzzles[muzzle];
                    if (muzzleState.chamber)
                        weapon.PushCartridgeToChamber(muzzle, muzzleState.chamber.damage, muzzleState.chamber.ammo_type);
                    for (int internalIndex = muzzleState.internal_magazine.Count() - 1; internalIndex >= 0; internalIndex--)
                    {
                        CVCCartridgeState internalRound = muzzleState.internal_magazine[internalIndex];
                        weapon.PushCartridgeToInternalMagazine(muzzle, internalRound.damage, internalRound.ammo_type);
                    }
                    weapon.SetCurrentMode(muzzle, muzzleState.mode);
                }
                if (node.state.current_muzzle >= 0 && node.state.current_muzzle < weapon.GetMuzzleCount())
                    weapon.SetCurrentMuzzle(node.state.current_muzzle);
                if (node.state.weapon_muzzles.Count() > 0)
                    weapon.SetJammed(node.state.weapon_muzzles[0].jammed);
            }
        }
    }
}

class CVCItemAdapterRegistry
{
    protected static ref array<ref CVCWorldItemAdapter> s_Adapters;
    protected static ref CVCGenericItemAdapter s_Generic;

    static void Register(CVCWorldItemAdapter adapter)
    {
        if (!adapter)
            return;
        if (!s_Adapters)
            s_Adapters = new array<ref CVCWorldItemAdapter>;
        s_Adapters.Insert(adapter);
    }

    static CVCWorldItemAdapter ResolveForItem(ItemBase item)
    {
        if (s_Adapters)
        {
            foreach (CVCWorldItemAdapter adapter : s_Adapters)
            {
                if (adapter && adapter.CanHandle(item))
                    return adapter;
            }
        }
        if (!s_Generic)
            s_Generic = new CVCGenericItemAdapter;
        return s_Generic;
    }

    static CVCWorldItemAdapter ResolveForNode(CVCItemNode node)
    {
        if (!node || !node.adapter || node.adapter.id == "")
            return null;
        if (node.adapter.id == "dayz.state-v2" && node.adapter.version == 1)
        {
            if (!s_Generic)
                s_Generic = new CVCGenericItemAdapter;
            return s_Generic;
        }
        if (s_Adapters)
        {
            foreach (CVCWorldItemAdapter adapter : s_Adapters)
            {
                if (adapter && adapter.GetID() == node.adapter.id && adapter.GetVersion() == node.adapter.version)
                    return adapter;
            }
        }
        return null;
    }
}

class CVCItemTreeCodec
{
    static bool CanCaptureTree(ItemBase item, out string reason)
    {
        if (!item)
        {
            reason = "item no longer exists";
            return false;
        }
        CVCSettings settings = CVCSettingsManager.Get();
        if (settings.BlockedItemClassNames)
        {
            foreach (string blockedClass : settings.BlockedItemClassNames)
            {
                if (blockedClass != "" && item.IsKindOf(blockedClass))
                {
                    reason = item.GetDisplayName() + " is blocked by Settings.json";
                    return false;
                }
            }
        }
        if (settings.RejectContaminatedItems && item.GetAgents() != 0)
        {
            reason = item.GetDisplayName() + " carries an agent state that cannot be losslessly restored";
            return false;
        }
        if (settings.RejectActiveOrPluggedItems && item.HasEnergyManager() && item.GetCompEM())
        {
            if (item.GetCompEM().IsPlugged() || item.GetCompEM().IsWorking() || item.GetCompEM().IsSwitchedOn())
            {
                reason = item.GetDisplayName() + " must be switched off and unplugged first";
                return false;
            }
        }

        GameInventory inventory = item.GetInventory();
        for (int attachmentIndex = 0; attachmentIndex < inventory.AttachmentCount(); attachmentIndex++)
        {
            ItemBase attachment = ItemBase.Cast(inventory.GetAttachmentFromIndex(attachmentIndex));
            if (attachment && !CanCaptureTree(attachment, reason))
                return false;
        }
        CargoBase cargo = inventory.GetCargo();
        if (cargo)
        {
            for (int cargoIndex = 0; cargoIndex < cargo.GetItemCount(); cargoIndex++)
            {
                ItemBase cargoItem = ItemBase.Cast(cargo.GetItem(cargoIndex));
                if (cargoItem && !CanCaptureTree(cargoItem, reason))
                    return false;
            }
        }
        return true;
    }

    static CVCItemNode Capture(ItemBase item)
    {
        if (!item)
            return null;

        CVCItemNode node = new CVCItemNode;
        node.class_name = item.GetType();
        node.item_id = item.CVCGetVirtualItemID();

        InventoryLocation location = new InventoryLocation;
        if (item.GetInventory().GetCurrentInventoryLocation(location))
        {
            node.location.slot = location.GetSlot();
            node.location.index = location.GetIdx();
            node.location.row = location.GetRow();
            node.location.col = location.GetCol();
            node.location.flip = location.GetFlip();
            if (location.GetType() == InventoryLocationType.ATTACHMENT)
                node.location.kind = "attachment";
            else if (location.GetType() == InventoryLocationType.HANDS)
                node.location.kind = "hands";
            else
                node.location.kind = "cargo";
        }

        CVCWorldItemAdapter adapter = CVCItemAdapterRegistry.ResolveForItem(item);
        node.adapter.id = adapter.GetID();
        node.adapter.version = adapter.GetVersion();
        adapter.Serialize(item, node);

        GameInventory inventory = item.GetInventory();
        int attachmentCount = inventory.AttachmentCount();
        for (int attachmentIndex = 0; attachmentIndex < attachmentCount; attachmentIndex++)
        {
            ItemBase attachment = ItemBase.Cast(inventory.GetAttachmentFromIndex(attachmentIndex));
            if (attachment)
                node.children.Insert(Capture(attachment));
        }

        CargoBase cargo = inventory.GetCargo();
        if (cargo)
        {
            int cargoCount = cargo.GetItemCount();
            for (int cargoIndex = 0; cargoIndex < cargoCount; cargoIndex++)
            {
                ItemBase cargoItem = ItemBase.Cast(cargo.GetItem(cargoIndex));
                if (cargoItem)
                    node.children.Insert(Capture(cargoItem));
            }
        }
        return node;
    }

    static ItemBase RestoreRoot(CVCItemNode node, EntityAI destination, out string reason)
    {
        if (!node || !destination)
        {
            reason = "stored root or destination is missing";
            return null;
        }
        if (!CanRestoreTree(node, reason))
            return null;
        ItemBase root = ItemBase.Cast(destination.GetInventory().CreateEntityInCargo(node.class_name));
        if (!root)
        {
            reason = "could not create root " + node.class_name;
            return null;
        }
        if (!RestoreNode(root, node, reason))
        {
            root.DeleteSafe();
            return null;
        }
        return root;
    }

    protected static bool CanRestoreTree(CVCItemNode node, out string reason)
    {
        if (!node || node.class_name == "")
        {
            reason = "stored item node is incomplete";
            return false;
        }
        CVCWorldItemAdapter adapter = CVCItemAdapterRegistry.ResolveForNode(node);
        if (!adapter)
        {
            if (node.adapter)
                reason = string.Format("adapter %1 v%2 is not installed", node.adapter.id, node.adapter.version);
            else
                reason = "stored item has no adapter identity";
            return false;
        }
        if (node.children)
        {
            foreach (CVCItemNode childNode : node.children)
            {
                if (!CanRestoreTree(childNode, reason))
                    return false;
            }
        }
        return true;
    }

    protected static bool RestoreNode(ItemBase item, CVCItemNode node, out string reason)
    {
        if (!item || !node)
        {
            reason = "created item or stored node disappeared during restore";
            return false;
        }
        CVCWorldItemAdapter adapter = CVCItemAdapterRegistry.ResolveForNode(node);
        if (!adapter)
        {
            reason = "adapter became unavailable while restoring " + node.class_name;
            return false;
        }
        item.CVCSetVirtualItemID(node.item_id);
        adapter.Restore(item, node);
        if (!node.children)
            return true;

        foreach (CVCItemNode childNode : node.children)
        {
            if (!childNode)
            {
                reason = "stored child node is missing inside " + item.GetType();
                return false;
            }
            if (!childNode.location)
            {
                reason = "stored child location is missing for " + childNode.class_name;
                return false;
            }
            EntityAI created;
            if (childNode.location.kind == "attachment" && childNode.location.slot >= 0)
                created = item.GetInventory().CreateAttachmentEx(childNode.class_name, childNode.location.slot);
            else if (childNode.location.kind == "cargo")
                created = item.GetInventory().CreateEntityInCargoEx(childNode.class_name, childNode.location.index, childNode.location.row, childNode.location.col, childNode.location.flip);
            else
            {
                reason = "unsupported stored child location " + childNode.location.kind + " for " + childNode.class_name;
                return false;
            }
            ItemBase child = ItemBase.Cast(created);
            if (!child)
            {
                reason = "could not restore child " + childNode.class_name + " inside " + item.GetType();
                return false;
            }
            if (!RestoreNode(child, childNode, reason))
                return false;
        }
        return true;
    }
}


class CVCContainerPolicy
{
    static bool Matches(Object object, array<string> classes, bool includeInherited)
    {
        if (!object || !classes)
            return false;
        foreach (string className : classes)
        {
            if (className == "")
                continue;
            if (object.GetType() == className)
                return true;
            if (includeInherited && object.IsKindOf(className))
                return true;
        }
        return false;
    }

    static bool HasCargo(EntityAI entity)
    {
        return entity && entity.GetInventory() && entity.GetInventory().GetCargo();
    }

    static bool IsEligible(EntityAI entity)
    {
        if (!entity || entity.GetHierarchyParent() || !HasCargo(entity))
            return false;
        return IsConfiguredContainerClass(entity);
    }

    static bool IsConfiguredContainerClass(EntityAI entity)
    {
        if (!entity || !HasCargo(entity))
            return false;
        CVCSettings settings = CVCSettingsManager.Get();
        if (!settings.Enabled)
            return false;
        if (entity.IsKindOf("PlayerBase") || Matches(entity, settings.ExcludedContainerClassNames, true))
            return false;
        if (Transport.Cast(entity))
            return settings.EnableVehicleCargo;
        if (settings.AutoDiscoverCargoContainers)
            return true;
        return Matches(entity, settings.ContainerClassNames, settings.IncludeInheritedContainerClasses);
    }

    static string ProviderKey(EntityAI entity)
    {
        if (!entity)
            return "";
        int a;
        int b;
        int c;
        int d;
        entity.GetPersistentID(a, b, c, d);
        if (a == 0 && b == 0 && c == 0 && d == 0)
            return "";
        return string.Format("%1:%2:%3:%4:%5", entity.GetType(), a, b, c, d);
    }

    static bool CanAccess(EntityAI entity, PlayerBase player)
    {
        if (!IsEligible(entity) || !player || !player.IsAlive() || vector.Distance(player.GetPosition(), entity.GetPosition()) > CVCSettingsManager.Get().AccessDistanceMetres)
            return false;
        if (GetGame().IsServer() && ProviderKey(entity) == "")
            return false;
        return true;
    }
}

class CVCContainerRuntime
{
    EntityAI container;
    PlayerBase player;
    string provider_key;
    string storage_id;
    int revision;
    bool busy;
    int phase;
    string session_id;
    string cursor;
    string next_cursor;
    ref array<ItemBase> materialized = new array<ItemBase>;
    ref array<ItemBase> captured = new array<ItemBase>;
    bool internal_mutation;
    bool recovering;
    bool physical_fallback;
    bool migration_prepare_dispatched;
    int migration_retries;
    ref CVCSessionData recovery_session;
    ref CVCMigrationData active_migration;
    ref array<string> pending_mark_root_ids = new array<string>;
    ref array<string> pending_mark_source_keys = new array<string>;
    ref array<string> pending_cleanup_source_keys = new array<string>;
    int mark_retry_attempt;
    bool mark_request_prepared;
    bool recovery_exact_page;
    bool abort_pending;
    int abort_retry_attempt;
    int cleanup_retry_attempt;
    bool cleanup_committed;
    string pending_failure_reason;
}

class CVCPhysicalRootIdentity
{
    static string Key(ItemBase item)
    {
        if (!item)
            return "";
        int a;
        int b;
        int c;
        int d;
        item.GetPersistentID(a, b, c, d);
        if (a == 0 && b == 0 && c == 0 && d == 0)
            return "";
        return string.Format("%1:%2:%3:%4:%5", item.GetType(), a, b, c, d);
    }

    static ItemBase Find(EntityAI container, string sourceKey)
    {
        if (!container || sourceKey == "" || !container.GetInventory())
            return null;
        CargoBase cargo = container.GetInventory().GetCargo();
        if (!cargo)
            return null;
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            ItemBase item = ItemBase.Cast(cargo.GetItem(index));
            if (item && item.GetHierarchyParent() == container && Key(item) == sourceKey)
                return item;
        }
        return null;
    }

    static int Count(EntityAI container)
    {
        if (!container || !container.GetInventory() || !container.GetInventory().GetCargo())
            return 0;
        int count;
        CargoBase cargo = container.GetInventory().GetCargo();
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            if (cargoEntity && cargoEntity.GetHierarchyParent() == container)
                count++;
        }
        return count;
    }
}

class CVCSessionJournalRoot
{
    string source_key;
    string virtual_root_id;
    string class_name;
}

class CVCSessionJournalEntry
{
    string session_id;
    string provider_key;
    string status;
    ref array<string> offered_root_ids = new array<string>;
    ref array<string> mark_root_ids = new array<string>;
    ref array<string> mark_source_keys = new array<string>;
    ref array<string> cleanup_source_keys = new array<string>;
    ref array<ref CVCSessionJournalRoot> roots = new array<ref CVCSessionJournalRoot>;
}

class CVCMaterializationJob
{
    ref CVCContainerRuntime state;
    ref CVCSessionData session;
    ref CVCSessionJournalEntry journal;
}

class CVCSessionJournalFile
{
    int version = 1;
    int generation;
    ref array<ref CVCSessionJournalEntry> sessions = new array<ref CVCSessionJournalEntry>;
}

class CVCSessionJournal
{
    static const string PATH_A = "$profile:ClippyVirtualCargo/SessionJournal-A.json";
    static const string PATH_B = "$profile:ClippyVirtualCargo/SessionJournal-B.json";
    protected static ref CVCSessionJournalFile s_Data;

    protected static bool IsValid(CVCSessionJournalFile data)
    {
        return data && data.version == 1 && data.generation >= 0 && data.sessions;
    }

    protected static void EnsureLoaded()
    {
        if (s_Data)
            return;
        CVCSessionJournalFile dataA;
        CVCSessionJournalFile dataB;
        string errorA;
        string errorB;
        bool validA = JsonFileLoader<CVCSessionJournalFile>.LoadFile(PATH_A, dataA, errorA) && IsValid(dataA);
        bool validB = JsonFileLoader<CVCSessionJournalFile>.LoadFile(PATH_B, dataB, errorB) && IsValid(dataB);
        if (validA && (!validB || dataA.generation >= dataB.generation))
            s_Data = dataA;
        else if (validB)
            s_Data = dataB;
        else
            s_Data = new CVCSessionJournalFile;
    }

    static CVCSessionJournalEntry Get(string sessionID)
    {
        EnsureLoaded();
        foreach (CVCSessionJournalEntry entry : s_Data.sessions)
        {
            if (entry && entry.session_id == sessionID)
                return entry;
        }
        return null;
    }

    static CVCSessionJournalEntry Begin(CVCSessionData session)
    {
        if (!session || session.session_id == "")
            return null;
        CVCSessionJournalEntry entry = Ensure(session);
        if (!entry)
            return null;
        entry.status = "OPENING";
        entry.offered_root_ids.Clear();
        if (session.original_root_ids)
        {
            foreach (string offeredID : session.original_root_ids)
                entry.offered_root_ids.Insert(offeredID);
        }
        else if (session.items)
        {
            foreach (CVCItemNode offeredNode : session.items)
            {
                if (offeredNode)
                    entry.offered_root_ids.Insert(offeredNode.item_id);
            }
        }
        entry.mark_root_ids.Clear();
        entry.mark_source_keys.Clear();
        entry.cleanup_source_keys.Clear();
        entry.roots.Clear();
        if (!Save())
            return null;
        return entry;
    }

    static CVCSessionJournalEntry Ensure(CVCSessionData session)
    {
        if (!session || session.session_id == "")
            return null;
        CVCSessionJournalEntry entry = Get(session.session_id);
        if (entry)
            return entry;
        entry = new CVCSessionJournalEntry;
        entry.session_id = session.session_id;
        entry.provider_key = session.provider_key;
        if (session.original_root_ids)
        {
            foreach (string offeredID : session.original_root_ids)
                entry.offered_root_ids.Insert(offeredID);
        }
        s_Data.sessions.Insert(entry);
        if (!Save())
            return null;
        return entry;
    }

    static CVCSessionJournalRoot FindByVirtualID(CVCSessionJournalEntry entry, string virtualID)
    {
        if (!entry)
            return null;
        foreach (CVCSessionJournalRoot root : entry.roots)
        {
            if (root && root.virtual_root_id == virtualID)
                return root;
        }
        return null;
    }

    static CVCSessionJournalRoot FindBySourceKey(CVCSessionJournalEntry entry, string sourceKey)
    {
        if (!entry)
            return null;
        foreach (CVCSessionJournalRoot root : entry.roots)
        {
            if (root && root.source_key == sourceKey)
                return root;
        }
        return null;
    }

    static bool RecordRoot(CVCSessionJournalEntry entry, ItemBase item, string virtualID, bool persist = true)
    {
        if (!entry || !item || virtualID == "")
            return false;
        string sourceKey = CVCPhysicalRootIdentity.Key(item);
        if (sourceKey == "")
            return false;
        CVCSessionJournalRoot root = FindByVirtualID(entry, virtualID);
        if (!root)
        {
            root = new CVCSessionJournalRoot;
            entry.roots.Insert(root);
        }
        root.source_key = sourceKey;
        root.virtual_root_id = virtualID;
        root.class_name = item.GetType();
        if (persist)
            return Save();
        return true;
    }

    static bool SaveEntry(CVCSessionJournalEntry entry)
    {
        if (!entry)
            return false;
        return Save();
    }

    static bool Remove(string sessionID)
    {
        EnsureLoaded();
        for (int index = s_Data.sessions.Count() - 1; index >= 0; index--)
        {
            CVCSessionJournalEntry entry = s_Data.sessions[index];
            if (entry && entry.session_id == sessionID)
                s_Data.sessions.RemoveOrdered(index);
        }
        return Save();
    }

    protected static bool Save()
    {
        EnsureLoaded();
        s_Data.generation++;
        string path = PATH_A;
        if ((s_Data.generation % 2) == 0)
            path = PATH_B;
        string saveError;
        if (!JsonFileLoader<CVCSessionJournalFile>.SaveFile(path, s_Data, saveError))
        {
            s_Data.generation--;
            ErrorEx("[Clippy Virtual Cargo] Session journal write failed: " + saveError);
            return false;
        }
        return true;
    }
}

class CVCNativeResolveHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    protected bool m_NextPage;

    void CVCNativeResolveHandler(CVCContainerRuntime state, bool nextPage)
    {
        m_State = state;
        m_NextPage = nextPage;
    }

    override void OnSuccess(string raw)
    {
        CVCResolveEnvelope response = new CVCResolveEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            CVCContainerService.Fail(m_State, "container registration was rejected");
            return;
        }
        m_State.storage_id = response.data.storage_id;
        m_State.revision = response.data.revision;
        CVCContainerService.OpenResolved(m_State, m_NextPage);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.Fail(m_State, "storage host is unavailable");
    }
}

class CVCNativeAbortHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeAbortHandler(CVCContainerRuntime state) { m_State = state; }

    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError) || !response.ok || !response.data || response.data.status != "ABORTED")
        {
            CVCContainerService.AbortUncertain(m_State, "abort response was invalid: " + parseError);
            return;
        }
        CVCContainerService.AbortSucceeded(m_State);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.AbortUncertain(m_State, reason);
    }
}

class CVCNativeRecoveryAbortHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeRecoveryAbortHandler(CVCContainerRuntime state) { m_State = state; }
    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError) || !response.ok || !response.data || response.data.status != "ABORTED")
        {
            CVCContainerService.AbortUncertain(m_State, "recovery abort response was invalid: " + parseError);
            return;
        }
        CVCContainerService.AbortSucceeded(m_State);
        Print("[Clippy Virtual Cargo] Aborted an interrupted session only after proving that no offered physical root remained for " + m_State.provider_key);
    }
    override void OnFailure(string reason)
    {
        CVCContainerService.AbortUncertain(m_State, reason);
    }
}

class CVCNativeMarkHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeMarkHandler(CVCContainerRuntime state) { m_State = state; }

    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError) || !response.ok || !response.data)
        {
            CVCContainerService.MarkUncertain(m_State, "mark response was invalid: " + parseError);
            return;
        }
        CVCContainerService.MarkSucceeded(m_State, response.data);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.MarkUncertain(m_State, reason);
    }
}

class CVCNativeOpenHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeOpenHandler(CVCContainerRuntime state) { m_State = state; }

    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError))
        {
            CVCContainerService.OpenUncertain(m_State, "cargo open response was invalid; restart recovery is required");
            return;
        }
        if (!response.ok || !response.data)
        {
            CVCContainerService.Fail(m_State, "cargo page open was rejected by the storage host");
            return;
        }
        if (!CVCContainerPolicy.CanAccess(m_State.container, m_State.player))
        {
            m_State.session_id = response.data.session_id;
            CVCContainerService.AbortOpening(m_State, "player left interaction range");
            return;
        }

        string validationError;
        if (!CVCContainerService.ValidatePhysicalCargo(m_State.container, validationError))
        {
            m_State.session_id = response.data.session_id;
            CVCContainerService.AbortOpening(m_State, validationError);
            return;
        }

        m_State.session_id = response.data.session_id;
        response.data.provider_key = m_State.provider_key;
        CVCSessionJournalEntry journal = CVCSessionJournal.Begin(response.data);
        if (!journal)
        {
            CVCContainerService.AbortOpening(m_State, "session journal could not be written before materialization");
            return;
        }
        CVCContainerService.QueueMaterialization(m_State, response.data, journal);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.OpenUncertain(m_State, "storage host did not answer the open request; restart recovery is required");
    }
}

class CVCNativeCommitHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeCommitHandler(CVCContainerRuntime state) { m_State = state; }

    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError) || !response.ok || !response.data || (response.data.status != "COMMITTED" && response.data.status != "CLEANED"))
        {
            CVCContainerService.CommitFailed(m_State, "host rejected the cargo save");
            return;
        }
        CVCContainerService.CommitSucceeded(m_State, response.data);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.CommitFailed(m_State, "storage host did not answer; recovery lock remains active");
    }
}

class CVCNativeCleanupHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCNativeCleanupHandler(CVCContainerRuntime state) { m_State = state; }

    override void OnSuccess(string raw)
    {
        CVCSessionEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseSession(raw, response, parseError) || !response.ok || !response.data || response.data.status != "CLEANED")
        {
            CVCContainerService.CleanupUncertain(m_State, "cleanup acknowledgement was rejected");
            return;
        }
        CVCContainerService.FinishCleaned(m_State, response.data);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.CleanupUncertain(m_State, reason);
    }
}

class CVCContainerService
{
    static const int PHASE_IDLE = 0;
    static const int PHASE_OPENING = 1;
    static const int PHASE_ACTIVE = 2;
    static const int PHASE_COMMITTING = 3;
    static const int PHASE_RECOVERY = 4;
    static const int PHASE_MIGRATING = 5;
    static const int ACTION_NONE = 0;
    static const int ACTION_OPEN = 1;
    static const int ACTION_NEXT = 2;
    static const int ACTION_RETRY = 3;
    protected static ref map<string, ref CVCContainerRuntime> s_States = new map<string, ref CVCContainerRuntime>;
    protected static ref map<string, EntityAI> s_Registered = new map<string, EntityAI>;
    protected static ref map<string, ref CVCSessionData> s_PendingRecovery = new map<string, ref CVCSessionData>;
    protected static ref array<ref CVCMaterializationJob> s_MaterializationQueue = new array<ref CVCMaterializationJob>;
    protected static bool s_MaterializationPumpScheduled;
    protected static bool s_EnforcementReady;

    static void SetActionState(EntityAI container, int actionState)
    {
        if (!GetGame().IsServer() || !container)
            return;
        ItemBase item = ItemBase.Cast(container);
        if (item)
        {
            item.CVCSetActionState(actionState);
            return;
        }
        CarScript vehicle = CarScript.Cast(container);
        if (vehicle)
            vehicle.CVCSetActionState(actionState);
    }

    static void SetManagedLifecycle(EntityAI container, bool managed)
    {
        if (!GetGame().IsServer() || !container)
            return;
        ItemBase item = ItemBase.Cast(container);
        if (item)
            item.CVCSetManagedShell(managed);
    }

    static void QueueMaterialization(CVCContainerRuntime state, CVCSessionData session, CVCSessionJournalEntry journal)
    {
        if (!state || !session || !journal)
        {
            if (state)
                AbortOpening(state, "materialization queue received incomplete session state");
            return;
        }
        CVCMaterializationJob job = new CVCMaterializationJob;
        job.state = state;
        job.session = session;
        job.journal = journal;
        s_MaterializationQueue.Insert(job);
        ScheduleMaterializationPump();
    }

    protected static void ScheduleMaterializationPump()
    {
        if (s_MaterializationPumpScheduled || s_MaterializationQueue.Count() == 0)
            return;
        s_MaterializationPumpScheduled = true;
        int delay = Math.Clamp(CVCSettingsManager.Get().MaterializationIntervalMs, 1, 1000);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.PumpMaterialization, delay, false);
    }

    static void PumpMaterialization()
    {
        s_MaterializationPumpScheduled = false;
        if (s_MaterializationQueue.Count() == 0)
            return;

        CVCMaterializationJob job = s_MaterializationQueue.Get(0);
        s_MaterializationQueue.RemoveOrdered(0);
        if (!job || !job.state || !job.session || !job.journal)
        {
            ScheduleMaterializationPump();
            return;
        }

        CVCContainerRuntime state = job.state;
        CVCSessionData session = job.session;
        if (state.session_id != session.session_id || state.phase != PHASE_OPENING || !state.busy)
        {
            ErrorEx("[Clippy Virtual Cargo] Dropped a stale queued materialization job for session " + session.session_id);
            ScheduleMaterializationPump();
            return;
        }
        if (!state.container)
        {
            state.pending_failure_reason = "container disappeared before queued materialization";
            state.abort_pending = true;
            state.phase = PHASE_RECOVERY;
            DispatchAbort(state, false);
            ScheduleMaterializationPump();
            return;
        }
        if (!CVCContainerPolicy.CanAccess(state.container, state.player))
        {
            AbortOpening(state, "player left interaction range before queued materialization");
            ScheduleMaterializationPump();
            return;
        }
        string validationError;
        if (!ValidatePhysicalCargo(state.container, validationError))
        {
            AbortOpening(state, validationError);
            ScheduleMaterializationPump();
            return;
        }

        state.materialized.Clear();
        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        state.internal_mutation = true;
        if (session.items)
        {
            foreach (CVCItemNode node : session.items)
            {
                string restoreError;
                ItemBase restored = CVCItemTreeCodec.RestoreRoot(node, state.container, restoreError);
                if (!restored)
                {
                    state.internal_mutation = false;
                    AbortOpening(state, "cargo page restore failed: " + restoreError);
                    ScheduleMaterializationPump();
                    return;
                }
                state.materialized.Insert(restored);
                if (!CVCSessionJournal.RecordRoot(job.journal, restored, node.item_id, false))
                {
                    state.internal_mutation = false;
                    AbortOpening(state, "restored root did not receive a stable DayZ persistence identity");
                    ScheduleMaterializationPump();
                    return;
                }
                state.pending_mark_root_ids.Insert(node.item_id);
                state.pending_mark_source_keys.Insert(CVCPhysicalRootIdentity.Key(restored));
            }
        }
        state.internal_mutation = false;
        job.journal.status = "MARK_PENDING";
        foreach (string markRootID : state.pending_mark_root_ids)
            job.journal.mark_root_ids.Insert(markRootID);
        foreach (string markSourceKey : state.pending_mark_source_keys)
            job.journal.mark_source_keys.Insert(markSourceKey);
        if (!CVCSessionJournal.SaveEntry(job.journal))
        {
            AbortOpening(state, "session journal could not persist the queued mark request");
            ScheduleMaterializationPump();
            return;
        }
        state.mark_request_prepared = true;
        DispatchMark(state);
        ScheduleMaterializationPump();
    }

    static int ClientActionState(EntityAI container)
    {
        if (!container)
            return ACTION_NONE;
        ItemBase item = ItemBase.Cast(container);
        if (item)
            return item.CVCGetActionState();
        CarScript vehicle = CarScript.Cast(container);
        if (vehicle)
            return vehicle.CVCGetActionState();
        return ACTION_NONE;
    }

    static bool ClientCanInteract(EntityAI container, PlayerBase player)
    {
        return container && player && player.IsAlive() && vector.Distance(player.GetPosition(), container.GetPosition()) <= UAMaxDistances.DEFAULT;
    }

    static void EnableEnforcement()
    {
        s_EnforcementReady = true;
        foreach (string providerKey, EntityAI registeredContainer : s_Registered)
        {
            if (registeredContainer)
            {
                SetManagedLifecycle(registeredContainer, true);
                SetActionState(registeredContainer, ACTION_NONE);
            }
        }
    }

    static bool RecoverySettled()
    {
        if (s_PendingRecovery.Count() > 0)
            return false;
        foreach (string stateKey, CVCContainerRuntime state : s_States)
        {
            if (state && state.recovering)
                return false;
        }
        return true;
    }

    static void CompleteRecovery(CVCContainerRuntime state)
    {
        if (!state)
            return;
        if (state.provider_key != "")
            s_PendingRecovery.Remove(state.provider_key);
        state.recovering = false;
        state.recovery_session = null;
    }

    static void Register(EntityAI container)
    {
        if (!GetGame().IsServer() || !CVCContainerPolicy.IsEligible(container))
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return;
        if (s_EnforcementReady)
            SetManagedLifecycle(container, true);
        SetActionState(container, ACTION_NONE);
        s_Registered.Set(key, container);
        CVCContainerRuntime existing;
        if (s_States.Find(key, existing) && existing)
        {
            existing.container = container;
            if (existing.phase == PHASE_ACTIVE && existing.session_id != "" && !existing.busy)
                GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 1000, false, existing);
        }
        CVCSessionData recovery;
        if (s_PendingRecovery.Find(key, recovery))
        {
            s_PendingRecovery.Remove(key);
            StartRecovery(container, recovery);
        }
    }

    static void Unregister(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return;
        s_Registered.Remove(key);
        CVCContainerRuntime state;
        if (!s_States.Find(key, state) || !state)
            return;
        state.container = null;
        if (state.recovering && state.recovery_session)
            s_PendingRecovery.Set(key, state.recovery_session);
        if (state.phase == PHASE_IDLE && !state.busy && !state.recovering && !state.physical_fallback)
            s_States.Remove(key);
    }

    static CVCContainerRuntime State(EntityAI container)
    {
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return null;
        CVCContainerRuntime state;
        if (!s_States.Find(key, state))
        {
            state = new CVCContainerRuntime;
            state.container = container;
            state.provider_key = key;
            s_States.Set(key, state);
        }
        else
        {
            state.container = container;
        }
        return state;
    }

    static bool IsLocked(EntityAI container)
    {
        if (!container || !s_EnforcementReady || !CVCContainerPolicy.IsEligible(container))
            return false;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return false;
        CVCContainerRuntime state;
        if (!s_States.Find(key, state))
            return true;
        if (!state)
            return true;
        if (state.physical_fallback)
            return false;
        return !state.internal_mutation && state.phase != PHASE_ACTIVE;
    }

    static bool AllowsPhysicalCargo(EntityAI container)
    {
        if (!container || !CVCContainerPolicy.IsEligible(container))
            return true;
        if (!s_EnforcementReady)
            return true;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return true;
        CVCContainerRuntime state;
        if (!s_States.Find(key, state) || !state)
            return false;
        if (state.physical_fallback)
            return true;
        if (state.internal_mutation)
            return true;
        return state.phase == PHASE_ACTIVE && state.session_id != "" && state.player && CVCContainerPolicy.CanAccess(container, state.player);
    }

    static bool CanOpen(EntityAI container)
    {
        CVCContainerRuntime state;
        if (!container)
            return false;
        if (!GetGame().IsServer())
            return ClientActionState(container) == ACTION_OPEN;
        if (CVCContainerPolicy.ProviderKey(container) == "")
            return false;
        if (!s_States.Find(CVCContainerPolicy.ProviderKey(container), state))
            return false;
        return state && ClientActionState(container) == ACTION_OPEN && !state.physical_fallback && !state.busy && state.phase == PHASE_IDLE;
    }

    static bool IsPhysicalFallback(EntityAI container)
    {
        CVCContainerRuntime state;
        string key = CVCContainerPolicy.ProviderKey(container);
        return key != "" && s_States.Find(key, state) && state && state.physical_fallback;
    }

    static bool EnablePhysicalFallback(EntityAI container, string reason)
    {
        CVCContainerRuntime state = State(container);
        if (!state || state.active_migration || state.migration_prepare_dispatched || state.session_id != "")
            return false;
        state.physical_fallback = true;
        state.busy = false;
        state.phase = PHASE_IDLE;
        state.player = null;
        state.migration_retries = 0;
        SetManagedLifecycle(container, false);
        SetActionState(container, ACTION_NONE);
        ErrorEx("[Clippy Virtual Cargo] Using vanilla physical cargo for " + state.provider_key + " until the next server restart: " + reason);
        return true;
    }

    static bool HasActivePage(EntityAI container)
    {
        CVCContainerRuntime state;
        return container && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state.phase == PHASE_ACTIVE;
    }

    static bool HasNextPage(EntityAI container)
    {
        if (!GetGame().IsServer())
            return ClientActionState(container) == ACTION_NEXT;
        CVCContainerRuntime state;
        return container && ClientActionState(container) == ACTION_NEXT && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state.phase == PHASE_IDLE && state.next_cursor != "";
    }

    static bool NeedsRetry(EntityAI container)
    {
        if (!GetGame().IsServer())
            return ClientActionState(container) == ACTION_RETRY;
        CVCContainerRuntime state;
        return container && ClientActionState(container) == ACTION_RETRY && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state.phase == PHASE_RECOVERY;
    }

    static void Open(EntityAI container, PlayerBase player, bool nextPage = false)
    {
        if (!ClippyVirtualCargoAPI.IsReady() || !CanOpen(container) || !CVCContainerPolicy.CanAccess(container, player))
            return;
        CVCContainerRuntime state = State(container);
        if (!state)
            return;
        if (state.busy || state.phase != PHASE_IDLE)
        {
            player.MessageStatus("Virtual cargo is already open or saving.");
            return;
        }
        state.busy = true;
        state.phase = PHASE_OPENING;
        state.player = player;
        SetActionState(container, ACTION_NONE);
        if (state.storage_id == "")
        {
            CVCResolveRequest request = new CVCResolveRequest;
            request.provider_id = CVCSettingsManager.Get().ProviderID;
            request.provider_key = state.provider_key;
            request.display_name = container.GetDisplayName();
            request.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            if (!ClippyVirtualCargoAPI.Post("/v1/storage/resolve", request, new CVCNativeResolveHandler(state, nextPage)))
                Fail(state, "API is not ready");
            return;
        }
        OpenResolved(state, nextPage);
    }

    static void OpenResolved(CVCContainerRuntime state, bool nextPage)
    {
        if (!state || !state.player || !state.player.GetIdentity())
        {
            Fail(state, "player identity is unavailable");
            return;
        }
        CVCSessionOpenRequest request = new CVCSessionOpenRequest;
        request.storage_id = state.storage_id;
        request.expected_revision = state.revision;
        request.idempotency_key = string.Format("%1:open:%2:%3", state.provider_key, GetGame().GetTime(), Math.RandomInt(100000, 999999));
        request.player_id = state.player.GetIdentity().GetId();
        request.cursor = "";
        if (nextPage)
            request.cursor = state.next_cursor;
        request.limit = Math.Clamp(CVCSettingsManager.Get().NativePageSize, 1, 200);
        state.cursor = request.cursor;
        if (!ClippyVirtualCargoAPI.Post("/v1/session/open", request, new CVCNativeOpenHandler(state)))
            OpenUncertain(state, "could not dispatch the cargo open request; restart recovery is required");
    }

    static bool ValidatePhysicalCargo(EntityAI container, out string reason)
    {
        CargoBase cargo = container.GetInventory().GetCargo();
        if (!cargo)
        {
            reason = "container has no cargo grid";
            return false;
        }
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            ItemBase item = ItemBase.Cast(cargoEntity);
            if (cargoEntity && !item)
            {
                reason = "container holds a non-ItemBase cargo entity that has no safe state adapter";
                return false;
            }
            if (item && !CVCItemTreeCodec.CanCaptureTree(item, reason))
                return false;
        }
        return true;
    }

    static void CloseForPlayer(PlayerBase player)
    {
        if (!GetGame().IsServer() || !player)
            return;
        foreach (string key, CVCContainerRuntime state : s_States)
        {
            if (state && state.player == player && state.phase == PHASE_ACTIVE)
                Commit(state);
        }
    }

    static void QueueRecovery(CVCSessionData recovery)
    {
        if (!recovery || recovery.provider_key == "")
            return;
        EntityAI container;
        if (s_Registered.Find(recovery.provider_key, container) && container)
        {
            StartRecovery(container, recovery);
            return;
        }
        s_PendingRecovery.Set(recovery.provider_key, recovery);
        ErrorEx("[Clippy Virtual Cargo] Waiting for persistent container " + recovery.provider_key + " before recovering session " + recovery.session_id);
    }

    static void StartRecovery(EntityAI container, CVCSessionData recovery)
    {
        CVCContainerRuntime state = State(container);
        if (!state)
        {
            s_PendingRecovery.Set(recovery.provider_key, recovery);
            return;
        }
        state.storage_id = recovery.storage_id;
        state.revision = recovery.expected_revision;
        state.session_id = recovery.session_id;
        state.player = null;
        state.busy = false;
        state.recovering = true;
        state.physical_fallback = false;
        state.recovery_session = recovery;
        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        state.pending_cleanup_source_keys.Clear();
        state.cleanup_committed = false;
        state.mark_request_prepared = false;
        state.recovery_exact_page = false;
        state.abort_pending = false;
        SetActionState(container, ACTION_NONE);
        if (recovery.status == "OPEN")
        {
            RecoverOpen(state, recovery);
            return;
        }
        if (recovery.status == "MATERIALIZED")
        {
            RecoverMaterialized(state, recovery);
            return;
        }
        if (recovery.status == "COMMITTED")
        {
            state.phase = PHASE_RECOVERY;
            state.busy = true;
            CommitSucceeded(state, recovery);
            return;
        }
        LockAmbiguous(state, "unsupported interrupted session state " + recovery.status);
    }

    static bool ContainsString(array<string> values, string value)
    {
        if (!values)
            return false;
        foreach (string candidate : values)
        {
            if (candidate == value)
                return true;
        }
        return false;
    }

    static void CopyStrings(array<string> source, array<string> destination)
    {
        destination.Clear();
        if (!source)
            return;
        foreach (string value : source)
            destination.Insert(value);
    }

    static void LockAmbiguous(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        SetActionState(state.container, ACTION_NONE);
        ErrorEx("[Clippy Virtual Cargo] Recovery remains locked for " + state.provider_key + ": " + reason);
        CompleteRecovery(state);
    }

    static void RecoverOpen(CVCContainerRuntime state, CVCSessionData recovery)
    {
        state.phase = PHASE_RECOVERY;
        CVCSessionJournalEntry journal = CVCSessionJournal.Get(recovery.session_id);
        int physicalRoots = CVCPhysicalRootIdentity.Count(state.container);
        if (!journal)
        {
            LockAmbiguous(state, "the host session is OPEN but no valid local journal proves whether materialization began");
            return;
        }

        if (journal.status == "MARK_PENDING")
        {
            if (!journal.mark_root_ids || !journal.mark_source_keys || journal.mark_root_ids.Count() != journal.mark_source_keys.Count())
            {
                LockAmbiguous(state, "the pending mark journal does not contain paired virtual and physical identities");
                return;
            }
            for (int markIndex = 0; markIndex < journal.mark_root_ids.Count(); markIndex++)
            {
                ItemBase physical = CVCPhysicalRootIdentity.Find(state.container, journal.mark_source_keys[markIndex]);
                if (!physical)
                {
                    LockAmbiguous(state, "a root recorded by the pending mark journal is no longer inside the container");
                    return;
                }
                physical.CVCSetVirtualItemID(journal.mark_root_ids[markIndex]);
            }
            CopyStrings(journal.mark_root_ids, state.pending_mark_root_ids);
            CopyStrings(journal.mark_source_keys, state.pending_mark_source_keys);
            if (physicalRoots != state.pending_mark_root_ids.Count())
                state.recovery_exact_page = false;
            else
                state.recovery_exact_page = true;
            state.mark_request_prepared = true;
            DispatchMark(state);
            return;
        }

        if (journal.status != "OPENING")
        {
            LockAmbiguous(state, "the OPEN host state conflicts with local journal state " + journal.status);
            return;
        }

        if (!journal.roots || journal.roots.Count() == 0)
        {
            if (physicalRoots != 0)
            {
                LockAmbiguous(state, "physical roots exist although the OPENING journal recorded no materialization");
                return;
            }
            if (journal.offered_root_ids && journal.offered_root_ids.Count() == 0)
            {
                journal.status = "MARK_PENDING";
                if (!CVCSessionJournal.SaveEntry(journal))
                {
                    LockAmbiguous(state, "the empty-page materialization mark could not be journaled");
                    return;
                }
                state.recovery_exact_page = true;
                state.mark_request_prepared = true;
                DispatchMark(state);
                return;
            }
            state.abort_pending = true;
            DispatchAbort(state, true);
            return;
        }

        foreach (CVCSessionJournalRoot recordedRoot : journal.roots)
        {
            if (!recordedRoot || recordedRoot.virtual_root_id == "" || recordedRoot.source_key == "")
            {
                LockAmbiguous(state, "the OPENING journal contains an incomplete root mapping");
                return;
            }
            ItemBase recordedPhysical = CVCPhysicalRootIdentity.Find(state.container, recordedRoot.source_key);
            if (!recordedPhysical)
            {
                LockAmbiguous(state, "a root recorded during OPENING is no longer inside the container");
                return;
            }
            recordedPhysical.CVCSetVirtualItemID(recordedRoot.virtual_root_id);
            state.pending_mark_root_ids.Insert(recordedRoot.virtual_root_id);
            state.pending_mark_source_keys.Insert(recordedRoot.source_key);
        }
        journal.status = "MARK_PENDING";
        CopyStrings(state.pending_mark_root_ids, journal.mark_root_ids);
        CopyStrings(state.pending_mark_source_keys, journal.mark_source_keys);
        if (!CVCSessionJournal.SaveEntry(journal))
        {
            LockAmbiguous(state, "the recovered materialization mapping could not be journaled");
            return;
        }
        state.recovery_exact_page = physicalRoots == state.pending_mark_root_ids.Count();
        state.mark_request_prepared = true;
        DispatchMark(state);
    }

    static void RecoverMaterialized(CVCContainerRuntime state, CVCSessionData recovery)
    {
        state.phase = PHASE_RECOVERY;
        CVCSessionJournalEntry existingJournal = CVCSessionJournal.Get(recovery.session_id);
        if (existingJournal && existingJournal.status == "COMMIT_PENDING")
        {
            if (!existingJournal.cleanup_source_keys || CVCPhysicalRootIdentity.Count(state.container) != existingJournal.cleanup_source_keys.Count())
            {
                LockAmbiguous(state, "the pending commit journal does not exactly match the physical root count");
                return;
            }
            foreach (string pendingSourceKey : existingJournal.cleanup_source_keys)
            {
                ItemBase pendingPhysical = CVCPhysicalRootIdentity.Find(state.container, pendingSourceKey);
                CVCSessionJournalRoot pendingMapping = CVCSessionJournal.FindBySourceKey(existingJournal, pendingSourceKey);
                if (!pendingPhysical || !pendingMapping || pendingMapping.virtual_root_id == "")
                {
                    LockAmbiguous(state, "a pending commit root cannot be proven from the local journal and physical container");
                    return;
                }
                pendingPhysical.CVCSetVirtualItemID(pendingMapping.virtual_root_id);
            }
            if (!recovery.original_root_ids || !recovery.physical_source_keys || recovery.original_root_ids.Count() != recovery.physical_source_keys.Count())
            {
                LockAmbiguous(state, "the MATERIALIZED host record lacks a paired identity list for pending commit verification");
                return;
            }
            for (int hostRootIndex = 0; hostRootIndex < recovery.original_root_ids.Count(); hostRootIndex++)
            {
                string hostSourceKey = recovery.physical_source_keys[hostRootIndex];
                CVCSessionJournalRoot hostMapping = CVCSessionJournal.FindBySourceKey(existingJournal, hostSourceKey);
                if (!hostMapping || hostMapping.virtual_root_id != recovery.original_root_ids[hostRootIndex])
                {
                    LockAmbiguous(state, "the pending commit journal conflicts with the MATERIALIZED host identity mapping");
                    return;
                }
            }
            state.recovery_exact_page = true;
            state.phase = PHASE_ACTIVE;
            state.busy = false;
            Print("[Clippy Virtual Cargo] Exact pending commit recovered for " + recovery.provider_key + "; replaying it idempotently.");
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 1000, false, state);
            return;
        }
        if (!recovery.original_root_ids || !recovery.physical_source_keys || recovery.original_root_ids.Count() != recovery.physical_source_keys.Count())
        {
            LockAmbiguous(state, "the MATERIALIZED host record has no complete root-to-physical identity mapping");
            return;
        }
        if (CVCPhysicalRootIdentity.Count(state.container) != recovery.physical_source_keys.Count())
        {
            LockAmbiguous(state, "physical root count does not exactly match the MATERIALIZED host record");
            return;
        }

        CVCSessionJournalEntry journal = CVCSessionJournal.Ensure(recovery);
        if (!journal)
        {
            LockAmbiguous(state, "the MATERIALIZED recovery journal could not be created");
            return;
        }
        journal.status = "MATERIALIZED";
        CopyStrings(recovery.original_root_ids, journal.mark_root_ids);
        CopyStrings(recovery.physical_source_keys, journal.mark_source_keys);
        for (int rootIndex = 0; rootIndex < recovery.original_root_ids.Count(); rootIndex++)
        {
            string sourceKey = recovery.physical_source_keys[rootIndex];
            ItemBase physical = CVCPhysicalRootIdentity.Find(state.container, sourceKey);
            if (!physical)
            {
                LockAmbiguous(state, "a MATERIALIZED physical source key is absent from the container");
                return;
            }
            string virtualID = recovery.original_root_ids[rootIndex];
            physical.CVCSetVirtualItemID(virtualID);
            if (!CVCSessionJournal.RecordRoot(journal, physical, virtualID, false))
            {
                LockAmbiguous(state, "a MATERIALIZED root mapping could not be journaled");
                return;
            }
        }
        if (!CVCSessionJournal.SaveEntry(journal))
        {
            LockAmbiguous(state, "the MATERIALIZED recovery state could not be journaled");
            return;
        }
        state.recovery_exact_page = true;
        state.phase = PHASE_ACTIVE;
        state.busy = false;
        Print("[Clippy Virtual Cargo] Exact MATERIALIZED recovery proven for " + recovery.provider_key + "; replaying the commit.");
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 1000, false, state);
    }

    static void DispatchMark(CVCContainerRuntime state)
    {
        if (!state || state.session_id == "" || !state.mark_request_prepared || state.pending_mark_root_ids.Count() != state.pending_mark_source_keys.Count())
        {
            LockAmbiguous(state, "the materialization mark lacks an exact root-to-physical identity mapping");
            return;
        }
        state.busy = true;
        SetActionState(state.container, ACTION_NONE);
        CVCSessionMaterializedRequest materialized = new CVCSessionMaterializedRequest;
        materialized.session_id = state.session_id;
        CopyStrings(state.pending_mark_root_ids, materialized.root_ids);
        CopyStrings(state.pending_mark_source_keys, materialized.physical_source_keys);
        if (!ClippyVirtualCargoAPI.Post("/v1/session/mark-materialized", materialized, new CVCNativeMarkHandler(state)))
            MarkUncertain(state, "could not dispatch the materialization mark");
    }

    static void MarkUncertain(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        state.mark_retry_attempt++;
        SetActionState(state.container, ACTION_RETRY);
        int retryDelay = Math.Clamp(state.mark_retry_attempt * 2000, 2000, 30000);
        ErrorEx("[Clippy Virtual Cargo] Materialization mark is uncertain for " + state.provider_key + "; the physical roots remain locked and the identical identity mapping will be retried: " + reason);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.RetryMark, retryDelay, false, state, state.mark_retry_attempt);
    }

    static void RetryMark(CVCContainerRuntime state, int retryAttempt)
    {
        if (!state || state.busy || state.phase != PHASE_RECOVERY || retryAttempt != state.mark_retry_attempt || !state.mark_request_prepared)
            return;
        DispatchMark(state);
    }

    static void MarkSucceeded(CVCContainerRuntime state, CVCSessionData session)
    {
        if (!state || !session)
            return;
        if (session.status == "COMMITTED" || session.status == "CLEANED")
        {
            CommitSucceeded(state, session);
            return;
        }
        if (session.status != "MATERIALIZED")
        {
            MarkUncertain(state, "host returned session state " + session.status);
            return;
        }
        CVCSessionJournalEntry journal = CVCSessionJournal.Ensure(session);
        if (!journal)
        {
            LockAmbiguous(state, "the confirmed MATERIALIZED state could not be journaled");
            return;
        }
        journal.status = "MATERIALIZED";
        CopyStrings(state.pending_mark_root_ids, journal.mark_root_ids);
        CopyStrings(state.pending_mark_source_keys, journal.mark_source_keys);
        if (!CVCSessionJournal.SaveEntry(journal))
        {
            LockAmbiguous(state, "the confirmed MATERIALIZED state could not be persisted locally");
            return;
        }
        state.mark_retry_attempt = 0;
        state.next_cursor = session.next_cursor;
        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        state.mark_request_prepared = false;
        state.busy = false;
        state.phase = PHASE_ACTIVE;

        if (state.recovering)
        {
            if (!state.recovery_exact_page)
            {
                LockAmbiguous(state, "the known materialized roots coexist with one or more unmapped physical roots");
                return;
            }
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 1000, false, state);
            return;
        }

        if (!state.player || !CVCContainerPolicy.CanAccess(state.container, state.player))
        {
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 250, false, state);
            return;
        }
        if (CVCSettingsManager.Get().AutoOpenInventory)
            state.player.RPCSingleParam(CVCRPC.OPEN_INVENTORY, new Param1<string>(state.provider_key), true, state.player.GetIdentity());
    }

    static string NewVirtualRootID(CVCContainerRuntime state)
    {
        return string.Format("cvc-%1-%2-%3", GetGame().GetTime(), Math.RandomInt(100000, 999999), Math.RandomInt(100000, 999999));
    }

    static CVCSessionJournalEntry EnsureStateJournal(CVCContainerRuntime state)
    {
        CVCSessionData session = new CVCSessionData;
        session.session_id = state.session_id;
        session.provider_key = state.provider_key;
        return CVCSessionJournal.Ensure(session);
    }

    static void Commit(CVCContainerRuntime state)
    {
        if (!state || !state.container || state.busy || state.session_id == "")
            return;
        string validationError;
        if (!ValidatePhysicalCargo(state.container, validationError))
        {
            if (state.player)
                state.phase = PHASE_ACTIVE;
            else
                state.phase = PHASE_RECOVERY;
            if (state.player)
                state.player.MessageStatus("Virtual cargo not saved: " + validationError);
            else
                ErrorEx("[Clippy Virtual Cargo] Automatic recovery is blocked: " + validationError);
            return;
        }

        CVCSessionCommitRequest request = new CVCSessionCommitRequest;
        request.session_id = state.session_id;
        CVCSessionJournalEntry journal = EnsureStateJournal(state);
        if (!journal)
        {
            CommitFailed(state, "the commit journal is unavailable; physical cargo remains locked");
            return;
        }
        state.captured.Clear();
        state.cleanup_committed = false;
        state.pending_cleanup_source_keys.Clear();
        CargoBase cargo = state.container.GetInventory().GetCargo();
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            ItemBase item = ItemBase.Cast(cargo.GetItem(index));
            if (!item || item.GetHierarchyParent() != state.container)
                continue;
            string sourceKey = CVCPhysicalRootIdentity.Key(item);
            if (sourceKey == "" || ContainsString(request.physical_source_keys, sourceKey))
            {
                CommitFailed(state, "a physical root lacks a unique stable persistence identity");
                return;
            }
            string virtualID = item.CVCGetVirtualItemID();
            CVCSessionJournalRoot existingRoot = CVCSessionJournal.FindBySourceKey(journal, sourceKey);
            if (virtualID == "" && existingRoot)
                virtualID = existingRoot.virtual_root_id;
            if (virtualID == "")
                virtualID = NewVirtualRootID(state);
            item.CVCSetVirtualItemID(virtualID);
            if (!CVCSessionJournal.RecordRoot(journal, item, virtualID, false))
            {
                CommitFailed(state, "a stable root identity could not be written to the commit journal");
                return;
            }
            CVCItemNode capturedNode = CVCItemTreeCodec.Capture(item);
            if (!capturedNode)
            {
                CommitFailed(state, "a physical root could not be serialized for commit");
                return;
            }
            state.captured.Insert(item);
            request.items.Insert(capturedNode);
            request.physical_source_keys.Insert(sourceKey);
            state.pending_cleanup_source_keys.Insert(sourceKey);
        }
        journal.status = "COMMIT_PENDING";
        CopyStrings(state.pending_cleanup_source_keys, journal.cleanup_source_keys);
        if (!CVCSessionJournal.SaveEntry(journal))
        {
            CommitFailed(state, "the exact commit request could not be journaled");
            return;
        }
        state.busy = true;
        state.phase = PHASE_COMMITTING;
        SetActionState(state.container, ACTION_NONE);
        if (!ClippyVirtualCargoAPI.Post("/v1/session/commit", request, new CVCNativeCommitHandler(state)))
            CommitFailed(state, "could not dispatch cargo save; recovery lock remains active");
    }

    static void Retry(EntityAI container, PlayerBase player)
    {
        if (!CVCContainerPolicy.CanAccess(container, player))
            return;
        CVCContainerRuntime state = State(container);
        if (!state || state.phase != PHASE_RECOVERY || state.busy || ClientActionState(container) != ACTION_RETRY)
            return;
        state.player = player;
        if (state.abort_pending)
        {
            DispatchAbort(state, state.recovering);
            return;
        }
        if (state.mark_request_prepared)
        {
            DispatchMark(state);
            return;
        }
        if (state.cleanup_committed && state.pending_cleanup_source_keys.Count() > 0)
        {
            BeginCleanup(state);
            return;
        }
        state.phase = PHASE_ACTIVE;
        Commit(state);
    }

    static void AbortOpening(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.pending_failure_reason = reason;
        state.abort_pending = true;
        state.phase = PHASE_RECOVERY;
        state.busy = true;
        SetActionState(state.container, ACTION_NONE);
        state.internal_mutation = true;
        foreach (ItemBase item : state.materialized)
        {
            if (item && item.GetHierarchyParent() == state.container)
            {
                string sourceKey = CVCPhysicalRootIdentity.Key(item);
                if (sourceKey != "" && !ContainsString(state.pending_mark_source_keys, sourceKey))
                    state.pending_mark_source_keys.Insert(sourceKey);
                item.DeleteSafe();
            }
        }
        state.internal_mutation = false;
        state.materialized.Clear();
        CVCSessionJournalEntry journal = CVCSessionJournal.Get(state.session_id);
        if (journal)
        {
            journal.status = "ABORT_CLEANUP";
            CVCSessionJournal.SaveEntry(journal);
        }
        if (state.session_id == "")
        {
            state.abort_pending = false;
            Fail(state, reason);
            return;
        }
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.ConfirmAbortCleanup, 250, false, state, 1);
    }

    static void ConfirmAbortCleanup(CVCContainerRuntime state, int attempt)
    {
        if (!state || !state.container || !state.abort_pending)
            return;
        state.internal_mutation = true;
        foreach (string sourceKey : state.pending_mark_source_keys)
        {
            ItemBase physical = CVCPhysicalRootIdentity.Find(state.container, sourceKey);
            if (physical)
                physical.DeleteSafe();
        }
        state.internal_mutation = false;
        if (CVCPhysicalRootIdentity.Count(state.container) == 0)
        {
            DispatchAbort(state, state.recovering);
            return;
        }
        if (attempt < 12)
        {
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.ConfirmAbortCleanup, 500, false, state, attempt + 1);
            return;
        }
        state.busy = false;
        SetActionState(state.container, ACTION_NONE);
        ErrorEx("[Clippy Virtual Cargo] Session abort remains locked because physical roots still exist for " + state.provider_key + ". The host session was not aborted.");
        CompleteRecovery(state);
    }

    static void DispatchAbort(CVCContainerRuntime state, bool recoveryHandler)
    {
        if (!state || state.session_id == "")
            return;
        if (CVCPhysicalRootIdentity.Count(state.container) != 0)
        {
            LockAmbiguous(state, "refusing to abort an OPEN session while physical roots still exist");
            return;
        }
        state.busy = true;
        state.phase = PHASE_RECOVERY;
        state.abort_pending = true;
        SetActionState(state.container, ACTION_NONE);
        CVCSessionRequest sessionAbort = new CVCSessionRequest;
        sessionAbort.session_id = state.session_id;
        CVCResponseHandler handler;
        if (recoveryHandler)
            handler = new CVCNativeRecoveryAbortHandler(state);
        else
            handler = new CVCNativeAbortHandler(state);
        if (!ClippyVirtualCargoAPI.Post("/v1/session/abort", sessionAbort, handler))
            AbortUncertain(state, "could not dispatch the session abort");
    }

    static void AbortSucceeded(CVCContainerRuntime state)
    {
        if (!state)
            return;
        string completedSession = state.session_id;
        CVCSessionJournal.Remove(completedSession);
        state.session_id = "";
        state.abort_pending = false;
        state.abort_retry_attempt = 0;
        state.busy = false;
        state.phase = PHASE_IDLE;
        state.materialized.Clear();
        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        state.mark_request_prepared = false;
        if (state.player && state.pending_failure_reason != "")
            state.player.MessageStatus("Virtual cargo: " + state.pending_failure_reason);
        state.pending_failure_reason = "";
        state.player = null;
        CompleteRecovery(state);
        if (state.next_cursor != "")
            SetActionState(state.container, ACTION_NEXT);
        else
            SetActionState(state.container, ACTION_OPEN);
    }

    static void AbortUncertain(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        state.abort_pending = true;
        state.abort_retry_attempt++;
        SetActionState(state.container, ACTION_RETRY);
        int retryDelay = Math.Clamp(state.abort_retry_attempt * 2000, 2000, 30000);
        ErrorEx("[Clippy Virtual Cargo] Session abort is uncertain for " + state.provider_key + "; physical cargo remains locked and the abort will be retried: " + reason);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.RetryAbort, retryDelay, false, state, state.abort_retry_attempt);
    }

    static void RetryAbort(CVCContainerRuntime state, int retryAttempt)
    {
        if (!state || state.busy || !state.abort_pending || retryAttempt != state.abort_retry_attempt)
            return;
        DispatchAbort(state, state.recovering);
    }

    static void OpenUncertain(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        state.player = null;
        SetActionState(state.container, ACTION_NONE);
        ErrorEx("[Clippy Virtual Cargo] " + reason + " for " + state.provider_key + ". The container remains locked so an unknown OPEN session cannot be duplicated.");
    }

    static void CommitSucceeded(CVCContainerRuntime state, CVCSessionData session)
    {
        if (!state || !session)
            return;
        if (session.result_revision > 0)
            state.revision = session.result_revision;
        state.next_cursor = session.next_cursor;
        state.pending_cleanup_source_keys.Clear();
        state.cleanup_committed = false;
        if (session.cleanup_source_keys)
            CopyStrings(session.cleanup_source_keys, state.pending_cleanup_source_keys);
        else if (session.physical_source_keys)
            CopyStrings(session.physical_source_keys, state.pending_cleanup_source_keys);
        if (session.status == "CLEANED" || state.pending_cleanup_source_keys.Count() == 0)
        {
            FinishCleaned(state, session);
            return;
        }
        state.cleanup_committed = true;
        CVCSessionJournalEntry journal = CVCSessionJournal.Ensure(session);
        if (!journal)
        {
            CleanupUncertain(state, "the committed cleanup ledger could not be journaled");
            return;
        }
        journal.status = "COMMITTED";
        CopyStrings(state.pending_cleanup_source_keys, journal.cleanup_source_keys);
        if (!CVCSessionJournal.SaveEntry(journal))
        {
            CleanupUncertain(state, "the committed cleanup ledger could not be persisted");
            return;
        }
        BeginCleanup(state);
    }

    static void BeginCleanup(CVCContainerRuntime state)
    {
        if (!state || !state.container || state.session_id == "" || !state.cleanup_committed)
            return;
        state.busy = true;
        state.phase = PHASE_COMMITTING;
        SetActionState(state.container, ACTION_NONE);
        ConfirmCleanup(state, 1);
    }

    static void ConfirmCleanup(CVCContainerRuntime state, int attempt)
    {
        if (!state || !state.container || state.session_id == "")
        {
            CleanupUncertain(state, "cleanup state or container is unavailable");
            return;
        }
        bool remaining;
        state.internal_mutation = true;
        foreach (string sourceKey : state.pending_cleanup_source_keys)
        {
            ItemBase physical = CVCPhysicalRootIdentity.Find(state.container, sourceKey);
            if (physical)
            {
                remaining = true;
                physical.DeleteSafe();
            }
        }
        state.internal_mutation = false;
        if (remaining)
        {
            if (attempt < 12)
            {
                GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.ConfirmCleanup, 500, false, state, attempt + 1);
                return;
            }
            CleanupUncertain(state, "one or more committed physical roots still exist after cleanup retries");
            return;
        }
        CVCSessionCleanupRequest cleanup = new CVCSessionCleanupRequest;
        cleanup.session_id = state.session_id;
        CopyStrings(state.pending_cleanup_source_keys, cleanup.cleaned_source_keys);
        if (!ClippyVirtualCargoAPI.Post("/v1/session/ack-cleaned", cleanup, new CVCNativeCleanupHandler(state)))
            CleanupUncertain(state, "could not dispatch the cleanup acknowledgement");
    }

    static void CleanupUncertain(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        state.cleanup_retry_attempt++;
        SetActionState(state.container, ACTION_RETRY);
        int retryDelay = Math.Clamp(state.cleanup_retry_attempt * 2000, 2000, 30000);
        ErrorEx("[Clippy Virtual Cargo] Committed cleanup is incomplete for " + state.provider_key + "; the exact cleanup ledger will be retried: " + reason);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.RetryCleanup, retryDelay, false, state, state.cleanup_retry_attempt);
    }

    static void RetryCleanup(CVCContainerRuntime state, int retryAttempt)
    {
        if (!state || state.busy || state.phase != PHASE_RECOVERY || retryAttempt != state.cleanup_retry_attempt || state.pending_cleanup_source_keys.Count() == 0)
            return;
        BeginCleanup(state);
    }

    static void FinishCleaned(CVCContainerRuntime state, CVCSessionData session)
    {
        if (!state)
            return;
        if (session && session.result_revision > 0)
            state.revision = session.result_revision;
        if (session)
            state.next_cursor = session.next_cursor;
        CVCSessionJournal.Remove(state.session_id);
        state.session_id = "";
        state.busy = false;
        state.phase = PHASE_IDLE;
        state.cleanup_retry_attempt = 0;
        state.cleanup_committed = false;
        state.abort_pending = false;
        state.materialized.Clear();
        state.captured.Clear();
        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        state.mark_request_prepared = false;
        state.pending_cleanup_source_keys.Clear();
        state.player = null;
        CompleteRecovery(state);
        if (state.next_cursor != "")
            SetActionState(state.container, ACTION_NEXT);
        else
            SetActionState(state.container, ACTION_OPEN);
    }

    static void CommitFailed(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_RECOVERY;
        SetActionState(state.container, ACTION_RETRY);
        if (state.player)
            state.player.MessageStatus("Virtual cargo: " + reason);
        ErrorEx("[Clippy Virtual Cargo] " + reason + " for " + state.provider_key);
    }

    static void Fail(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        if (state.player)
            state.player.MessageStatus("Virtual cargo: " + reason);
        state.busy = false;
        state.phase = PHASE_IDLE;
        state.player = null;
        if (state.next_cursor != "")
            SetActionState(state.container, ACTION_NEXT);
        else
            SetActionState(state.container, ACTION_OPEN);
    }

    static void Tick()
    {
        if (!GetGame().IsServer())
            return;
        foreach (string key, CVCContainerRuntime state : s_States)
        {
            if (state && state.container && state.phase == PHASE_ACTIVE && (!state.player || !CVCContainerPolicy.CanAccess(state.container, state.player)))
                Commit(state);
        }
    }
}

class CVCMigrationObservationHandler: CVCResponseHandler
{
    override void OnSuccess(string raw)
    {
        CVCMigrationService.ObservationSucceeded();
    }

    override void OnFailure(string reason)
    {
        CVCMigrationService.ObservationFailed(reason);
    }
}

class CVCMigrationResolveHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCMigrationResolveHandler(CVCContainerRuntime state) { m_State = state; }
    override void OnSuccess(string raw)
    {
        CVCResolveEnvelope response = new CVCResolveEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            CVCMigrationService.Fail(m_State, "container resolve was rejected");
            return;
        }
        m_State.storage_id = response.data.storage_id;
        m_State.revision = response.data.revision;
        CVCMigrationService.Prepare(m_State);
    }
    override void OnFailure(string reason) { CVCMigrationService.Fail(m_State, "container resolve failed: " + reason); }
}

class CVCMigrationPrepareHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCMigrationPrepareHandler(CVCContainerRuntime state) { m_State = state; }
    override void OnSuccess(string raw)
    {
        CVCMigrationEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseMigration(raw, response, parseError) || !response.ok || !response.data)
        {
            CVCMigrationService.Fail(m_State, "migration prepare response was invalid");
            return;
        }
        CVCMigrationService.Continue(response.data, m_State);
    }
    override void OnFailure(string reason) { CVCMigrationService.Fail(m_State, "migration prepare failed: " + reason); }
}

class CVCMigrationCommitHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCMigrationCommitHandler(CVCContainerRuntime state) { m_State = state; }
    override void OnSuccess(string raw)
    {
        CVCMigrationEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseMigration(raw, response, parseError) || !response.ok || !response.data)
        {
            CVCMigrationService.Fail(m_State, "migration commit response was invalid");
            return;
        }
        CVCMigrationService.Cleanup(response.data, m_State);
    }
    override void OnFailure(string reason) { CVCMigrationService.Fail(m_State, "migration commit failed: " + reason); }
}

class CVCMigrationCleanupHandler: CVCResponseHandler
{
    protected ref CVCContainerRuntime m_State;
    void CVCMigrationCleanupHandler(CVCContainerRuntime state) { m_State = state; }
    override void OnSuccess(string raw)
    {
        CVCMigrationEnvelope response;
        string parseError;
        if (!ClippyVirtualCargoAPI.ParseMigration(raw, response, parseError) || !response.ok || !response.data)
        {
            CVCMigrationService.Fail(m_State, "migration cleanup response was invalid");
            return;
        }
        if (response.data.result_revision > 0)
            m_State.revision = response.data.result_revision;
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.FinishCleanup, 500, false, m_State);
    }
    override void OnFailure(string reason) { CVCMigrationService.Fail(m_State, "migration cleanup acknowledgement failed: " + reason); }
}

class CVCMigrationService
{
    static const int MAX_IDENTITY_RETRIES = 30;
    static const int MAX_OPERATION_RETRIES = 8;
    static const int MAX_CLEANUP_RETRIES = 12;
    protected static ref map<string, EntityAI> s_Containers = new map<string, EntityAI>;
    protected static ref map<string, bool> s_Queued = new map<string, bool>;
    protected static ref array<EntityAI> s_Queue = new array<EntityAI>;
    protected static ref array<string> s_QueueKeys = new array<string>;
    protected static ref map<string, ref CVCMigrationData> s_Pending = new map<string, ref CVCMigrationData>;
    protected static bool s_Started;
    protected static int s_StartTime;
    protected static int s_InFlight;
    protected static int s_Scanned;
    protected static bool s_CompleteLogged;
    protected static bool s_TickerScheduled;
    protected static ref array<ref CVCMigrationObservationRequest> s_ObservationQueue = new array<ref CVCMigrationObservationRequest>;
    protected static ref CVCMigrationObservationRequest s_CurrentObservation;
    protected static bool s_ObservationInFlight;
    protected static int s_ObservationRetries;
    protected static int s_ObservationQueued;
    protected static int s_TickCount;

    static void ScheduleCandidate(EntityAI container, int identityAttempt = 0)
    {
        if (!GetGame().IsServer() || !container)
            return;
        if (!s_TickerScheduled)
        {
            s_TickerScheduled = true;
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.Tick, 1000, true);
        }
        array<int> cargoSize = new array<int>;
        container.ConfigGetIntArray("itemsCargoSize", cargoSize);
        if (cargoSize.Count() < 2 || cargoSize[0] <= 0 || cargoSize[1] <= 0)
            return;
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(RegisterCandidate, 1000, false, container, identityAttempt);
    }

    static void RegisterCandidate(EntityAI container, int identityAttempt = 0)
    {
        if (!GetGame().IsServer() || !container || container.GetHierarchyParent() || !container.GetInventory() || !container.GetInventory().GetCargo())
            return;
        if (!CVCContainerPolicy.IsEligible(container) && (container.IsKindOf("Clothing") || container.IsKindOf("Bag_Base") || container.IsKindOf("FireplaceBase")))
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
        {
            if (identityAttempt < MAX_IDENTITY_RETRIES)
                GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(RegisterCandidate, 2000, false, container, identityAttempt + 1);
            else
                ErrorEx("[Clippy Virtual Cargo] Skipping " + container.GetType() + " because DayZ did not assign a stable persistent ID. It will be retried after its next persistence load.");
            return;
        }
        CVCContainerService.Register(container);
        if (CVCContainerService.IsPhysicalFallback(container))
            return;
        s_Containers.Set(key, container);
        CVCMigrationData recovery;
        if (s_Pending.Find(key, recovery) && recovery)
        {
            s_Pending.Remove(key);
            BeginRecovery(container, recovery);
            return;
        }
        Enqueue(container);
    }

    static void Enqueue(EntityAI container)
    {
        if (!container || CVCContainerService.IsPhysicalFallback(container))
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return;
        bool queued;
        if (s_Queued.Find(key, queued))
            return;
        s_Queued.Set(key, true);
        s_Queue.Insert(container);
        s_QueueKeys.Insert(key);
        s_CompleteLogged = false;
    }

    static void UnregisterCandidate(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key != "")
        {
            s_Containers.Remove(key);
            s_Queued.Remove(key);
        }
        for (int index = s_Queue.Count() - 1; index >= 0; index--)
        {
            if (s_Queue[index] == container)
            {
                s_Queue.RemoveOrdered(index);
                s_QueueKeys.RemoveOrdered(index);
            }
        }
    }

    static void Start()
    {
        if (s_Started)
            return;
        s_Started = true;
        s_StartTime = GetGame().GetTime();
        s_InFlight = 0;
        s_Scanned = 0;
        array<string> recoveryKeys = new array<string>;
        foreach (string pendingKey, CVCMigrationData pendingMigration : s_Pending)
            recoveryKeys.Insert(pendingKey);
        foreach (string recoveryKey : recoveryKeys)
        {
            EntityAI recoveryContainer;
            CVCMigrationData recovery;
            if (s_Containers.Find(recoveryKey, recoveryContainer) && recoveryContainer && s_Pending.Find(recoveryKey, recovery))
            {
                s_Pending.Remove(recoveryKey);
                BeginRecovery(recoveryContainer, recovery);
            }
        }
        Print("[Clippy Virtual Cargo] Automatic per-container SQL activation started with " + s_Queue.Count().ToString() + " queued cargo candidates.");
        Tick();
    }

    static void Tick()
    {
        if (!s_Started)
            return;
        if (!ClippyVirtualCargoAPI.IsReady())
            return;
        CVCSettings settings = CVCSettingsManager.Get();
        if (GetGame().GetTime() - s_StartTime < Math.Max(0, settings.MigrationStartDelaySeconds) * 1000)
            return;
        s_TickCount++;
        int maximum = Math.Clamp(settings.MigrationConcurrentContainers, 1, 32);
        int scanLimit = Math.Clamp(settings.MigrationScanBatchSize, 1, 64);
        if (s_TickCount == 1 || (s_TickCount % 30) == 0)
            Print("[Clippy Virtual Cargo] Migration scan progress: queued=" + s_Queue.Count().ToString() + ", in_flight=" + s_InFlight.ToString() + ", scanned=" + s_Scanned.ToString() + ", report_queue=" + s_ObservationQueue.Count().ToString() + ".");
        for (int scannedThisTick = 0; scannedThisTick < scanLimit; scannedThisTick++)
        {
            if (s_InFlight >= maximum || s_Queue.Count() == 0)
                break;
            EntityAI container = s_Queue.Get(0);
            string queuedKey = s_QueueKeys.Get(0);
            s_Queue.RemoveOrdered(0);
            s_QueueKeys.RemoveOrdered(0);
            s_Queued.Remove(queuedKey);
            if (!container)
                continue;
            Scan(container);
            s_Scanned++;
        }
        if (s_Queue.Count() == 0 && s_InFlight == 0 && s_ObservationQueue.Count() == 0 && !s_ObservationInFlight && !s_CurrentObservation && !s_CompleteLogged)
        {
            s_CompleteLogged = true;
            Print("[Clippy Virtual Cargo] Existing-cargo scan finished after processing " + s_Scanned.ToString() + " world cargo candidates.");
        }
    }

    static int PhysicalRootCount(EntityAI container)
    {
        if (!container || !container.GetInventory())
            return 0;
        CargoBase cargo = container.GetInventory().GetCargo();
        if (!cargo)
            return 0;
        int count;
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            if (cargoEntity && cargoEntity.GetHierarchyParent() == container)
                count++;
        }
        return count;
    }

    static bool IsUnlistedCandidate(EntityAI container)
    {
        if (!container || container.GetHierarchyParent() || container.IsKindOf("Clothing") || container.IsKindOf("Bag_Base") || container.IsKindOf("FireplaceBase"))
            return false;
        if (CVCSettingsManager.Get().ReportEmptyUnlistedStorageCandidates)
            return true;
        return PhysicalRootCount(container) >= Math.Max(1, CVCSettingsManager.Get().MinimumUnlistedPhysicalRoots);
    }

    static void Scan(EntityAI container)
    {
        if (!container || container.GetHierarchyParent())
            return;
        if (!CVCContainerPolicy.IsEligible(container))
        {
            if (CVCSettingsManager.Get().ReportUnlistedStorageCandidates && IsUnlistedCandidate(container))
                Observe(container, "UNLISTED_CANDIDATE", PhysicalRootCount(container), 0, 0, "Cargo-bearing world item is blocked by the current virtual cargo container policy.");
            return;
        }
        int roots = PhysicalRootCount(container);
        if (roots > 0 && !CVCSettingsManager.Get().EnableExistingCargoMigration)
        {
            Observe(container, "PHYSICAL_FALLBACK", roots, 0, 0, "Existing physical cargo migration is disabled for this fresh-server installation.");
            CVCContainerService.EnablePhysicalFallback(container, "existing physical cargo migration is disabled");
            return;
        }
        string validationError;
        CargoBase cargo = container.GetInventory().GetCargo();
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            ItemBase item = ItemBase.Cast(cargoEntity);
            if (cargoEntity && cargoEntity.GetHierarchyParent() == container && !item)
            {
                Observe(container, "REJECTED", roots, 0, 1, "Container holds a non-ItemBase cargo entity that has no safe state adapter.");
                CVCContainerService.EnablePhysicalFallback(container, "container holds unsupported non-ItemBase cargo");
                return;
            }
            if (item && item.GetHierarchyParent() == container && !CVCItemTreeCodec.CanCaptureTree(item, validationError))
            {
                Observe(container, "REJECTED", roots, 0, 1, validationError);
                CVCContainerService.EnablePhysicalFallback(container, validationError);
                return;
            }
            if (item && item.GetHierarchyParent() == container && SourceKey(item, index) == "")
            {
                RetryContainer(container, "A physical root does not yet have a stable DayZ persistent ID.");
                return;
            }
        }

        CVCContainerRuntime state = CVCContainerService.State(container);
        if (!state)
            return;
        if (state.physical_fallback)
            return;
        if (state.busy || state.phase != CVCContainerService.PHASE_IDLE)
        {
            RetryContainer(container, "Container is busy with a cargo session or recovery.");
            return;
        }
        state.migration_retries = 0;
        state.busy = true;
        state.phase = CVCContainerService.PHASE_MIGRATING;
        s_InFlight++;
        if (state.storage_id == "")
        {
            CVCResolveRequest resolve = new CVCResolveRequest;
            resolve.provider_id = CVCSettingsManager.Get().ProviderID;
            resolve.provider_key = state.provider_key;
            resolve.display_name = container.GetDisplayName();
            resolve.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            if (!ClippyVirtualCargoAPI.Post("/v1/storage/resolve", resolve, new CVCMigrationResolveHandler(state)))
                Fail(state, "could not dispatch container resolve");
            return;
        }
        Prepare(state);
    }

    static void RetryContainer(EntityAI container, string reason)
    {
        CVCContainerRuntime state = CVCContainerService.State(container);
        if (!state || state.physical_fallback)
            return;
        state.migration_retries++;
        if (state.migration_retries <= MAX_OPERATION_RETRIES)
        {
            int delay = Math.Clamp(state.migration_retries * 1000, 1000, 10000);
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(Enqueue, delay, false, container);
            return;
        }
        Observe(container, "PHYSICAL_FALLBACK", PhysicalRootCount(container), 0, 1, reason + " Retry limit reached; vanilla physical cargo is enabled until the next server restart.");
        CVCContainerService.EnablePhysicalFallback(container, reason);
    }

    static string SourceKey(ItemBase item, int cargoIndex)
    {
        int a;
        int b;
        int c;
        int d;
        item.GetPersistentID(a, b, c, d);
        if (a == 0 && b == 0 && c == 0 && d == 0)
            return "";
        return string.Format("%1:%2:%3:%4:%5", item.GetType(), a, b, c, d);
    }

    static ItemBase FindSource(EntityAI container, string sourceKey)
    {
        if (!container || !container.GetInventory())
            return null;
        CargoBase cargo = container.GetInventory().GetCargo();
        if (!cargo)
            return null;
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            ItemBase item = ItemBase.Cast(cargo.GetItem(index));
            if (item && item.GetHierarchyParent() == container && SourceKey(item, index) == sourceKey)
                return item;
        }
        return null;
    }

    static void Prepare(CVCContainerRuntime state)
    {
        if (!state || !state.container)
        {
            Fail(state, "container disappeared before migration prepare");
            return;
        }
        CVCMigrationPrepareRequest request = new CVCMigrationPrepareRequest;
        request.storage_id = state.storage_id;
        request.expected_revision = state.revision;
        request.container_class = state.container.GetType();
        int limit = Math.Clamp(CVCSettingsManager.Get().MigrationBatchRootLimit, 1, 200);
        CargoBase cargo = state.container.GetInventory().GetCargo();
        for (int index = 0; index < cargo.GetItemCount() && request.roots.Count() < limit; index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            ItemBase item = ItemBase.Cast(cargoEntity);
            if (cargoEntity && cargoEntity.GetHierarchyParent() == state.container && !item)
            {
                FallbackBeforePrepare(state, "container holds unsupported non-ItemBase cargo");
                return;
            }
            if (!item || item.GetHierarchyParent() != state.container)
                continue;
            string validationError;
            if (!CVCItemTreeCodec.CanCaptureTree(item, validationError))
            {
                FallbackBeforePrepare(state, validationError);
                return;
            }
            CVCMigrationRootRequest root = new CVCMigrationRootRequest;
            root.source_key = SourceKey(item, index);
            if (root.source_key == "")
            {
                FallbackBeforePrepare(state, "a physical root lost its stable persistent ID before migration prepare");
                return;
            }
            root.item = CVCItemTreeCodec.Capture(item);
            if (!root.item)
            {
                FallbackBeforePrepare(state, "a physical root could not be serialized before migration prepare");
                return;
            }
            request.roots.Insert(root);
        }
        if (request.roots.Count() == 0)
        {
            Finish(state, "MIGRATED", "No physical roots remain.", false);
            return;
        }
        state.migration_prepare_dispatched = true;
        if (!ClippyVirtualCargoAPI.Post("/v1/migration/prepare", request, new CVCMigrationPrepareHandler(state)))
        {
            state.migration_prepare_dispatched = false;
            Fail(state, "could not dispatch migration prepare");
        }
    }

    static void FallbackBeforePrepare(CVCContainerRuntime state, string reason)
    {
        if (!state || state.active_migration || state.migration_prepare_dispatched)
        {
            Fail(state, reason);
            return;
        }
        if (state.container)
        {
            Observe(state.container, "PHYSICAL_FALLBACK", PhysicalRootCount(state.container), 0, 1, reason);
            CVCContainerService.EnablePhysicalFallback(state.container, reason);
        }
        state.busy = false;
        state.phase = CVCContainerService.PHASE_IDLE;
        if (s_InFlight > 0)
            s_InFlight--;
    }

    static void Continue(CVCMigrationData migration, CVCContainerRuntime state)
    {
        if (!migration || !state)
        {
            Fail(state, "migration state is missing");
            return;
        }
        state.active_migration = migration;
        if (migration.status == "PREPARED")
        {
            CVCMigrationRequest commit = new CVCMigrationRequest;
            commit.migration_id = migration.migration_id;
            if (!ClippyVirtualCargoAPI.Post("/v1/migration/commit", commit, new CVCMigrationCommitHandler(state)))
                Fail(state, "could not dispatch migration commit");
            return;
        }
        Cleanup(migration, state);
    }

    static void Cleanup(CVCMigrationData migration, CVCContainerRuntime state)
    {
        if (!migration || !state || !state.container || !migration.source_roots)
        {
            Fail(state, "migration cleanup state is missing");
            return;
        }
        if (migration.result_revision > 0)
            state.revision = migration.result_revision;
        state.active_migration = migration;
        state.internal_mutation = true;
        for (int sourceIndex = migration.source_roots.Count() - 1; sourceIndex >= 0; sourceIndex--)
        {
            CVCMigrationSourceData source = migration.source_roots[sourceIndex];
            if (!source)
                continue;
            ItemBase physical = FindSource(state.container, source.source_key);
            if (physical)
                physical.DeleteSafe();
        }
        state.internal_mutation = false;
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.ConfirmCleanup, 250, false, migration, state, 1);
    }

    static void ConfirmCleanup(CVCMigrationData migration, CVCContainerRuntime state, int attempt)
    {
        if (!migration || !state || !state.container || !migration.source_roots)
        {
            Fail(state, "migration cleanup confirmation state is missing");
            return;
        }

        bool remaining;
        CVCMigrationCleanupRequest cleanup = new CVCMigrationCleanupRequest;
        cleanup.migration_id = migration.migration_id;
        state.internal_mutation = true;
        for (int sourceIndex = migration.source_roots.Count() - 1; sourceIndex >= 0; sourceIndex--)
        {
            CVCMigrationSourceData source = migration.source_roots[sourceIndex];
            if (!source || source.source_key == "")
                continue;
            ItemBase physical = FindSource(state.container, source.source_key);
            if (physical)
            {
                remaining = true;
                physical.DeleteSafe();
            }
            else
            {
                cleanup.cleaned_source_keys.Insert(source.source_key);
            }
        }
        state.internal_mutation = false;

        if (remaining)
        {
            if (attempt >= MAX_CLEANUP_RETRIES)
            {
                Fail(state, "physical roots still exist after the cleanup retry limit");
                return;
            }
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.ConfirmCleanup, 500, false, migration, state, attempt + 1);
            return;
        }
        if (cleanup.cleaned_source_keys.Count() != migration.source_roots.Count())
        {
            Fail(state, "one or more migration roots lacked a stable cleanup identity");
            return;
        }
        if (!ClippyVirtualCargoAPI.Post("/v1/migration/ack-cleaned", cleanup, new CVCMigrationCleanupHandler(state)))
            Fail(state, "could not dispatch physical cleanup acknowledgement");
    }

    static void QueueRecovery(CVCMigrationData migration)
    {
        if (!migration || migration.provider_key == "")
            return;
        EntityAI container;
        if (s_Containers.Find(migration.provider_key, container) && container)
        {
            BeginRecovery(container, migration);
            return;
        }
        s_Pending.Set(migration.provider_key, migration);
        ErrorEx("[Clippy Virtual Cargo] Waiting for container " + migration.provider_key + " to resume migration " + migration.migration_id);
    }

    static void BeginRecovery(EntityAI container, CVCMigrationData migration)
    {
        if (!s_Started || !container || !migration)
        {
            if (migration)
                s_Pending.Set(migration.provider_key, migration);
            return;
        }
        CVCContainerRuntime state = CVCContainerService.State(container);
        if (!state)
        {
            s_Pending.Set(migration.provider_key, migration);
            return;
        }
        if (state.busy)
        {
            s_Pending.Set(migration.provider_key, migration);
            return;
        }
        state.storage_id = migration.storage_id;
        state.revision = migration.expected_revision;
        if (migration.result_revision > 0)
            state.revision = migration.result_revision;
        state.busy = true;
        state.phase = CVCContainerService.PHASE_MIGRATING;
        state.physical_fallback = false;
        state.active_migration = migration;
        CVCContainerService.SetActionState(container, CVCContainerService.ACTION_NONE);
        s_InFlight++;
        Continue(migration, state);
    }

    static void FinishCleanup(CVCContainerRuntime state)
    {
        if (!state || !state.container)
        {
            Finish(state, "MIGRATED", "Container disappeared after cleanup.", false);
            return;
        }
        bool more = PhysicalRootCount(state.container) > 0;
        if (more)
            Finish(state, "MIGRATED", "Additional physical roots remain for another batch.", true);
        else
            Finish(state, "MIGRATED", "Physical cargo imported and removed.", false);
    }

    static void Finish(CVCContainerRuntime state, string status, string detail, bool requeue)
    {
        if (state)
        {
            state.busy = false;
            state.phase = CVCContainerService.PHASE_IDLE;
            state.migration_retries = 0;
            state.migration_prepare_dispatched = false;
            state.active_migration = null;
            if (state.container)
            {
                Observe(state.container, status, PhysicalRootCount(state.container), 0, 0, detail);
                if (requeue)
                    Enqueue(state.container);
                else if (!state.physical_fallback)
                    CVCContainerService.SetActionState(state.container, CVCContainerService.ACTION_OPEN);
            }
        }
        if (s_InFlight > 0)
            s_InFlight--;
    }

    static void Fail(CVCContainerRuntime state, string reason)
    {
        if (state && state.container)
            Observe(state.container, "FAILED", PhysicalRootCount(state.container), 0, 1, reason);
        ErrorEx("[Clippy Virtual Cargo] Existing-cargo migration failed: " + reason);
        if (state)
        {
            state.busy = false;
            state.phase = CVCContainerService.PHASE_IDLE;
            state.migration_retries++;
        }
        if (s_InFlight > 0)
            s_InFlight--;
        if (state && state.migration_retries <= MAX_OPERATION_RETRIES)
        {
            int delay = Math.Clamp(state.migration_retries * 2000, 2000, 15000);
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.RetryFailed, delay, false, state);
        }
        else if (state)
        {
            if (!state.active_migration && !state.migration_prepare_dispatched && state.container)
                CVCContainerService.EnablePhysicalFallback(state.container, "migration could not start after the retry limit: " + reason);
            else
                ErrorEx("[Clippy Virtual Cargo] Migration retry limit reached for " + state.provider_key + ". SQL may already contain prepared or committed work, so physical cargo remains locked for safe recovery after reload.");
        }
    }

    static void RetryFailed(CVCContainerRuntime state)
    {
        if (!s_Started || !state || state.busy || state.physical_fallback)
            return;
        if (!state.container)
        {
            if (state.active_migration)
                s_Pending.Set(state.provider_key, state.active_migration);
            return;
        }
        state.busy = true;
        state.phase = CVCContainerService.PHASE_MIGRATING;
        s_InFlight++;
        if (state.active_migration)
        {
            Continue(state.active_migration, state);
            return;
        }
        if (state.storage_id == "")
        {
            CVCResolveRequest resolve = new CVCResolveRequest;
            resolve.provider_id = CVCSettingsManager.Get().ProviderID;
            resolve.provider_key = state.provider_key;
            resolve.display_name = state.container.GetDisplayName();
            resolve.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            if (!ClippyVirtualCargoAPI.Post("/v1/storage/resolve", resolve, new CVCMigrationResolveHandler(state)))
                Fail(state, "could not retry container resolve");
            return;
        }
        Prepare(state);
    }

    static void Observe(EntityAI container, string status, int physicalRoots, int capturedRoots, int rejectedRoots, string detail)
    {
        if (!container || !ClippyVirtualCargoAPI.IsReady())
            return;
        CVCMigrationObservationRequest observation = new CVCMigrationObservationRequest;
        observation.provider_id = CVCSettingsManager.Get().ProviderID;
        observation.provider_key = CVCContainerPolicy.ProviderKey(container);
        observation.container_class = container.GetType();
        observation.status = status;
        observation.physical_roots = physicalRoots;
        observation.captured_roots = capturedRoots;
        observation.rejected_roots = rejectedRoots;
        observation.detail = detail;
        s_ObservationQueue.Insert(observation);
        s_ObservationQueued++;
        if (s_ObservationQueued == 1)
            Print("[Clippy Virtual Cargo] First migration observation: provider=" + observation.provider_key + ", class=" + observation.container_class + ", status=" + observation.status + ".");
        s_CompleteLogged = false;
        PumpObservations();
    }

    static void PumpObservations()
    {
        if (s_ObservationInFlight || !ClippyVirtualCargoAPI.IsReady())
            return;
        if (!s_CurrentObservation)
        {
            if (s_ObservationQueue.Count() == 0)
                return;
            s_CurrentObservation = s_ObservationQueue.Get(0);
            s_ObservationQueue.RemoveOrdered(0);
            s_ObservationRetries = 0;
        }
        s_ObservationInFlight = true;
        ClippyVirtualCargoAPI.Post("/v1/migration/observe", s_CurrentObservation, new CVCMigrationObservationHandler);
    }

    static void ObservationSucceeded()
    {
        s_ObservationInFlight = false;
        s_CurrentObservation = null;
        s_ObservationRetries = 0;
        PumpObservations();
    }

    static void ObservationFailed(string reason)
    {
        s_ObservationInFlight = false;
        s_ObservationRetries++;
        if (s_ObservationRetries <= 8)
        {
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.PumpObservations, 2000, false);
            return;
        }
        ErrorEx("[Clippy Virtual Cargo] Migration reporting failed after retries: " + reason);
        s_CurrentObservation = null;
        s_ObservationRetries = 0;
        PumpObservations();
    }
}

class ActionCVCOpenNativeCargo: ActionSingleUseBase
{
    void ActionCVCOpenNativeCargo() { m_Text = "Open virtual cargo"; }
    override void CreateConditionComponents()
    {
        m_ConditionItem = new CCINone;
        m_ConditionTarget = new CCTObject(UAMaxDistances.DEFAULT);
    }
    override bool ActionCondition(PlayerBase player, ActionTarget target, ItemBase item)
    {
        EntityAI container = EntityAI.Cast(target.GetObject());
        if (!GetGame().IsServer())
            return !item && CVCContainerService.ClientCanInteract(container, player) && CVCContainerService.CanOpen(container);
        return !item && CVCContainerPolicy.CanAccess(container, player) && CVCContainerService.CanOpen(container);
    }
    override void OnExecuteServer(ActionData action_data)
    {
        CVCContainerService.Open(EntityAI.Cast(action_data.m_Target.GetObject()), action_data.m_Player, false);
    }
}

class ActionCVCOpenNextPage: ActionSingleUseBase
{
    void ActionCVCOpenNextPage() { m_Text = "Open next virtual cargo page"; }
    override void CreateConditionComponents()
    {
        m_ConditionItem = new CCINone;
        m_ConditionTarget = new CCTObject(UAMaxDistances.DEFAULT);
    }
    override bool ActionCondition(PlayerBase player, ActionTarget target, ItemBase item)
    {
        EntityAI container = EntityAI.Cast(target.GetObject());
        if (!GetGame().IsServer())
            return !item && CVCContainerService.ClientCanInteract(container, player) && CVCContainerService.HasNextPage(container);
        return !item && CVCContainerPolicy.CanAccess(container, player) && CVCContainerService.HasNextPage(container);
    }
    override void OnExecuteServer(ActionData action_data)
    {
        CVCContainerService.Open(EntityAI.Cast(action_data.m_Target.GetObject()), action_data.m_Player, true);
    }
}

class ActionCVCRetrySave: ActionSingleUseBase
{
    void ActionCVCRetrySave() { m_Text = "Retry virtual cargo save"; }
    override void CreateConditionComponents()
    {
        m_ConditionItem = new CCINone;
        m_ConditionTarget = new CCTObject(UAMaxDistances.DEFAULT);
    }
    override bool ActionCondition(PlayerBase player, ActionTarget target, ItemBase item)
    {
        EntityAI container = EntityAI.Cast(target.GetObject());
        if (!GetGame().IsServer())
            return !item && CVCContainerService.ClientCanInteract(container, player) && CVCContainerService.NeedsRetry(container);
        return !item && CVCContainerPolicy.CanAccess(container, player) && CVCContainerService.NeedsRetry(container);
    }
    override void OnExecuteServer(ActionData action_data)
    {
        CVCContainerService.Retry(EntityAI.Cast(action_data.m_Target.GetObject()), action_data.m_Player);
    }
}

modded class ItemBase
{
    protected string m_CVCVirtualItemID;
    protected int m_CVCActionState;
    protected bool m_CVCManagedShell;
    protected bool m_CVCPreviousAllowDamage;

    void ItemBase()
    {
        RegisterNetSyncVariableInt("m_CVCActionState", CVCContainerService.ACTION_NONE, CVCContainerService.ACTION_RETRY);
        RegisterNetSyncVariableBool("m_CVCManagedShell");
    }

    string CVCGetVirtualItemID() { return m_CVCVirtualItemID; }
    void CVCSetVirtualItemID(string itemID) { m_CVCVirtualItemID = itemID; }
    int CVCGetActionState() { return m_CVCActionState; }
    bool CVCIsManagedShell() { return m_CVCManagedShell; }
    void CVCSetManagedShell(bool managed)
    {
        if (!GetGame().IsServer())
            return;
        if (managed)
        {
            if (!m_CVCManagedShell)
                m_CVCPreviousAllowDamage = GetAllowDamage();
            m_CVCManagedShell = true;
            SetAllowDamage(false);
        }
        else
        {
            if (!m_CVCManagedShell)
                return;
            m_CVCManagedShell = false;
            SetAllowDamage(m_CVCPreviousAllowDamage);
        }
        SetSynchDirty();
    }
    void CVCSetActionState(int actionState)
    {
        if (!GetGame().IsServer() || m_CVCActionState == actionState)
            return;
        m_CVCActionState = actionState;
        SetSynchDirty();
    }

    override void EEInit()
    {
        super.EEInit();
        if (GetGame().IsServer())
        {
            CVCContainerService.Register(this);
            CVCMigrationService.ScheduleCandidate(this);
        }
    }

    override void AfterStoreLoad()
    {
        super.AfterStoreLoad();
        if (GetGame().IsServer())
        {
            CVCContainerService.Register(this);
            CVCMigrationService.ScheduleCandidate(this);
        }
    }

    override void SetActions()
    {
        super.SetActions();
        AddAction(ActionCVCOpenNativeCargo);
        AddAction(ActionCVCOpenNextPage);
        AddAction(ActionCVCRetrySave);
    }

    override bool CanReceiveItemIntoCargo(EntityAI item)
    {
        if (GetGame().IsServer() && !CVCContainerService.AllowsPhysicalCargo(this))
            return false;
        return super.CanReceiveItemIntoCargo(item);
    }

    override bool CanPutInCargo(EntityAI parent)
    {
        if (m_CVCManagedShell)
            return false;
        return super.CanPutInCargo(parent);
    }

    override bool CanPutIntoHands(EntityAI parent)
    {
        if (m_CVCManagedShell)
            return false;
        return super.CanPutIntoHands(parent);
    }

    override bool CanReleaseCargo(EntityAI cargo)
    {
        if (GetGame().IsServer() && !CVCContainerService.AllowsPhysicalCargo(this))
            return false;
        return super.CanReleaseCargo(cargo);
    }

    override void EEDelete(EntityAI parent)
    {
        if (GetGame().IsServer())
        {
            CVCContainerService.Unregister(this);
            CVCMigrationService.UnregisterCandidate(this);
        }
        super.EEDelete(parent);
    }

    override void OnItemLocationChanged(EntityAI old_owner, EntityAI new_owner)
    {
        super.OnItemLocationChanged(old_owner, new_owner);
        if (!GetGame().IsServer())
            return;
        CVCSettings settings = CVCSettingsManager.Get();
        if (!CVCContainerPolicy.IsConfiguredContainerClass(this) && !settings.ReportUnlistedStorageCandidates)
            return;
        if (GetHierarchyParent())
        {
            CVCContainerService.Unregister(this);
            CVCMigrationService.UnregisterCandidate(this);
            CVCSetManagedShell(false);
            return;
        }
        CVCContainerService.Register(this);
        CVCMigrationService.ScheduleCandidate(this);
    }
}

modded class CarScript
{
    protected int m_CVCActionState;

    void CarScript()
    {
        RegisterNetSyncVariableInt("m_CVCActionState", CVCContainerService.ACTION_NONE, CVCContainerService.ACTION_RETRY);
    }

    int CVCGetActionState() { return m_CVCActionState; }
    void CVCSetActionState(int actionState)
    {
        if (!GetGame().IsServer() || m_CVCActionState == actionState)
            return;
        m_CVCActionState = actionState;
        SetSynchDirty();
    }

    override void EEInit()
    {
        super.EEInit();
        if (GetGame().IsServer())
        {
            CVCContainerService.Register(this);
            CVCMigrationService.RegisterCandidate(this);
        }
    }

    override void AfterStoreLoad()
    {
        super.AfterStoreLoad();
        if (GetGame().IsServer())
        {
            CVCContainerService.Register(this);
            CVCMigrationService.RegisterCandidate(this);
        }
    }

    override void SetActions()
    {
        super.SetActions();
        AddAction(ActionCVCOpenNativeCargo);
        AddAction(ActionCVCOpenNextPage);
        AddAction(ActionCVCRetrySave);
    }

    override bool CanReceiveItemIntoCargo(EntityAI item)
    {
        if (GetGame().IsServer() && !CVCContainerService.AllowsPhysicalCargo(this))
            return false;
        return super.CanReceiveItemIntoCargo(item);
    }

    override bool CanReleaseCargo(EntityAI cargo)
    {
        if (GetGame().IsServer() && !CVCContainerService.AllowsPhysicalCargo(this))
            return false;
        return super.CanReleaseCargo(cargo);
    }

    override void EEDelete(EntityAI parent)
    {
        if (GetGame().IsServer())
        {
            CVCContainerService.Unregister(this);
            CVCMigrationService.UnregisterCandidate(this);
        }
        super.EEDelete(parent);
    }
}

modded class PlayerBase
{
    override void OnRPC(PlayerIdentity sender, int rpc_type, ParamsReadContext ctx)
    {
        super.OnRPC(sender, rpc_type, ctx);
        if (rpc_type == CVCRPC.OPEN_INVENTORY && GetGame().IsClient())
        {
            Param1<string> openData;
            if (ctx.Read(openData) && GetGame().GetMission())
                GetGame().GetMission().ShowInventory();
        }
        else if (rpc_type == CVCRPC.CLOSE_INVENTORY && GetGame().IsServer())
        {
            CVCContainerService.CloseForPlayer(this);
        }
    }
}

modded class ActionConstructor
{
    override void RegisterActions(TTypenameArray actions)
    {
        super.RegisterActions(actions);
        actions.Insert(ActionCVCOpenNativeCargo);
        actions.Insert(ActionCVCOpenNextPage);
        actions.Insert(ActionCVCRetrySave);
    }
}
