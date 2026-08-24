class CVCBuildInfo
{
    static const string VERSION = "1.2.0";
    static const int MAINTAINER_REVISION = 62;

    static string Label()
    {
        return VERSION + "-r" + MAINTAINER_REVISION.ToString();
    }
}

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
        node.state.cvc_provider_key = CVCProviderIdentityRegistry.KnownProviderKey(item);

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
            if (node.state.cvc_provider_key != "")
                CVCProviderIdentityRegistry.Bind(item, node.state.cvc_provider_key);
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

class CVCProviderIdentityBinding
{
    string physical_key;
    string provider_key;
}

class CVCProviderIdentityRegistryData
{
    int version = 2;
    ref array<string> provider_keys = new array<string>;
    ref array<ref CVCProviderIdentityBinding> bindings = new array<ref CVCProviderIdentityBinding>;
}

class CVCProviderIdentityRegistry
{
    static const string PATH = "$profile:ClippyVirtualCargo/ProviderIdentityRegistry.json";
    protected static ref CVCProviderIdentityRegistryData s_Data;
    protected static bool s_Loaded;

    protected static void EnsureLoaded()
    {
        if (s_Loaded)
            return;
        s_Loaded = true;
        s_Data = new CVCProviderIdentityRegistryData;
        if (FileExist(PATH))
        {
            CVCProviderIdentityRegistryData loaded;
            string loadError;
            if (JsonFileLoader<CVCProviderIdentityRegistryData>.LoadFile(PATH, loaded, loadError) && loaded)
                s_Data = loaded;
            else if (loadError != "")
                ErrorEx("[Clippy Virtual Cargo] Provider identity registry could not be loaded: " + loadError);
        }
        if (!s_Data.provider_keys)
            s_Data.provider_keys = new array<string>;
        if (!s_Data.bindings)
            s_Data.bindings = new array<ref CVCProviderIdentityBinding>;
        s_Data.version = 2;
    }

    static string PhysicalKey(EntityAI entity)
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

    static string KnownProviderKey(EntityAI entity)
    {
        string physicalKey = PhysicalKey(entity);
        if (physicalKey == "")
            return "";
        EnsureLoaded();
        foreach (CVCProviderIdentityBinding binding : s_Data.bindings)
        {
            if (binding && binding.physical_key == physicalKey && binding.provider_key != "")
                return binding.provider_key;
        }
        if (s_Data.provider_keys.Find(physicalKey) >= 0)
            return physicalKey;
        return "";
    }

    static string ResolveProviderKey(EntityAI entity)
    {
        string known = KnownProviderKey(entity);
        if (known != "")
            return known;
        return PhysicalKey(entity);
    }

    static void Save()
    {
        MakeDirectory("$profile:ClippyVirtualCargo");
        string saveError;
        if (!JsonFileLoader<CVCProviderIdentityRegistryData>.SaveFile(PATH, s_Data, saveError))
            ErrorEx("[Clippy Virtual Cargo] Provider identity registry could not be saved: " + saveError);
    }

    static void Bind(EntityAI entity, string providerKey)
    {
        if (!GetGame().IsServer() || !entity || providerKey == "")
            return;
        string physicalKey = PhysicalKey(entity);
        if (physicalKey == "")
            return;
        EnsureLoaded();
        foreach (CVCProviderIdentityBinding binding : s_Data.bindings)
        {
            if (binding && binding.physical_key == physicalKey)
            {
                if (binding.provider_key == providerKey)
                    return;
                binding.provider_key = providerKey;
                Save();
                return;
            }
        }
        CVCProviderIdentityBinding created = new CVCProviderIdentityBinding;
        created.physical_key = physicalKey;
        created.provider_key = providerKey;
        s_Data.bindings.Insert(created);
        if (s_Data.provider_keys.Find(providerKey) < 0)
            s_Data.provider_keys.Insert(providerKey);
        Save();
    }

    static void Remember(EntityAI entity)
    {
        if (!GetGame().IsServer())
            return;
        string providerKey = ResolveProviderKey(entity);
        if (providerKey != "")
            Bind(entity, providerKey);
    }

