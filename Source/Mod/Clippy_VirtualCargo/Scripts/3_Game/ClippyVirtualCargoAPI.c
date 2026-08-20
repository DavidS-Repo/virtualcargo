class CVCSettings
{
    int Version = 7;
    bool Enabled = true;
    string HostURL = "http://127.0.0.1:27815";
    string ApiToken = "COPY_API_TOKEN_FROM_ClippyStorageHost.json";
    int ConnectionTimeoutSeconds = 5;
    int RequestTimeoutSeconds = 10;
    string ProviderID = "clippy.dayz.virtual-cargo";
    int VirtualRootCapacity = 10000;
    int NativePageSize = 20;
    int MaterializationIntervalMs = 1;
    float AccessDistanceMetres = 2.0;
    bool AutoOpenInventory = true;
    bool RejectContaminatedItems = true;
    bool RejectActiveOrPluggedItems = true;
    bool AutoDiscoverCargoContainers = true;
    bool IncludeInheritedContainerClasses = true;
    bool EnableVehicleCargo = true;
    bool EnableExistingCargoMigration = false;
    int MigrationStartDelaySeconds = 5;
    int MigrationBatchRootLimit = 20;
    int MigrationConcurrentContainers = 2;
    int MigrationScanBatchSize = 8;
    bool ReportUnlistedStorageCandidates = false;
    bool ReportEmptyUnlistedStorageCandidates = false;
    int MinimumUnlistedPhysicalRoots = 1;
    bool EnablePlayerTelemetry = false;
    bool EnablePlayerNetworkTelemetry = true;
    bool EnablePlayerPositionTelemetry = true;
    int PlayerSnapshotIntervalSeconds = 120;
    bool EnableLivePlayerControl = false;
    int PlayerCommandPollIntervalSeconds = 2;
    int PlayerCommandExpirySeconds = 30;
    ref array<string> ContainerClassNames;
    ref array<string> ExcludedContainerClassNames;
    ref array<string> BlockedItemClassNames;

    void CVCSettings()
    {
        ContainerClassNames = {};
        ExcludedContainerClassNames = {"ClippyVirtualCargoQuarantine", "FireplaceBase"};
        BlockedItemClassNames = {"ExplosivesBase", "TrapBase"};
    }
}

class CVCRPC
{
    static const int OPEN_INVENTORY = 834061;
    static const int CLOSE_INVENTORY = 834062;
    static const int INVENTORY_OPEN = 834063;
}

class CVCSettingsManager
{
    static const string DIRECTORY = "$profile:ClippyVirtualCargo";
    static const string FILE = "$profile:ClippyVirtualCargo/Settings.json";
    protected static ref CVCSettings s_Settings;

    static CVCSettings Get()
    {
        if (!s_Settings)
            s_Settings = new CVCSettings;
        return s_Settings;
    }

    static bool LoadServer()
    {
        MakeDirectory(DIRECTORY);
        if (!FileExist(FILE))
        {
            s_Settings = new CVCSettings;
            string saveError;
            JsonFileLoader<CVCSettings>.SaveFile(FILE, s_Settings, saveError);
            ErrorEx("[Clippy Virtual Cargo] Created Settings.json. Copy apiToken from ClippyStorageHost.json into ApiToken before enabling virtual cargo.");
            return false;
        }

        CVCSettings loaded;
        string loadError;
        if (!JsonFileLoader<CVCSettings>.LoadFile(FILE, loaded, loadError))
        {
            ErrorEx("[Clippy Virtual Cargo] " + loadError);
            return false;
        }
        s_Settings = loaded;
        if (!s_Settings.Enabled)
        {
            Print("[Clippy Virtual Cargo] Disabled in Settings.json.");
            return false;
        }
        if (s_Settings.ApiToken == "" || s_Settings.ApiToken.IndexOf("COPY_API_TOKEN") == 0)
        {
            ErrorEx("[Clippy Virtual Cargo] ApiToken is not configured. Service remains disabled.");
            return false;
        }
        return true;
    }
}

class CVCErrorData
{
    string code;
    string message;
    bool retryable;
}

class CVCResponseHandler
{
    void OnSuccess(string raw) {}
    void OnFailure(string reason) {}
}

class CVCRequestBase
{
    string api_token;
    string request_id;
}

