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

modded class MissionServer
{
    void MissionServer()
    {
        if (ClippyVirtualCargoAPI.InitializeServer())
        {
            CVCContainerService.EnableEnforcement();
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
    override void HideInventory()
    {
        PlayerBase player = PlayerBase.Cast(GetGame().GetPlayer());
        if (player)
            player.RPCSingleParam(CVCRPC.CLOSE_INVENTORY, new Param1<int>(1), true, null);
        super.HideInventory();
    }
}