    static bool Contains(EntityAI entity)
    {
        return KnownProviderKey(entity) != "";
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

    protected static void CaptureLocation(ItemBase item, CVCItemLocation storedLocation)
    {
        if (!item || !storedLocation)
            return;

        InventoryLocation location = new InventoryLocation;
        if (!item.GetInventory().GetCurrentInventoryLocation(location) || !location.IsValid())
            return;

        storedLocation.slot = location.GetSlot();
        storedLocation.index = location.GetIdx();
        storedLocation.row = location.GetRow();
        storedLocation.col = location.GetCol();

        int locationType = location.GetType();
        if (locationType == InventoryLocationType.ATTACHMENT)
        {
            storedLocation.kind = "attachment";
            storedLocation.flip = false;
        }
        else if (locationType == InventoryLocationType.HANDS)
        {
            storedLocation.kind = "hands";
            storedLocation.flip = false;
        }
        else if (locationType == InventoryLocationType.CARGO || locationType == InventoryLocationType.PROXYCARGO)
        {
            storedLocation.kind = "cargo";
            // DayZ's inventory move code reads GetFlipCargo() from the item itself when
            // rebuilding a cargo InventoryLocation. Use that value as the authoritative
            // orientation instead of relying only on the location snapshot.
            storedLocation.flip = item.GetInventory().GetFlipCargo();
        }
        else
        {
            storedLocation.kind = "unsupported";
            storedLocation.flip = location.GetFlip();
        }
    }

    protected static bool IsLegacyCargoLocation(CVCItemLocation location)
    {
        return location && location.index == -1 && location.row == -1 && location.col == -1;
    }

    protected static bool IsExactCargoLocation(CVCItemLocation location)
    {
        return location && location.index >= 0 && location.row >= 0 && location.col >= 0;
    }

    protected static bool ValidateExactCargoLocation(ItemBase item, EntityAI destination, CVCItemLocation expected, out string detail)
    {
        if (!item || !destination || !expected)
        {
            detail = "item, destination, or saved location is missing";
            return false;
        }

        InventoryLocation actual = new InventoryLocation;
        bool valid = item.GetInventory().GetCurrentInventoryLocation(actual) && actual.IsValid();
        if (!valid)
        {
            detail = "restored inventory location is invalid";
            return false;
        }
        if (actual.GetType() != InventoryLocationType.CARGO || actual.GetParent() != destination)
        {
            detail = "restored item is not in the expected parent cargo";
            return false;
        }

        bool cargoFlip = item.GetInventory().GetFlipCargo();
        if (actual.GetIdx() != expected.index || actual.GetRow() != expected.row || actual.GetCol() != expected.col || actual.GetFlip() != expected.flip || cargoFlip != expected.flip)
        {
            detail = "expected_index=" + expected.index.ToString();
            detail += " expected_row=" + expected.row.ToString() + " expected_col=" + expected.col.ToString() + " expected_flip=" + expected.flip.ToString();
            detail += " actual_index=" + actual.GetIdx().ToString() + " actual_row=" + actual.GetRow().ToString() + " actual_col=" + actual.GetCol().ToString();
            detail += " actual_flip=" + actual.GetFlip().ToString() + " cargo_flip=" + cargoFlip.ToString();
            return false;
        }
        return true;
    }

    protected static bool RelocateCargoItemExact(ItemBase item, EntityAI destination, CVCItemLocation expected, out string detail)
    {
        if (!item || !destination || !expected || !IsExactCargoLocation(expected))
        {
            detail = "item, destination, or saved exact cargo location is missing";
            return false;
        }

        string currentDetail;
        if (ValidateExactCargoLocation(item, destination, expected, currentDetail))
            return true;

        InventoryLocation source = new InventoryLocation;
        if (!item.GetInventory().GetCurrentInventoryLocation(source) || !source.IsValid())
        {
            detail = "staged cargo item has no valid source location";
            return false;
        }
        if (source.GetType() != InventoryLocationType.CARGO || source.GetParent() != destination)
        {
            detail = "staged cargo item is not inside the expected destination cargo";
            return false;
        }

        if (source.GetIdx() == expected.index && source.GetRow() == expected.row && source.GetCol() == expected.col)
        {
            item.GetInventory().SetFlipCargo(expected.flip);
            return ValidateExactCargoLocation(item, destination, expected, detail);
        }

        InventoryLocation target = new InventoryLocation;
        target.SetCargo(destination, item, expected.index, expected.row, expected.col, expected.flip);
        if (!GameInventory.LocationSyncMoveEntity(source, target))
        {
            detail = "DayZ rejected the exact in-cargo relocation after normal staging";
            return false;
        }
        item.GetInventory().SetFlipCargo(expected.flip);
        return ValidateExactCargoLocation(item, destination, expected, detail);
    }

    protected static ItemBase CreateExactCargoItem(EntityAI destination, CVCItemNode node, out string reason)
    {
        if (!destination || !node || !node.location || !IsExactCargoLocation(node.location))
        {
            reason = "exact cargo restore request is incomplete";
            return null;
        }

        string locationDetail = "item_id=" + node.item_id + " class=" + node.class_name;
        locationDetail += " index=" + node.location.index.ToString() + " row=" + node.location.row.ToString() + " col=" + node.location.col.ToString() + " flip=" + node.location.flip.ToString();

        EntityAI created = destination.GetInventory().CreateEntityInCargoEx(node.class_name, node.location.index, node.location.row, node.location.col, node.location.flip);
        ItemBase item = ItemBase.Cast(created);
        if (!item)
        {
            // A direct location-create fallback bypasses the normal DayZ cargo creation
            // path and can leave children counted by the server while the nested container
            // UI does not expose them. Stage through normal native cargo creation, then move
            // that already-networked entity
            // to the saved exact cell. The temporary location is never accepted as the
            // final restore location.
            CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_STAGE_NATIVE_" + node.item_id, "result=stage_for_exact_relocation " + locationDetail, 100);
            created = destination.GetInventory().CreateEntityInCargo(node.class_name);
            item = ItemBase.Cast(created);
            if (!item)
            {
                reason = "could not stage " + node.class_name + " in native cargo before exact relocation";
                CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_STAGE_BLOCKED_" + node.item_id, "result=fail_closed " + locationDetail, 100);
                return null;
            }

            string relocateDetail;
            if (!RelocateCargoItemExact(item, destination, node.location, relocateDetail))
            {
                item.DeleteSafe();
                reason = "could not move staged " + node.class_name + " to its saved cargo position and orientation";
                CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_RELOCATE_BLOCKED_" + node.item_id, "result=fail_closed " + locationDetail + " " + relocateDetail, 100);
                return null;
            }
            CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_RELOCATE_EXACT_" + node.item_id, "result=exact_after_native_stage " + locationDetail, 100);
        }

        item.GetInventory().SetFlipCargo(node.location.flip);

        string mismatchDetail;
        if (!ValidateExactCargoLocation(item, destination, node.location, mismatchDetail))
        {
            item.DeleteSafe();
            reason = "restored " + node.class_name + " did not retain its saved cargo position and orientation";
            CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_EXACT_MISMATCH_" + node.item_id, "result=fail_closed " + locationDetail + " " + mismatchDetail, 100);
            return null;
        }
        return item;
    }

    protected static ItemBase CreateStoredCargoItem(EntityAI destination, CVCItemNode node, out string reason)
    {
        if (!destination || !node || !node.location)
        {
            reason = "stored cargo item or location is missing";
            return null;
        }
        if (IsExactCargoLocation(node.location))
            return CreateExactCargoItem(destination, node, reason);
        if (!IsLegacyCargoLocation(node.location))
        {
            reason = "stored cargo location is partially populated for " + node.class_name;
            return null;
        }

        // Old records without coordinates cannot be restored to an unknown original cell.
        // Keep compatibility for those records only. New/exact records must never auto-place.
        CVCContainerDiagnostics.TracePortable(destination, "CARGO_RESTORE_LEGACY_AUTOPLACE_" + node.item_id, "result=legacy_only item_id=" + node.item_id + " class=" + node.class_name + " index=-1 row=-1 col=-1", 100);
        ItemBase legacy = ItemBase.Cast(destination.GetInventory().CreateEntityInCargo(node.class_name));
        if (!legacy)
            reason = "could not restore legacy cargo item " + node.class_name + " by native auto-placement";
        return legacy;
    }

    protected static bool ValidateAttachmentLocation(ItemBase item, EntityAI destination, CVCItemLocation expected, out string detail)
    {
        if (!item || !destination || !expected)
        {
            detail = "attachment item, destination, or saved location is missing";
            return false;
        }
        InventoryLocation actual = new InventoryLocation;
        if (!item.GetInventory().GetCurrentInventoryLocation(actual) || !actual.IsValid())
        {
            detail = "restored attachment location is invalid";
            return false;
        }
        if (actual.GetType() != InventoryLocationType.ATTACHMENT || actual.GetParent() != destination || actual.GetSlot() != expected.slot)
        {
            detail = "expected_slot=" + expected.slot.ToString() + " actual_slot=" + actual.GetSlot().ToString();
            return false;
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
        CaptureLocation(item, node.location);

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

    static CVCItemNode CaptureLive(ItemBase item)
    {
        if (!item)
            return null;

        CVCItemNode node = new CVCItemNode;
        node.class_name = item.GetType();
        node.item_id = item.CVCGetLiveItemID();
        CaptureLocation(item, node.location);

        CVCWorldItemAdapter adapter = CVCItemAdapterRegistry.ResolveForItem(item);
        node.adapter.id = adapter.GetID();
        node.adapter.version = adapter.GetVersion();
        adapter.Serialize(item, node);

        GameInventory inventory = item.GetInventory();
        for (int attachmentIndex = 0; attachmentIndex < inventory.AttachmentCount(); attachmentIndex++)
        {
            ItemBase attachment = ItemBase.Cast(inventory.GetAttachmentFromIndex(attachmentIndex));
            if (attachment)
                node.children.Insert(CaptureLive(attachment));
        }
        CargoBase cargo = inventory.GetCargo();
        if (cargo)
        {
            for (int cargoIndex = 0; cargoIndex < cargo.GetItemCount(); cargoIndex++)
            {
                ItemBase cargoItem = ItemBase.Cast(cargo.GetItem(cargoIndex));
                if (cargoItem)
                    node.children.Insert(CaptureLive(cargoItem));
            }
        }
        return node;
    }

    protected static bool RestoreLiveNode(ItemBase item, CVCItemNode node, out string reason)
    {
        if (!item || !node)
        {
            reason = "created live item or stored node disappeared during restore";
            return false;
        }
        CVCWorldItemAdapter adapter = CVCItemAdapterRegistry.ResolveForNode(node);
        if (!adapter)
        {
            reason = "adapter became unavailable while restoring " + node.class_name;
            return false;
        }
        item.CVCSetLiveItemID(node.item_id);
        adapter.Restore(item, node);
        if (!node.children)
            return true;
        foreach (CVCItemNode childNode : node.children)
        {
            if (!childNode || !childNode.location)
            {
                reason = "stored live child data is incomplete";
                return false;
            }
            ItemBase child;
            if (childNode.location.kind == "attachment" && childNode.location.slot >= 0)
            {
                child = ItemBase.Cast(item.GetInventory().CreateAttachmentEx(childNode.class_name, childNode.location.slot));
                string attachmentDetail;
                if (child && !ValidateAttachmentLocation(child, item, childNode.location, attachmentDetail))
                {
                    child.DeleteSafe();
                    child = null;
                    reason = "restored live attachment " + childNode.class_name + " did not retain slot " + childNode.location.slot.ToString();
                }
            }
            else if (childNode.location.kind == "cargo")
                child = CreateStoredCargoItem(item, childNode, reason);
            else
            {
                reason = "unsupported stored live child location " + childNode.location.kind + " for " + childNode.class_name;
                return false;
            }
            if (!child)
            {
                if (reason == "")
                    reason = "could not restore live child " + childNode.class_name + " inside " + item.GetType();
                return false;
            }
            if (!RestoreLiveNode(child, childNode, reason))
                return false;
        }
        return true;
    }

    static ItemBase RestoreLiveRoot(CVCItemNode node, PlayerBase destination, out string reason)
    {
        if (!node || !destination)
        {
            reason = "stored live item or destination player is missing";
            return null;
        }
        if (!CanRestoreTree(node, reason))
            return null;
        EntityAI created = destination.GetInventory().CreateInInventory(node.class_name);
        ItemBase root = ItemBase.Cast(created);
        if (!root)
        {
            reason = "could not create " + node.class_name + " in the player inventory";
            return null;
        }
        if (!RestoreLiveNode(root, node, reason))
        {
            root.DeleteSafe();
            return null;
        }
        return root;
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

        if (!node.location)
        {
            reason = "stored root location is missing for " + node.class_name;
            CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_LOCATION_INVALID_" + node.item_id, "result=fail_closed item_id=" + node.item_id + " class=" + node.class_name + " reason=location_missing", 100);
            return null;
        }
        if (node.location.kind != "cargo")
        {
            reason = "unsupported stored root location " + node.location.kind + " for " + node.class_name;
            CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_LOCATION_INVALID_" + node.item_id, "result=fail_closed item_id=" + node.item_id + " class=" + node.class_name + " kind=" + node.location.kind, 100);
            return null;
        }

        bool legacyLocation = IsLegacyCargoLocation(node.location);
        bool exactLocation = IsExactCargoLocation(node.location);
        if (!legacyLocation && !exactLocation)
        {
            reason = "stored root cargo location is partially populated for " + node.class_name;
            string invalidLocationDetail = "result=fail_closed item_id=" + node.item_id + " class=" + node.class_name;
            invalidLocationDetail += " index=" + node.location.index.ToString() + " row=" + node.location.row.ToString() + " col=" + node.location.col.ToString() + " flip=" + node.location.flip.ToString();
            CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_LOCATION_INVALID_" + node.item_id, invalidLocationDetail, 100);
            return null;
        }

        if (exactLocation)
        {
            string exactLocationDetail = "item_id=" + node.item_id + " class=" + node.class_name;
            exactLocationDetail += " index=" + node.location.index.ToString() + " row=" + node.location.row.ToString() + " col=" + node.location.col.ToString() + " flip=" + node.location.flip.ToString();
            CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_EXACT_REQUEST_" + node.item_id, "result=request " + exactLocationDetail, 100);
        }

        ItemBase root = CreateStoredCargoItem(destination, node, reason);
        if (!root)
            return null;

        if (!RestoreNode(root, node, reason))
        {
            root.DeleteSafe();
            return null;
        }

        if (exactLocation)
        {
            string finalDetail;
            if (!ValidateExactCargoLocation(root, destination, node.location, finalDetail))
            {
                root.DeleteSafe();
                reason = "restored root " + node.class_name + " moved away from its saved cargo position or orientation while rebuilding its item tree";
                CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_FINAL_MISMATCH_" + node.item_id, "result=fail_closed item_id=" + node.item_id + " class=" + node.class_name + " " + finalDetail, 100);
                return null;
            }
            CVCContainerDiagnostics.TracePortable(destination, "ROOT_RESTORE_EXACT_SUCCESS_" + node.item_id, "result=exact_tree_restored item_id=" + node.item_id + " class=" + node.class_name + " index=" + node.location.index.ToString() + " row=" + node.location.row.ToString() + " col=" + node.location.col.ToString() + " flip=" + node.location.flip.ToString(), 100);
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

            ItemBase child;
            if (childNode.location.kind == "attachment" && childNode.location.slot >= 0)
            {
                child = ItemBase.Cast(item.GetInventory().CreateAttachmentEx(childNode.class_name, childNode.location.slot));
                string attachmentDetail;
                if (child && !ValidateAttachmentLocation(child, item, childNode.location, attachmentDetail))
                {
                    child.DeleteSafe();
                    reason = "restored attachment " + childNode.class_name + " did not retain slot " + childNode.location.slot.ToString();
                    return false;
                }
            }
            else if (childNode.location.kind == "cargo")
                child = CreateStoredCargoItem(item, childNode, reason);
            else
            {
                reason = "unsupported stored child location " + childNode.location.kind + " for " + childNode.class_name;
                return false;
            }
            if (!child)
            {
                if (reason == "")
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

    static bool IsWorldProviderLocation(EntityAI entity)
    {
        if (!entity || !entity.GetInventory() || entity.GetHierarchyParent() || entity.GetHierarchyRootPlayer())
            return false;

        InventoryLocation location = new InventoryLocation;
        if (!entity.GetInventory().GetCurrentInventoryLocation(location) || !location.IsValid())
            return false;

        return location.GetType() == InventoryLocationType.GROUND;
    }

    static bool IsAutomaticContainerClass(EntityAI entity)
    {
        if (!entity)
            return false;

        // Ground clothing and backpacks are loot, not world storage providers. Treating
        // them as providers filled the migration queue with thousands of transient items
        // and delayed barrels, crates, tents, and vehicles that players were using.
        if (entity.IsKindOf("Clothing") || entity.IsKindOf("Bag_Base") || entity.IsKindOf("FireplaceBase"))
            return false;

        return true;
    }

    static bool IsEligible(EntityAI entity)
    {
        if (!IsWorldProviderLocation(entity) || !HasCargo(entity))
            return false;
        return IsConfiguredContainerClass(entity);
    }

    // A portable container that leaves the top-level ground location still needs
    // its virtual roots present in the native DayZ cargo grid. Hands, attachments,
    // and nested vehicle/storage cargo all use vanilla inventory rules, but they
    // cannot display SQL-only roots. The transition service materializes those roots
    // before the container is left outside the world-provider boundary.
    static bool IsVehicleContainmentLocation(EntityAI entity)
    {
        if (!entity || IsWorldProviderLocation(entity))
            return false;
        EntityAI cursor = entity.GetHierarchyParent();
        while (cursor)
        {
            if (Transport.Cast(cursor))
                return true;
            cursor = cursor.GetHierarchyParent();
        }
        return false;
    }

    static bool IsPhysicalInteractionLocation(EntityAI entity)
    {
        if (!entity || !HasCargo(entity) || IsWorldProviderLocation(entity))
            return false;
        if (entity.GetHierarchyRootPlayer())
            return true;
        EntityAI parent = entity.GetHierarchyParent();
        if (!parent)
            return false;
        InventoryLocation location = new InventoryLocation;
        if (!entity.GetInventory().GetCurrentInventoryLocation(location) || !location.IsValid())
            return false;
        int locationType = location.GetType();
        if (locationType == InventoryLocationType.CARGO || locationType == InventoryLocationType.ATTACHMENT)
            return true;
        return parent.GetInventory() && (parent.GetInventory().GetCargo() || Transport.Cast(parent));
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
        // 1.2.0-r62: keep the vanilla M3S inventory completely outside Clippy.
        // M3S has special container/material attachment rules and r60 could leave
        // its main cargo fail-closed while portable providers were transitioning.
        if (entity.IsKindOf("Truck_01_Base"))
            return false;
        if (Transport.Cast(entity))
            return settings.EnableVehicleCargo;
        if (Matches(entity, settings.ContainerClassNames, settings.IncludeInheritedContainerClasses))
            return true;
        return settings.AutoDiscoverCargoContainers && IsAutomaticContainerClass(entity);
    }

    static string ProviderKey(EntityAI entity)
    {
        return CVCProviderIdentityRegistry.ResolveProviderKey(entity);
    }

    static bool CheckAccess(EntityAI entity, PlayerBase player, out string reason)
    {
        reason = "allow";
        if (!IsEligible(entity))
        {
            reason = "container_ineligible";
            return false;
        }
        if (!player)
        {
            reason = "player_missing";
            return false;
        }
        if (!player.IsAlive())
        {
            reason = "player_not_alive";
            return false;
        }
        float accessDistance = vector.Distance(player.GetPosition(), entity.GetPosition());
        if (accessDistance > CVCSettingsManager.Get().AccessDistanceMetres)
        {
            reason = "beyond_access_distance";
            return false;
        }
        if (GetGame().IsServer() && ProviderKey(entity) == "")
        {
            reason = "provider_key_empty";
            return false;
        }
        return true;
    }

    static bool CanAccess(EntityAI entity, PlayerBase player)
    {
        string accessReason;
        return CheckAccess(entity, player, accessReason);
    }

    // Item-based containers can have their own open, lock, ownership, or door state.
    // ItemBase itself reports IsOpen() == true, so this only blocks subclasses that
    // deliberately expose a closed state. Clippy never calls Open() here because doing
    // that directly could bypass another mod's normal unlock/access action.
    static bool IsNativeInteractionReady(EntityAI entity)
    {
        ItemBase item = ItemBase.Cast(entity);
        if (!item)
            return true;
        return item.IsOpen();
    }

    static bool CheckNativeInteractionReady(EntityAI entity, out string reason)
    {
        reason = "";
        if (IsNativeInteractionReady(entity))
            return true;
        reason = "open or unlock this container normally before accessing its cargo";
        return false;
    }
}

class CVCContainerDiagnostics
{
    protected static ref map<string, int> s_LastEventMs = new map<string, int>;

    static bool IsPortableTarget(EntityAI entity)
    {
        if (!entity)
            return false;
        if (CVCSettingsManager.Get().EnableContainerLifecycleDiagnostics && CVCContainerPolicy.IsEligible(entity))
            return true;
        if (entity.IsKindOf("Barrel_ColorBase"))
            return true;
        if (entity.IsKindOf("BarrelHoles_ColorBase"))
            return true;
        if (entity.IsKindOf("SeaChest"))
            return true;
        return entity.IsKindOf("WoodenCrate");
    }

    static string LocationName(EntityAI entity)
    {
        if (!entity || !entity.GetInventory())
            return "NO_INVENTORY";
        InventoryLocation diagnosticLocation = new InventoryLocation;
        if (!entity.GetInventory().GetCurrentInventoryLocation(diagnosticLocation))
            return "INVALID";
        if (!diagnosticLocation.IsValid())
            return "INVALID";
        int diagnosticLocationType = diagnosticLocation.GetType();
        if (diagnosticLocationType == InventoryLocationType.GROUND)
            return "GROUND";
        if (diagnosticLocationType == InventoryLocationType.ATTACHMENT)
            return "ATTACHMENT";
        if (diagnosticLocationType == InventoryLocationType.CARGO)
            return "CARGO";
        if (diagnosticLocationType == InventoryLocationType.HANDS)
            return "HANDS";
        if (diagnosticLocationType == InventoryLocationType.PROXYCARGO)
            return "PROXYCARGO";
        if (diagnosticLocationType == InventoryLocationType.VEHICLE)
            return "VEHICLE";
        if (diagnosticLocationType == InventoryLocationType.TEMP)
            return "TEMP";
        return "UNKNOWN";
    }

    static string Hierarchy(EntityAI entity)
    {
        if (!entity)
            return "entity=null";
        EntityAI diagnosticParent = entity.GetHierarchyParent();
        EntityAI diagnosticRoot = entity.GetHierarchyRoot();
        string diagnosticParentType = "null";
        string diagnosticRootType = "null";
        if (diagnosticParent)
            diagnosticParentType = diagnosticParent.GetType();
        if (diagnosticRoot)
            diagnosticRootType = diagnosticRoot.GetType();
        string diagnosticDetail = "location=" + LocationName(entity);
        diagnosticDetail += " parent=" + diagnosticParentType;
        diagnosticDetail += " root=" + diagnosticRootType;
        diagnosticDetail += " root_player=" + (entity.GetHierarchyRootPlayer() != null).ToString();
        diagnosticDetail += " root_transport=" + (Transport.Cast(diagnosticRoot) != null).ToString();
        return diagnosticDetail;
    }

    static void TracePortable(EntityAI entity, string eventName, string detail, int intervalMs = 1000)
    {
        if (!IsPortableTarget(entity))
            return;
        string diagnosticProviderKey = CVCContainerPolicy.ProviderKey(entity);
        string diagnosticRateKey = diagnosticProviderKey + ":" + eventName;
        if (diagnosticProviderKey == "")
            diagnosticRateKey = entity.GetType() + ":" + eventName;
        int diagnosticNowMs = GetGame().GetTime();
        int diagnosticLastMs;
        if (s_LastEventMs.Find(diagnosticRateKey, diagnosticLastMs))
        {
            if (diagnosticNowMs - diagnosticLastMs < intervalMs)
                return;
        }
        s_LastEventMs.Set(diagnosticRateKey, diagnosticNowMs);
        string diagnosticMessage = "[CVC-DIAG] t_ms=" + diagnosticNowMs.ToString();
        diagnosticMessage += " realm=";
        if (GetGame().IsServer())
            diagnosticMessage += "server";
        else
            diagnosticMessage += "client";
        diagnosticMessage += " event=" + eventName;
        diagnosticMessage += " type=" + entity.GetType();
        diagnosticMessage += " provider=" + diagnosticProviderKey;
        diagnosticMessage += " " + detail;
        Print(diagnosticMessage);
    }

    static void Trace(ItemBase item, string eventName, EntityAI oldOwner = null, EntityAI newOwner = null)
    {
        if (!GetGame().IsServer() || !item || !CVCSettingsManager.Get().EnableContainerLifecycleDiagnostics || !CVCContainerPolicy.HasCargo(item))
            return;

        InventoryLocation location = new InventoryLocation;
        int locationType = -1;
        if (item.GetInventory().GetCurrentInventoryLocation(location) && location.IsValid())
            locationType = location.GetType();

        EntityAI parent = item.GetHierarchyParent();
        string parentType = "null";
        string oldOwnerType = "null";
        string newOwnerType = "null";
        if (parent)
            parentType = parent.GetType();
        if (oldOwner)
            oldOwnerType = oldOwner.GetType();
        if (newOwner)
            newOwnerType = newOwner.GetType();

        string message = "[Clippy Virtual Cargo] Lifecycle event=" + eventName;
        message += " type=" + item.GetType();
        message += " provider=" + CVCContainerPolicy.ProviderKey(item);
        message += " parent=" + parentType;
        message += " old_owner=" + oldOwnerType;
        message += " new_owner=" + newOwnerType;
        message += " location_type=" + locationType.ToString();
        message += " eligible=" + CVCContainerPolicy.IsEligible(item).ToString();
        message += " allow_damage=" + item.GetAllowDamage().ToString();
        message += " takeable=" + item.IsTakeable().ToString();
        message += " being_placed=" + item.IsBeingPlaced().ToString();
        message += " open=" + item.IsOpen().ToString() + ".";
        Print(message);
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
    bool physical_interaction_pending;
    bool physical_interaction_tracking;
    string physical_interaction_baseline;
    bool native_inventory_blocked;
    string native_inventory_block_reason;
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
    bool nested_materialization;
    bool vehicle_inventory_materialization;
    int abort_retry_attempt;
    int cleanup_retry_attempt;
    bool cleanup_committed;
    string pending_failure_reason;
    int last_metadata_report_ms;
}

class CVCContainerMetadataHandler: CVCResponseHandler
{
    override void OnSuccess(string raw) {}
    override void OnFailure(string reason)
    {
        ErrorEx("[Clippy Virtual Cargo] Container metadata update failed: " + reason);
    }
}

class CVCContainerMetadata
{
    static string MapName()
    {
        string worldName = GetGame().GetWorldName();
        if (worldName == "")
            return "unknown";
        return worldName;
    }

    static void FillResolve(CVCResolveRequest request, EntityAI container)
    {
        if (!request || !container)
            return;
        vector position = container.GetPosition();
        request.container_class = container.GetType();
        request.world_position_x = position[0];
        request.world_position_y = position[1];
        request.world_position_z = position[2];
        request.map_name = MapName();
    }

    static void Observe(CVCContainerRuntime state)
    {
        if (!state || !state.container || state.storage_id == "" || !ClippyVirtualCargoAPI.IsReady())
            return;
        CVCContainerObservationRequest request = new CVCContainerObservationRequest;
        request.storage_id = state.storage_id;
        request.display_name = state.container.GetDisplayName();
        request.container_class = state.container.GetType();
        vector position = state.container.GetPosition();
        request.world_position_x = position[0];
        request.world_position_y = position[1];
        request.world_position_z = position[2];
        request.map_name = MapName();
        state.last_metadata_report_ms = GetGame().GetTime();
        ClippyVirtualCargoAPI.Post("/v1/storage/observe", request, new CVCContainerMetadataHandler);
    }
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
    bool roots_restored;
    int identity_retry_attempts;
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
        {
            CVCContainerDiagnostics.TracePortable(item, "JOURNAL_ROOT_IDENTITY_MISSING_" + virtualID, "result=fail source_key_empty class=" + item.GetType() + " virtual_id=" + virtualID, 1000);
            return false;
        }
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
        bool diagnosticResolveParsed = serializer.ReadFromString(response, raw, parseError);
        bool diagnosticResolveOK;
        bool diagnosticResolveDataPresent;
        if (diagnosticResolveParsed)
        {
            diagnosticResolveOK = response.ok;
            diagnosticResolveDataPresent = response.data != null;
        }
        string diagnosticResolveParseDetail = "parsed=" + diagnosticResolveParsed.ToString();
        diagnosticResolveParseDetail += " ok=" + diagnosticResolveOK.ToString();
        diagnosticResolveParseDetail += " data_present=" + diagnosticResolveDataPresent.ToString();
        if (!diagnosticResolveParsed || !diagnosticResolveOK || !diagnosticResolveDataPresent)
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/storage/resolve", "RESOLVE_RESULT_REJECTED", diagnosticResolveParseDetail);
            CVCContainerService.Fail(m_State, "container registration was rejected");
            return;
        }
        m_State.storage_id = response.data.storage_id;
        m_State.revision = response.data.revision;
        string diagnosticResolveSuccessDetail = "result=success storage_present=" + (m_State.storage_id != "").ToString();
        diagnosticResolveSuccessDetail += " revision=" + m_State.revision.ToString();
        CVCContainerService.DiagnosticRoute(m_State, "/v1/storage/resolve", "RESOLVE_RESULT_SUCCESS", diagnosticResolveSuccessDetail);
        if (m_State.storage_id != "")
            CVCProviderIdentityRegistry.Remember(m_State.container);
        CVCContainerService.OpenResolved(m_State, m_NextPage);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.DiagnosticRoute(m_State, "/v1/storage/resolve", "RESOLVE_RESULT_FAILURE", "result=callback_failure");
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
        bool diagnosticOpenParsed = ClippyVirtualCargoAPI.ParseSession(raw, response, parseError);
        if (!diagnosticOpenParsed)
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_PARSE_FAILED", "parsed=false");
            CVCContainerService.OpenUncertain(m_State, "cargo open response was invalid; restart recovery is required");
            return;
        }
        if (!response.ok || !response.data)
        {
            string diagnosticOpenRejectedDetail = "parsed=true ok=" + response.ok.ToString();
            diagnosticOpenRejectedDetail += " data_present=" + (response.data != null).ToString();
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_REJECTED", diagnosticOpenRejectedDetail);
            CVCContainerService.Fail(m_State, "cargo page open was rejected by the storage host");
            return;
        }
        bool nestedMaterialization = m_State.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(m_State.container);
        string diagnosticOpenAccessReason;
        if (nestedMaterialization)
            diagnosticOpenAccessReason = "nested_physical_location";
        else
            diagnosticOpenAccessReason = "player_missing";
        bool diagnosticOpenCanAccess = nestedMaterialization || CVCContainerPolicy.CheckAccess(m_State.container, m_State.player, diagnosticOpenAccessReason);
        if (!diagnosticOpenCanAccess)
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_ACCESS_LOST", "can_access=false access_reason=" + diagnosticOpenAccessReason);
            m_State.session_id = response.data.session_id;
            CVCContainerService.AbortOpening(m_State, "player left interaction range");
            return;
        }

        string validationError;
        if (!CVCContainerService.ValidatePhysicalCargo(m_State.container, validationError))
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_PHYSICAL_VALIDATION_FAILED", "result=abort_opening");
            m_State.session_id = response.data.session_id;
            CVCContainerService.AbortOpening(m_State, validationError);
            return;
        }

        m_State.session_id = response.data.session_id;
        if (response.data.next_cursor != "")
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_PAGE_LIMIT_BLOCKED", "next_cursor_present=true");
            m_State.native_inventory_blocked = true;
            m_State.native_inventory_block_reason = "stored cargo exceeds the transparent native-cargo materialization limit";
            CVCContainerService.AbortOpening(m_State, m_State.native_inventory_block_reason + "; no partial cargo view was exposed");
            return;
        }
        response.data.provider_key = m_State.provider_key;
        CVCSessionJournalEntry journal = CVCSessionJournal.Begin(response.data);
        if (!journal)
        {
            CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_JOURNAL_FAILED", "session_present=true");
            CVCContainerService.AbortOpening(m_State, "session journal could not be written before materialization");
            return;
        }
        int diagnosticOpenItemCount;
        if (response.data.items)
            diagnosticOpenItemCount = response.data.items.Count();
        string diagnosticOpenSuccessDetail = "result=queue_materialization session_present=" + (response.data.session_id != "").ToString();
        diagnosticOpenSuccessDetail += " status=" + response.data.status;
        diagnosticOpenSuccessDetail += " item_count=" + diagnosticOpenItemCount.ToString();
        CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_SUCCESS", diagnosticOpenSuccessDetail);
        CVCContainerService.QueueMaterialization(m_State, response.data, journal);
    }

    override void OnFailure(string reason)
    {
        CVCContainerService.DiagnosticRoute(m_State, "/v1/session/open", "OPEN_RESULT_FAILURE", "result=callback_failure");
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
    protected static ref array<PlayerBase> s_InventoryOpenPlayers = new array<PlayerBase>;
    protected static ref array<PlayerBase> s_InventoryPreparingPlayers = new array<PlayerBase>;
    protected static bool s_MaterializationPumpScheduled;
    protected static bool s_EnforcementReady;
    protected static bool s_DiagnosticProbeFirstTick;
    protected static string s_DiagnosticProbeGuards;
    protected static ref map<PlayerBase, int> s_LastNearbyDiscoveryMs = new map<PlayerBase, int>;

    protected static string DiagnosticRuntime(CVCContainerRuntime state)
    {
        if (!state)
            return "runtime=missing";
        string diagnosticRuntime = "runtime=present phase=" + state.phase.ToString();
        diagnosticRuntime += " busy=" + state.busy.ToString();
        diagnosticRuntime += " recovering=" + state.recovering.ToString();
        diagnosticRuntime += " fallback=" + state.physical_fallback.ToString();
        diagnosticRuntime += " internal_mutation=" + state.internal_mutation.ToString();
        diagnosticRuntime += " native_blocked=" + state.native_inventory_blocked.ToString();
        diagnosticRuntime += " native_block_reason=" + state.native_inventory_block_reason;
        diagnosticRuntime += " nested_materialization=" + state.nested_materialization.ToString();
        diagnosticRuntime += " vehicle_inventory_materialization=" + state.vehicle_inventory_materialization.ToString();
        diagnosticRuntime += " session_present=" + (state.session_id != "").ToString();
        diagnosticRuntime += " active_migration=" + (state.active_migration != null).ToString();
        diagnosticRuntime += " migration_prepared=" + state.migration_prepare_dispatched.ToString();
        diagnosticRuntime += " physical_pending=" + state.physical_interaction_pending.ToString();
        diagnosticRuntime += " physical_tracking=" + state.physical_interaction_tracking.ToString();
        return diagnosticRuntime;
    }

    protected static bool DiagnosticPrepareReturn(EntityAI container, CVCContainerRuntime state, bool result, string reason, string beforeRuntime)
    {
        if (container)
        {
            string diagnosticPrepareDetail = "result=" + result.ToString();
            diagnosticPrepareDetail += " reason=" + reason;
            diagnosticPrepareDetail += " before={" + beforeRuntime + "}";
            diagnosticPrepareDetail += " after={" + DiagnosticRuntime(state) + "}";
            diagnosticPrepareDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            CVCContainerDiagnostics.TracePortable(container, "PREPARE_RETURN_" + reason, diagnosticPrepareDetail, 250);
        }
        return result;
    }

    protected static bool DiagnosticCargoDecision(EntityAI container, CVCContainerRuntime state, bool result, string reason)
    {
        if (container)
        {
            string diagnosticCargoDetail = "allow=" + result.ToString();
            diagnosticCargoDetail += " reason=" + reason;
            diagnosticCargoDetail += " api_ready=" + ClippyVirtualCargoAPI.IsReady().ToString();
            diagnosticCargoDetail += " enforcement_ready=" + s_EnforcementReady.ToString();
            diagnosticCargoDetail += " migration_started=" + CVCMigrationService.IsStarted().ToString();
            diagnosticCargoDetail += " physical_access=" + HasPhysicalInteractionAccess(container).ToString();
            diagnosticCargoDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            diagnosticCargoDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
            diagnosticCargoDetail += " " + DiagnosticRuntime(state);
            CVCContainerDiagnostics.TracePortable(container, "ALLOWS_PHYSICAL_CARGO_" + reason, diagnosticCargoDetail, 250);
        }
        return result;
    }

    static void DiagnosticRoute(CVCContainerRuntime state, string route, string eventName, string detail = "")
    {
        if (!state || !state.container)
            return;
        string diagnosticRouteDetail = "route=" + route;
        if (detail != "")
            diagnosticRouteDetail += " " + detail;
        diagnosticRouteDetail += " " + DiagnosticRuntime(state);
        CVCContainerDiagnostics.TracePortable(state.container, "NATIVE_ROUTE_" + eventName, diagnosticRouteDetail, 100);
    }

    static void SetActionState(EntityAI container, int actionState)
    {
        if (!GetGame().IsServer() || !container)
            return;
        ItemBase item = ItemBase.Cast(container);
        if (item)
        {
            item.CVCSetActionState(actionState);
            SyncMovementLock(container);
            return;
        }
        CarScript vehicle = CarScript.Cast(container);
        if (vehicle)
            vehicle.CVCSetActionState(actionState);
    }

    static void SetManagedLifecycle(EntityAI container, bool managed)
    {
        // 1.0.7 intentionally has no generic ItemBase shell mutation. In 1.0.6,
        // EEInit could briefly classify a trader-created portable container as a
        // top-level provider before the trader moved it into hands. Changing the
        // item's damage state during that transient window made Clippy part of the
        // held-item lifecycle. SQL state now protects cargo without mutating the
        // physical container entity.
    }

    static bool ShouldLockMovement(EntityAI container)
    {
        // Never override DayZ's native movement rules. Provider identity stays with the
        // same physical entity while it moves, and nested-provider protection is handled
        // separately. A stale workflow flag must never trap a container in a player's hands.
        return false;
    }

    static void SyncMovementLock(EntityAI container)
    {
        // Kept as a state-machine call site so active workflows remain readable.
        // Clippy never changes portable-container movement state.
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
            Print("[Clippy Virtual Cargo] Dropped a stale queued materialization job for session " + session.session_id);
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
        bool nestedMaterialization = state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container);
        if (!nestedMaterialization && !CVCContainerPolicy.CanAccess(state.container, state.player))
        {
            AbortOpening(state, "player left interaction range before queued materialization");
            ScheduleMaterializationPump();
            return;
        }
        if (state.nested_materialization && !nestedMaterialization)
        {
            AbortOpening(state, "nested destination changed before queued materialization");
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

        if (!job.roots_restored)
        {
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
                }
            }
            state.internal_mutation = false;
            job.roots_restored = true;
        }

        state.pending_mark_root_ids.Clear();
        state.pending_mark_source_keys.Clear();
        if (session.items)
        {
            if (state.materialized.Count() != session.items.Count())
            {
                AbortOpening(state, "materialized root count no longer matches the open cargo page");
                ScheduleMaterializationPump();
                return;
            }
            for (int rootIndex = 0; rootIndex < session.items.Count(); rootIndex++)
            {
                CVCItemNode materializedNode = session.items[rootIndex];
                ItemBase materializedRoot = state.materialized[rootIndex];
                string sourceKey = CVCPhysicalRootIdentity.Key(materializedRoot);
                if (sourceKey == "")
                {
                    if (nestedMaterialization && job.identity_retry_attempts < 40)
                    {
                        job.identity_retry_attempts++;
                        string identityWaitDetail = "result=wait item_id=" + materializedNode.item_id + " class=" + materializedRoot.GetType();
                        identityWaitDetail += " attempt=" + job.identity_retry_attempts.ToString() + " location=" + CVCContainerDiagnostics.Hierarchy(state.container);
                        CVCContainerDiagnostics.TracePortable(state.container, "ROOT_IDENTITY_WAIT_" + materializedNode.item_id, identityWaitDetail, 1000);
                        s_MaterializationQueue.Insert(job);
                        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.PumpMaterialization, 250, false);
                        return;
                    }
                    AbortOpening(state, "restored root did not receive a stable DayZ persistence identity");
                    ScheduleMaterializationPump();
                    return;
                }
                if (!CVCSessionJournal.RecordRoot(job.journal, materializedRoot, materializedNode.item_id, false))
                {
                    AbortOpening(state, "materialized root journal identity could not be recorded");
                    ScheduleMaterializationPump();
                    return;
                }
                state.pending_mark_root_ids.Insert(materializedNode.item_id);
                state.pending_mark_source_keys.Insert(sourceKey);
            }
        }
        string nativeInteractionError;
        if (!ValidateNativeMaterializedInteraction(state, nativeInteractionError))
        {
            AbortOpening(state, nativeInteractionError);
            ScheduleMaterializationPump();
            return;
        }
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
        // Clippy world actions must never remain available on an item after it moves
        // into hands, cargo, or an attachment. The server clears the synced action
        // state on that move, but this hierarchy check also closes the client-side
        // replication window so vanilla held-item actions such as placement win.
        return CVCContainerPolicy.IsWorldProviderLocation(container) && CVCContainerPolicy.HasCargo(container) && player && player.IsAlive() && vector.Distance(player.GetPosition(), container.GetPosition()) <= UAMaxDistances.DEFAULT;
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
        // A pending record has no matching live provider yet. Keep it queued so the
        // matching container is recovered before it can be used, but do not let an
        // unloaded or deleted provider block every unrelated container on the server.
        foreach (string stateKey, CVCContainerRuntime state : s_States)
        {
            if (state && state.recovering)
                return false;
        }
        return true;
    }

    static int PendingRecoveryCount()
    {
        return s_PendingRecovery.Count();
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

    static bool Register(EntityAI container)
    {
        if (GetGame().IsServer() && CVCContainerDiagnostics.IsPortableTarget(container))
        {
            string diagnosticRegisterDetail = "configured=" + CVCContainerPolicy.IsConfiguredContainerClass(container).ToString();
            diagnosticRegisterDetail += " world=" + CVCContainerPolicy.IsWorldProviderLocation(container).ToString();
            diagnosticRegisterDetail += " eligible=" + CVCContainerPolicy.IsEligible(container).ToString();
            diagnosticRegisterDetail += " native_ready=" + CVCContainerPolicy.IsNativeInteractionReady(container).ToString();
            diagnosticRegisterDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
            CVCContainerDiagnostics.TracePortable(container, "REGISTER_ENTRY", diagnosticRegisterDetail, 5000);
        }
        bool persistentPhysicalLocation = CVCContainerPolicy.IsPhysicalInteractionLocation(container) && CVCContainerPolicy.IsConfiguredContainerClass(container) && CVCProviderIdentityRegistry.Contains(container);
        if (!GetGame().IsServer() || (!CVCContainerPolicy.IsEligible(container) && !persistentPhysicalLocation))
            return false;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return false;

        EntityAI registeredContainer;
        bool registeredFound = s_Registered.Find(key, registeredContainer);
        if (registeredFound && registeredContainer && registeredContainer != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "REGISTER_COLLISION", "map=registered result=refused existing_type=" + registeredContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            ErrorEx("[Clippy Virtual Cargo] Refusing duplicate live provider key " + key + " while registering " + container.GetType() + ".");
            return false;
        }

        CVCContainerRuntime existing;
        bool stateFound = s_States.Find(key, existing);
        if (stateFound && existing && existing.container && existing.container != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "REGISTER_COLLISION", "map=runtime result=refused existing_type=" + existing.container.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            ErrorEx("[Clippy Virtual Cargo] Refusing to rebind live runtime for duplicate provider key " + key + " to " + container.GetType() + ".");
            return false;
        }

        if (s_EnforcementReady)
            SetManagedLifecycle(container, true);
        s_Registered.Set(key, container);
        if (CVCContainerDiagnostics.IsPortableTarget(container))
        {
            string diagnosticRegisteredDetail = "registered_count=" + s_Registered.Count().ToString();
            diagnosticRegisteredDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            diagnosticRegisteredDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
            CVCContainerDiagnostics.TracePortable(container, "REGISTERED", diagnosticRegisteredDetail, 5000);
        }
        if (stateFound && existing)
        {
            existing.container = container;
            if (existing.phase == PHASE_ACTIVE && existing.session_id != "" && !existing.busy && !existing.nested_materialization)
                GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 1000, false, existing);
        }
        else if (persistentPhysicalLocation || PhysicalRootCount(container) == 0)
        {
            // An empty portable container does not need an existing-cargo migration
            // before the player can open its virtual store. Prime its runtime now so
            // dropping a barrel/crate never creates a locked-but-unusable window.
            existing = State(container);
        }

        CVCSessionData recovery;
        if (s_PendingRecovery.Find(key, recovery))
        {
            SetActionState(container, ACTION_NONE);
            if (persistentPhysicalLocation && existing)
            {
                existing.vehicle_inventory_materialization = false;
                existing.nested_materialization = !CVCContainerPolicy.IsVehicleContainmentLocation(container);
            }
            s_PendingRecovery.Remove(key);
            StartRecovery(container, recovery);
            return true;
        }

        if (persistentPhysicalLocation)
        {
            if (existing)
            {
                existing.vehicle_inventory_materialization = false;
                existing.nested_materialization = !CVCContainerPolicy.IsVehicleContainmentLocation(container);
            }
            SetActionState(container, ACTION_NONE);
        }
        else if (CVCMigrationService.IsStarted() && existing && !existing.busy && !existing.recovering && !existing.physical_fallback && existing.phase == PHASE_IDLE && PhysicalRootCount(container) == 0)
            SetActionState(container, ACTION_OPEN);
        else
            SetActionState(container, ACTION_NONE);
        return true;
    }

    static void Unregister(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        if (CVCContainerPolicy.IsPhysicalInteractionLocation(container))
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return;

        EntityAI registeredContainer;
        bool registeredFound = s_Registered.Find(key, registeredContainer);
        if (registeredFound && registeredContainer && registeredContainer != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "UNREGISTER_COLLISION", "map=registered result=refused existing_type=" + registeredContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            return;
        }

        CVCContainerRuntime state;
        bool stateFound = s_States.Find(key, state);
        if (stateFound && state && state.container && state.container != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "UNREGISTER_COLLISION", "map=runtime result=refused existing_type=" + state.container.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            return;
        }
        if (registeredFound)
            s_Registered.Remove(key);
        if (!stateFound || !state)
            return;
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
        state.container = null;
        state.physical_interaction_tracking = false;
        state.physical_interaction_baseline = "";
        if (state.recovering && state.recovery_session)
            s_PendingRecovery.Set(key, state.recovery_session);
        if (state.phase == PHASE_IDLE && !state.busy && !state.recovering && !state.physical_fallback && !state.physical_interaction_pending)
            s_States.Remove(key);
    }

    static CVCContainerRuntime State(EntityAI container)
    {
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return null;
        CVCContainerRuntime state;
        if (!s_States.Find(key, state) || !state)
        {
            state = new CVCContainerRuntime;
            state.container = container;
            state.provider_key = key;
            s_States.Set(key, state);
        }
        else
        {
            if (state.container && state.container != container)
            {
                CVCContainerDiagnostics.TracePortable(container, "STATE_COLLISION", "result=refused existing_type=" + state.container.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
                ErrorEx("[Clippy Virtual Cargo] Refusing to rebind runtime for duplicate provider key " + key + ".");
                return null;
            }
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
        if (state.container != container)
            return true;
        if (state.physical_fallback)
            return false;
        return !state.internal_mutation && state.phase != PHASE_ACTIVE;
    }

    protected static string PhysicalRootFingerprint(EntityAI container)
    {
        if (!container || !container.GetInventory() || !container.GetInventory().GetCargo())
            return "";
        array<string> keys = new array<string>;
        CargoBase cargo = container.GetInventory().GetCargo();
        for (int index = 0; index < cargo.GetItemCount(); index++)
        {
            EntityAI cargoEntity = cargo.GetItem(index);
            if (!cargoEntity || cargoEntity.GetHierarchyParent() != container)
                continue;
            ItemBase item = ItemBase.Cast(cargoEntity);
            string rootKey;
            if (item)
                rootKey = CVCPhysicalRootIdentity.Key(item);
            if (rootKey == "")
                rootKey = string.Format("%1:%2", cargoEntity.GetType(), index);
            keys.Insert(rootKey);
        }
        keys.Sort();
        string fingerprint = keys.Count().ToString();
        foreach (string key : keys)
            fingerprint += "|" + key;
        return fingerprint;
    }

    protected static void BeginPhysicalInteractionTracking(CVCContainerRuntime state, EntityAI container)
    {
        if (!state || !container || state.physical_interaction_tracking)
            return;
        state.physical_interaction_tracking = true;
        state.physical_interaction_baseline = PhysicalRootFingerprint(container);
    }

    // Lifecycle callbacks can be missed for a persisted or script-moved portable
    // container. Recover that proven state gap at the interaction boundary without
    // weakening the fail-closed cargo policy: only eligible, accessible top-level
    // providers are registered, and their normal migration/session workflow still
    // has to complete before physical cargo becomes writable.
    protected static int DiscoverNearbyWorldProviders(PlayerBase player, string origin, int minimumIntervalMs = 250)
    {
        if (!GetGame().IsServer() || !player || !player.GetIdentity() || !player.IsAlive())
            return 0;

        int discoveryNowMs = GetGame().GetTime();
        int lastDiscoveryMs;
        if (minimumIntervalMs > 0 && s_LastNearbyDiscoveryMs.Find(player, lastDiscoveryMs))
        {
            int discoveryElapsedMs = discoveryNowMs - lastDiscoveryMs;
            if (discoveryElapsedMs >= 0 && discoveryElapsedMs < minimumIntervalMs)
                return 0;
        }
        s_LastNearbyDiscoveryMs.Set(player, discoveryNowMs);

        ref array<Object> nearbyObjects = new array<Object>;
        ref array<CargoBase> proxyCargos = new array<CargoBase>;
        GetGame().GetObjectsAtPosition3D(player.GetPosition(), CVCSettingsManager.Get().AccessDistanceMetres, nearbyObjects, proxyCargos);

        int discoveredCount;
        foreach (Object nearbyObject : nearbyObjects)
        {
            EntityAI container = EntityAI.Cast(nearbyObject);
            if (!container || !CVCContainerPolicy.IsEligible(container))
                continue;

            string providerKey = CVCContainerPolicy.ProviderKey(container);
            if (providerKey == "")
            {
                CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_PROVIDER_KEY_EMPTY", "origin=" + origin + " result=waiting_for_lifecycle_identity " + CVCContainerDiagnostics.Hierarchy(container), 1000);
                continue;
            }

            string accessReason;
            if (!CVCContainerPolicy.CheckAccess(container, player, accessReason))
                continue;

            EntityAI registeredContainer;
            bool registeredFound = s_Registered.Find(providerKey, registeredContainer);
            if (registeredFound && registeredContainer && registeredContainer != container)
            {
                CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_COLLISION", "origin=" + origin + " map=registered result=refused existing_type=" + registeredContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
                continue;
            }
            bool wasRegistered = registeredFound;
            if (wasRegistered && registeredContainer != container)
                wasRegistered = false;

            CVCContainerRuntime runtimeBefore;
            bool runtimeFound = s_States.Find(providerKey, runtimeBefore);
            if (runtimeFound && runtimeBefore && runtimeBefore.container && runtimeBefore.container != container)
            {
                CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_COLLISION", "origin=" + origin + " map=runtime result=refused existing_type=" + runtimeBefore.container.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
                continue;
            }
            bool hadRuntime = runtimeFound;
            if (hadRuntime && (!runtimeBefore || runtimeBefore.container != container))
                hadRuntime = false;
            if (wasRegistered && hadRuntime)
                continue;

            // Adopt synchronously so pending migration recovery wins before a native
            // session can open. Empty providers are deliberately not migration-queued.
            bool adopted = CVCMigrationService.AdoptDiscoveredCandidate(container, origin);

            EntityAI registeredAfter;
            CVCContainerRuntime runtimeAfter;
            bool isRegistered = s_Registered.Find(providerKey, registeredAfter);
            if (isRegistered && registeredAfter != container)
                isRegistered = false;
            bool hasRuntime = s_States.Find(providerKey, runtimeAfter);
            if (hasRuntime && (!runtimeAfter || runtimeAfter.container != container))
                hasRuntime = false;
            string discoveryDetail = "origin=" + origin;
            discoveryDetail += " adopted=" + adopted.ToString();
            discoveryDetail += " was_registered=" + wasRegistered.ToString();
            discoveryDetail += " had_runtime=" + hadRuntime.ToString();
            discoveryDetail += " is_registered=" + isRegistered.ToString();
            discoveryDetail += " has_runtime=" + hasRuntime.ToString();
            discoveryDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            discoveryDetail += " access_reason=" + accessReason;
            discoveryDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
            CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_REGISTERED", discoveryDetail, 250);
            if (isRegistered && !wasRegistered)
                discoveredCount++;
        }
        return discoveredCount;
    }

    static void InventoryOpened(PlayerBase player)
    {
        if (!GetGame().IsServer() || !player)
            return;
        if (s_InventoryPreparingPlayers.Find(player) >= 0)
            return;
        // The RPC is already protected by s_InventoryPreparingPlayers. Do not let a
        // probe that ran just before a drop suppress discovery at the actual UI edge.
        DiscoverNearbyWorldProviders(player, "inventory_rpc", 0);
        s_InventoryPreparingPlayers.Insert(player);
        PrepareInventoryOpen(player, GetGame().GetTime());
    }

    protected static void RemoveInventoryPreparingPlayer(PlayerBase player)
    {
        if (!player)
            return;
        int index = s_InventoryPreparingPlayers.Find(player);
        if (index >= 0)
            s_InventoryPreparingPlayers.RemoveOrdered(index);
    }

    protected static void RemoveInventoryOpenPlayer(PlayerBase player)
    {
        if (!player)
            return;
        int index = s_InventoryOpenPlayers.Find(player);
        if (index >= 0)
            s_InventoryOpenPlayers.RemoveOrdered(index);
    }

    protected static void PruneInventoryOpenPlayers()
    {
        for (int index = s_InventoryOpenPlayers.Count() - 1; index >= 0; index--)
        {
            PlayerBase player = s_InventoryOpenPlayers[index];
            if (!player || !player.GetIdentity() || !player.IsAlive())
                s_InventoryOpenPlayers.RemoveOrdered(index);
        }
        for (int preparingIndex = s_InventoryPreparingPlayers.Count() - 1; preparingIndex >= 0; preparingIndex--)
        {
            PlayerBase preparingPlayer = s_InventoryPreparingPlayers[preparingIndex];
            if (!preparingPlayer || !preparingPlayer.GetIdentity() || !preparingPlayer.IsAlive())
                s_InventoryPreparingPlayers.RemoveOrdered(preparingIndex);
        }

        array<PlayerBase> staleDiscoveryPlayers = new array<PlayerBase>;
        foreach (PlayerBase discoveryPlayer, int discoveryTimeMs : s_LastNearbyDiscoveryMs)
        {
            if (!discoveryPlayer || !discoveryPlayer.GetIdentity() || !discoveryPlayer.IsAlive())
                staleDiscoveryPlayers.Insert(discoveryPlayer);
        }
        foreach (PlayerBase staleDiscoveryPlayer : staleDiscoveryPlayers)
            s_LastNearbyDiscoveryMs.Remove(staleDiscoveryPlayer);
    }

    protected static PlayerBase FindNearbyInteractionPlayer(EntityAI container)
    {
        if (!container)
            return null;

        ref array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        PlayerBase nearestPlayer;
        float nearestDistance = CVCSettingsManager.Get().AccessDistanceMetres + 1.0;
        PlayerBase nearestObservedPlayer;
        float nearestObservedDistance = 1000000.0;
        bool nearestObservedIdentity;
        bool nearestObservedAlive;
        bool nearestObservedCanAccess;
        string nearestObservedAccessReason = "no_player";
        foreach (Man man : players)
        {
            PlayerBase player = PlayerBase.Cast(man);
            if (!player)
                continue;
            float distance = vector.Distance(player.GetPosition(), container.GetPosition());
            bool identityPresent = player.GetIdentity() != null;
            bool alive = player.IsAlive();
            string accessReason;
            bool canAccess = CVCContainerPolicy.CheckAccess(container, player, accessReason);
            if (distance < nearestObservedDistance)
            {
                nearestObservedDistance = distance;
                nearestObservedPlayer = player;
                nearestObservedIdentity = identityPresent;
                nearestObservedAlive = alive;
                nearestObservedCanAccess = canAccess;
                nearestObservedAccessReason = accessReason;
            }
            if (!identityPresent)
                continue;
            if (!canAccess)
                continue;
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestPlayer = player;
            }
        }
        string observedDistanceText = "none";
        if (nearestObservedPlayer)
            observedDistanceText = nearestObservedDistance.ToString();
        string selectedDistanceText = "none";
        if (nearestPlayer)
            selectedDistanceText = nearestDistance.ToString();
        string nearestDetail = "players=" + players.Count().ToString();
        nearestDetail += " nearest_present=" + (nearestObservedPlayer != null).ToString();
        nearestDetail += " nearest_distance=" + observedDistanceText;
        nearestDetail += " identity_present=" + nearestObservedIdentity.ToString();
        nearestDetail += " alive=" + nearestObservedAlive.ToString();
        nearestDetail += " can_access=" + nearestObservedCanAccess.ToString();
        nearestDetail += " access_reason=" + nearestObservedAccessReason;
        nearestDetail += " selected_present=" + (nearestPlayer != null).ToString();
        nearestDetail += " selected_distance=" + selectedDistanceText;
        CVCContainerDiagnostics.TracePortable(container, "NEAREST_PLAYER", nearestDetail, 1000);
        return nearestPlayer;
    }

    protected static PlayerBase FindNearbyPhysicalPlayer(EntityAI container)
    {
        if (!container)
            return null;
        ref array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        PlayerBase nearestPlayer;
        float nearestDistance = CVCSettingsManager.Get().AccessDistanceMetres + 1.0;
        foreach (Man man : players)
        {
            PlayerBase player = PlayerBase.Cast(man);
            if (!player || !player.GetIdentity() || !player.IsAlive())
                continue;
            float distance = vector.Distance(player.GetPosition(), container.GetPosition());
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestPlayer = player;
            }
        }
        return nearestPlayer;
    }

    protected static bool IsInventoryOpenForPlayer(PlayerBase player)
    {
        return player && s_InventoryOpenPlayers.Find(player) >= 0;
    }

    protected static PlayerBase FindOpenInventoryPlayer(EntityAI container)
    {
        if (!container)
            return null;
        PlayerBase nearestPlayer;
        float nearestDistance = CVCSettingsManager.Get().AccessDistanceMetres + 1.0;
        foreach (PlayerBase player : s_InventoryOpenPlayers)
        {
            if (!player || !player.GetIdentity() || !player.IsAlive())
                continue;
            float distance = vector.Distance(player.GetPosition(), container.GetPosition());
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearestPlayer = player;
            }
        }
        return nearestPlayer;
    }

    protected static void TryStartVehicleInventoryMaterialization(CVCContainerRuntime state, PlayerBase player)
    {
        if (!GetGame().IsServer() || !state || !state.container || !player || !player.GetIdentity() || !player.IsAlive())
            return;
        if (!CVCContainerPolicy.IsVehicleContainmentLocation(state.container))
            return;
        if (!CVCContainerPolicy.IsConfiguredContainerClass(state.container) || !CVCProviderIdentityRegistry.Contains(state.container))
            return;
        if (!CVCContainerPolicy.IsNativeInteractionReady(state.container))
        {
            CVCContainerDiagnostics.TracePortable(state.container, "VEHICLE_INVENTORY_NATIVE_NOT_READY", "result=defer_until_native_container_ready " + DiagnosticRuntime(state), 250);
            return;
        }
        if (vector.Distance(player.GetPosition(), state.container.GetPosition()) > CVCSettingsManager.Get().AccessDistanceMetres)
            return;
        if (state.physical_fallback || state.native_inventory_blocked || state.recovering || state.active_migration != null || state.migration_prepare_dispatched)
            return;
        if (state.phase == PHASE_ACTIVE || state.busy || state.session_id != "" || state.phase != PHASE_IDLE)
            return;

        int physicalRoots = PhysicalRootCount(state.container);
        if (physicalRoots > 0)
        {
            CVCContainerDiagnostics.TracePortable(state.container, "VEHICLE_INVENTORY_NATIVE_ROOTS_PRESENT", "result=leave_physical_roots physical_roots=" + physicalRoots.ToString() + " " + DiagnosticRuntime(state), 250);
            return;
        }
        state.nested_materialization = true;
        state.vehicle_inventory_materialization = true;
        state.player = player;
        state.busy = true;
        state.phase = PHASE_OPENING;
        SetActionState(state.container, ACTION_NONE);
        CVCContainerDiagnostics.TracePortable(state.container, "VEHICLE_INVENTORY_MATERIALIZATION_REQUEST", "result=dispatch player_present=true " + DiagnosticRuntime(state), 250);
        if (state.storage_id == "")
        {
            CVCResolveRequest request = new CVCResolveRequest;
            request.provider_id = CVCSettingsManager.Get().ProviderID;
            request.provider_key = state.provider_key;
            request.display_name = state.container.GetDisplayName();
            request.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            CVCContainerMetadata.FillResolve(request, state.container);
            bool dispatched = ClippyVirtualCargoAPI.Post("/v1/storage/resolve", request, new CVCNativeResolveHandler(state, false));
            DiagnosticRoute(state, "/v1/storage/resolve", "VEHICLE_INVENTORY_RESOLVE_DISPATCH", "accepted=" + dispatched.ToString());
            if (!dispatched)
                OpenUncertain(state, "could not dispatch vehicle-contained cargo resolve; restart recovery is required");
            return;
        }
        OpenResolved(state, false);
    }

    protected static void PrepareVehicleProvidersForInventory(PlayerBase player)
    {
        if (!GetGame().IsServer() || !player || !player.GetIdentity() || !player.IsAlive())
            return;
        foreach (string providerKey, CVCContainerRuntime state : s_States)
        {
            if (!state || !state.container || state.provider_key != providerKey)
                continue;
            if (!CVCContainerPolicy.IsVehicleContainmentLocation(state.container))
                continue;
            TryStartVehicleInventoryMaterialization(state, player);
        }
    }

    protected static bool PrepareContainerForNativeInventory(EntityAI container, PlayerBase player)
    {
        if (!container)
            return false;
        string diagnosticPrepareProviderKey = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime diagnosticPrepareStateBefore;
        if (diagnosticPrepareProviderKey != "")
            s_States.Find(diagnosticPrepareProviderKey, diagnosticPrepareStateBefore);
        bool diagnosticPreparePlayerPresent = player != null;
        bool diagnosticPrepareIdentityPresent;
        bool diagnosticPrepareAlive;
        bool diagnosticPrepareCanAccess;
        string diagnosticPrepareAccessReason = "player_missing";
        if (player)
        {
            diagnosticPrepareIdentityPresent = player.GetIdentity() != null;
            diagnosticPrepareAlive = player.IsAlive();
            diagnosticPrepareCanAccess = CVCContainerPolicy.CheckAccess(container, player, diagnosticPrepareAccessReason);
        }
        bool diagnosticPrepareNativeReady = CVCContainerPolicy.IsNativeInteractionReady(container);
        string diagnosticPrepareBeforeRuntime = DiagnosticRuntime(diagnosticPrepareStateBefore);
        string diagnosticPrepareEntryDetail = "player_present=" + diagnosticPreparePlayerPresent.ToString();
        diagnosticPrepareEntryDetail += " identity_present=" + diagnosticPrepareIdentityPresent.ToString();
        diagnosticPrepareEntryDetail += " alive=" + diagnosticPrepareAlive.ToString();
        diagnosticPrepareEntryDetail += " can_access=" + diagnosticPrepareCanAccess.ToString();
        diagnosticPrepareEntryDetail += " access_reason=" + diagnosticPrepareAccessReason;
        diagnosticPrepareEntryDetail += " native_ready=" + diagnosticPrepareNativeReady.ToString();
        diagnosticPrepareEntryDetail += " configured=" + CVCContainerPolicy.IsConfiguredContainerClass(container).ToString();
        diagnosticPrepareEntryDetail += " world=" + CVCContainerPolicy.IsWorldProviderLocation(container).ToString();
        diagnosticPrepareEntryDetail += " eligible=" + CVCContainerPolicy.IsEligible(container).ToString();
        diagnosticPrepareEntryDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
        diagnosticPrepareEntryDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
        diagnosticPrepareEntryDetail += " " + diagnosticPrepareBeforeRuntime;
        CVCContainerDiagnostics.TracePortable(container, "PREPARE_ENTRY", diagnosticPrepareEntryDetail, 250);
        if (!player)
            return DiagnosticPrepareReturn(container, diagnosticPrepareStateBefore, false, "player_missing", diagnosticPrepareBeforeRuntime);
        if (!diagnosticPrepareCanAccess)
            return DiagnosticPrepareReturn(container, diagnosticPrepareStateBefore, false, "can_access_false", diagnosticPrepareBeforeRuntime);
        if (!diagnosticPrepareNativeReady)
            return DiagnosticPrepareReturn(container, diagnosticPrepareStateBefore, false, "native_interaction_not_ready", diagnosticPrepareBeforeRuntime);
        CVCContainerRuntime state = State(container);
        if (!state)
            return DiagnosticPrepareReturn(container, state, false, "runtime_state_missing", diagnosticPrepareBeforeRuntime);
        if (state.physical_fallback)
            return DiagnosticPrepareReturn(container, state, false, "physical_fallback", diagnosticPrepareBeforeRuntime);
        if (state.native_inventory_blocked)
            return DiagnosticPrepareReturn(container, state, true, "native_inventory_blocked", diagnosticPrepareBeforeRuntime);

        if (state.phase == PHASE_RECOVERY)
        {
            if (!state.busy && ClientActionState(container) == ACTION_RETRY)
                Retry(container, player);
            bool diagnosticRecoveryBusy = state.busy;
            return DiagnosticPrepareReturn(container, state, diagnosticRecoveryBusy, "recovery_phase", diagnosticPrepareBeforeRuntime);
        }

        if (state.phase == PHASE_ACTIVE)
            return DiagnosticPrepareReturn(container, state, false, "active_materialized_session", diagnosticPrepareBeforeRuntime);

        if (state.busy)
            return DiagnosticPrepareReturn(container, state, true, "busy", diagnosticPrepareBeforeRuntime);
        if (state.recovering)
            return DiagnosticPrepareReturn(container, state, true, "recovering", diagnosticPrepareBeforeRuntime);
        if (state.phase != PHASE_IDLE)
            return DiagnosticPrepareReturn(container, state, true, "phase_not_idle", diagnosticPrepareBeforeRuntime);
        if (state.session_id != "")
            return DiagnosticPrepareReturn(container, state, true, "session_present", diagnosticPrepareBeforeRuntime);
        if (state.active_migration != null)
            return DiagnosticPrepareReturn(container, state, true, "active_migration", diagnosticPrepareBeforeRuntime);
        if (state.migration_prepare_dispatched)
            return DiagnosticPrepareReturn(container, state, true, "migration_prepare_dispatched", diagnosticPrepareBeforeRuntime);

        int diagnosticPreparePhysicalRoots = PhysicalRootCount(container);
        if (diagnosticPreparePhysicalRoots > 0)
        {
            // Physical roots and SQL roots must never be exposed as two inventories.
            // Import the physical roots first, then materialize the combined storage
            // back into this same DayZ cargo grid before the inventory UI opens.
            MarkPhysicalInteractionPending(state);
            CVCMigrationService.Enqueue(container);
            return DiagnosticPrepareReturn(container, state, true, "physical_roots_queue_migration", diagnosticPrepareBeforeRuntime);
        }

        Open(container, player, false);
        bool diagnosticOpenWaiting = state.busy;
        if (state.phase == PHASE_OPENING)
            diagnosticOpenWaiting = true;
        return DiagnosticPrepareReturn(container, state, diagnosticOpenWaiting, "open_called", diagnosticPrepareBeforeRuntime);
    }

    // Client inventory-menu hooks are advisory only. A later-loaded UI mod can open
    // the native inventory without forwarding that signal, which previously left
    // managed ground cargo permanently fail-closed. The server now derives access
    // from authoritative container state and player proximity, then uses the same
    // session/migration path before it permits any physical cargo write.
    static void ProbeNativeInventoryAccess()
    {
        bool diagnosticIsServer = GetGame().IsServer();
        bool diagnosticApiReady = ClippyVirtualCargoAPI.IsReady();
        bool diagnosticMigrationStarted = CVCMigrationService.IsStarted();
        string diagnosticGuards = "server=" + diagnosticIsServer.ToString();
        diagnosticGuards += " api_ready=" + diagnosticApiReady.ToString();
        diagnosticGuards += " enforcement_ready=" + s_EnforcementReady.ToString();
        diagnosticGuards += " migration_started=" + diagnosticMigrationStarted.ToString();
        diagnosticGuards += " registered_count=" + s_Registered.Count().ToString();
        if (!s_DiagnosticProbeFirstTick)
        {
            s_DiagnosticProbeFirstTick = true;
            Print("[CVC-DIAG] t_ms=" + GetGame().GetTime().ToString() + " realm=server event=PROBE_FIRST_TICK " + diagnosticGuards);
        }
        if (diagnosticGuards != s_DiagnosticProbeGuards)
        {
            s_DiagnosticProbeGuards = diagnosticGuards;
            Print("[CVC-DIAG] t_ms=" + GetGame().GetTime().ToString() + " realm=server event=PROBE_GLOBAL_GATES " + diagnosticGuards);
        }
        if (!diagnosticIsServer)
            return;
        if (!diagnosticApiReady)
            return;
        if (!s_EnforcementReady)
            return;
        if (!diagnosticMigrationStarted)
            return;

        int diagnosticDiscoveryNowMs = GetGame().GetTime();
        ref array<Man> diagnosticDiscoveryPlayers = new array<Man>;
        GetGame().GetPlayers(diagnosticDiscoveryPlayers);
        int diagnosticDiscoveredCount;
        foreach (Man diagnosticDiscoveryMan : diagnosticDiscoveryPlayers)
        {
            PlayerBase diagnosticDiscoveryPlayer = PlayerBase.Cast(diagnosticDiscoveryMan);
            diagnosticDiscoveredCount += DiscoverNearbyWorldProviders(diagnosticDiscoveryPlayer, "server_probe", 1000);
        }
        if (diagnosticDiscoveredCount > 0)
            Print("[CVC-DIAG] t_ms=" + diagnosticDiscoveryNowMs.ToString() + " realm=server event=NEARBY_DISCOVERY_SUMMARY origin=server_probe registered=" + diagnosticDiscoveredCount.ToString());

        foreach (string providerKey, EntityAI container : s_Registered)
        {
            if (!container)
                continue;
            bool diagnosticWorldLocation = CVCContainerPolicy.IsWorldProviderLocation(container);
            bool diagnosticNativeReady = CVCContainerPolicy.IsNativeInteractionReady(container);
            bool diagnosticItemBase = ItemBase.Cast(container) != null;
            CVCContainerRuntime diagnosticStateBefore;
            s_States.Find(providerKey, diagnosticStateBefore);
            string diagnosticProviderGate = "pass";
            if (!diagnosticWorldLocation)
                diagnosticProviderGate = "not_world_provider_location";
            else if (!diagnosticNativeReady)
                diagnosticProviderGate = "native_interaction_not_ready";
            else if (!diagnosticItemBase)
                diagnosticProviderGate = "not_itembase_vehicle_boundary";
            string diagnosticProviderDetail = "gate=" + diagnosticProviderGate;
            diagnosticProviderDetail += " map_key=" + providerKey;
            diagnosticProviderDetail += " configured=" + CVCContainerPolicy.IsConfiguredContainerClass(container).ToString();
            diagnosticProviderDetail += " world=" + diagnosticWorldLocation.ToString();
            diagnosticProviderDetail += " eligible=" + CVCContainerPolicy.IsEligible(container).ToString();
            diagnosticProviderDetail += " server_open=" + diagnosticNativeReady.ToString();
            diagnosticProviderDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            diagnosticProviderDetail += " " + CVCContainerDiagnostics.Hierarchy(container);
            diagnosticProviderDetail += " " + DiagnosticRuntime(diagnosticStateBefore);
            CVCContainerDiagnostics.TracePortable(container, "PROBE_PROVIDER_GATES", diagnosticProviderDetail, 1000);
            if (diagnosticProviderGate != "pass")
                continue;
            // Vehicle cargo has its own explicit interaction lifecycle. Proximity to
            // a driven vehicle must not materialize its storage in the background.
            CVCContainerRuntime state = State(container);
            string diagnosticRuntimeGate = "pass";
            if (!state)
                diagnosticRuntimeGate = "runtime_state_missing";
            else if (state.physical_fallback)
                diagnosticRuntimeGate = "physical_fallback";
            else if (state.native_inventory_blocked)
                diagnosticRuntimeGate = "native_inventory_blocked";
            else if (state.busy)
                diagnosticRuntimeGate = "busy";
            else if (state.recovering)
                diagnosticRuntimeGate = "recovering";
            else if (state.phase != PHASE_IDLE)
                diagnosticRuntimeGate = "phase_not_idle";
            else if (state.session_id != "")
                diagnosticRuntimeGate = "session_present";
            else if (state.active_migration != null)
                diagnosticRuntimeGate = "active_migration";
            else if (state.migration_prepare_dispatched)
                diagnosticRuntimeGate = "migration_prepare_dispatched";
            string diagnosticRuntimeDetail = "gate=" + diagnosticRuntimeGate;
            diagnosticRuntimeDetail += " physical_roots=" + PhysicalRootCount(container).ToString();
            diagnosticRuntimeDetail += " " + DiagnosticRuntime(state);
            CVCContainerDiagnostics.TracePortable(container, "PROBE_RUNTIME_GATES", diagnosticRuntimeDetail, 1000);
            if (diagnosticRuntimeGate != "pass")
                continue;

            PlayerBase player = FindNearbyInteractionPlayer(container);
            if (player)
                PrepareContainerForNativeInventory(container, player);
            else
                CVCContainerDiagnostics.TracePortable(container, "PROBE_RESULT", "result=no_accessible_player " + DiagnosticRuntime(state), 1000);
        }
    }

    protected static void PrepareInventoryOpen(PlayerBase player, int startedMs)
    {
        if (!GetGame().IsServer() || !player || !player.GetIdentity() || !player.IsAlive())
        {
            RemoveInventoryPreparingPlayer(player);
            return;
        }

        // Restrict the inventory gate to providers actually returned by the same
        // server-side proximity query used at the inventory edge. Scanning every
        // registered provider made a distant or closed container participate in an
        // unrelated Tab-open decision and needlessly repeated policy checks as the
        // registry grew. Discovery has already adopted eligible nearby providers;
        // this pass only resolves those nearby entities back to the authoritative map.
        ref array<Object> nearbyObjects = new array<Object>;
        ref array<CargoBase> proxyCargos = new array<CargoBase>;
        GetGame().GetObjectsAtPosition3D(player.GetPosition(), CVCSettingsManager.Get().AccessDistanceMetres, nearbyObjects, proxyCargos);
        ref array<EntityAI> accessibleProviders = new array<EntityAI>;
        foreach (Object nearbyObject : nearbyObjects)
        {
            EntityAI nearbyContainer = EntityAI.Cast(nearbyObject);
            if (!nearbyContainer || !CVCContainerPolicy.IsWorldProviderLocation(nearbyContainer))
                continue;
            string nearbyKey = CVCContainerPolicy.ProviderKey(nearbyContainer);
            if (nearbyKey == "")
                continue;
            EntityAI registeredContainer;
            if (!s_Registered.Find(nearbyKey, registeredContainer) || registeredContainer != nearbyContainer)
                continue;
            CVCContainerRuntime nearbyState;
            if (s_States.Find(nearbyKey, nearbyState) && nearbyState && nearbyState.physical_fallback)
                continue;
            if (!CVCContainerPolicy.CanAccess(nearbyContainer, player) || !CVCContainerPolicy.IsNativeInteractionReady(nearbyContainer))
                continue;
            if (accessibleProviders.Find(nearbyContainer) < 0)
                accessibleProviders.Insert(nearbyContainer);
        }

        bool hasAccessibleProvider = accessibleProviders.Count() > 0;

        // Tab is the player's native inventory control. Start any relevant provider
        // work here, but never use a pending SQL session to suppress the entire UI.
        // AllowsPhysicalCargo remains fail-closed until the provider reaches ACTIVE,
        // so an early inventory view cannot write into an empty physical grid.
        if (hasAccessibleProvider && ClippyVirtualCargoAPI.IsReady() && s_EnforcementReady && CVCMigrationService.IsStarted())
        {
            foreach (EntityAI container : accessibleProviders)
            {
                if (!container)
                    continue;
                CVCContainerRuntime state = State(container);
                if (state && state.native_inventory_blocked)
                    continue;
                PrepareContainerForNativeInventory(container, player);
            }
        }

        // 1.2.0: portable providers keep the same persistent provider key when they
        // move into a vehicle. They are not world providers there, so prepare them
        // explicitly from the registered runtime map when Tab opens. The physical
        // roots exist only while the player's inventory is open and are committed
        // back to PostgreSQL when that view closes.
        if (ClippyVirtualCargoAPI.IsReady() && s_EnforcementReady && CVCMigrationService.IsStarted())
            PrepareVehicleProvidersForInventory(player);

        ApproveInventoryOpen(player);
    }

    protected static void ApproveInventoryOpen(PlayerBase player)
    {
        if (!GetGame().IsServer() || !player || !player.GetIdentity())
            return;
        RemoveInventoryPreparingPlayer(player);
        if (s_InventoryOpenPlayers.Find(player) < 0)
            s_InventoryOpenPlayers.Insert(player);
        player.RPCSingleParam(CVCRPC.OPEN_INVENTORY, new Param1<string>("native"), true, player.GetIdentity());
    }

    static bool HasPhysicalInteractionAccess(EntityAI container)
    {
        // 1.0.6 no longer treats an open Tab menu as permission to write directly
        // into an idle provider. Managed cargo is writable only while its SQL session
        // is materialized into the native grid. This closes the old two-inventory path
        // when a player walks into range with inventory already open.
        return false;
    }

    static bool HasPendingPhysicalInteraction(EntityAI container)
    {
        if (!container)
            return false;
        string key = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime state;
        return key != "" && s_States.Find(key, state) && state && state.container == container && state.physical_interaction_pending;
    }

    static void ClearPhysicalInteractionPending(EntityAI container)
    {
        if (!container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime state;
        if (key == "" || !s_States.Find(key, state) || !state || state.container != container)
            return;
        state.physical_interaction_pending = false;
        if (!state.physical_fallback && !state.busy && !state.recovering && state.phase == PHASE_IDLE)
            SetActionState(container, ACTION_OPEN);
    }

    protected static void MarkPhysicalInteractionPending(CVCContainerRuntime state)
    {
        if (!state || state.physical_fallback || state.internal_mutation || state.busy || state.recovering || state.phase != PHASE_IDLE)
            return;
        state.physical_interaction_pending = true;
        if (state.container)
            SetActionState(state.container, ACTION_NONE);
    }

    static void NotePhysicalCargoChange(EntityAI container)
    {
        if (!GetGame().IsServer() || !container || !CVCContainerPolicy.IsEligible(container))
            return;
        CVCContainerRuntime state = State(container);
        MarkPhysicalInteractionPending(state);
    }

    protected static void FlushPendingPhysicalInteractions()
    {
        foreach (string key, CVCContainerRuntime state : s_States)
        {
            if (!state || !state.container || state.physical_fallback)
                continue;
            bool interactionOpen = HasPhysicalInteractionAccess(state.container);
            if (state.physical_interaction_tracking && !interactionOpen)
            {
                string currentFingerprint = PhysicalRootFingerprint(state.container);
                if (currentFingerprint != state.physical_interaction_baseline)
                    MarkPhysicalInteractionPending(state);
                state.physical_interaction_tracking = false;
                state.physical_interaction_baseline = "";
            }
            if (!state.physical_interaction_pending || interactionOpen)
                continue;
            if (PhysicalRootCount(state.container) == 0)
            {
                state.physical_interaction_pending = false;
                if (!state.busy && !state.recovering && state.phase == PHASE_IDLE)
                    SetActionState(state.container, ACTION_OPEN);
                continue;
            }
            CVCMigrationService.Enqueue(state.container);
        }
    }

    static bool AllowsPhysicalCargo(EntityAI container)
    {
        if (!container)
            return true;
        // M3S cargo is always vanilla in r62. Never let a stale Clippy runtime,
        // recovery record, or migration state veto its normal cargo grid.
        if (container.IsKindOf("Truck_01_Base"))
            return DiagnosticCargoDecision(container, null, true, "m3s_vanilla_boundary");
        if (!CVCContainerPolicy.IsEligible(container))
        {
            string nestedProviderKey = CVCContainerPolicy.ProviderKey(container);
            CVCContainerRuntime nestedState;
            if (nestedProviderKey != "" && s_States.Find(nestedProviderKey, nestedState) && nestedState && nestedState.container == container && nestedState.nested_materialization)
            {
                if (nestedState.internal_mutation)
                    return DiagnosticCargoDecision(container, nestedState, true, "nested_internal_mutation");
                if (nestedState.phase != PHASE_ACTIVE || nestedState.busy || nestedState.session_id == "")
                    return DiagnosticCargoDecision(container, nestedState, false, "nested_materialization_pending");
                return DiagnosticCargoDecision(container, nestedState, true, "nested_materialized_vanilla");
            }
            // Hands, cargo, and attachments are a hard Clippy boundary. A portable
            // container may retain an unsettled runtime for a short commit window,
            // but that stale top-level state must never veto its normal nested
            // inventory behavior.
            return DiagnosticCargoDecision(container, null, true, "ineligible_policy_boundary");
        }
        if (!s_EnforcementReady)
            return DiagnosticCargoDecision(container, null, true, "enforcement_not_ready");
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return DiagnosticCargoDecision(container, null, true, "provider_key_empty");
        CVCContainerRuntime state;
        bool diagnosticStateFound = s_States.Find(key, state);
        if (!diagnosticStateFound)
            return DiagnosticCargoDecision(container, state, false, "runtime_state_missing");
        if (!state)
            return DiagnosticCargoDecision(container, state, false, "runtime_state_null");
        if (state.container != container)
            return DiagnosticCargoDecision(container, state, false, "runtime_container_mismatch");
        if (state.native_inventory_blocked)
            return DiagnosticCargoDecision(container, state, false, "native_inventory_blocked");
        if (state.physical_fallback)
            return DiagnosticCargoDecision(container, state, true, "physical_fallback");
        if (state.internal_mutation)
            return DiagnosticCargoDecision(container, state, true, "internal_mutation");
        bool diagnosticActiveSession = state.phase == PHASE_ACTIVE;
        if (state.session_id == "")
            diagnosticActiveSession = false;
        if (!state.player)
            diagnosticActiveSession = false;
        if (diagnosticActiveSession)
        {
            string diagnosticSessionAccessReason;
            bool diagnosticSessionCanAccess = CVCContainerPolicy.CheckAccess(container, state.player, diagnosticSessionAccessReason);
            if (diagnosticSessionCanAccess)
                return DiagnosticCargoDecision(container, state, true, "active_authorized_materialized_session");
        }
        bool diagnosticPhysicalAccess = HasPhysicalInteractionAccess(container);
        if (diagnosticPhysicalAccess)
            return DiagnosticCargoDecision(container, state, true, "physical_interaction_access");
        return DiagnosticCargoDecision(container, state, false, "no_active_authorized_materialized_session");
    }

    static bool HasUnsettledWorkflow(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return false;
        string key = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime state;
        if (key == "" || !s_States.Find(key, state) || !state || state.container != container)
            return false;
        return state.busy || state.recovering || state.phase != PHASE_IDLE || state.session_id != "" || state.active_migration != null || state.migration_prepare_dispatched;
    }

    static void SettleAfterHierarchyMove(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime state;
        if (key == "" || !s_States.Find(key, state) || !state || state.container != container)
            return;
        if (CVCContainerPolicy.IsPhysicalInteractionLocation(container))
        {
            // Containers riding in a vehicle must remain physically empty while
            // their roots are virtual. Vanilla barrels and clothing use the
            // physical child count to decide whether they can be moved/detached.
            // Materializing here made an attached barrel impossible to remove.
            if (CVCContainerPolicy.IsVehicleContainmentLocation(container))
            {
                state.vehicle_inventory_materialization = false;
                state.nested_materialization = false;
                if (state.phase == PHASE_ACTIVE && state.session_id != "" && !state.busy)
                    Commit(state);
                CVCContainerDiagnostics.TracePortable(container, "VEHICLE_BOUNDARY_KEEP_VIRTUAL", "result=vanilla_container_shell physical_roots=" + PhysicalRootCount(container).ToString() + " " + DiagnosticRuntime(state), 250);
                return;
            }
            state.vehicle_inventory_materialization = false;
            state.nested_materialization = true;
            TryStartNestedMaterialization(state);
            return;
        }
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
        if (state.phase == PHASE_ACTIVE && state.session_id != "" && !state.busy)
            Commit(state);
    }

    static void HandleWorldHierarchyMove(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime state;
        if (key == "" || !s_States.Find(key, state) || !state || state.container != container)
            return;
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
    }

    static void TryStartNestedMaterialization(CVCContainerRuntime state)
    {
        if (!GetGame().IsServer() || !state || !state.container || !state.nested_materialization)
            return;
        if (!CVCContainerPolicy.IsPhysicalInteractionLocation(state.container))
            return;
        if (CVCContainerPolicy.IsVehicleContainmentLocation(state.container))
            return;
        if (state.phase != PHASE_IDLE || state.busy || state.recovering || state.physical_fallback || state.session_id != "" || state.active_migration != null || state.migration_prepare_dispatched)
            return;
        if (PhysicalRootCount(state.container) > 0)
            return;
        PlayerBase player = FindNearbyPhysicalPlayer(state.container);
        if (!player)
            return;
        state.player = player;
        state.busy = true;
        state.phase = PHASE_OPENING;
        SetActionState(state.container, ACTION_NONE);
        CVCContainerDiagnostics.TracePortable(state.container, "NESTED_MATERIALIZATION_REQUEST", "result=dispatch player_present=true " + DiagnosticRuntime(state), 250);
        if (state.storage_id == "")
        {
            CVCResolveRequest request = new CVCResolveRequest;
            request.provider_id = CVCSettingsManager.Get().ProviderID;
            request.provider_key = state.provider_key;
            request.display_name = state.container.GetDisplayName();
            request.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            CVCContainerMetadata.FillResolve(request, state.container);
            bool dispatched = ClippyVirtualCargoAPI.Post("/v1/storage/resolve", request, new CVCNativeResolveHandler(state, false));
            DiagnosticRoute(state, "/v1/storage/resolve", "NESTED_RESOLVE_DISPATCH", "accepted=" + dispatched.ToString());
            if (!dispatched)
                OpenUncertain(state, "could not dispatch nested cargo resolve; restart recovery is required");
            return;
        }
        OpenResolved(state, false);
    }

    static bool CanOpen(EntityAI container)
    {
        CVCContainerRuntime state;
        if (!container || !GetGame().IsServer())
            return false;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "" || !s_States.Find(key, state))
            return false;
        return state && state.container == container && !state.physical_fallback && !state.busy && !state.recovering && state.phase == PHASE_IDLE && state.session_id == "" && state.active_migration == null && !state.migration_prepare_dispatched;
    }

    static bool IsPhysicalFallback(EntityAI container)
    {
        CVCContainerRuntime state;
        string key = CVCContainerPolicy.ProviderKey(container);
        return key != "" && s_States.Find(key, state) && state && state.container == container && state.physical_fallback;
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
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
        state.migration_retries = 0;
        state.physical_interaction_pending = false;
        state.physical_interaction_tracking = false;
        state.physical_interaction_baseline = "";
        SetManagedLifecycle(container, false);
        SyncMovementLock(container);
        SetActionState(container, ACTION_NONE);
        Print("[Clippy Virtual Cargo] Physical fallback enabled for " + state.provider_key + ". Normal DayZ cargo remains active until restart. Reason: " + reason + ".");
        return true;
    }

    static bool HasActivePage(EntityAI container)
    {
        CVCContainerRuntime state;
        return container && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state && state.container == container && state.phase == PHASE_ACTIVE;
    }

    static bool HasNextPage(EntityAI container)
    {
        if (!GetGame().IsServer())
            return ClientActionState(container) == ACTION_NEXT;
        CVCContainerRuntime state;
        return container && ClientActionState(container) == ACTION_NEXT && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state && state.container == container && state.phase == PHASE_IDLE && state.next_cursor != "";
    }

    static bool NeedsRetry(EntityAI container)
    {
        if (!GetGame().IsServer())
            return ClientActionState(container) == ACTION_RETRY;
        CVCContainerRuntime state;
        return container && ClientActionState(container) == ACTION_RETRY && s_States.Find(CVCContainerPolicy.ProviderKey(container), state) && state && state.container == container && state.phase == PHASE_RECOVERY;
    }

    static void Open(EntityAI container, PlayerBase player, bool nextPage = false)
    {
        bool diagnosticOpenApiReady = ClippyVirtualCargoAPI.IsReady();
        bool diagnosticOpenCanOpen = CanOpen(container);
        string diagnosticOpenAccessReason;
        bool diagnosticOpenCanAccess = CVCContainerPolicy.CheckAccess(container, player, diagnosticOpenAccessReason);
        bool diagnosticOpenNativeReady = CVCContainerPolicy.IsNativeInteractionReady(container);
        string diagnosticOpenProviderKey = CVCContainerPolicy.ProviderKey(container);
        CVCContainerRuntime diagnosticOpenStateBefore;
        if (diagnosticOpenProviderKey != "")
            s_States.Find(diagnosticOpenProviderKey, diagnosticOpenStateBefore);
        string diagnosticOpenGate = "pass";
        if (!diagnosticOpenApiReady)
            diagnosticOpenGate = "api_not_ready";
        else if (!diagnosticOpenCanOpen)
            diagnosticOpenGate = "can_open_false";
        else if (!diagnosticOpenCanAccess)
            diagnosticOpenGate = "can_access_false";
        else if (!diagnosticOpenNativeReady)
            diagnosticOpenGate = "native_interaction_not_ready";
        string diagnosticOpenEntryDetail = "gate=" + diagnosticOpenGate;
        diagnosticOpenEntryDetail += " api_ready=" + diagnosticOpenApiReady.ToString();
        diagnosticOpenEntryDetail += " can_open=" + diagnosticOpenCanOpen.ToString();
        diagnosticOpenEntryDetail += " can_access=" + diagnosticOpenCanAccess.ToString();
        diagnosticOpenEntryDetail += " access_reason=" + diagnosticOpenAccessReason;
        diagnosticOpenEntryDetail += " native_ready=" + diagnosticOpenNativeReady.ToString();
        diagnosticOpenEntryDetail += " next_page=" + nextPage.ToString();
        diagnosticOpenEntryDetail += " " + DiagnosticRuntime(diagnosticOpenStateBefore);
        CVCContainerDiagnostics.TracePortable(container, "OPEN_ENTRY", diagnosticOpenEntryDetail, 100);
        if (!diagnosticOpenApiReady)
            return;
        if (!diagnosticOpenCanOpen)
            return;
        if (!diagnosticOpenCanAccess)
            return;
        string nativeInteractionError;
        if (!CVCContainerPolicy.CheckNativeInteractionReady(container, nativeInteractionError))
        {
            return;
        }
        CVCContainerRuntime state = State(container);
        if (!state)
        {
            CVCContainerDiagnostics.TracePortable(container, "OPEN_RETURN_STATE_MISSING", "result=return", 100);
            return;
        }
        if (state.busy || state.phase != PHASE_IDLE)
        {
            DiagnosticRoute(state, "native", "OPEN_RETURN_BUSY", "result=return");
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
            CVCContainerMetadata.FillResolve(request, container);
            bool diagnosticResolveDispatched = ClippyVirtualCargoAPI.Post("/v1/storage/resolve", request, new CVCNativeResolveHandler(state, nextPage));
            DiagnosticRoute(state, "/v1/storage/resolve", "RESOLVE_DISPATCH", "accepted=" + diagnosticResolveDispatched.ToString());
            if (!diagnosticResolveDispatched)
                Fail(state, "API is not ready");
            return;
        }
        DiagnosticRoute(state, "/v1/storage/resolve", "RESOLVE_BYPASS_EXISTING_STORAGE", "storage_present=true");
        OpenResolved(state, nextPage);
    }

    static void OpenResolved(CVCContainerRuntime state, bool nextPage)
    {
        if (state && state.container)
        {
            bool diagnosticResolvedPlayerPresent = state.player != null;
            bool diagnosticResolvedIdentityPresent;
            if (state.player)
                diagnosticResolvedIdentityPresent = state.player.GetIdentity() != null;
            string diagnosticResolvedEntryDetail = "player_present=" + diagnosticResolvedPlayerPresent.ToString();
            diagnosticResolvedEntryDetail += " identity_present=" + diagnosticResolvedIdentityPresent.ToString();
            diagnosticResolvedEntryDetail += " storage_present=" + (state.storage_id != "").ToString();
            diagnosticResolvedEntryDetail += " next_page=" + nextPage.ToString();
            DiagnosticRoute(state, "/v1/session/open", "OPEN_RESOLVED_ENTRY", diagnosticResolvedEntryDetail);
        }
        if (!state || !state.container || !state.player || !state.player.GetIdentity())
        {
            DiagnosticRoute(state, "/v1/session/open", "OPEN_RESOLVED_IDENTITY_MISSING", "result=fail");
            Fail(state, "player or container identity is unavailable");
            return;
        }
        bool nestedMaterialization = state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container);
        string diagnosticResolvedAccessReason;
        if (nestedMaterialization)
            diagnosticResolvedAccessReason = "nested_physical_location";
        else
            diagnosticResolvedAccessReason = "player_missing";
        bool diagnosticResolvedCanAccess = nestedMaterialization || CVCContainerPolicy.CheckAccess(state.container, state.player, diagnosticResolvedAccessReason);
        bool diagnosticResolvedNativeReady = nestedMaterialization || CVCContainerPolicy.IsNativeInteractionReady(state.container);
        if (!diagnosticResolvedCanAccess || !diagnosticResolvedNativeReady)
        {
            string diagnosticResolvedUnavailableDetail = "can_access=" + diagnosticResolvedCanAccess.ToString();
            diagnosticResolvedUnavailableDetail += " access_reason=" + diagnosticResolvedAccessReason;
            diagnosticResolvedUnavailableDetail += " native_ready=" + diagnosticResolvedNativeReady.ToString();
            DiagnosticRoute(state, "/v1/session/open", "OPEN_RESOLVED_UNAVAILABLE", diagnosticResolvedUnavailableDetail);
            Fail(state, "container is no longer available for virtual cargo");
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
        request.limit = Math.Clamp(CVCSettingsManager.Get().VirtualRootCapacity, 1, 200);
        state.cursor = request.cursor;
        bool diagnosticOpenDispatched = ClippyVirtualCargoAPI.Post("/v1/session/open", request, new CVCNativeOpenHandler(state));
        DiagnosticRoute(state, "/v1/session/open", "OPEN_DISPATCH", "accepted=" + diagnosticOpenDispatched.ToString());
        if (!diagnosticOpenDispatched)
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

    // Validate real container/item interaction rules after the virtual page has been
    // rebuilt but before the host is told materialization succeeded. Clippy's own
    // temporary physical-cargo guard is suspended for this probe; third-party rules
    // still execute. A failed probe aborts and cleans the temporary physical page.
    static bool ValidateNativeMaterializedInteraction(CVCContainerRuntime state, out string reason)
    {
        reason = "";
        if (!state || !state.container)
        {
            reason = "container interaction state is unavailable";
            return false;
        }

        bool nestedMaterialization = state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container);
        string nativeInteractionError;
        if (!nestedMaterialization && !CVCContainerPolicy.CheckNativeInteractionReady(state.container, nativeInteractionError))
        {
            reason = nativeInteractionError;
            return false;
        }

        bool previousInternalMutation = state.internal_mutation;
        state.internal_mutation = true;
        foreach (ItemBase root : state.materialized)
        {
            if (!root || root.GetHierarchyParent() != state.container)
                continue;
            bool canRelease = state.container.CanReleaseCargo(root);
            bool canRemove = root.CanRemoveFromCargo(state.container);
            bool canReceive = state.container.CanReceiveItemIntoCargo(root);
            bool canPut = root.CanPutInCargo(state.container);

            // A nested provider has already passed RestoreRoot's native cargo-location
            // postcheck. Parent CanReleaseCargo/CanReceiveItemIntoCargo can still be
            // false solely because the parent is closed, such as a barrel in vehicle
            // cargo. Those are player-interaction rules, not a reason to roll back a
            // successfully restored virtual root. Keep the root-level checks strict;
            // vanilla evaluates the parent rules when the player later uses the cargo.
            if (nestedMaterialization && (!canRelease || !canReceive))
            {
                string nestedParentGateDetail = "result=deferred_after_native_restore";
                nestedParentGateDetail += " can_release=" + canRelease.ToString();
                nestedParentGateDetail += " can_receive=" + canReceive.ToString();
                nestedParentGateDetail += " can_remove=" + canRemove.ToString();
                nestedParentGateDetail += " can_put=" + canPut.ToString();
                nestedParentGateDetail += " physical_roots=" + PhysicalRootCount(state.container).ToString();
                CVCContainerDiagnostics.TracePortable(state.container, "NESTED_MATERIALIZED_PARENT_GATE_DEFERRED", nestedParentGateDetail, 1000);
            }

            if ((!nestedMaterialization && !canRelease) || !canRemove)
            {
                CVCContainerDiagnostics.TracePortable(state.container, "NATIVE_MATERIALIZED_PROBE_DENY_" + root.GetType(), "branch=release_or_remove nested=" + nestedMaterialization.ToString() + " can_release=" + canRelease.ToString() + " can_remove=" + canRemove.ToString() + " root_parent_matches=" + (root.GetHierarchyParent() == state.container).ToString(), 1000);
                state.internal_mutation = previousInternalMutation;
                reason = "native container rules block removing materialized cargo; open or unlock the container normally, or exclude this container class from virtual cargo";
                return false;
            }
            if ((!nestedMaterialization && !canReceive) || !canPut)
            {
                CVCContainerDiagnostics.TracePortable(state.container, "NATIVE_MATERIALIZED_PROBE_DENY_" + root.GetType(), "branch=receive_or_put nested=" + nestedMaterialization.ToString() + " can_receive=" + canReceive.ToString() + " can_put=" + canPut.ToString() + " root_parent_matches=" + (root.GetHierarchyParent() == state.container).ToString(), 1000);
                state.internal_mutation = previousInternalMutation;
                reason = "native container rules block returning cargo; open or unlock the container normally, or exclude this container class from virtual cargo";
                return false;
            }
        }
        state.internal_mutation = previousInternalMutation;
        return true;
    }

    static void CloseForPlayer(PlayerBase player)
    {
        if (!GetGame().IsServer() || !player)
            return;
        RemoveInventoryPreparingPlayer(player);
        RemoveInventoryOpenPlayer(player);
        s_LastNearbyDiscoveryMs.Remove(player);
        foreach (string key, CVCContainerRuntime state : s_States)
        {
            if (state && state.player == player && state.phase == PHASE_ACTIVE)
            {
                if (state.vehicle_inventory_materialization)
                {
                    state.vehicle_inventory_materialization = false;
                    state.nested_materialization = false;
                    Commit(state);
                }
                else if (state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container))
                    state.player = null;
                else
                    Commit(state);
            }
        }
        FlushPendingPhysicalInteractions();
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
        Print("[Clippy Virtual Cargo] Waiting for persistent container " + recovery.provider_key + " before recovering session " + recovery.session_id);
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
        if (state.storage_id != "")
            CVCProviderIdentityRegistry.Remember(container);
        state.session_id = recovery.session_id;
        state.player = null;
        state.busy = false;
        state.recovering = true;
        state.physical_fallback = false;
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = CVCContainerPolicy.IsPhysicalInteractionLocation(container) && !CVCContainerPolicy.IsVehicleContainmentLocation(container);
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
        if (state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container))
        {
            CompleteRecovery(state);
            Print("[Clippy Virtual Cargo] Exact MATERIALIZED recovery retained physical nested cargo for " + recovery.provider_key + ".");
            return;
        }
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

        if (state.nested_materialization && CVCContainerPolicy.IsPhysicalInteractionLocation(state.container))
        {
            CVCContainerDiagnostics.TracePortable(state.container, "NESTED_MATERIALIZATION_SUCCESS", "result=physical_roots_ready physical_roots=" + PhysicalRootCount(state.container).ToString() + " " + DiagnosticRuntime(state), 250);
            return;
        }

        if (!state.player || !CVCContainerPolicy.CanAccess(state.container, state.player))
        {
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Commit, 250, false, state);
            return;
        }
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
            if (!state.player)
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
        if (state.container)
            CVCContainerDiagnostics.TracePortable(state.container, "MATERIALIZATION_ABORT", "reason=" + reason + " " + DiagnosticRuntime(state), 1000);
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
        state.pending_failure_reason = "";
        state.player = null;
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
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
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
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
        ErrorEx("[Clippy Virtual Cargo] " + reason + " for " + state.provider_key);
    }

    static void Fail(CVCContainerRuntime state, string reason)
    {
        if (!state)
            return;
        state.busy = false;
        state.phase = PHASE_IDLE;
        state.player = null;
        state.vehicle_inventory_materialization = false;
        state.nested_materialization = false;
        if (state.next_cursor != "")
            SetActionState(state.container, ACTION_NEXT);
        else
            SetActionState(state.container, ACTION_OPEN);
    }

    static void Tick()
    {
        if (!GetGame().IsServer())
            return;
        PruneInventoryOpenPlayers();

        // A container can enter vicinity while Tab is already open. Prepare newly
        // visible providers here too; until they reach PHASE_ACTIVE their native cargo
        // hooks remain locked, so SQL contents can never coexist with a writable empty grid.
        foreach (PlayerBase inventoryPlayer : s_InventoryOpenPlayers)
        {
            if (!inventoryPlayer || !inventoryPlayer.GetIdentity() || !inventoryPlayer.IsAlive())
                continue;
            foreach (string openProviderKey, EntityAI openContainer : s_Registered)
            {
                if (!CVCContainerPolicy.IsWorldProviderLocation(openContainer))
                    continue;
                if (!CVCContainerPolicy.CanAccess(openContainer, inventoryPlayer) || !CVCContainerPolicy.IsNativeInteractionReady(openContainer))
                    continue;
                PrepareContainerForNativeInventory(openContainer, inventoryPlayer);
            }
        }

        FlushPendingPhysicalInteractions();
        int nowMs = GetGame().GetTime();
        array<EntityAI> settledNested = new array<EntityAI>;
        foreach (string key, CVCContainerRuntime state : s_States)
        {
            if (!state || !state.container)
                continue;
            if (!CVCContainerPolicy.IsWorldProviderLocation(state.container))
            {
                if (CVCContainerPolicy.IsPhysicalInteractionLocation(state.container))
                {
                    if (CVCContainerPolicy.IsVehicleContainmentLocation(state.container))
                    {
                        PlayerBase vehicleViewer = FindOpenInventoryPlayer(state.container);
                        if (state.phase == PHASE_ACTIVE && state.session_id != "" && !state.busy)
                        {
                            if (state.vehicle_inventory_materialization && state.player && IsInventoryOpenForPlayer(state.player))
                                continue;
                            state.vehicle_inventory_materialization = false;
                            state.nested_materialization = false;
                            Commit(state);
                            continue;
                        }
                        if (!state.busy && !state.recovering && state.phase == PHASE_IDLE && state.session_id == "" && vehicleViewer)
                            TryStartVehicleInventoryMaterialization(state, vehicleViewer);
                        continue;
                    }
                    state.vehicle_inventory_materialization = false;
                    state.nested_materialization = true;
                    TryStartNestedMaterialization(state);
                    continue;
                }
                state.vehicle_inventory_materialization = false;
                state.nested_materialization = false;
                if (state.phase == PHASE_ACTIVE && state.session_id != "" && !state.busy)
                    Commit(state);
                if (!state.busy && !state.recovering && state.phase == PHASE_IDLE && state.session_id == "" && state.active_migration == null && !state.migration_prepare_dispatched)
                    settledNested.Insert(state.container);
                continue;
            }
            state.vehicle_inventory_materialization = false;
            state.nested_materialization = false;
            if (state.phase == PHASE_ACTIVE && (!state.player || !CVCContainerPolicy.CanAccess(state.container, state.player)))
            {
                Commit(state);
            }
            else if (state.phase == PHASE_ACTIVE && !CVCContainerPolicy.IsNativeInteractionReady(state.container))
            {
                Commit(state);
            }
            if (state.storage_id != "" && (state.last_metadata_report_ms == 0 || nowMs - state.last_metadata_report_ms >= 60000))
                CVCContainerMetadata.Observe(state);
        }
        foreach (EntityAI nestedContainer : settledNested)
        {
            Unregister(nestedContainer);
            CVCMigrationService.UnregisterCandidate(nestedContainer);
            ItemBase nestedItem = ItemBase.Cast(nestedContainer);
            if (nestedItem)
                nestedItem.CVCSetActionState(ACTION_NONE);
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
        if (m_State.storage_id != "")
            CVCProviderIdentityRegistry.Remember(m_State.container);
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
    protected static ref map<string, bool> s_PendingRecoveryRetry = new map<string, bool>;
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
        bool persistentPhysicalLocation = container && CVCContainerPolicy.IsPhysicalInteractionLocation(container) && CVCContainerPolicy.IsConfiguredContainerClass(container) && CVCProviderIdentityRegistry.Contains(container);
        if (!GetGame().IsServer() || (!CVCContainerPolicy.IsWorldProviderLocation(container) && !persistentPhysicalLocation) || !container.GetInventory() || !container.GetInventory().GetCargo())
            return;
        if (!CVCContainerPolicy.IsEligible(container) && !persistentPhysicalLocation)
        {
            if (CVCSettingsManager.Get().ReportUnlistedStorageCandidates && IsUnlistedCandidate(container))
                Observe(container, "UNLISTED_CANDIDATE", PhysicalRootCount(container), 0, 0, "Cargo-bearing world item is blocked by the current virtual cargo container policy.");
            return;
        }
        CVCContainerDiagnostics.Trace(ItemBase.Cast(container), "DELAYED_REGISTER");
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
        {
            if (identityAttempt < MAX_IDENTITY_RETRIES)
                GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(RegisterCandidate, 2000, false, container, identityAttempt + 1);
            else
                ErrorEx("[Clippy Virtual Cargo] Skipping " + container.GetType() + " because DayZ did not assign a stable persistent ID. It will be retried after its next persistence load.");
            return;
        }
        EntityAI migrationContainer;
        bool migrationContainerFound = s_Containers.Find(key, migrationContainer);
        if (migrationContainerFound && migrationContainer && migrationContainer != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "REGISTER_COLLISION", "map=migration result=refused existing_type=" + migrationContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            ErrorEx("[Clippy Virtual Cargo] Refusing duplicate migration provider key " + key + " while registering " + container.GetType() + ".");
            return;
        }
        if (!CVCContainerService.Register(container))
            return;
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
        if (!persistentPhysicalLocation)
            Enqueue(container);
        else
            CVCContainerService.SettleAfterHierarchyMove(container);
    }

    // Repairs a missed lifecycle registration at the player interaction boundary.
    // This path synchronously consults pending migration recovery before the caller
    // prepares a normal cargo session. A pre-existing workflow remains fail-closed
    // and receives one deduplicated retry chain. Empty providers never enter the
    // existing-physical-cargo queue.
    static bool AdoptDiscoveredCandidate(EntityAI container, string origin)
    {
        if (!GetGame().IsServer() || !container || !CVCContainerPolicy.IsWorldProviderLocation(container))
            return false;
        if (!container.GetInventory() || !container.GetInventory().GetCargo() || !CVCContainerPolicy.IsEligible(container))
            return false;

        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return false;

        EntityAI migrationContainer;
        bool migrationContainerFound = s_Containers.Find(key, migrationContainer);
        if (migrationContainerFound && migrationContainer && migrationContainer != container)
        {
            CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_COLLISION", "origin=" + origin + " map=migration result=refused existing_type=" + migrationContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            ErrorEx("[Clippy Virtual Cargo] Refusing discovery adoption for duplicate migration provider key " + key + ".");
            return false;
        }

        if (!CVCContainerService.Register(container))
            return false;
        if (CVCContainerService.IsPhysicalFallback(container))
            return true;

        s_Containers.Set(key, container);
        CVCContainerRuntime state = CVCContainerService.State(container);
        if (!state || state.container != container)
            return false;

        CVCMigrationData recovery;
        if (s_Pending.Find(key, recovery) && recovery)
        {
            if (CVCContainerService.HasUnsettledWorkflow(container))
            {
                SchedulePendingRecoveryRetry(container, origin);
                CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_MIGRATION_RECOVERY", "origin=" + origin + " result=deferred_existing_workflow " + CVCContainerDiagnostics.Hierarchy(container), 1000);
                return true;
            }
            s_Pending.Remove(key);
            BeginRecovery(container, recovery);
            CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_MIGRATION_RECOVERY", "origin=" + origin + " result=started " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            return true;
        }

        int physicalRoots = PhysicalRootCount(container);
        bool queuedPhysicalRoots = false;
        if (physicalRoots > 0 && !CVCContainerService.HasUnsettledWorkflow(container))
        {
            Enqueue(container);
            queuedPhysicalRoots = true;
        }
        CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_ADOPTED", "origin=" + origin + " physical_roots=" + physicalRoots.ToString() + " queued_physical_roots=" + queuedPhysicalRoots.ToString() + " " + CVCContainerDiagnostics.Hierarchy(container), 250);
        return true;
    }

    protected static void SchedulePendingRecoveryRetry(EntityAI container, string origin)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key == "")
            return;
        bool retryScheduled;
        if (s_PendingRecoveryRetry.Find(key, retryScheduled))
            return;
        s_PendingRecoveryRetry.Set(key, true);
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.RetryPendingRecovery, 1000, false, container, key, origin);
    }

    protected static void RetryPendingRecovery(EntityAI container, string expectedKey, string origin)
    {
        if (expectedKey != "")
            s_PendingRecoveryRetry.Remove(expectedKey);
        if (!s_Started || !container || expectedKey == "")
            return;
        if (CVCContainerPolicy.ProviderKey(container) != expectedKey || !CVCContainerPolicy.IsEligible(container))
            return;

        CVCMigrationData recovery;
        if (!s_Pending.Find(expectedKey, recovery) || !recovery)
            return;
        if (CVCContainerService.HasUnsettledWorkflow(container))
        {
            SchedulePendingRecoveryRetry(container, origin);
            return;
        }

        s_Pending.Remove(expectedKey);
        BeginRecovery(container, recovery);
        CVCContainerDiagnostics.TracePortable(container, "NEARBY_DISCOVERY_MIGRATION_RECOVERY", "origin=" + origin + " result=retry_started " + CVCContainerDiagnostics.Hierarchy(container), 1000);
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
        if (s_Started)
        {
            // Runtime drops must not wait behind the startup persistence scan.
            s_Queue.InsertAt(container, 0);
            s_QueueKeys.InsertAt(key, 0);
        }
        else
        {
            s_Queue.Insert(container);
            s_QueueKeys.Insert(key);
        }
        s_CompleteLogged = false;
    }

    static void UnregisterCandidate(EntityAI container)
    {
        if (!GetGame().IsServer() || !container)
            return;
        string key = CVCContainerPolicy.ProviderKey(container);
        if (key != "")
        {
            EntityAI migrationContainer;
            bool migrationContainerFound = s_Containers.Find(key, migrationContainer);
            if (migrationContainerFound && migrationContainer && migrationContainer != container)
            {
                CVCContainerDiagnostics.TracePortable(container, "UNREGISTER_COLLISION", "map=migration result=refused existing_type=" + migrationContainer.GetType() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            }
            else if (migrationContainerFound)
            {
                s_Containers.Remove(key);
                s_Queued.Remove(key);
                s_PendingRecoveryRetry.Remove(key);
            }
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

    static bool IsStarted()
    {
        return s_Started;
    }

    static int PendingRecoveryCount()
    {
        return s_Pending.Count();
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
        if (!CVCContainerPolicy.IsWorldProviderLocation(container) || container.IsKindOf("Clothing") || container.IsKindOf("Bag_Base") || container.IsKindOf("FireplaceBase"))
            return false;
        if (CVCSettingsManager.Get().ReportEmptyUnlistedStorageCandidates)
            return true;
        return PhysicalRootCount(container) >= Math.Max(1, CVCSettingsManager.Get().MinimumUnlistedPhysicalRoots);
    }

    static void Scan(EntityAI container)
    {
        if (!CVCContainerPolicy.IsWorldProviderLocation(container))
            return;
        if (!CVCContainerPolicy.IsEligible(container))
        {
            if (CVCSettingsManager.Get().ReportUnlistedStorageCandidates && IsUnlistedCandidate(container))
                Observe(container, "UNLISTED_CANDIDATE", PhysicalRootCount(container), 0, 0, "Cargo-bearing world item is blocked by the current virtual cargo container policy.");
            return;
        }

        CVCContainerRuntime state = CVCContainerService.State(container);
        if (!state || state.physical_fallback)
            return;

        if (CVCContainerService.HasPhysicalInteractionAccess(container))
        {
            GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCMigrationService.Enqueue, 1000, false, container);
            return;
        }

        int roots = PhysicalRootCount(container);
        bool livePhysicalCargo = state.physical_interaction_pending;
        if (roots == 0 && livePhysicalCargo)
        {
            CVCContainerService.ClearPhysicalInteractionPending(container);
            return;
        }
        if (roots > 0 && !CVCSettingsManager.Get().EnableExistingCargoMigration && !livePhysicalCargo)
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
                CVCContainerService.EnablePhysicalFallback(container, "a child item cannot be virtualized: " + validationError);
                return;
            }
            if (item && item.GetHierarchyParent() == container && SourceKey(item, index) == "")
            {
                RetryContainer(container, "A physical root does not yet have a stable DayZ persistent ID.");
                return;
            }
        }

        if (state.busy || state.phase != CVCContainerService.PHASE_IDLE)
        {
            RetryContainer(container, "Container is busy with a cargo session or recovery.");
            return;
        }
        state.migration_retries = 0;
        state.busy = true;
        state.phase = CVCContainerService.PHASE_MIGRATING;
        CVCContainerService.SyncMovementLock(state.container);
        s_InFlight++;
        if (state.storage_id == "")
        {
            CVCResolveRequest resolve = new CVCResolveRequest;
            resolve.provider_id = CVCSettingsManager.Get().ProviderID;
            resolve.provider_key = state.provider_key;
            resolve.display_name = container.GetDisplayName();
            resolve.capacity_slots = CVCSettingsManager.Get().VirtualRootCapacity;
            CVCContainerMetadata.FillResolve(resolve, state.container);
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
        if (!CVCContainerPolicy.IsEligible(state.container))
        {
            CancelMovedBeforePrepare(state);
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

    static void CancelMovedBeforePrepare(CVCContainerRuntime state)
    {
        if (!state)
            return;

        EntityAI movedContainer = state.container;
        state.busy = false;
        state.phase = CVCContainerService.PHASE_IDLE;
        state.migration_retries = 0;
        state.migration_prepare_dispatched = false;
        state.active_migration = null;
        if (s_InFlight > 0)
            s_InFlight--;

        if (movedContainer)
        {
            CVCContainerService.SetActionState(movedContainer, CVCContainerService.ACTION_NONE);
            CVCContainerService.Unregister(movedContainer);
            UnregisterCandidate(movedContainer);
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
        Print("[Clippy Virtual Cargo] Waiting for container " + migration.provider_key + " to resume migration " + migration.migration_id);
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
        bool sameActiveMigration;
        if (state.active_migration && state.active_migration.migration_id == migration.migration_id)
            sameActiveMigration = true;
        if (sameActiveMigration && state.busy && !state.recovering && state.session_id == "" && state.phase == CVCContainerService.PHASE_MIGRATING)
        {
            CVCContainerDiagnostics.TracePortable(container, "MIGRATION_RECOVERY_ALREADY_ACTIVE", "migration_id=" + migration.migration_id + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            return;
        }

        bool sessionWorkflowConflict = state.busy;
        if (state.recovering || state.session_id != "")
            sessionWorkflowConflict = true;
        if (state.active_migration && !sameActiveMigration)
            sessionWorkflowConflict = true;
        if (state.phase == CVCContainerService.PHASE_OPENING)
            sessionWorkflowConflict = true;
        if (state.phase == CVCContainerService.PHASE_ACTIVE)
            sessionWorkflowConflict = true;
        if (state.phase == CVCContainerService.PHASE_COMMITTING)
            sessionWorkflowConflict = true;
        if (state.phase == CVCContainerService.PHASE_RECOVERY)
            sessionWorkflowConflict = true;
        if (sessionWorkflowConflict)
        {
            s_Pending.Set(migration.provider_key, migration);
            SchedulePendingRecoveryRetry(container, "begin_recovery_session_conflict");
            CVCContainerDiagnostics.TracePortable(container, "MIGRATION_RECOVERY_DEFERRED", "busy=" + state.busy.ToString() + " recovering=" + state.recovering.ToString() + " phase=" + state.phase.ToString() + " session_present=" + (state.session_id != "").ToString() + " " + CVCContainerDiagnostics.Hierarchy(container), 1000);
            return;
        }
        state.storage_id = migration.storage_id;
        state.revision = migration.expected_revision;
        if (state.storage_id != "")
            CVCProviderIdentityRegistry.Remember(container);
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
        EntityAI movedContainer;
        if (state)
        {
            state.busy = false;
            state.phase = CVCContainerService.PHASE_IDLE;
            state.migration_retries = 0;
            state.migration_prepare_dispatched = false;
            state.active_migration = null;
            if (state.container)
            {
                CVCContainerService.SyncMovementLock(state.container);
                Observe(state.container, status, PhysicalRootCount(state.container), 0, 0, detail);
                if (!CVCContainerPolicy.IsEligible(state.container))
                {
                    movedContainer = state.container;
                    CVCContainerService.SetActionState(state.container, CVCContainerService.ACTION_NONE);
                }
                else if (requeue)
                    Enqueue(state.container);
                else
                {
                    state.physical_interaction_pending = false;
                    state.physical_interaction_tracking = false;
                    state.physical_interaction_baseline = "";
                    if (!state.physical_fallback)
                        CVCContainerService.SetActionState(state.container, CVCContainerService.ACTION_OPEN);
                }
            }
        }
        if (s_InFlight > 0)
            s_InFlight--;
        if (movedContainer)
        {
            CVCContainerService.Unregister(movedContainer);
            UnregisterCandidate(movedContainer);
        }
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
            if (!state.active_migration)
                state.migration_prepare_dispatched = false;
            if (state.container)
                CVCContainerService.SyncMovementLock(state.container);
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
        if (!state.active_migration && !CVCContainerPolicy.IsEligible(state.container))
        {
            CancelMovedBeforePrepare(state);
            return;
        }
        state.busy = true;
        state.phase = CVCContainerService.PHASE_MIGRATING;
        CVCContainerService.SyncMovementLock(state.container);
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
            CVCContainerMetadata.FillResolve(resolve, state.container);
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

modded class ItemBase
{
    protected string m_CVCVirtualItemID;
    protected string m_CVCLiveItemID;
    protected int m_CVCActionState;

    void ItemBase()
    {
        RegisterNetSyncVariableInt("m_CVCActionState", CVCContainerService.ACTION_NONE, CVCContainerService.ACTION_RETRY);
    }

    string CVCGetVirtualItemID() { return m_CVCVirtualItemID; }
    void CVCSetVirtualItemID(string itemID) { m_CVCVirtualItemID = itemID; }
    void CVCSetLiveItemID(string itemID) { m_CVCLiveItemID = itemID; }
    string CVCGetLiveItemID()
    {
        if (m_CVCVirtualItemID != "")
            return m_CVCVirtualItemID;
        if (m_CVCLiveItemID != "")
            return m_CVCLiveItemID;
        int a;
        int b;
        int c;
        int d;
        GetPersistentID(a, b, c, d);
        if (a != 0 || b != 0 || c != 0 || d != 0)
            m_CVCLiveItemID = string.Format("live:%1:%2:%3:%4:%5", GetType(), a, b, c, d);
        else
            m_CVCLiveItemID = string.Format("live:%1:%2:%3", GetType(), GetGame().GetTime(), Math.RandomInt(100000, 999999));
        return m_CVCLiveItemID;
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
            CVCContainerDiagnostics.Trace(this, "EEINIT");
            // Trader and admin creation can run EEInit before the item reaches
            // its final inventory location. Register only after the one-second
            // candidate check confirms that it is still a top-level provider.
            CVCMigrationService.ScheduleCandidate(this);
        }
    }

    override void AfterStoreLoad()
    {
        super.AfterStoreLoad();
        if (GetGame().IsServer())
        {
            CVCContainerDiagnostics.Trace(this, "AFTER_STORE_LOAD");
            CVCMigrationService.ScheduleCandidate(this);
        }
    }


    override bool CanReceiveItemIntoCargo(EntityAI item)
    {
        bool diagnosticReceiveCVCResult = true;
        if (GetGame().IsServer())
            diagnosticReceiveCVCResult = CVCContainerService.AllowsPhysicalCargo(this);
        bool diagnosticReceiveSuperResult = super.CanReceiveItemIntoCargo(item);
        bool diagnosticReceiveFinalResult = diagnosticReceiveCVCResult && diagnosticReceiveSuperResult;
        if (CVCContainerDiagnostics.IsPortableTarget(this))
        {
            string diagnosticIncomingType = "null";
            if (item)
                diagnosticIncomingType = item.GetType();
            string diagnosticReceiveDetail = "incoming=" + diagnosticIncomingType;
            diagnosticReceiveDetail += " cvc_result=" + diagnosticReceiveCVCResult.ToString();
            diagnosticReceiveDetail += " super_result=" + diagnosticReceiveSuperResult.ToString();
            diagnosticReceiveDetail += " final=" + diagnosticReceiveFinalResult.ToString();
            diagnosticReceiveDetail += " server_open=" + IsOpen().ToString();
            diagnosticReceiveDetail += " " + CVCContainerDiagnostics.Hierarchy(this);
            CVCContainerDiagnostics.TracePortable(this, "ITEMBASE_CAN_RECEIVE_" + diagnosticIncomingType, diagnosticReceiveDetail, 250);
        }
        return diagnosticReceiveFinalResult;
    }


    override bool CanReleaseCargo(EntityAI cargo)
    {
        bool diagnosticReleaseCVCResult = true;
        if (GetGame().IsServer())
            diagnosticReleaseCVCResult = CVCContainerService.AllowsPhysicalCargo(this);
        bool diagnosticReleaseSuperResult = super.CanReleaseCargo(cargo);
        bool diagnosticReleaseFinalResult = diagnosticReleaseCVCResult && diagnosticReleaseSuperResult;
        if (CVCContainerDiagnostics.IsPortableTarget(this))
        {
            string diagnosticCargoType = "null";
            if (cargo)
                diagnosticCargoType = cargo.GetType();
            string diagnosticReleaseDetail = "cargo=" + diagnosticCargoType;
            diagnosticReleaseDetail += " cvc_result=" + diagnosticReleaseCVCResult.ToString();
            diagnosticReleaseDetail += " super_result=" + diagnosticReleaseSuperResult.ToString();
            diagnosticReleaseDetail += " final=" + diagnosticReleaseFinalResult.ToString();
            diagnosticReleaseDetail += " server_open=" + IsOpen().ToString();
            diagnosticReleaseDetail += " " + CVCContainerDiagnostics.Hierarchy(this);
            CVCContainerDiagnostics.TracePortable(this, "ITEMBASE_CAN_RELEASE_" + diagnosticCargoType, diagnosticReleaseDetail, 250);
        }
        return diagnosticReleaseFinalResult;
    }

    override void EECargoIn(EntityAI item)
    {
        super.EECargoIn(item);
        if (GetGame().IsServer())
            CVCContainerService.NotePhysicalCargoChange(this);
    }

    override void EECargoOut(EntityAI item)
    {
        super.EECargoOut(item);
        if (GetGame().IsServer())
            CVCContainerService.NotePhysicalCargoChange(this);
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
        {
            // The hierarchy move is already known locally before the server's synced
            // Clippy flags arrive. Clear stale world-provider UI state immediately so
            // normal held-item placement and inventory actions are not hidden.
            if (!CVCContainerPolicy.IsWorldProviderLocation(this))
                m_CVCActionState = CVCContainerService.ACTION_NONE;
            return;
        }
        CVCContainerDiagnostics.Trace(this, "LOCATION_CHANGED", old_owner, new_owner);
        CVCSettings settings = CVCSettingsManager.Get();
        if (!CVCContainerPolicy.IsConfiguredContainerClass(this) && !settings.ReportUnlistedStorageCandidates)
            return;
        if (!CVCContainerPolicy.IsWorldProviderLocation(this))
        {
            CVCSetActionState(CVCContainerService.ACTION_NONE);
            if (CVCContainerPolicy.IsPhysicalInteractionLocation(this))
            {
                CVCContainerService.SettleAfterHierarchyMove(this);
                return;
            }
            if (CVCContainerService.HasUnsettledWorkflow(this))
            {
                CVCContainerService.SettleAfterHierarchyMove(this);
                return;
            }
            CVCContainerService.Unregister(this);
            CVCMigrationService.UnregisterCandidate(this);
            return;
        }
        CVCContainerService.HandleWorldHierarchyMove(this);
        // A drop, placement, trader transfer, and failed attachment can emit more
        // than one location callback. Recheck the final hierarchy after it settles.
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
            CVCMigrationService.ScheduleCandidate(this);
    }

    override void AfterStoreLoad()
    {
        super.AfterStoreLoad();
        if (GetGame().IsServer())
            CVCMigrationService.ScheduleCandidate(this);
    }


    override bool CanReceiveItemIntoCargo(EntityAI item)
    {
        if (GetGame().IsServer())
        {
            if (!CVCContainerService.AllowsPhysicalCargo(this))
                return false;
        }
        return super.CanReceiveItemIntoCargo(item);
    }

    override bool CanReleaseCargo(EntityAI cargo)
    {
        if (GetGame().IsServer() && !CVCContainerService.AllowsPhysicalCargo(this))
            return false;
        return super.CanReleaseCargo(cargo);
    }

    override void EECargoIn(EntityAI item)
    {
        super.EECargoIn(item);
        if (GetGame().IsServer())
            CVCContainerService.NotePhysicalCargoChange(this);
    }

    override void EECargoOut(EntityAI item)
    {
        super.EECargoOut(item);
        if (GetGame().IsServer())
            CVCContainerService.NotePhysicalCargoChange(this);
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
    // 4_World cannot reference classes declared by the later 5_Mission module.
    // Store the server reply here and let the mission layer consume it on the client.
    protected int m_CVCInventoryOpenResponse;

    int CVCConsumeInventoryOpenResponse()
    {
        int response = m_CVCInventoryOpenResponse;
        m_CVCInventoryOpenResponse = 0;
        return response;
    }

    override void OnRPC(PlayerIdentity sender, int rpc_type, ParamsReadContext ctx)
    {
        super.OnRPC(sender, rpc_type, ctx);
        if (rpc_type == CVCRPC.OPEN_INVENTORY && GetGame().IsClient())
        {
            Param1<string> openData;
            if (ctx.Read(openData))
            {
                string diagnosticRPCReply = "[CVC-DIAG] t_ms=" + GetGame().GetTime().ToString();
                diagnosticRPCReply += " realm=client event=INVENTORY_OPEN_RPC_REPLY value=";
                diagnosticRPCReply += openData.param1;
                Print(diagnosticRPCReply);
                if (openData.param1 == "native")
                    m_CVCInventoryOpenResponse = 1;
                else
                    m_CVCInventoryOpenResponse = -1;
            }
            else
            {
                string diagnosticRPCReadFailure = "[CVC-DIAG] t_ms=" + GetGame().GetTime().ToString();
                diagnosticRPCReadFailure += " realm=client event=INVENTORY_OPEN_RPC_REPLY value=read_failed";
                Print(diagnosticRPCReadFailure);
                m_CVCInventoryOpenResponse = -1;
            }
        }
        else if (rpc_type == CVCRPC.INVENTORY_OPEN && GetGame().IsServer())
        {
            string diagnosticRPCReceipt = "[CVC-DIAG] t_ms=" + GetGame().GetTime().ToString();
            diagnosticRPCReceipt += " realm=server event=INVENTORY_OPEN_RPC_RECEIVED";
            diagnosticRPCReceipt += " sender_present=" + (sender != null).ToString();
            diagnosticRPCReceipt += " player_identity_present=" + (GetIdentity() != null).ToString();
            diagnosticRPCReceipt += " alive=" + IsAlive().ToString();
            Print(diagnosticRPCReceipt);
            CVCContainerService.InventoryOpened(this);
        }
        else if (rpc_type == CVCRPC.CLOSE_INVENTORY && GetGame().IsServer())
        {
            CVCContainerService.CloseForPlayer(this);
        }
    }
}