class CVCListRequest: CVCRequestBase
{
    string provider_id;
    int limit = 5000;
}

class CVCResolveRequest: CVCRequestBase
{
    string provider_id;
    string provider_key;
    string display_name;
    int capacity_slots;
    string container_class;
    float world_position_x;
    float world_position_y;
    float world_position_z;
    string map_name;
}

class CVCContainerObservationRequest: CVCRequestBase
{
    string storage_id;
    string display_name;
    string container_class;
    float world_position_x;
    float world_position_y;
    float world_position_z;
    string map_name;
}

class CVCSnapshotRequest: CVCRequestBase
{
    string storage_id;
    string cursor = "";
    int limit = 100;
}

class CVCItemTreeRequest: CVCRequestBase
{
    string storage_id;
    string item_id;
}

class CVCOperationRequest: CVCRequestBase
{
    string operation_id;
}

class CVCSessionOpenRequest: CVCRequestBase
{
    string storage_id;
    int expected_revision;
    string idempotency_key;
    string player_id;
    string cursor;
    int limit = 20;
}

class CVCSessionRequest: CVCRequestBase
{
    string session_id;
}

class CVCSessionMaterializedRequest: CVCSessionRequest
{
    ref array<string> root_ids = new array<string>;
    ref array<string> physical_source_keys = new array<string>;
}

class CVCSessionCleanupRequest: CVCSessionRequest
{
    ref array<string> cleaned_source_keys = new array<string>;
}

class CVCAbortRequest: CVCOperationRequest
{
    string reason;
}

class CVCItemLocation
{
    string kind = "cargo";
    int slot = -1;
    int index = -1;
    int row = -1;
    int col = -1;
    bool flip = false;
}

class CVCItemAdapter
{
    string id = "vanilla.generic";
    int version = 1;
}

class CVCHealthZoneState
{
    string name;
    float health;
}

class CVCCartridgeState
{
    float damage;
    string ammo_type;
}

class CVCWeaponMuzzleState
{
    int mode;
    bool jammed;
    ref CVCCartridgeState chamber;
    ref array<ref CVCCartridgeState> internal_magazine = new array<ref CVCCartridgeState>;
}

class CVCItemState
{
    bool has_quantity;
    float wetness;
    float temperature;
    int liquid_type;
    int food_stage = -1;
    int agents;
    bool has_energy;
    float energy;
    bool switched_on;
    ref array<ref CVCHealthZoneState> health_zones = new array<ref CVCHealthZoneState>;
    ref array<ref CVCCartridgeState> magazine_cartridges = new array<ref CVCCartridgeState>;
    int current_muzzle = -1;
    ref array<ref CVCWeaponMuzzleState> weapon_muzzles = new array<ref CVCWeaponMuzzleState>;
    string cvc_provider_key;
    string custom_json;
}

class CVCItemNode
{
    string item_id;
    string class_name;
    float quantity;
    float health;
    ref CVCItemLocation location = new CVCItemLocation;
    ref CVCItemAdapter adapter = new CVCItemAdapter;
    ref CVCItemState state = new CVCItemState;
    ref array<ref CVCItemNode> children = new array<ref CVCItemNode>;
}

class CVCPlayerCommandPayload
{
    string item_id;
    string class_name;
    string target_player_id;
    float health = 1.0;
    float quantity = -1.0;
    string quarantine_id;
    ref CVCItemNode item_tree;
}

class CVCPlayerCommandResult
{
    string item_id;
    string created_item_id;
    string target_player_id;
    string quarantine_id;
    ref CVCItemNode item_tree;
}

class CVCPlayerProfileTelemetry
{
    string plain_name;
    string full_name;
    int session_player_id = -1;
}

class CVCPlayerNetworkTelemetry
{
    bool available = false;
    int ping_act_ms = 0;
    int ping_min_ms = 0;
    int ping_max_ms = 0;
    int ping_avg_ms = 0;
    int bandwidth_min_kbps = 0;
    int bandwidth_max_kbps = 0;
    int bandwidth_avg_kbps = 0;
    float output_throttle = 0.0;
}

class CVCPlayerPositionTelemetry
{
    bool available = false;
    string map_name;
    float world_position_x = 0.0;
    float world_position_y = 0.0;
    float world_position_z = 0.0;
}

