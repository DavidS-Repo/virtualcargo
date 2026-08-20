class CVCStartupHandler: CVCResponseHandler
{
    protected int m_Attempt;

    void CVCStartupHandler(int attempt)
    {
        m_Attempt = attempt;
    }

    override void OnSuccess(string raw)
    {
        Print("[Clippy Virtual Cargo] Storage host health check passed.");
        CVCRecoveryCoordinator.Start();
    }

    override void OnFailure(string reason)
    {
        CVCStartupCoordinator.Retry(m_Attempt, reason);
    }
}

class CVCStartupCoordinator
{
    static void Check(int attempt)
    {
        CVCRequestBase health = new CVCRequestBase;
        if (!ClippyVirtualCargoAPI.Post("/v1/health", health, new CVCStartupHandler(attempt)))
            Retry(attempt, "health request could not be dispatched");
    }

    static void Retry(int failedAttempt, string reason)
    {
        int nextAttempt = Math.Clamp(failedAttempt + 1, 1, 15);
        int delay = Math.Clamp(failedAttempt * 2000, 2000, 30000);
        ErrorEx(string.Format("[Clippy Virtual Cargo] Storage host health check attempt %1 failed (%2); retrying in %3 ms. Cargo remains fail-closed.", failedAttempt, reason, delay));
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(Check, delay, false, nextAttempt);
    }
}

class CVCRecoveryCoordinator
{
    protected static bool s_Started;
    protected static bool s_Ready;

    static void Start()
    {
        if (s_Started)
            return;
        s_Started = true;
        QueryOperations(1);
    }

    static int RetryDelay(int attempt)
    {
        return Math.Clamp(attempt * 2000, 2000, 30000);
    }

    static int NextAttempt(int attempt)
    {
        return Math.Clamp(attempt + 1, 1, 15);
    }

    static void QueryOperations(int attempt)
    {
        CVCListRequest request = new CVCListRequest;
        request.provider_id = CVCSettingsManager.Get().ProviderID;
        if (!ClippyVirtualCargoAPI.Post("/v1/operation/incomplete", request, new CVCIncompleteHandler(attempt)))
            RetryOperations(attempt, "request could not be dispatched");
    }

    static void RetryOperations(int attempt, string reason)
    {
        int delay = RetryDelay(attempt);
        ErrorEx(string.Format("[Clippy Virtual Cargo] Interrupted-operation recovery is blocked (%1). Retrying in %2 ms; migration remains disabled.", reason, delay));
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(QueryOperations, delay, false, NextAttempt(attempt));
    }

    static void QuerySessions(int attempt)
    {
        CVCListRequest request = new CVCListRequest;
        request.provider_id = CVCSettingsManager.Get().ProviderID;
        if (!ClippyVirtualCargoAPI.Post("/v1/session/incomplete", request, new CVCIncompleteSessionHandler(attempt)))
            RetrySessions(attempt, "request could not be dispatched");
    }

    static void RetrySessions(int attempt, string reason)
    {
        int delay = RetryDelay(attempt);
        ErrorEx(string.Format("[Clippy Virtual Cargo] Native-session recovery query failed (%1). Retrying in %2 ms; migration remains disabled.", reason, delay));
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(QuerySessions, delay, false, NextAttempt(attempt));
    }

    static void QueryMigrations(int attempt)
    {
        CVCListRequest request = new CVCListRequest;
        request.provider_id = CVCSettingsManager.Get().ProviderID;
        if (!ClippyVirtualCargoAPI.Post("/v1/migration/incomplete", request, new CVCIncompleteMigrationHandler(attempt)))
            RetryMigrations(attempt, "request could not be dispatched");
    }

    static void RetryMigrations(int attempt, string reason)
    {
        int delay = RetryDelay(attempt);
        ErrorEx(string.Format("[Clippy Virtual Cargo] Existing-cargo recovery query failed (%1). Retrying in %2 ms; migration remains disabled.", reason, delay));
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(QueryMigrations, delay, false, NextAttempt(attempt));
    }