class CVCPlayerSnapshotRequest: CVCRequestBase
{
    string player_id;
    string display_name;
    ref CVCPlayerProfileTelemetry profile = new CVCPlayerProfileTelemetry;
    ref CVCPlayerNetworkTelemetry network = new CVCPlayerNetworkTelemetry;
    ref CVCPlayerPositionTelemetry position = new CVCPlayerPositionTelemetry;
    ref array<ref CVCItemNode> inventory = new array<ref CVCItemNode>;
    ref map<string,string> equipment = new map<string,string>;
}

class CVCPlayerCommandPollRequest: CVCRequestBase
{
    string player_id;
    int limit = 4;
}

class CVCPlayerCommandCompleteRequest: CVCRequestBase
{
    string command_id;
    string player_id;
    string status;
    string result_json;
    string error;
}

class CVCPlayerCommandData
{
    string command_id;
    string action;
    string payload_json;
}

class CVCPlayerCommandListData
{
    string player_id;
    ref array<ref CVCPlayerCommandData> commands = new array<ref CVCPlayerCommandData>;
}

class CVCPlayerCommandListEnvelope
{
    bool ok;
    ref CVCPlayerCommandListData data;
    ref CVCErrorData error;
}

class CVCDepositPrepareRequest: CVCRequestBase
{
    string storage_id;
    int expected_revision;
    string idempotency_key;
    ref CVCItemNode item;
}

class CVCWithdrawPrepareRequest: CVCRequestBase
{
    string storage_id;
    int expected_revision;
    string idempotency_key;
    string item_id;
}

class CVCResolveData
{
    string storage_id;
    string provider_id;
    string provider_key;
    string display_name;
    int capacity_slots;
    int revision;
}

class CVCResolveEnvelope
{
    bool ok;
    string protocol;
    string request_id;
    ref CVCResolveData data;
    ref CVCErrorData error;
}

class CVCItemSummary
{
    string item_id;
    string class_name;
    float quantity;
    float health;
}

class CVCSnapshotData
{
    string storage_id;
    int revision;
    int total_roots;
    string next_cursor;
    ref array<ref CVCItemSummary> items;
}

class CVCSnapshotEnvelope
{
    bool ok;
    ref CVCSnapshotData data;
    ref CVCErrorData error;
}

class CVCOperationData
{
    string operation_id;
    string kind;
    string status;
    string storage_id;
    string root_item_id;
    int expected_revision;
    int revision;
    ref CVCItemNode item;
}

class CVCOperationEnvelope
{
    bool ok;
    ref CVCOperationData data;
    ref CVCErrorData error;
}

class CVCSessionData
{
    string session_id;
    string storage_id;
    string provider_id;
    string provider_key;
    string player_id;
    string status;
    int expected_revision;
    int result_revision;
    string cursor;
    string next_cursor;
    ref array<string> original_root_ids;
    ref array<string> materialized_root_ids;
    ref array<string> physical_source_keys;
    ref array<string> cleanup_source_keys;
    ref array<ref CVCItemNode> items;
}

class CVCSessionEnvelope
{
    bool ok;
    ref CVCSessionData data;
    ref CVCErrorData error;
}

class CVCSessionCommitRequest: CVCSessionRequest
{
    ref array<ref CVCItemNode> items = new array<ref CVCItemNode>;
    ref array<string> physical_source_keys = new array<string>;
}

class CVCMigrationRootRequest
{
    string source_key;
    ref CVCItemNode item;
}

class CVCMigrationPrepareRequest: CVCRequestBase
{
    string storage_id;
    int expected_revision;
    string container_class;
    ref array<ref CVCMigrationRootRequest> roots = new array<ref CVCMigrationRootRequest>;
}

class CVCMigrationRequest: CVCRequestBase
{
    string migration_id;
}

class CVCMigrationCleanupRequest: CVCMigrationRequest
{
    ref array<string> cleaned_source_keys = new array<string>;
}

class CVCMigrationObservationRequest: CVCRequestBase
{
    string provider_id;
    string provider_key;
    string container_class;
    string status;
    int physical_roots;
    int captured_roots;
    int rejected_roots;
    string detail;
}

class CVCMigrationSourceData
{
    string source_key;
    string virtual_root_id;
    bool cleaned;
}

class CVCMigrationData
{
    string migration_id;
    string storage_id;
    string provider_key;
    string container_class;
    string status;
    string source_fingerprint;
    int expected_revision;
    int result_revision;
    ref array<ref CVCMigrationSourceData> source_roots;
}

class CVCMigrationEnvelope
{
    bool ok;
    ref CVCMigrationData data;
    ref CVCErrorData error;
}

class CVCIncompleteMigrationData
{
    ref array<ref CVCMigrationData> migrations;
}

class CVCIncompleteMigrationEnvelope
{
    bool ok;
    ref CVCIncompleteMigrationData data;
    ref CVCErrorData error;
}

class CVCInternalRestCallback: RestCallback
{
    protected ref CVCResponseHandler m_Handler;

    void CVCInternalRestCallback(CVCResponseHandler handler)
    {
        m_Handler = handler;
    }

    override void OnSuccess(string data, int dataSize)
    {
        if (m_Handler)
            m_Handler.OnSuccess(data);
    }

    override void OnError(int errorCode)
    {
        if (m_Handler)
            m_Handler.OnFailure("REST error " + errorCode.ToString());
    }

    override void OnTimeout()
    {
        if (m_Handler)
            m_Handler.OnFailure("REST request timed out");
    }
}

class ClippyVirtualCargoAPI
{
    protected static bool s_Ready;
    protected static RestContext s_Context;

    static bool InitializeServer()
    {
        s_Ready = false;
        if (!CVCSettingsManager.LoadServer())
            return false;

        RestApi api = GetRestApi();
        if (!api)
            api = CreateRestApi();
        if (!api)
        {
            ErrorEx("[Clippy Virtual Cargo] DayZ REST API could not be created.");
            return false;
        }

        CVCSettings settings = CVCSettingsManager.Get();
        api.SetOption(ERestOption.ERESTOPTION_CONNECTION, Math.Clamp(settings.ConnectionTimeoutSeconds, 3, 120));
        api.SetOption(ERestOption.ERESTOPTION_READOPERATION, Math.Clamp(settings.RequestTimeoutSeconds, 3, 120));
        s_Context = api.GetRestContext(settings.HostURL);
        if (!s_Context)
        {
            ErrorEx("[Clippy Virtual Cargo] Could not create REST context for " + settings.HostURL);
            return false;
        }
        s_Context.SetHeader("application/json");
        s_Ready = true;
        Print("[Clippy Virtual Cargo] Asynchronous host API initialized at " + settings.HostURL);
        return true;
    }

    static bool IsReady()
    {
        return s_Ready && s_Context;
    }