    static void WaitForSessions(int attempt)
    {
        if (s_Ready)
            return;
        if (CVCContainerService.RecoverySettled())
        {
            s_Ready = true;
            Print("[Clippy Virtual Cargo] Recovery barrier passed. Automatic per-container SQL activation is starting.");
            CVCMigrationService.Start();
            return;
        }

        if (attempt == 1 || (attempt % 15) == 0)
            ErrorEx("[Clippy Virtual Cargo] Waiting for materialized cargo sessions to recover. New migration remains fail-closed.");
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(WaitForSessions, 2000, false, attempt + 1);
    }
}

class CVCIncompleteOperation
{
    string operation_id;
    string kind;
    string status;
    string storage_id;
    string root_item_id;
}

class CVCIncompleteData
{
    ref array<ref CVCIncompleteOperation> operations;
}

class CVCIncompleteEnvelope
{
    bool ok;
    ref CVCIncompleteData data;
    ref CVCErrorData error;
}

class CVCIncompleteHandler: CVCResponseHandler
{
    protected int m_Attempt;
    void CVCIncompleteHandler(int attempt) { m_Attempt = attempt; }

    override void OnSuccess(string raw)
    {
        CVCIncompleteEnvelope response = new CVCIncompleteEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            CVCRecoveryCoordinator.RetryOperations(m_Attempt, "invalid response: " + parseError);
            return;
        }

        if (response.data.operations && response.data.operations.Count() > 0)
        {
            ErrorEx(string.Format("[Clippy Virtual Cargo] %1 legacy direct-operation transaction(s) require manual reconciliation. Automatic migration will not start.", response.data.operations.Count()));
            foreach (CVCIncompleteOperation operation : response.data.operations)
                ErrorEx(string.Format("[Clippy Virtual Cargo] Pending %1 %2 is %3 for storage %4.", operation.kind, operation.operation_id, operation.status, operation.storage_id));
            CVCRecoveryCoordinator.RetryOperations(m_Attempt, "legacy direct operations are still incomplete");
            return;
        }

        Print("[Clippy Virtual Cargo] Interrupted-operation recovery check passed.");
        CVCRecoveryCoordinator.QuerySessions(1);
    }

    override void OnFailure(string reason)
    {
        CVCRecoveryCoordinator.RetryOperations(m_Attempt, reason);
    }
}

class CVCIncompleteSessionData
{
    ref array<ref CVCSessionData> sessions;
}

class CVCIncompleteSessionEnvelope
{
    bool ok;
    ref CVCIncompleteSessionData data;
    ref CVCErrorData error;
}

class CVCIncompleteSessionHandler: CVCResponseHandler
{
    protected int m_Attempt;
    void CVCIncompleteSessionHandler(int attempt) { m_Attempt = attempt; }

    override void OnSuccess(string raw)
    {
        CVCIncompleteSessionEnvelope response = new CVCIncompleteSessionEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            CVCRecoveryCoordinator.RetrySessions(m_Attempt, "invalid response: " + parseError);
            return;
        }

        int sessionCount;
        if (response.data.sessions)
        {
            sessionCount = response.data.sessions.Count();
            foreach (CVCSessionData session : response.data.sessions)
                CVCContainerService.QueueRecovery(session);
        }
        Print(string.Format("[Clippy Virtual Cargo] Queued %1 interrupted native cargo session(s) for recovery.", sessionCount));
        CVCRecoveryCoordinator.QueryMigrations(1);
    }

    override void OnFailure(string reason)
    {
        CVCRecoveryCoordinator.RetrySessions(m_Attempt, reason);
    }
}

class CVCIncompleteMigrationHandler: CVCResponseHandler
{
    protected int m_Attempt;
    void CVCIncompleteMigrationHandler(int attempt) { m_Attempt = attempt; }

    override void OnSuccess(string raw)
    {
        CVCIncompleteMigrationEnvelope response = new CVCIncompleteMigrationEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            CVCRecoveryCoordinator.RetryMigrations(m_Attempt, "invalid response: " + parseError);
            return;
        }

        int migrationCount;
        if (response.data.migrations)
        {
            migrationCount = response.data.migrations.Count();
            foreach (CVCMigrationData migration : response.data.migrations)
                CVCMigrationService.QueueRecovery(migration);
        }
        Print(string.Format("[Clippy Virtual Cargo] Queued %1 existing-cargo migration(s) for recovery.", migrationCount));
        CVCRecoveryCoordinator.WaitForSessions(1);
    }

    override void OnFailure(string reason)
    {
        CVCRecoveryCoordinator.RetryMigrations(m_Attempt, reason);
    }
}


class CVCPlayerSnapshotHandler: CVCResponseHandler
{
    protected string m_PlayerID;

    void CVCPlayerSnapshotHandler(string playerID)
    {
        m_PlayerID = playerID;
    }

    override void OnSuccess(string raw) {}

    override void OnFailure(string reason)
    {
        ErrorEx("[Clippy Virtual Cargo] Player telemetry snapshot failed for " + m_PlayerID + ": " + reason);
    }
}

class CVCPlayerCommandCompleteHandler: CVCResponseHandler
{
    protected string m_CommandID;

    void CVCPlayerCommandCompleteHandler(string commandID)
    {
        m_CommandID = commandID;
    }

    override void OnSuccess(string raw) {}

    override void OnFailure(string reason)
    {
        ErrorEx("[Clippy Virtual Cargo] Live admin command result could not be recorded for " + m_CommandID + ": " + reason);
    }
}

class CVCPlayerCommandPollHandler: CVCResponseHandler
{
    protected string m_PlayerID;

    void CVCPlayerCommandPollHandler(string playerID)
    {
        m_PlayerID = playerID;
    }

    override void OnSuccess(string raw)
    {
        CVCPlayerCommandListEnvelope response = new CVCPlayerCommandListEnvelope;
        JsonSerializer serializer = new JsonSerializer;
        string parseError;
        if (!serializer.ReadFromString(response, raw, parseError) || !response.ok || !response.data)
        {
            ErrorEx("[Clippy Virtual Cargo] Invalid player command response for " + m_PlayerID + ": " + parseError);
            return;
        }
        if (!response.data.commands)
            return;
        foreach (CVCPlayerCommandData command : response.data.commands)
        {
            if (command)
                CVCPlayerTelemetry.ExecuteCommand(m_PlayerID, command);
        }
    }

    override void OnFailure(string reason)
    {
        ErrorEx("[Clippy Virtual Cargo] Live player command poll failed for " + m_PlayerID + ": " + reason);
    }
}

class CVCPlayerTelemetry
{
    protected static ref map<string,int> s_LastSnapshot = new map<string,int>;
    protected static ref map<string,int> s_LastCommandPoll = new map<string,int>;
    protected static bool s_Started;

    static void Start()
    {
        if (s_Started || !GetGame().IsServer())
            return;
        s_Started = true;
        if (!CVCSettingsManager.Get().EnablePlayerTelemetry)
        {
            Print("[Clippy Virtual Cargo] Optional player telemetry is disabled.");
            return;
        }
        Print("[Clippy Virtual Cargo] Optional player telemetry enabled.");
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(Tick, 1000, true);
    }

    static PlayerBase FindPlayer(string playerID)
    {
        if (playerID == "")
            return null;
        ref array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        foreach (Man man : players)
        {
            PlayerBase player = PlayerBase.Cast(man);
            if (player && player.GetIdentity() && player.GetIdentity().GetId() == playerID)
                return player;
        }
        return null;
    }