    static bool Post(string route, CVCRequestBase request, CVCResponseHandler handler)
    {
        if (!IsReady() || !request || !handler)
            return false;

        request.api_token = CVCSettingsManager.Get().ApiToken;
        request.request_id = string.Format("dayz-%1-%2", GetGame().GetTime(), Math.RandomInt(100000, 999999));
        string payload;
        JsonSerializer serializer = new JsonSerializer;
        bool serialized;
        CVCAbortRequest abortRequest = CVCAbortRequest.Cast(request);
        CVCListRequest listRequest = CVCListRequest.Cast(request);
        CVCDepositPrepareRequest depositRequest = CVCDepositPrepareRequest.Cast(request);
        CVCWithdrawPrepareRequest withdrawRequest = CVCWithdrawPrepareRequest.Cast(request);
        CVCSessionCommitRequest sessionCommitRequest = CVCSessionCommitRequest.Cast(request);
        CVCSessionCleanupRequest sessionCleanupRequest = CVCSessionCleanupRequest.Cast(request);
        CVCSessionMaterializedRequest materializedRequest = CVCSessionMaterializedRequest.Cast(request);
        CVCSessionOpenRequest sessionOpenRequest = CVCSessionOpenRequest.Cast(request);
        CVCSessionRequest sessionRequest = CVCSessionRequest.Cast(request);
        CVCMigrationCleanupRequest migrationCleanupRequest = CVCMigrationCleanupRequest.Cast(request);
        CVCMigrationPrepareRequest migrationPrepareRequest = CVCMigrationPrepareRequest.Cast(request);
        CVCMigrationObservationRequest observationRequest = CVCMigrationObservationRequest.Cast(request);
        CVCMigrationRequest migrationRequest = CVCMigrationRequest.Cast(request);
        CVCContainerObservationRequest containerObservationRequest = CVCContainerObservationRequest.Cast(request);
        CVCPlayerSnapshotRequest playerSnapshotRequest = CVCPlayerSnapshotRequest.Cast(request);
        CVCPlayerCommandPollRequest playerCommandPollRequest = CVCPlayerCommandPollRequest.Cast(request);
        CVCPlayerCommandCompleteRequest playerCommandCompleteRequest = CVCPlayerCommandCompleteRequest.Cast(request);
        CVCResolveRequest resolveRequest = CVCResolveRequest.Cast(request);
        CVCSnapshotRequest snapshotRequest = CVCSnapshotRequest.Cast(request);
        CVCItemTreeRequest itemTreeRequest = CVCItemTreeRequest.Cast(request);
        CVCOperationRequest operationRequest = CVCOperationRequest.Cast(request);
        if (abortRequest)
            serialized = serializer.WriteToString(abortRequest, false, payload);
        else if (listRequest)
            serialized = serializer.WriteToString(listRequest, false, payload);
        else if (depositRequest)
            serialized = serializer.WriteToString(depositRequest, false, payload);
        else if (withdrawRequest)
            serialized = serializer.WriteToString(withdrawRequest, false, payload);
        else if (sessionCommitRequest)
            serialized = serializer.WriteToString(sessionCommitRequest, false, payload);
        else if (sessionCleanupRequest)
            serialized = serializer.WriteToString(sessionCleanupRequest, false, payload);
        else if (materializedRequest)
            serialized = serializer.WriteToString(materializedRequest, false, payload);
        else if (sessionOpenRequest)
            serialized = serializer.WriteToString(sessionOpenRequest, false, payload);
        else if (sessionRequest)
            serialized = serializer.WriteToString(sessionRequest, false, payload);
        else if (migrationCleanupRequest)
            serialized = serializer.WriteToString(migrationCleanupRequest, false, payload);
        else if (migrationPrepareRequest)
            serialized = serializer.WriteToString(migrationPrepareRequest, false, payload);
        else if (observationRequest)
            serialized = serializer.WriteToString(observationRequest, false, payload);
        else if (migrationRequest)
            serialized = serializer.WriteToString(migrationRequest, false, payload);
        else if (containerObservationRequest)
            serialized = serializer.WriteToString(containerObservationRequest, false, payload);
        else if (playerSnapshotRequest)
            serialized = serializer.WriteToString(playerSnapshotRequest, false, payload);
        else if (playerCommandPollRequest)
            serialized = serializer.WriteToString(playerCommandPollRequest, false, payload);
        else if (playerCommandCompleteRequest)
            serialized = serializer.WriteToString(playerCommandCompleteRequest, false, payload);
        else if (resolveRequest)
            serialized = serializer.WriteToString(resolveRequest, false, payload);
        else if (snapshotRequest)
            serialized = serializer.WriteToString(snapshotRequest, false, payload);
        else if (itemTreeRequest)
            serialized = serializer.WriteToString(itemTreeRequest, false, payload);
        else if (operationRequest)
            serialized = serializer.WriteToString(operationRequest, false, payload);
        else
            serialized = serializer.WriteToString(request, false, payload);
        if (!serialized)
        {
            handler.OnFailure("Could not serialize request");
            return false;
        }

        CVCInternalRestCallback callback = new CVCInternalRestCallback(handler);
        int state = s_Context.POST(callback, route, payload);
        if (state >= ERestResultState.EREST_ERROR)
        {
            handler.OnFailure("REST request was rejected before dispatch");
            return false;
        }
        return true;
    }

    static bool ParseOperation(string raw, out CVCOperationEnvelope response, out string error)
    {
        response = new CVCOperationEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        return serializer.ReadFromString(response, raw, error);
    }

    static bool ParseSession(string raw, out CVCSessionEnvelope response, out string error)
    {
        response = new CVCSessionEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        return serializer.ReadFromString(response, raw, error);
    }

    static bool ParseMigration(string raw, out CVCMigrationEnvelope response, out string error)
    {
        response = new CVCMigrationEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        return serializer.ReadFromString(response, raw, error);
    }
}