    static ItemBase FindLiveItem(PlayerBase player, string itemID)
    {
        if (!player || itemID == "")
            return null;
        ref array<EntityAI> entities = new array<EntityAI>;
        player.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, entities);
        foreach (EntityAI entity : entities)
        {
            ItemBase item = ItemBase.Cast(entity);
            if (item && item.CVCGetLiveItemID() == itemID)
                return item;
        }
        return null;
    }

    static void BuildSnapshot(PlayerBase player, CVCPlayerSnapshotRequest request)
    {
        if (!player || !request || !player.GetIdentity())
            return;
        PlayerIdentity identity = player.GetIdentity();
        request.player_id = identity.GetId();
        request.display_name = identity.GetName();
        request.profile.plain_name = identity.GetPlainName();
        request.profile.full_name = identity.GetFullName();
        request.profile.session_player_id = identity.GetPlayerId();

        CVCSettings settings = CVCSettingsManager.Get();
        if (settings.EnablePlayerNetworkTelemetry)
        {
            request.network.available = true;
            request.network.ping_act_ms = Math.Max(identity.GetPingAct(), 0);
            request.network.ping_min_ms = Math.Max(identity.GetPingMin(), 0);
            request.network.ping_max_ms = Math.Max(identity.GetPingMax(), 0);
            request.network.ping_avg_ms = Math.Max(identity.GetPingAvg(), 0);
            request.network.bandwidth_min_kbps = Math.Max(identity.GetBandwidthMin(), 0);
            request.network.bandwidth_max_kbps = Math.Max(identity.GetBandwidthMax(), 0);
            request.network.bandwidth_avg_kbps = Math.Max(identity.GetBandwidthAvg(), 0);
            request.network.output_throttle = Math.Clamp(identity.GetOutputThrottle(), 0.0, 1.0);
        }

        if (settings.EnablePlayerPositionTelemetry)
        {
            vector playerPosition = player.GetPosition();
            request.position.available = true;
            request.position.map_name = GetGame().GetWorldName();
            request.position.world_position_x = playerPosition[0];
            request.position.world_position_y = playerPosition[1];
            request.position.world_position_z = playerPosition[2];
        }

        ref array<EntityAI> entities = new array<EntityAI>;
        player.GetInventory().EnumerateInventory(InventoryTraversalType.PREORDER, entities);
        foreach (EntityAI entity : entities)
        {
            ItemBase item = ItemBase.Cast(entity);
            if (!item || item.GetHierarchyParent() != player)
                continue;
            CVCItemNode node = CVCItemTreeCodec.CaptureLive(item);
            if (node)
                request.inventory.Insert(node);
        }

        EntityAI hands = player.GetItemInHands();
        if (hands)
            request.equipment.Set("hands", hands.GetType());
        request.equipment.Set("root_items", request.inventory.Count().ToString());
    }

    static void SnapshotPlayer(PlayerBase player)
    {
        if (!player || !player.GetIdentity() || !CVCSettingsManager.Get().EnablePlayerTelemetry)
            return;
        CVCPlayerSnapshotRequest request = new CVCPlayerSnapshotRequest;
        BuildSnapshot(player, request);
        if (request.player_id == "")
            return;
        ClippyVirtualCargoAPI.Post("/v1/player/snapshot", request, new CVCPlayerSnapshotHandler(request.player_id));
        s_LastSnapshot.Set(request.player_id, GetGame().GetTime());
    }

    static void PollCommands(PlayerBase player)
    {
        if (!player || !player.GetIdentity() || !CVCSettingsManager.Get().EnableLivePlayerControl)
            return;
        CVCPlayerCommandPollRequest request = new CVCPlayerCommandPollRequest;
        request.player_id = player.GetIdentity().GetId();
        request.limit = 4;
        if (ClippyVirtualCargoAPI.Post("/v1/player/commands/poll", request, new CVCPlayerCommandPollHandler(request.player_id)))
            s_LastCommandPoll.Set(request.player_id, GetGame().GetTime());
    }

    static void Complete(string playerID, string commandID, bool success, CVCPlayerCommandResult result, string error)
    {
        CVCPlayerCommandCompleteRequest request = new CVCPlayerCommandCompleteRequest;
        request.player_id = playerID;
        request.command_id = commandID;
        if (success)
            request.status = "SUCCEEDED";
        else
            request.status = "FAILED";
        request.error = error;
        if (result)
        {
            JsonSerializer serializer = new JsonSerializer;
            string resultJson;
            if (serializer.WriteToString(result, false, resultJson))
                request.result_json = resultJson;
        }
        ClippyVirtualCargoAPI.Post("/v1/player/commands/complete", request, new CVCPlayerCommandCompleteHandler(commandID));
    }

    static bool ParsePayload(CVCPlayerCommandData command, out CVCPlayerCommandPayload payload, out string error)
    {
        payload = new CVCPlayerCommandPayload;
        if (!command)
        {
            error = "command is missing";
            return false;
        }
        if (command.payload_json == "")
            return true;
        JsonSerializer serializer = new JsonSerializer;
        return serializer.ReadFromString(payload, command.payload_json, error);
    }

    static void ExecuteCommand(string playerID, CVCPlayerCommandData command)
    {
        PlayerBase player = FindPlayer(playerID);
        if (!player)
        {
            Complete(playerID, command.command_id, false, null, "Player is no longer online.");
            return;
        }

        CVCPlayerCommandPayload payload;
        string parseError;
        if (!ParsePayload(command, payload, parseError))
        {
            Complete(playerID, command.command_id, false, null, "Invalid command payload: " + parseError);
            return;
        }

        CVCPlayerCommandResult result = new CVCPlayerCommandResult;
        string failure;

        if (command.action == "REQUEST_SNAPSHOT")
        {
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        if (command.action == "GIVE_ITEM")
        {
            if (!payload || payload.class_name == "")
            {
                Complete(playerID, command.command_id, false, null, "class_name is required.");
                return;
            }
            ItemBase created = ItemBase.Cast(player.GetInventory().CreateInInventory(payload.class_name));
            if (!created)
            {
                Complete(playerID, command.command_id, false, null, "DayZ could not place the requested item in the player inventory.");
                return;
            }
            if (payload.quantity >= 0 && created.HasQuantity())
                created.SetQuantity(payload.quantity);
            created.SetHealth01("", "Health", Math.Clamp(payload.health, 0.0, 1.0));
            result.created_item_id = created.CVCGetLiveItemID();
            result.item_id = result.created_item_id;
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        if (command.action == "RESTORE_QUARANTINE")
        {
            if (!payload || !payload.item_tree)
            {
                Complete(playerID, command.command_id, false, null, "The quarantine item tree is missing.");
                return;
            }
            ItemBase restored = CVCItemTreeCodec.RestoreLiveRoot(payload.item_tree, player, failure);
            if (!restored)
            {
                Complete(playerID, command.command_id, false, null, failure);
                return;
            }
            result.item_id = restored.CVCGetLiveItemID();
            result.quarantine_id = payload.quarantine_id;
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        ItemBase item = FindLiveItem(player, payload.item_id);
        if (!item)
        {
            Complete(playerID, command.command_id, false, null, "The requested live item is no longer in this player's inventory.");
            return;
        }
        if (item.CVCGetVirtualItemID() != "")
        {
            Complete(playerID, command.command_id, false, null, "This item belongs to an active virtual-cargo materialization and cannot be changed by live player controls.");
            return;
        }

        if (command.action == "REPAIR_ITEM")
        {
            item.SetHealth01("", "Health", Math.Clamp(payload.health, 0.0, 1.0));
            result.item_id = item.CVCGetLiveItemID();
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        if (command.action == "REMOVE_ITEM")
        {
            result.item_id = item.CVCGetLiveItemID();
            item.DeleteSafe();
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        if (command.action == "QUARANTINE_ITEM")
        {
            result.item_tree = CVCItemTreeCodec.CaptureLive(item);
            if (!result.item_tree)
            {
                Complete(playerID, command.command_id, false, null, "The live item could not be serialized for quarantine.");
                return;
            }
            result.item_id = result.item_tree.item_id;
            item.DeleteSafe();
            SnapshotPlayer(player);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        if (command.action == "MOVE_ITEM")
        {
            PlayerBase target = FindPlayer(payload.target_player_id);
            if (!target)
            {
                Complete(playerID, command.command_id, false, null, "The target player is not online.");
                return;
            }
            CVCItemNode tree = CVCItemTreeCodec.CaptureLive(item);
            if (!tree)
            {
                Complete(playerID, command.command_id, false, null, "The live item could not be serialized before moving it.");
                return;
            }
            ItemBase moved = CVCItemTreeCodec.RestoreLiveRoot(tree, target, failure);
            if (!moved)
            {
                Complete(playerID, command.command_id, false, null, failure);
                return;
            }
            item.DeleteSafe();
            result.item_id = moved.CVCGetLiveItemID();
            result.target_player_id = payload.target_player_id;
            SnapshotPlayer(player);
            SnapshotPlayer(target);
            Complete(playerID, command.command_id, true, result, "");
            return;
        }

        Complete(playerID, command.command_id, false, null, "Unsupported live player command.");
    }

    static void Tick()
    {
        if (!GetGame().IsServer() || !CVCSettingsManager.Get().EnablePlayerTelemetry)
            return;

        int nowMs = GetGame().GetTime();
        int snapshotInterval = Math.Clamp(CVCSettingsManager.Get().PlayerSnapshotIntervalSeconds, 30, 3600) * 1000;
        int commandInterval = Math.Clamp(CVCSettingsManager.Get().PlayerCommandPollIntervalSeconds, 1, 60) * 1000;

        ref array<Man> players = new array<Man>;
        GetGame().GetPlayers(players);
        foreach (Man man : players)
        {
            PlayerBase player = PlayerBase.Cast(man);
            if (!player || !player.GetIdentity())
                continue;
            string playerID = player.GetIdentity().GetId();
            int lastSnapshot;
            if (!s_LastSnapshot.Find(playerID, lastSnapshot) || nowMs - lastSnapshot >= snapshotInterval)
                SnapshotPlayer(player);
            if (CVCSettingsManager.Get().EnableLivePlayerControl)
            {
                int lastPoll;
                if (!s_LastCommandPoll.Find(playerID, lastPoll) || nowMs - lastPoll >= commandInterval)
                    PollCommands(player);
            }
        }
    }
}

modded class MissionServer
{
    void MissionServer()
    {
        if (ClippyVirtualCargoAPI.InitializeServer())
        {
            CVCContainerService.EnableEnforcement();
            CVCPlayerTelemetry.Start();
            Print("[Clippy Virtual Cargo] Physical cargo enforcement enabled before host recovery.");
            CVCStartupCoordinator.Check(1);
        }
    }

    override void OnInit()
    {
        super.OnInit();
        GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(CVCContainerService.Tick, 2000, true);
    }
}

modded class MissionGameplay
{
    protected bool m_CVCInventoryOpenApproved;
    protected bool m_CVCInventoryOpenRequested;

    void CVCCancelInventoryOpen()
    {
        m_CVCInventoryOpenRequested = false;
    }

    void CVCShowInventoryApproved()
    {
        m_CVCInventoryOpenRequested = false;
        if (GetGame().GetUIManager().FindMenu(MENU_INVENTORY))
            return;
        m_CVCInventoryOpenApproved = true;
        ShowInventory();
        m_CVCInventoryOpenApproved = false;
    }

    override void OnUpdate(float timeslice)
    {
        super.OnUpdate(timeslice);

        PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
        if (!player)
            return;

        int response = player.CVCConsumeInventoryOpenResponse();
        if (response > 0)
            CVCShowInventoryApproved();
        else if (response < 0)
            CVCCancelInventoryOpen();
    }

    override void ShowInventory()
    {
        if (m_CVCInventoryOpenApproved)
        {
            super.ShowInventory();
            return;
        }

        PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
        if (!player || m_CVCInventoryOpenRequested)
            return;

        m_CVCInventoryOpenRequested = true;
        player.RPCSingleParam(CVCRPC.INVENTORY_OPEN, new Param1<int>(1), true, null);
    }

    override void HideInventory()
    {
        m_CVCInventoryOpenRequested = false;
        PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
        if (player)
            player.RPCSingleParam(CVCRPC.CLOSE_INVENTORY, new Param1<int>(1), true, null);
        super.HideInventory();
    }
}
