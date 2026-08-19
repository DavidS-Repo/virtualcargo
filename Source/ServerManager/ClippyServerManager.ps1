param(
    [Parameter(Position=0,Mandatory=$false)][string]$Command = '',
    [Parameter(Position=1,Mandatory=$false)][string]$Value = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$managerPowerShell = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$managerRoot = if ($env:CLIPPY_MANAGER_ROOT) { [IO.Path]::GetFullPath($env:CLIPPY_MANAGER_ROOT).TrimEnd('\') } else { [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\') }
$managerScript = if ($env:CLIPPY_MANAGER_SCRIPT) { [IO.Path]::GetFullPath($env:CLIPPY_MANAGER_SCRIPT) } else { [IO.Path]::GetFullPath((Join-Path $managerRoot 'START-CLIPPY-SERVER.bat')) }
$rawCommand = if ($Command) { $Command } elseif ($env:CLIPPY_MANAGER_COMMAND) { $env:CLIPPY_MANAGER_COMMAND } else { 'start' }
$command = $rawCommand.ToLowerInvariant()
$managerRevision = 20
Write-Host "Clippy Server Manager revision $managerRevision" -ForegroundColor Cyan
$modName = '@Clippy SQLite Virtual Cargo'
$workshopId = '3782296362'

function Write-TextAtomic {
    param([string]$Path, [string]$Text)
    $full = [IO.Path]::GetFullPath($Path)
    $directory = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $temporary = Join-Path $directory ('.' + [IO.Path]::GetFileName($full) + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $stream = New-Object IO.FileStream($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $writer = New-Object IO.StreamWriter($stream, (New-Object Text.UTF8Encoding($false)))
            try {
                $writer.Write($Text)
                $writer.Flush()
                $stream.Flush($true)
            } finally {
                $writer.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
        if (Test-Path -LiteralPath $full -PathType Leaf) {
            $replaceBackup = Join-Path $directory ('.' + [IO.Path]::GetFileName($full) + '.' + [Guid]::NewGuid().ToString('N') + '.replace-backup')
            try {
                [IO.File]::Replace($temporary, $full, $replaceBackup, $true)
            } finally {
                if (Test-Path -LiteralPath $replaceBackup -PathType Leaf) { Remove-Item -LiteralPath $replaceBackup -Force }
            }
        } else {
            [IO.File]::Move($temporary, $full)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Write-JsonAtomic {
    param([string]$Path, [object]$Document, [int]$Depth = 50)
    Write-TextAtomic -Path $Path -Text (($Document | ConvertTo-Json -Depth $Depth) + [Environment]::NewLine)
}

function New-RandomToken {
    $bytes = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return -join ($bytes | ForEach-Object { $_.ToString('x2') })
}

function Get-DirectorySourceHash {
    param([string]$Root)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (-not (Test-Path -LiteralPath $fullRoot -PathType Container)) { throw "Source folder is missing: $fullRoot" }
    $records = New-Object System.Collections.Generic.List[string]
    foreach ($file in Get-ChildItem -LiteralPath $fullRoot -Recurse -File | Sort-Object FullName) {
        $relative = $file.FullName.Substring($fullRoot.Length).TrimStart('\').Replace('\','/').ToLowerInvariant()
        $records.Add($relative + ':' + (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($records -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('X2') }) } finally { $sha.Dispose() }
}

function New-WindowsServicePassword {
    # Prefix supplies upper/lower/digit/symbol classes for normal Windows password policy.
    # The random body carries 256 bits of entropy and contains no shell metacharacters.
    return 'Cvc!A1' + (New-RandomToken)
}

function Resolve-AbsolutePath {
    param([string]$Value, [string]$Base)
    $expanded = [Environment]::ExpandEnvironmentVariables($Value).Trim()
    if ([IO.Path]::IsPathRooted($expanded)) { return [IO.Path]::GetFullPath($expanded) }
    return [IO.Path]::GetFullPath((Join-Path $Base $expanded))
}

function Resolve-PostgresDataDirectory {
    param([string]$Value, [string]$Base)
    $expanded = [Environment]::ExpandEnvironmentVariables($Value).Trim()
    if (-not $expanded) {
        $expanded = '%ProgramData%\ClippyVirtualCargo\PostgreSQL\data'
        $expanded = [Environment]::ExpandEnvironmentVariables($expanded)
    }
    $candidate = if ([IO.Path]::IsPathRooted($expanded)) {
        [IO.Path]::GetFullPath($expanded)
    } else {
        [IO.Path]::GetFullPath((Join-Path $Base $expanded))
    }

    # A complete existing cluster is left where it is. New or incomplete clusters
    # are moved out of Program Files because initdb must control the data ACLs.
    if (Test-Path -LiteralPath (Join-Path $candidate 'PG_VERSION') -PathType Leaf) { return $candidate }

    $protectedRoots = New-Object System.Collections.Generic.List[string]
    foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if (-not $root) { continue }
        try {
            $fullRoot = [IO.Path]::GetFullPath($root).TrimEnd('\')
            if (-not ($protectedRoots | Where-Object { $_.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase) })) {
                $protectedRoots.Add($fullRoot)
            }
        } catch { }
    }
    foreach ($root in $protectedRoots) {
        $prefix = $root + '\'
        if ($candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            $programData = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)
            if (-not $programData) { $programData = $env:ProgramData }
            if (-not $programData) { throw 'Windows ProgramData folder could not be resolved.' }
            $safe = [IO.Path]::GetFullPath((Join-Path $programData 'ClippyVirtualCargo\PostgreSQL\data'))
            Write-Warning "PostgreSQL data cannot be initialized safely under Program Files. Using $safe instead."
            return $safe
        }
    }
    return $candidate
}

function Assert-UnderRoot {
    param([string]$Path, [string]$Root)
    $full = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $prefix = $fullRoot + '\'
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe path outside the expected root: $full"
    }
    return $full
}

function Read-JsonFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing JSON file: $Path" }
    try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { throw "Invalid JSON in $Path`: $($_.Exception.Message)" }
}

function Get-BuiltInConfiguration {
    return @'
{
  "ConfigVersion": 6,
  "ServerRoot": "Auto",
  "ServerExecutable": "DayZServer_x64.exe",
  "BaseStartScript": "StartServer.bat",
  "WorkshopUpdater": { "Enabled": false, "Script": "Update-DayZMods.ps1" },
  "DayZLaunch": {
    "ArgumentsOverride": "",
    "DefaultArguments": "-config=serverDZ.cfg -port=2302 -profiles=profiles -dologs -adminlog -netlog -freezecheck",
    "ServerConfig": "", "ServerPort": 0, "SteamPort": 0, "SteamQueryPort": 0,
    "ProfilesDirectory": "", "CpuCount": 0, "LimitFPS": 0,
    "AdditionalMods": [], "AdditionalArguments": []
  },
  "StorageHostSettings": {
    "BindAddress": "127.0.0.1", "Port": 27815, "ApiToken": "",
    "BackupDirectory": "backups", "HttpThreads": 16, "MaxQueuedRequests": 1024,
    "PostgresHost": "127.0.0.1", "PostgresPort": 27816,
    "PostgresDatabase": "clippy_virtual_cargo", "PostgresUser": "clippy_virtual_cargo",
    "PostgresPoolSize": 16, "PostgresConnectTimeoutSeconds": 5,
    "PostgresStatementTimeoutMs": 10000, "PostgresLockTimeoutMs": 3000,
    "PostgresIdleTransactionTimeoutMs": 15000,
    "MaxBackupFiles": 10, "TerminalRetentionDays": 30,
    "PlayerTelemetryRetentionDays": 30, "PlayerSnapshotHistoryLimit": 250,
    "AdminAuditRetentionDays": 90,
    "MaintenancePruneBatchRows": 500, "MaintenanceIntervalSeconds": 300,
    "MaxRequestBytes": 2097152, "MaxItemNodes": 4096,
    "MaxPageNodes": 256, "MaxItemDepth": 16
  },
  "PostgreSQL": {
    "Version": "18.4-1", "Port": 27816, "ServiceName": "ClippyPostgreSQL18",
    "InstallDirectory": "ClippyPostgreSQL/18", "DataDirectory": "%ProgramData%/ClippyVirtualCargo/PostgreSQL/data",
    "InstallerUrl": "https://get.enterprisedb.com/postgresql/postgresql-18.4-1-windows-x64.exe",
    "InstallerSHA256": "44B8187D2DB7E866495952D8260A1D7252CBB5125843142E1F0BF30115D23279"
  },
  "VirtualCargoSettings": {
    "Version": 7, "Enabled": true, "ConnectionTimeoutSeconds": 5,
    "RequestTimeoutSeconds": 10, "ProviderID": "clippy.dayz.virtual-cargo",
    "VirtualRootCapacity": 10000, "NativePageSize": 20, "MaterializationIntervalMs": 1, "AccessDistanceMetres": 2.0,
    "AutoOpenInventory": true, "RejectContaminatedItems": true,
    "RejectActiveOrPluggedItems": true, "AutoDiscoverCargoContainers": true,
    "IncludeInheritedContainerClasses": true, "EnableVehicleCargo": true,
    "EnableExistingCargoMigration": false,
    "MigrationStartDelaySeconds": 5,
    "MigrationBatchRootLimit": 20, "MigrationConcurrentContainers": 2,
    "MigrationScanBatchSize": 8,
    "ReportUnlistedStorageCandidates": false, "ReportEmptyUnlistedStorageCandidates": false,
    "MinimumUnlistedPhysicalRoots": 1,
    "ContainerClassNames": [],
    "ExcludedContainerClassNames": ["ClippyVirtualCargoQuarantine", "FireplaceBase"],
    "BlockedItemClassNames": ["ExplosivesBase", "TrapBase"]
  },
  "AdminPanel": {
    "Enabled": true, "Port": 27817, "AutoOpenBrowser": true, "IdleShutdownMinutes": 30,
    "HttpThreads": 8, "MaxQueuedRequests": 256, "MaxRequestBytes": 65536,
    "PostgresPoolSize": 4, "PostgresConnectTimeoutSeconds": 3,
    "PostgresStatementTimeoutMs": 3000, "PostgresLockTimeoutMs": 500,
    "PostgresIdleTransactionTimeoutMs": 5000, "EnableEditing": true,
    "MaintenanceLockSeconds": 300, "PostgresWritePoolSize": 2,
    "PostgresWriteStatementTimeoutMs": 5000, "PostgresWriteLockTimeoutMs": 1500,
    "EnablePlayerTelemetry": false, "EnablePlayerNetworkTelemetry": true,
    "EnablePlayerPositionTelemetry": true, "PlayerSnapshotIntervalSeconds": 120,
    "PlayerTelemetryRetentionDays": 30, "PlayerSnapshotHistoryLimit": 250,
    "AdminAuditRetentionDays": 90,
    "EnableLivePlayerControl": false, "PlayerCommandPollIntervalSeconds": 2,
    "PlayerCommandExpirySeconds": 30
  },
  "Persistence": {
    "Path": "Auto", "MissionTemplate": "", "InstanceId": 0,
    "ColdBackupDirectory": "ClippyStorageHost/migration-backups"
  },
  "Management": {
    "InstallOrUpdatePayloadOnStart": true, "RunIntegrityCheck": true,
    "BackupBeforeServerStart": true, "BackupAfterServerStops": true,
    "StopHostAfterServerStops": true, "RestartHostOnFailure": true,
    "RestartServerOnCrash": false, "MaximumAutomaticRestarts": 3,
    "RestartDelaySeconds": 10, "HealthTimeoutSeconds": 20,
    "HeartbeatSeconds": 30, "LogDirectory": "ClippyServerManagerLogs"
  }
}
'@ | ConvertFrom-Json
}

function Test-JsonObject {
    param([object]$Value)
    return $null -ne $Value -and $Value -is [pscustomobject]
}

function Merge-JsonObject {
    param([object]$Defaults, [object]$Overrides)
    if (-not (Test-JsonObject $Defaults) -or -not (Test-JsonObject $Overrides)) { return $Overrides }
    $result = [ordered]@{}
    foreach ($property in $Defaults.PSObject.Properties) {
        $overrideProperty = $Overrides.PSObject.Properties[$property.Name]
        if ($overrideProperty) {
            if ((Test-JsonObject $property.Value) -and (Test-JsonObject $overrideProperty.Value)) {
                $result[$property.Name] = Merge-JsonObject -Defaults $property.Value -Overrides $overrideProperty.Value
            } else {
                $result[$property.Name] = $overrideProperty.Value
            }
        } else {
            $result[$property.Name] = $property.Value
        }
    }
    foreach ($property in $Overrides.PSObject.Properties) {
        if (-not $result.Contains($property.Name)) { $result[$property.Name] = $property.Value }
    }
    return [pscustomobject]$result
}

function Resolve-ServerRootValue {
    param([string]$Value, [string]$AutoBase = $managerRoot)
    $expanded = [Environment]::ExpandEnvironmentVariables($Value).Trim()
    if (-not $expanded -or $expanded -eq '.' -or $expanded.Equals('Auto', [StringComparison]::OrdinalIgnoreCase)) {
        $candidate = $AutoBase
    } elseif ([IO.Path]::IsPathRooted($expanded)) {
        $candidate = $expanded
    } else {
        $candidate = Join-Path $AutoBase $expanded
    }
    return [IO.Path]::GetFullPath($candidate).TrimEnd('\')
}

function Get-InitialConfiguration {
    $bootstrap = Join-Path $managerRoot 'ClippyServerManager.json'
    $example = Join-Path $managerRoot 'ClippyServerManager.example.json'
    $defaults = Get-BuiltInConfiguration
    $initial = if (Test-Path -LiteralPath $bootstrap -PathType Leaf) {
        Merge-JsonObject -Defaults $defaults -Overrides (Read-JsonFile -Path $bootstrap)
    } elseif (Test-Path -LiteralPath $example -PathType Leaf) {
        Merge-JsonObject -Defaults $defaults -Overrides (Read-JsonFile -Path $example)
    } else {
        $defaults
    }
    $initialServerRoot = Resolve-ServerRootValue -Value $initial.ServerRoot.ToString()
    $installed = Join-Path $initialServerRoot 'ClippyServerManager.json'
    if (-not $managerRoot.Equals($initialServerRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $installed -PathType Leaf)) {
        $initial = Merge-JsonObject -Defaults $defaults -Overrides (Read-JsonFile -Path $installed)
        $initialServerRoot = Resolve-ServerRootValue -Value $initial.ServerRoot.ToString() -AutoBase (Split-Path -Parent $installed)
        if (-not $initialServerRoot.Equals((Split-Path -Parent $installed), [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The installed ClippyServerManager.json points at a different ServerRoot.'
        }
        $sourcePath = $installed
    } else {
        $sourcePath = $bootstrap
    }
    $initial.ConfigVersion = 6
    $initial.VirtualCargoSettings.Version = 6
    if ($initial.VirtualCargoSettings.PSObject.Properties['ExistingCargoMigrationMode']) {
        $initial.VirtualCargoSettings.PSObject.Properties.Remove('ExistingCargoMigrationMode')
    }
    if ($initial.VirtualCargoSettings.PSObject.Properties['MigrationOnlyWhenServerEmpty']) {
        $initial.VirtualCargoSettings.PSObject.Properties.Remove('MigrationOnlyWhenServerEmpty')
    }
    foreach ($obsolete in @('DatabasePath','Durability','ReadConnections','BusyTimeoutMs','CheckpointIntervalSeconds','OptimizeIntervalSeconds')) {
        if ($initial.StorageHostSettings.PSObject.Properties[$obsolete]) {
            $initial.StorageHostSettings.PSObject.Properties.Remove($obsolete)
        }
    }
    return @{ Path = $sourcePath; Data = $initial; ServerRoot = $initialServerRoot }
}

$configurationResult = Get-InitialConfiguration
$configPath = $configurationResult.Path
$config = $configurationResult.Data
$serverRoot = $configurationResult.ServerRoot
$serverExe = Resolve-AbsolutePath -Value $config.ServerExecutable.ToString() -Base $serverRoot
$baseStartScript = $null
if ($config.BaseStartScript -and $config.BaseStartScript.ToString().Trim()) {
    $baseStartScript = Resolve-AbsolutePath -Value $config.BaseStartScript.ToString() -Base $serverRoot
}
$payloadRoot = $managerRoot
$stagedPayload = Join-Path $managerRoot 'ClippyVirtualCargoPayload'
$builtPayload = Join-Path $managerRoot 'Built-Server-Package'
if (Test-Path -LiteralPath (Join-Path $stagedPayload $modName) -PathType Container) {
    # Release archives keep new binaries in a staging folder. This lets users
    # extract an update without overwriting their live host configuration.
    $payloadRoot = $stagedPayload
} elseif (Test-Path -LiteralPath (Join-Path $builtPayload $modName) -PathType Container) {
    $payloadRoot = $builtPayload
}
$payloadMod = Join-Path $payloadRoot $modName
$payloadHost = Join-Path $payloadRoot 'ClippyStorageHost'
$payloadAdmin = Join-Path $payloadRoot 'ClippyAdmin'
$payloadSettings = Join-Path $payloadRoot 'ServerProfileTemplate\ClippyVirtualCargo\Settings.json'
$installedMod = Join-Path $serverRoot $modName
$installedHost = Join-Path $serverRoot 'ClippyStorageHost'
$installedHostExe = Join-Path $installedHost 'ClippyStorageHost.exe'
$installedHostConfig = Join-Path $installedHost 'ClippyStorageHost.json'
$installedAdmin = Join-Path $serverRoot 'ClippyAdmin'
$installedAdminExe = Join-Path $installedAdmin 'ClippyAdminHost.exe'
$installedAdminConfig = Join-Path $installedAdmin 'ClippyAdminHost.json'
$installedAdminLauncher = Join-Path $serverRoot 'OPEN-CLIPPY-ADMIN.bat'
$managerAdminLauncher = Join-Path $managerRoot 'OPEN-CLIPPY-ADMIN.bat'
$installedManager = Join-Path $serverRoot 'START-CLIPPY-SERVER.bat'
$installedManagerPowerShell = Join-Path $serverRoot 'ClippyServerManager.ps1'
$installedManagerConfig = Join-Path $serverRoot 'ClippyServerManager.json'
$installedManagerState = Join-Path $serverRoot 'ClippyServerManager.state.json'
$installedSecrets = Join-Path $installedHost '.clippy-secrets.json'
$postgresInstall = Resolve-AbsolutePath -Value $config.PostgreSQL.InstallDirectory.ToString() -Base $serverRoot
$postgresData = Resolve-PostgresDataDirectory -Value $config.PostgreSQL.DataDirectory.ToString() -Base $serverRoot
$postgresBin = Join-Path $postgresInstall 'bin'
$postgresPsql = Join-Path $postgresBin 'psql.exe'
$postgresCreatedb = Join-Path $postgresBin 'createdb.exe'
$postgresLibpq = Join-Path $postgresBin 'libpq.dll'
$postgresServiceName = $config.PostgreSQL.ServiceName.ToString()
$postgresPort = [int]$config.PostgreSQL.Port
$payloadHostExe = Join-Path $payloadHost 'ClippyStorageHost.exe'
$payloadAdminExe = Join-Path $payloadAdmin 'ClippyAdminHost.exe'
$adminReadRole = 'clippy_virtual_cargo_admin_read'
$adminEditRole = 'clippy_virtual_cargo_admin_edit'
$script:adminEditingAvailable = $false
$script:managerState = $null
$script:secrets = $null

function Protect-SensitiveFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    try {
        $acl = New-Object Security.AccessControl.FileSecurity
        $acl.SetAccessRuleProtection($true, $false)
        $rights = [Security.AccessControl.FileSystemRights]::FullControl
        $allow = [Security.AccessControl.AccessControlType]::Allow
        foreach ($identity in @(
            [Security.Principal.WindowsIdentity]::GetCurrent().User,
            (New-Object Security.Principal.SecurityIdentifier('S-1-5-18')),
            (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544'))
        )) {
            $rule = New-Object Security.AccessControl.FileSystemAccessRule($identity, $rights, $allow)
            $acl.AddAccessRule($rule) | Out-Null
        }
        Set-Acl -LiteralPath $Path -AclObject $acl
    } catch {
        throw "Could not lock down sensitive file $Path`: $($_.Exception.Message)"
    }
}

function Assert-SafePostgresName {
    param([string]$Value, [string]$Label)
    if ($Value -notmatch '^[a-z][a-z0-9_]{0,62}$') { throw "$Label must use only lowercase letters, digits, and underscores and must start with a letter." }
    return $Value
}

function Get-OrCreateSecrets {
    if ($script:secrets) { return $script:secrets }
    New-Item -ItemType Directory -Path $installedHost -Force | Out-Null
    $existing = $null
    if (Test-Path -LiteralPath $installedSecrets -PathType Leaf) {
        $existing = Read-JsonFile -Path $installedSecrets
    }
    $oldHost = $null
    if (Test-Path -LiteralPath $installedHostConfig -PathType Leaf) {
        try { $oldHost = Read-JsonFile -Path $installedHostConfig } catch { }
    }
    $apiToken = $config.StorageHostSettings.ApiToken.ToString()
    if (-not $apiToken -and $existing -and $existing.ApiToken) { $apiToken = $existing.ApiToken.ToString() }
    if (-not $apiToken -and $oldHost -and $oldHost.PSObject.Properties['apiToken'] -and $oldHost.apiToken) { $apiToken = $oldHost.apiToken.ToString() }
    if (-not $apiToken) { $apiToken = New-RandomToken }

    $appPassword = ''
    if ($existing -and $existing.PostgresAppPassword) { $appPassword = $existing.PostgresAppPassword.ToString() }
    if (-not $appPassword -and $oldHost -and $oldHost.PSObject.Properties['postgresPassword'] -and $oldHost.postgresPassword) { $appPassword = $oldHost.postgresPassword.ToString() }
    if (-not $appPassword) { $appPassword = New-RandomToken }

    $adminPassword = ''
    if ($existing -and $existing.PostgresAdminPassword) { $adminPassword = $existing.PostgresAdminPassword.ToString() }
    if (-not $adminPassword) { $adminPassword = New-WindowsServicePassword }

    $adminReadPassword = ''
    if ($existing -and $existing.PSObject.Properties['PostgresAdminReadPassword'] -and $existing.PostgresAdminReadPassword) { $adminReadPassword = $existing.PostgresAdminReadPassword.ToString() }
    if (-not $adminReadPassword) { $adminReadPassword = New-RandomToken }

    $adminEditPassword = ''
    if ($existing -and $existing.PSObject.Properties['PostgresAdminEditPassword'] -and $existing.PostgresAdminEditPassword) { $adminEditPassword = $existing.PostgresAdminEditPassword.ToString() }
    if (-not $adminEditPassword) { $adminEditPassword = New-RandomToken }

    foreach ($pair in @(@('API token',$apiToken), @('PostgreSQL application password',$appPassword), @('PostgreSQL administrator password',$adminPassword), @('PostgreSQL admin read password',$adminReadPassword), @('PostgreSQL admin edit password',$adminEditPassword))) {
        if ($pair[1].Length -lt 32 -or $pair[1].Length -gt 256) { throw "$($pair[0]) must contain 32 to 256 characters." }
    }
    $document = [pscustomobject][ordered]@{
        Version = 3
        ApiToken = $apiToken
        PostgresAppPassword = $appPassword
        PostgresAdminPassword = $adminPassword
        PostgresAdminReadPassword = $adminReadPassword
        PostgresAdminEditPassword = $adminEditPassword
    }
    Write-JsonAtomic -Path $installedSecrets -Document $document -Depth 10
    Protect-SensitiveFile -Path $installedSecrets
    $script:secrets = $document
    return $document
}

function Get-PostgresService {
    return Get-Service -Name $postgresServiceName -ErrorAction SilentlyContinue
}

function Grant-PostgresServiceAccess {
    $icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
    if (-not (Test-Path -LiteralPath $icacls -PathType Leaf)) { throw "icacls.exe is missing: $icacls" }
    foreach ($entry in @(
        @($postgresInstall, '*S-1-5-20:(OI)(CI)(RX)'),
        @($postgresData, '*S-1-5-20:(OI)(CI)(M)')
    )) {
        $path = $entry[0]
        $grant = $entry[1]
        if (-not (Test-Path -LiteralPath $path -PathType Container)) { throw "PostgreSQL path is missing while setting service permissions: $path" }
        & $icacls $path '/grant' $grant '/T' '/C' '/Q' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Could not grant the PostgreSQL service account access to $path. icacls exit code: $LASTEXITCODE" }
    }
}

function Initialize-PostgresDataDirectoryIfNeeded {
    $pgVersion = Join-Path $postgresData 'PG_VERSION'
    if (Test-Path -LiteralPath $pgVersion -PathType Leaf) { return }
    $initdb = Join-Path $postgresBin 'initdb.exe'
    if (-not (Test-Path -LiteralPath $initdb -PathType Leaf)) { throw "PostgreSQL initdb.exe is missing: $initdb" }

    if (Test-Path -LiteralPath $postgresData -PathType Container) {
        $entries = @(Get-ChildItem -LiteralPath $postgresData -Force -ErrorAction SilentlyContinue)
        if ($entries.Count -gt 0) {
            $preserved = $postgresData + '.incomplete-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
            Write-Warning "PostgreSQL data directory was incomplete. Preserving it at $preserved before creating a clean cluster."
            Move-Item -LiteralPath $postgresData -Destination $preserved
        }
    }
    $postgresParent = Split-Path -Parent $postgresData
    New-Item -ItemType Directory -Path $postgresParent -Force | Out-Null
    if (Test-Path -LiteralPath $postgresData -PathType Container) {
        $remaining = @(Get-ChildItem -LiteralPath $postgresData -Force -ErrorAction SilentlyContinue)
        if ($remaining.Count -eq 0) { Remove-Item -LiteralPath $postgresData -Force }
    }

    $passwordFile = Join-Path $installedHost ('.initdb-password-' + [Guid]::NewGuid().ToString('N') + '.txt')
    try {
        Write-TextAtomic -Path $passwordFile -Text ($script:secrets.PostgresAdminPassword.ToString() + [Environment]::NewLine)
        Protect-SensitiveFile -Path $passwordFile
        Write-Host 'Initializing the private PostgreSQL data directory...' -ForegroundColor Cyan
        & $initdb '--pgdata' $postgresData '--username' 'postgres' '--pwfile' $passwordFile '--auth-host' 'scram-sha-256' '--auth-local' 'scram-sha-256' '--encoding' 'UTF8'
        if ($LASTEXITCODE -ne 0) { throw "initdb failed with exit code $LASTEXITCODE." }
    } finally {
        if (Test-Path -LiteralPath $passwordFile -PathType Leaf) { Remove-Item -LiteralPath $passwordFile -Force }
    }
    if (-not (Test-Path -LiteralPath $pgVersion -PathType Leaf)) { throw 'PostgreSQL initdb completed without creating PG_VERSION.' }
}

function Register-ClippyPostgresService {
    $pgCtl = Join-Path $postgresBin 'pg_ctl.exe'
    if (-not (Test-Path -LiteralPath $pgCtl -PathType Leaf)) { throw "PostgreSQL pg_ctl.exe is missing: $pgCtl" }
    Initialize-PostgresDataDirectoryIfNeeded
    Grant-PostgresServiceAccess
    $networkServiceSid = New-Object Security.Principal.SecurityIdentifier('S-1-5-20')
    $networkService = $networkServiceSid.Translate([Security.Principal.NTAccount]).Value
    Write-Host "Registering PostgreSQL service '$postgresServiceName' with $networkService..." -ForegroundColor Cyan
    & $pgCtl 'register' '-N' $postgresServiceName '-D' $postgresData '-S' 'auto' '-U' $networkService '-o' ("-p $postgresPort")
    if ($LASTEXITCODE -ne 0) { throw "pg_ctl could not register PostgreSQL service '$postgresServiceName'. Exit code: $LASTEXITCODE" }
    Start-Sleep -Milliseconds 500
    Assert-PostgresServiceOwnership
    if (-not (Get-PostgresService)) { throw "pg_ctl reported success but PostgreSQL service '$postgresServiceName' is still missing." }
}

function Assert-PostgresServiceOwnership {
    $service = Get-CimInstance Win32_Service -Filter "Name='$postgresServiceName'" -ErrorAction SilentlyContinue
    if (-not $service) { return }
    $expected = [IO.Path]::GetFullPath((Join-Path $postgresBin 'pg_ctl.exe'))
    $pathText = $service.PathName.ToString()
    if ($pathText.IndexOf([IO.Path]::GetFullPath($postgresInstall), [StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "Windows service '$postgresServiceName' already exists but is not the private Clippy PostgreSQL instance under $postgresInstall."
    }
}

function Invoke-Psql {
    param(
        [string]$Database = 'postgres',
        [string]$Sql,
        [string]$Password,
        [switch]$TuplesOnly
    )
    if (-not (Test-Path -LiteralPath $postgresPsql -PathType Leaf)) { throw "psql.exe is missing: $postgresPsql" }
    $old = [Environment]::GetEnvironmentVariable('PGPASSWORD','Process')
    [Environment]::SetEnvironmentVariable('PGPASSWORD',$Password,'Process')
    try {
        $psqlArguments = @('-X','--no-psqlrc','--host','127.0.0.1','--port',$postgresPort.ToString(),'--username','postgres','--dbname',$Database,'--set','ON_ERROR_STOP=1')
        if ($TuplesOnly) { $psqlArguments += @('-A','-t') }
        $psqlArguments += @('-c',$Sql)
        $output = & $postgresPsql @psqlArguments 2>&1
        $exit = $LASTEXITCODE
        if ($exit -ne 0) { throw "psql failed with exit code $exit`: $($output -join [Environment]::NewLine)" }
        return @($output)
    } finally {
        [Environment]::SetEnvironmentVariable('PGPASSWORD',$old,'Process')
    }
}

function Invoke-PostgresTool {
    param(
        [string]$Tool,
        [string[]]$Arguments,
        [string]$Password
    )
    if (-not (Test-Path -LiteralPath $Tool -PathType Leaf)) { throw "PostgreSQL tool is missing: $Tool" }
    $old = [Environment]::GetEnvironmentVariable('PGPASSWORD','Process')
    [Environment]::SetEnvironmentVariable('PGPASSWORD',$Password,'Process')
    try {
        $output = & $Tool @Arguments 2>&1
        $exit = $LASTEXITCODE
        if ($exit -ne 0) { throw "$([IO.Path]::GetFileName($Tool)) failed with exit code $exit`: $($output -join [Environment]::NewLine)" }
        return @($output)
    } finally {
        [Environment]::SetEnvironmentVariable('PGPASSWORD',$old,'Process')
    }
}

function Restart-ClippyPostgresService {
    $service = Get-PostgresService
    if (-not $service) { throw "PostgreSQL service '$postgresServiceName' is missing." }
    if ($service.Status -ne 'Stopped') {
        Stop-Service -Name $postgresServiceName -Force
        (Get-Service -Name $postgresServiceName).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    }
    Start-Service -Name $postgresServiceName
    (Get-Service -Name $postgresServiceName).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
}

function Set-PrivatePostgresConfiguration {
    $postgresqlConf = Join-Path $postgresData 'postgresql.conf'
    $hba = Join-Path $postgresData 'pg_hba.conf'
    if (-not (Test-Path -LiteralPath $postgresqlConf -PathType Leaf) -or -not (Test-Path -LiteralPath $hba -PathType Leaf)) { throw "PostgreSQL data directory is incomplete: $postgresData" }
    $changed = $false
    $text = Get-Content -LiteralPath $postgresqlConf -Raw
    $begin = '# BEGIN CLIPPY MANAGED SETTINGS'
    $end = '# END CLIPPY MANAGED SETTINGS'
    $baseText = [regex]::Replace($text, '(?ms)^# BEGIN CLIPPY MANAGED SETTINGS.*?^# END CLIPPY MANAGED SETTINGS\s*', '').TrimEnd()
    $managed = @"
$begin
listen_addresses = '127.0.0.1'
port = $postgresPort
password_encryption = 'scram-sha-256'
max_connections = 64
fsync = on
synchronous_commit = on
full_page_writes = on
checkpoint_timeout = '15min'
checkpoint_completion_target = 0.9
max_wal_size = '2GB'
min_wal_size = '256MB'
autovacuum = on
autovacuum_naptime = '10s'
autovacuum_max_workers = 4
$end
"@
    $desiredConf = $baseText + [Environment]::NewLine + [Environment]::NewLine + $managed.TrimEnd() + [Environment]::NewLine
    if ($text.Replace("`r`n","`n") -ne $desiredConf.Replace("`r`n","`n")) {
        Write-TextAtomic -Path $postgresqlConf -Text $desiredConf
        $changed = $true
    }

    $hbaText = @"
# Clippy Virtual Cargo private PostgreSQL instance.
# PostgreSQL listens on 127.0.0.1 only. No remote database access is allowed.
host all all 127.0.0.1/32 scram-sha-256
host all all ::1/128 scram-sha-256
"@
    $hbaText = $hbaText.TrimStart() + [Environment]::NewLine
    $currentHba = Get-Content -LiteralPath $hba -Raw
    if ($currentHba.Replace("`r`n","`n") -ne $hbaText.Replace("`r`n","`n")) {
        $hbaBackup = $hba + '.before-clippy-hardening'
        if (-not (Test-Path -LiteralPath $hbaBackup -PathType Leaf)) { Copy-Item -LiteralPath $hba -Destination $hbaBackup -Force }
        Write-TextAtomic -Path $hba -Text $hbaText
        $changed = $true
    }
    if ($changed) { Restart-ClippyPostgresService }
}


function Ensure-PostgreSQLInstalled {
    $secrets = Get-OrCreateSecrets
    Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresDatabase.ToString() -Label 'PostgresDatabase' | Out-Null
    Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresUser.ToString() -Label 'PostgresUser' | Out-Null
    if ($postgresServiceName -notmatch '^[A-Za-z0-9_.-]{1,80}$') { throw 'PostgreSQL.ServiceName contains unsafe characters.' }
    if ($postgresPort -lt 1024 -or $postgresPort -gt 65535) { throw 'PostgreSQL.Port must be between 1024 and 65535.' }
    if ([int]$config.StorageHostSettings.PostgresPort -ne $postgresPort) { throw 'StorageHostSettings.PostgresPort must match PostgreSQL.Port.' }

    Assert-PostgresServiceOwnership
    $service = Get-PostgresService
    if (-not $service) {
        $pgCtlCandidate = Join-Path $postgresBin 'pg_ctl.exe'
        if ((Test-Path -LiteralPath $pgCtlCandidate -PathType Leaf) -and (Test-Path -LiteralPath $postgresInstall -PathType Container)) {
            Write-Warning "PostgreSQL files are present but service '$postgresServiceName' is missing. Repairing the partial installation."
            Register-ClippyPostgresService
            $service = Get-PostgresService
        }
    }
    if (-not $service) {
        if (Test-Path -LiteralPath $postgresInstall) {
            $entries = @(Get-ChildItem -LiteralPath $postgresInstall -Force -ErrorAction SilentlyContinue)
            if ($entries.Count -gt 0) { throw "PostgreSQL install directory is incomplete and cannot be repaired automatically: $postgresInstall" }
        }
        $cache = Join-Path $serverRoot 'ClippySetupCache'
        New-Item -ItemType Directory -Path $cache -Force | Out-Null
        $installer = Join-Path $cache ('postgresql-' + $config.PostgreSQL.Version.ToString() + '-windows-x64.exe')
        $expectedHash = $config.PostgreSQL.InstallerSHA256.ToString().ToUpperInvariant()
        if (-not (Test-Path -LiteralPath $installer -PathType Leaf) -or (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $expectedHash) {
            if (Test-Path -LiteralPath $installer -PathType Leaf) { Remove-Item -LiteralPath $installer -Force }
            Write-Host "Downloading PostgreSQL $($config.PostgreSQL.Version)..." -ForegroundColor Cyan
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -UseBasicParsing -Uri $config.PostgreSQL.InstallerUrl.ToString() -OutFile $installer
        }
        if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $expectedHash) {
            throw 'PostgreSQL installer SHA-256 verification failed. The installer was not executed.'
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $postgresData) -Force | Out-Null
        $installArgs = @(
            '--mode','unattended','--unattendedmodeui','none',
            '--prefix',$postgresInstall,'--datadir',$postgresData,
            '--serverport',$postgresPort.ToString(),'--servicename',$postgresServiceName,
            '--superaccount','postgres','--superpassword',$secrets.PostgresAdminPassword.ToString(),
            '--enable_acledit','1',
            '--enable-components','server,commandlinetools',
            '--create_shortcuts','0'
        )
        Write-Host 'Installing the private PostgreSQL instance...' -ForegroundColor Cyan
        & $installer @installArgs
        if ($LASTEXITCODE -ne 0) { throw "PostgreSQL installer failed with exit code $LASTEXITCODE." }
        Assert-PostgresServiceOwnership
        if (-not (Get-PostgresService)) {
            Write-Warning "PostgreSQL installer completed without creating service '$postgresServiceName'. Registering the service directly with pg_ctl."
            Register-ClippyPostgresService
        }
    }
    foreach ($required in @($postgresPsql,$postgresCreatedb,$postgresLibpq)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "PostgreSQL runtime file is missing: $required" }
    }
    $service = Get-PostgresService
    if ($service.Status -ne 'Running') {
        Start-Service -Name $postgresServiceName
        (Get-Service -Name $postgresServiceName).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
    }

    Set-PrivatePostgresConfiguration

    $appRole = Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresUser.ToString() -Label 'PostgresUser'
    $dbName = Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresDatabase.ToString() -Label 'PostgresDatabase'
    $appPassword = $secrets.PostgresAppPassword.ToString()
    if ($appPassword -notmatch '^[0-9a-f]{64}$') { throw 'Generated PostgreSQL application password is not in its expected safe format.' }

    $roleExists = ((Invoke-Psql -Sql "SELECT 1 FROM pg_roles WHERE rolname='$appRole';" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
    if ($roleExists -ne '1') {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; CREATE ROLE $appRole LOGIN PASSWORD '$appPassword' NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 48;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    } else {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; ALTER ROLE $appRole PASSWORD '$appPassword'; ALTER ROLE $appRole NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 48;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    }

    $dbExists = ((Invoke-Psql -Sql "SELECT 1 FROM pg_database WHERE datname='$dbName';" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
    if ($dbExists -ne '1') {
        $old = [Environment]::GetEnvironmentVariable('PGPASSWORD','Process')
        [Environment]::SetEnvironmentVariable('PGPASSWORD',$secrets.PostgresAdminPassword.ToString(),'Process')
        try {
            & $postgresCreatedb '--host' '127.0.0.1' '--port' $postgresPort.ToString() '--username' 'postgres' '--owner' $appRole $dbName
            if ($LASTEXITCODE -ne 0) { throw "createdb failed with exit code $LASTEXITCODE." }
        } finally {
            [Environment]::SetEnvironmentVariable('PGPASSWORD',$old,'Process')
        }
    }
    Invoke-Psql -Sql "REVOKE ALL ON DATABASE $dbName FROM PUBLIC; GRANT CONNECT, TEMPORARY ON DATABASE $dbName TO $appRole;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    Invoke-Psql -Database $dbName -Sql "REVOKE CREATE ON SCHEMA public FROM PUBLIC; REVOKE ALL ON SCHEMA public FROM PUBLIC; GRANT USAGE ON SCHEMA public TO $appRole; ALTER ROLE $appRole SET search_path TO clippy, pg_catalog;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null

    Assert-SafePostgresName -Value $adminReadRole -Label 'Admin read PostgreSQL role' | Out-Null
    $adminReadPassword = $secrets.PostgresAdminReadPassword.ToString()
    if ($adminReadPassword -notmatch '^[0-9a-f]{64}$') { throw 'Generated PostgreSQL admin read password is not in its expected safe format.' }
    $adminRoleExists = ((Invoke-Psql -Sql "SELECT 1 FROM pg_roles WHERE rolname='$adminReadRole';" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
    if ($adminRoleExists -ne '1') {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; CREATE ROLE $adminReadRole LOGIN PASSWORD '$adminReadPassword' NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 12;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    } else {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; ALTER ROLE $adminReadRole PASSWORD '$adminReadPassword'; ALTER ROLE $adminReadRole NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 12;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    }
    Invoke-Psql -Sql "ALTER ROLE $adminReadRole SET default_transaction_read_only=on; ALTER ROLE $adminReadRole SET search_path TO clippy, pg_catalog;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    Invoke-Psql -Sql "REVOKE ALL ON DATABASE $dbName FROM $adminReadRole; GRANT CONNECT ON DATABASE $dbName TO $adminReadRole;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    $clippySchemaExists = ((Invoke-Psql -Database $dbName -Sql "SELECT 1 FROM pg_namespace WHERE nspname='clippy';" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
    if ($clippySchemaExists -eq '1') {
        Invoke-Psql -Database $dbName -Sql "GRANT USAGE ON SCHEMA clippy TO $adminReadRole; GRANT SELECT ON ALL TABLES IN SCHEMA clippy TO $adminReadRole; ALTER DEFAULT PRIVILEGES FOR ROLE $appRole IN SCHEMA clippy GRANT SELECT ON TABLES TO $adminReadRole;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    }

    Assert-SafePostgresName -Value $adminEditRole -Label 'Admin edit PostgreSQL role' | Out-Null
    $adminEditPassword = $secrets.PostgresAdminEditPassword.ToString()
    if ($adminEditPassword -notmatch '^[0-9a-f]{64}$') { throw 'Generated PostgreSQL admin edit password is not in its expected safe format.' }
    $adminEditRoleExists = ((Invoke-Psql -Sql "SELECT 1 FROM pg_roles WHERE rolname='$adminEditRole';" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
    if ($adminEditRoleExists -ne '1') {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; CREATE ROLE $adminEditRole LOGIN PASSWORD '$adminEditPassword' NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 4;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    } else {
        Invoke-Psql -Sql "SET password_encryption='scram-sha-256'; ALTER ROLE $adminEditRole PASSWORD '$adminEditPassword'; ALTER ROLE $adminEditRole NOSUPERUSER NOCREATEDB NOCREATEROLE NOINHERIT NOREPLICATION CONNECTION LIMIT 4;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    }
    Invoke-Psql -Sql "ALTER ROLE $adminEditRole RESET default_transaction_read_only; ALTER ROLE $adminEditRole SET search_path TO clippy, pg_catalog;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    Invoke-Psql -Sql "REVOKE ALL ON DATABASE $dbName FROM $adminEditRole; GRANT CONNECT ON DATABASE $dbName TO $adminEditRole;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
    if ($clippySchemaExists -eq '1') {
        Invoke-Psql -Database $dbName -Sql "GRANT USAGE ON SCHEMA clippy TO $adminEditRole; GRANT SELECT ON ALL TABLES IN SCHEMA clippy TO $adminEditRole;" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
        $adminEditTablesReady = ((Invoke-Psql -Database $dbName -Sql "SELECT CASE WHEN to_regclass('clippy.admin_container_locks') IS NOT NULL AND to_regclass('clippy.admin_change_sets') IS NOT NULL AND to_regclass('clippy.admin_change_entries') IS NOT NULL AND to_regclass('clippy.admin_quarantine') IS NOT NULL AND to_regclass('clippy.admin_storage_snapshots') IS NOT NULL AND to_regclass('clippy.admin_snapshot_roots') IS NOT NULL AND to_regclass('clippy.admin_audit_events') IS NOT NULL THEN 1 ELSE 0 END;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        if ($adminEditTablesReady -eq '1') {
            $editSql = @"
GRANT UPDATE(revision,updated_ms) ON clippy.storage_containers TO $adminEditRole;
GRANT INSERT,UPDATE,DELETE ON clippy.cargo_roots TO $adminEditRole;
GRANT INSERT,DELETE ON clippy.cargo_item_index TO $adminEditRole;
GRANT INSERT,DELETE ON clippy.admin_container_locks TO $adminEditRole;
GRANT INSERT ON clippy.admin_change_sets TO $adminEditRole;
GRANT UPDATE(status,undone_ms,undo_change_id) ON clippy.admin_change_sets TO $adminEditRole;
GRANT INSERT ON clippy.admin_change_entries TO $adminEditRole;
GRANT INSERT ON clippy.admin_quarantine TO $adminEditRole;
GRANT UPDATE(restored_change_id,restored_ms) ON clippy.admin_quarantine TO $adminEditRole;
GRANT INSERT ON clippy.admin_storage_snapshots TO $adminEditRole;
GRANT INSERT ON clippy.admin_snapshot_roots TO $adminEditRole;
GRANT INSERT ON clippy.admin_audit_events TO $adminEditRole;
GRANT INSERT ON clippy.admin_player_commands TO $adminEditRole;
GRANT USAGE,SELECT ON SEQUENCE clippy.admin_change_entries_entry_id_seq TO $adminEditRole;
GRANT USAGE,SELECT ON SEQUENCE clippy.admin_audit_events_event_id_seq TO $adminEditRole;
"@
            Invoke-Psql -Database $dbName -Sql $editSql -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
        }
    }
    Write-Host "PostgreSQL ready on 127.0.0.1:$postgresPort using service $postgresServiceName." -ForegroundColor Green
}

function Get-PayloadHostExecutable {
    if (-not (Test-Path -LiteralPath $payloadHostExe -PathType Leaf)) {
        throw "Prebuilt ClippyStorageHost.exe is missing from the release payload: $payloadHostExe"
    }
    if ((Get-Item -LiteralPath $payloadHostExe).Length -lt 100000) {
        throw 'Prebuilt ClippyStorageHost.exe is unexpectedly small.'
    }
    $manifestPath = Join-Path $payloadRoot 'PAYLOAD-MANIFEST.json'
    $expectedVersion = '1.0.0'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifest = Read-JsonFile -Path $manifestPath
        $expectedHash = $manifest.HostExeSHA256.ToString().Trim().ToUpperInvariant()
        if ($expectedHash -notmatch '^[0-9A-F]{64}$') { throw 'Payload HostExeSHA256 is missing or invalid.' }
        if ((Get-FileHash -LiteralPath $payloadHostExe -Algorithm SHA256).Hash -ne $expectedHash) {
            throw 'Prebuilt ClippyStorageHost.exe hash does not match the release manifest.'
        }
        $expectedVersion = if ($manifest.PSObject.Properties['HostVersion']) { $manifest.HostVersion.ToString() } else { $manifest.Version.ToString() }
    }
    $help = & $payloadHostExe '--help' 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -or (($help | Out-String) -notmatch [regex]::Escape("ClippyStorageHost $expectedVersion"))) {
        throw 'Prebuilt ClippyStorageHost.exe failed its version smoke test.'
    }
    return $payloadHostExe
}

function Get-PayloadAdminExecutable {
    if (-not (Test-Path -LiteralPath $payloadAdminExe -PathType Leaf)) {
        throw "Prebuilt ClippyAdminHost.exe is missing from the release payload: $payloadAdminExe"
    }
    if ((Get-Item -LiteralPath $payloadAdminExe).Length -lt 100000) {
        throw 'Prebuilt ClippyAdminHost.exe is unexpectedly small.'
    }
    $manifestPath = Join-Path $payloadRoot 'PAYLOAD-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Payload manifest is missing: $manifestPath" }
    $manifest = Read-JsonFile -Path $manifestPath
    if (-not $manifest.PSObject.Properties['AdminExeSHA256']) { throw 'Payload AdminExeSHA256 is missing.' }
    $expectedHash = $manifest.AdminExeSHA256.ToString().Trim().ToUpperInvariant()
    if ($expectedHash -notmatch '^[0-9A-F]{64}$') { throw 'Payload AdminExeSHA256 is invalid.' }
    if ((Get-FileHash -LiteralPath $payloadAdminExe -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Prebuilt ClippyAdminHost.exe hash does not match the release manifest.'
    }
    $expectedVersion = if ($manifest.PSObject.Properties['AdminVersion']) { $manifest.AdminVersion.ToString() } else { $manifest.Version.ToString() }
    $help = & $payloadAdminExe '--help' 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -or (($help | Out-String) -notmatch [regex]::Escape("ClippyAdminHost $expectedVersion"))) {
        throw 'Prebuilt ClippyAdminHost.exe failed its version smoke test.'
    }
    return $payloadAdminExe
}

function Test-SqliteDatabaseFile {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    try {
        $stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try {
            if ($stream.Length -lt 16) { return $false }
            $bytes = New-Object byte[] 16
            if ($stream.Read($bytes,0,16) -ne 16) { return $false }
            return [Text.Encoding]::ASCII.GetString($bytes) -eq ("SQLite format 3" + [char]0)
        } finally { $stream.Dispose() }
    } catch { return $false }
}

function Get-LegacySqliteDatabase {
    $candidates = New-Object System.Collections.Generic.List[string]
    if (Test-Path -LiteralPath $installedHostConfig -PathType Leaf) {
        try {
            $old = Read-JsonFile -Path $installedHostConfig
            if ($old.PSObject.Properties['databasePath'] -and $old.databasePath) {
                $candidates.Add((Resolve-InstalledHostPath -Value $old.databasePath.ToString()))
            }
        } catch { }
    }
    $candidates.Add((Join-Path $installedHost 'data\ClippyVirtualCargo.db'))
    $valid = New-Object System.Collections.Generic.List[string]
    foreach ($candidate in $candidates | Select-Object -Unique) {
        if (Test-SqliteDatabaseFile -Path $candidate) { $valid.Add([IO.Path]::GetFullPath($candidate)) }
    }
    if ($valid.Count -eq 0 -and (Test-Path -LiteralPath $installedHost -PathType Container)) {
        foreach ($file in Get-ChildItem -LiteralPath $installedHost -File -Filter '*.db' -Recurse -ErrorAction SilentlyContinue) {
            if ($file.FullName -match '\\(backups|migration-backups|legacy-import-backups)\\') { continue }
            if (Test-SqliteDatabaseFile -Path $file.FullName) { $valid.Add([IO.Path]::GetFullPath($file.FullName)) }
        }
    }
    $unique = @($valid | Select-Object -Unique)
    if ($unique.Count -gt 1) { throw "More than one possible legacy SQLite database was found. Restore the old ClippyStorageHost.json so the active database can be identified safely: $($unique -join '; ')" }
    if ($unique.Count -eq 1) { return $unique[0] }
    return $null
}

function Backup-LegacyHostRuntime {
    param([string]$LegacyDatabase)
    if (-not $LegacyDatabase) { return }
    $backupRoot = Join-Path $installedHost 'legacy-import-backups'
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
    foreach ($file in @($installedHostConfig,$installedHostExe)) {
        if (Test-Path -LiteralPath $file -PathType Leaf) {
            Copy-Item -LiteralPath $file -Destination (Join-Path $backupRoot ($stamp + '-' + [IO.Path]::GetFileName($file))) -Force
        }
    }
}

function Invoke-LegacySqliteMigration {
    param([string]$LegacyDatabase)
    if (-not $LegacyDatabase) { return }
    if (-not (Test-SqliteDatabaseFile -Path $LegacyDatabase)) { throw "Legacy SQLite database disappeared before migration: $LegacyDatabase" }
    Write-Host "Legacy SQLite database detected. Importing it into PostgreSQL: $LegacyDatabase" -ForegroundColor Yellow
    $output = & $installedHostExe '--config' 'ClippyStorageHost.json' '--migrate-sqlite' $LegacyDatabase 2>&1
    $exit = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($exit -ne 0) { throw "Legacy SQLite import failed with exit code $exit. The original SQLite database was not modified." }
    Write-Host 'Legacy SQLite import completed. The original SQLite database was retained.' -ForegroundColor Green
}

function Assert-StagedPayloadIntegrity {
    if (-not [IO.Path]::GetFullPath($payloadRoot).TrimEnd('\').Equals([IO.Path]::GetFullPath($stagedPayload).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { return }
    $manifestPath = Join-Path $payloadRoot 'PAYLOAD-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Staged payload manifest is missing: $manifestPath" }
    $manifest = Read-JsonFile -Path $manifestPath
    $releaseVersion = $manifest.Version.ToString()
    $workshopVersion = if ($manifest.PSObject.Properties['WorkshopVersion']) { $manifest.WorkshopVersion.ToString() } else { $releaseVersion }
    $hostComponentVersion = if ($manifest.PSObject.Properties['HostVersion']) { $manifest.HostVersion.ToString() } else { $releaseVersion }
    $adminComponentVersion = if ($manifest.PSObject.Properties['AdminVersion']) { $manifest.AdminVersion.ToString() } else { $releaseVersion }
    if ($manifest.WorkshopID.ToString() -ne $workshopId -or
        $releaseVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$' -or
        $workshopVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$' -or
        $hostComponentVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$' -or
        $adminComponentVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { throw 'Staged payload identity is invalid.' }
    $signatureFile = $manifest.SignatureFile.ToString()
    $keyFile = $manifest.KeyFile.ToString()
    if ($signatureFile -notmatch '^[A-Za-z0-9_.-]+\.bisign$' -or $keyFile -notmatch '^[A-Za-z0-9_.-]+\.bikey$') { throw 'Staged payload signature or public-key filename is invalid.' }
    $checks = @{
        (Join-Path $payloadRoot "$modName\addons\clippy_virtual_cargo.pbo") = $manifest.PboSHA256.ToString()
        (Join-Path $payloadRoot "$modName\addons\$signatureFile") = $manifest.SignatureSHA256.ToString()
        (Join-Path $payloadRoot "$modName\keys\$keyFile") = $manifest.KeySHA256.ToString()
        (Join-Path $payloadRoot 'ServerProfileTemplate\ClippyVirtualCargo\Settings.json') = $manifest.SettingsSHA256.ToString()
        (Join-Path $payloadRoot 'ClippyStorageHost\ClippyStorageHost.exe') = $manifest.HostExeSHA256.ToString()
        (Join-Path $payloadRoot 'ClippyAdmin\ClippyAdminHost.exe') = $manifest.AdminExeSHA256.ToString()
    }
    foreach ($path in $checks.Keys) {
        $expected = $checks[$path].Trim().ToUpperInvariant()
        if ($expected -notmatch '^[0-9A-F]{64}$') { throw "Staged payload manifest has an invalid SHA-256 for $path" }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Staged payload file is missing: $path" }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected) { throw "Staged payload hash mismatch: $path" }
    }
}


function Test-CompatibilityPboInUse {
    $marker = Join-Path $payloadMod 'LEGACY-PBO-COMPATIBILITY.txt'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) { return $false }
    $payloadPbo = Join-Path $payloadMod 'addons\clippy_virtual_cargo.pbo'
    $installedPbo = Join-Path $installedMod 'addons\clippy_virtual_cargo.pbo'
    if (-not (Test-Path -LiteralPath $installedPbo -PathType Leaf)) { return $true }
    if (-not (Test-Path -LiteralPath $payloadPbo -PathType Leaf)) { throw 'Compatibility PBO marker exists but the staged PBO is missing.' }
    return (Get-FileHash -LiteralPath $installedPbo -Algorithm SHA256).Hash -eq
           (Get-FileHash -LiteralPath $payloadPbo -Algorithm SHA256).Hash
}

function Test-InstalledCurrentWorkshopMod {
    $marker = Join-Path $payloadMod 'LEGACY-PBO-COMPATIBILITY.txt'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf)) { return $false }
    $installedPbo = Join-Path $installedMod 'addons\clippy_virtual_cargo.pbo'
    $payloadPbo = Join-Path $payloadMod 'addons\clippy_virtual_cargo.pbo'
    $buildMarker = Join-Path $installedMod 'CLIPPY-WORKSHOP-BUILD.json'
    if (-not (Test-Path -LiteralPath $installedPbo -PathType Leaf) -or
        -not (Test-Path -LiteralPath $payloadPbo -PathType Leaf) -or
        -not (Test-Path -LiteralPath $buildMarker -PathType Leaf)) { return $false }
    if ((Get-FileHash -LiteralPath $installedPbo -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $payloadPbo -Algorithm SHA256).Hash) { return $false }
    try { $workshopBuild = Read-JsonFile -Path $buildMarker } catch { return $false }
    $manifestPath = Join-Path $payloadRoot 'PAYLOAD-MANIFEST.json'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return $false }
    $manifest = Read-JsonFile -Path $manifestPath
    $releaseVersion = $manifest.Version.ToString()
    $workshopVersion = if ($manifest.PSObject.Properties['WorkshopVersion']) { $manifest.WorkshopVersion.ToString() } else { $releaseVersion }
    if ($releaseVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$' -or
        $workshopVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { return $false }
    if ($workshopBuild.Version.ToString() -ne $workshopVersion) { return $false }
    $expectedPboHash = $workshopBuild.PboSHA256.ToString().Trim().ToUpperInvariant()
    if ($expectedPboHash -notmatch '^[0-9A-F]{64}$') { return $false }
    return (Get-FileHash -LiteralPath $installedPbo -Algorithm SHA256).Hash -eq $expectedPboHash
}

function Assert-Configuration {
    foreach ($required in @($serverRoot, $serverExe, $payloadMod, $payloadHost, $payloadHostExe,
            $payloadAdmin, $payloadAdminExe, (Join-Path $payloadMod 'addons\clippy_virtual_cargo.pbo'), $payloadSettings)) {
        if (-not (Test-Path -LiteralPath $required)) { throw "Required manager path is missing: $required" }
    }
    Assert-StagedPayloadIntegrity
    $port = [int]$config.StorageHostSettings.Port
    if ($port -lt 1024 -or $port -gt 65535) { throw 'StorageHostSettings.Port must be between 1024 and 65535.' }
    if ($config.StorageHostSettings.BindAddress -notin @('127.0.0.1', '::1', 'localhost')) { throw 'StorageHostSettings.BindAddress must be a loopback address.' }
    if ($config.StorageHostSettings.PostgresHost.ToString() -ne '127.0.0.1') { throw 'StorageHostSettings.PostgresHost must be 127.0.0.1 for the private PostgreSQL instance.' }
    if ($port -eq $postgresPort) { throw 'The Clippy HTTP port and PostgreSQL port must be different.' }
    if ([int]$config.StorageHostSettings.PostgresPoolSize -lt 4 -or [int]$config.StorageHostSettings.PostgresPoolSize -gt 48) { throw 'PostgresPoolSize must be between 4 and 48.' }
    if ([int]$config.StorageHostSettings.HttpThreads -lt 2 -or [int]$config.StorageHostSettings.HttpThreads -gt 64) { throw 'HttpThreads must be between 2 and 64.' }
    if ([int]$config.StorageHostSettings.MaxPageNodes -lt 1 -or [int]$config.StorageHostSettings.MaxPageNodes -gt [int]$config.StorageHostSettings.MaxItemNodes) { throw 'MaxPageNodes must be at least 1 and cannot exceed MaxItemNodes.' }
    if ([int]$config.VirtualCargoSettings.MaterializationIntervalMs -lt 1 -or [int]$config.VirtualCargoSettings.MaterializationIntervalMs -gt 1000) { throw 'VirtualCargoSettings.MaterializationIntervalMs must be between 1 and 1000.' }
    if ([int]$config.StorageHostSettings.MaxBackupFiles -lt 1 -or [int]$config.StorageHostSettings.MaxBackupFiles -gt 1000) { throw 'StorageHostSettings.MaxBackupFiles must be between 1 and 1000.' }
    if ([int]$config.StorageHostSettings.TerminalRetentionDays -lt 1 -or [int]$config.StorageHostSettings.TerminalRetentionDays -gt 3650) { throw 'StorageHostSettings.TerminalRetentionDays must be between 1 and 3650.' }
    if ([int]$config.StorageHostSettings.MaintenancePruneBatchRows -lt 10 -or [int]$config.StorageHostSettings.MaintenancePruneBatchRows -gt 10000) { throw 'StorageHostSettings.MaintenancePruneBatchRows must be between 10 and 10000.' }
    Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresDatabase.ToString() -Label 'PostgresDatabase' | Out-Null
    Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresUser.ToString() -Label 'PostgresUser' | Out-Null
    $adminPort = [int]$config.AdminPanel.Port
    if ($adminPort -lt 1024 -or $adminPort -gt 65535) { throw 'AdminPanel.Port must be between 1024 and 65535.' }
    if ($adminPort -eq $port -or $adminPort -eq $postgresPort) { throw 'AdminPanel.Port must be different from the storage host and PostgreSQL ports.' }
    if ([int]$config.AdminPanel.PostgresPoolSize -lt 2 -or [int]$config.AdminPanel.PostgresPoolSize -gt 12) { throw 'AdminPanel.PostgresPoolSize must be between 2 and 12.' }
    if ([int]$config.AdminPanel.HttpThreads -lt 2 -or [int]$config.AdminPanel.HttpThreads -gt 32) { throw 'AdminPanel.HttpThreads must be between 2 and 32.' }
    if ([int]$config.AdminPanel.MaintenanceLockSeconds -lt 30 -or [int]$config.AdminPanel.MaintenanceLockSeconds -gt 900) { throw 'AdminPanel.MaintenanceLockSeconds must be between 30 and 900.' }
    if ([int]$config.AdminPanel.PostgresWritePoolSize -lt 1 -or [int]$config.AdminPanel.PostgresWritePoolSize -gt 4) { throw 'AdminPanel.PostgresWritePoolSize must be between 1 and 4.' }
    if ([int]$config.AdminPanel.PostgresWriteStatementTimeoutMs -lt 500 -or [int]$config.AdminPanel.PostgresWriteStatementTimeoutMs -gt 15000) { throw 'AdminPanel.PostgresWriteStatementTimeoutMs must be between 500 and 15000.' }
    if ([int]$config.AdminPanel.PostgresWriteLockTimeoutMs -lt 100 -or [int]$config.AdminPanel.PostgresWriteLockTimeoutMs -gt 5000) { throw 'AdminPanel.PostgresWriteLockTimeoutMs must be between 100 and 5000.' }
    if ([int]$config.AdminPanel.PlayerSnapshotIntervalSeconds -lt 30 -or [int]$config.AdminPanel.PlayerSnapshotIntervalSeconds -gt 3600) { throw 'AdminPanel.PlayerSnapshotIntervalSeconds must be between 30 and 3600.' }
    if ([int]$config.AdminPanel.PlayerCommandPollIntervalSeconds -lt 1 -or [int]$config.AdminPanel.PlayerCommandPollIntervalSeconds -gt 30) { throw 'AdminPanel.PlayerCommandPollIntervalSeconds must be between 1 and 30.' }
    if ([int]$config.AdminPanel.PlayerCommandExpirySeconds -lt 5 -or [int]$config.AdminPanel.PlayerCommandExpirySeconds -gt 300) { throw 'AdminPanel.PlayerCommandExpirySeconds must be between 5 and 300.' }
    if ([int]$config.AdminPanel.PlayerTelemetryRetentionDays -lt 1 -or [int]$config.AdminPanel.PlayerTelemetryRetentionDays -gt 3650) { throw 'AdminPanel.PlayerTelemetryRetentionDays must be between 1 and 3650.' }
    if ([int]$config.AdminPanel.PlayerSnapshotHistoryLimit -lt 2 -or [int]$config.AdminPanel.PlayerSnapshotHistoryLimit -gt 10000) { throw 'AdminPanel.PlayerSnapshotHistoryLimit must be between 2 and 10000.' }
    if ([int]$config.AdminPanel.AdminAuditRetentionDays -lt 7 -or [int]$config.AdminPanel.AdminAuditRetentionDays -gt 3650) { throw 'AdminPanel.AdminAuditRetentionDays must be between 7 and 3650.' }
    if ([bool]$config.AdminPanel.EnableLivePlayerControl -and -not [bool]$config.AdminPanel.EnablePlayerTelemetry) { throw 'AdminPanel.EnableLivePlayerControl requires EnablePlayerTelemetry.' }
    if ([int]$config.ConfigVersion -ne 6) { throw 'Unsupported ClippyServerManager.json ConfigVersion.' }
}


function Get-DayZProcess {
    $expectedPath = [IO.Path]::GetFullPath($serverExe)
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='$([IO.Path]::GetFileName($serverExe))'" -ErrorAction SilentlyContinue)
    foreach ($candidate in $matches) {
        if ($candidate.ExecutablePath) {
            if ([IO.Path]::GetFullPath($candidate.ExecutablePath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
                return Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
            }
            continue
        }
        if ($candidate.CommandLine) {
            $commandLine = $candidate.CommandLine.TrimStart()
            $quotedExpected = '"' + $expectedPath + '"'
            $commandMatches = $commandLine.Equals($quotedExpected, [StringComparison]::OrdinalIgnoreCase) -or
                $commandLine.StartsWith($quotedExpected + ' ', [StringComparison]::OrdinalIgnoreCase)
            if (-not ($expectedPath -match '\s')) {
                $commandMatches = $commandMatches -or
                    $commandLine.Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase) -or
                    $commandLine.StartsWith($expectedPath + ' ', [StringComparison]::OrdinalIgnoreCase)
            }
            if ($commandMatches) { return Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue }
        }
    }
    return $null
}

function Get-InstalledHostProcess {
    if (-not (Test-Path -LiteralPath $installedHostExe -PathType Leaf)) { return $null }
    $expectedPath = [IO.Path]::GetFullPath($installedHostExe)
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='ClippyStorageHost.exe'" -ErrorAction SilentlyContinue)
    foreach ($candidate in $matches) {
        $pathMatches = $candidate.ExecutablePath -and
            [IO.Path]::GetFullPath($candidate.ExecutablePath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)
        $quotedExpected = '"' + $expectedPath + '"'
        $commandMatches = $candidate.CommandLine -and
            ($candidate.CommandLine.Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase) -or
             $candidate.CommandLine.StartsWith($quotedExpected + ' ', [StringComparison]::OrdinalIgnoreCase) -or
             $candidate.CommandLine.Equals($quotedExpected, [StringComparison]::OrdinalIgnoreCase))
        if ($pathMatches -or $commandMatches) {
            return Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
        }
    }
    foreach ($candidate in @(Get-Process -Name 'ClippyStorageHost' -ErrorAction SilentlyContinue)) {
        try {
            $candidatePath = $candidate.Path
            if ($candidatePath -and
                [IO.Path]::GetFullPath($candidatePath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
                return $candidate
            }
        } catch {
        }
    }
    return $null
}

function Get-InstalledAdminProcess {
    if (-not (Test-Path -LiteralPath $installedAdminExe -PathType Leaf)) { return $null }
    $expectedPath = [IO.Path]::GetFullPath($installedAdminExe)
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='ClippyAdminHost.exe'" -ErrorAction SilentlyContinue)
    foreach ($candidate in $matches) {
        $pathMatches = $candidate.ExecutablePath -and
            [IO.Path]::GetFullPath($candidate.ExecutablePath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)
        $quotedExpected = '"' + $expectedPath + '"'
        $commandMatches = $candidate.CommandLine -and
            ($candidate.CommandLine.Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase) -or
             $candidate.CommandLine.StartsWith($quotedExpected + ' ', [StringComparison]::OrdinalIgnoreCase) -or
             $candidate.CommandLine.Equals($quotedExpected, [StringComparison]::OrdinalIgnoreCase))
        if ($pathMatches -or $commandMatches) {
            return Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
        }
    }
    foreach ($candidate in @(Get-Process -Name 'ClippyAdminHost' -ErrorAction SilentlyContinue)) {
        try {
            $candidatePath = $candidate.Path
            if ($candidatePath -and
                [IO.Path]::GetFullPath($candidatePath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
                return $candidate
            }
        } catch { }
    }
    return $null
}

function Stop-InstalledAdmin {
    $process = Get-InstalledAdminProcess
    if (-not $process) { return }
    Stop-Process -Id $process.Id -Force -ErrorAction Stop
    $process.WaitForExit(5000) | Out-Null
}

function Invoke-HostPost {
    param([string]$Path, [object]$HostDocument, [hashtable]$Extra = @{}, [int]$TimeoutSeconds = 15)
    $body = @{ api_token = $HostDocument.apiToken; request_id = [Guid]::NewGuid().ToString('N') }
    foreach ($key in $Extra.Keys) { $body[$key] = $Extra[$key] }
    $address = $HostDocument.bindAddress.ToString()
    if ($address -eq '::1') { $address = '[::1]' }
    if ($address -eq 'localhost') { $address = '127.0.0.1' }
    return Invoke-RestMethod -Method Post -Uri ("http://$address`:$($HostDocument.port)$Path") -ContentType 'application/json' -Body ($body | ConvertTo-Json -Depth 30) -TimeoutSec $TimeoutSeconds
}

function Wait-HostHealthy {
    param([object]$HostDocument)
    $deadline = [DateTime]::UtcNow.AddSeconds([int]$config.Management.HealthTimeoutSeconds)
    do {
        try {
            $health = Invoke-HostPost -Path '/v1/health' -HostDocument $HostDocument -TimeoutSeconds 2
            if ($health.ok -and $health.data.database_healthy) { return $health }
        } catch {
        }
        Start-Sleep -Milliseconds 250
    } until ([DateTime]::UtcNow -ge $deadline)
    throw "ClippyStorageHost did not become healthy on port $($HostDocument.port)."
}

function Stop-InstalledHost {
    param([switch]$AllowForce)
    $process = Get-InstalledHostProcess
    if (-not $process) { return }
    if (Test-Path -LiteralPath $installedHostConfig -PathType Leaf) {
        try {
            $hostDocument = Read-JsonFile -Path $installedHostConfig
            Invoke-HostPost -Path '/v1/admin/shutdown' -HostDocument $hostDocument -TimeoutSeconds 5 | Out-Null
            if ($process.WaitForExit(10000)) { return }
        } catch {
            Write-Warning "Graceful host shutdown was unavailable: $($_.Exception.Message)"
        }
    }
    if ($AllowForce) {
        Stop-Process -Id $process.Id -Force -ErrorAction Stop
        $process.WaitForExit(5000) | Out-Null
        return
    }
    throw 'The storage host did not stop cleanly.'
}

function Copy-DirectoryAtomic {
    param([string]$Source, [string]$Target)
    $sourceFull = [IO.Path]::GetFullPath($Source).TrimEnd('\')
    $targetFull = [IO.Path]::GetFullPath($Target).TrimEnd('\')
    if ($sourceFull.Equals($targetFull, [StringComparison]::OrdinalIgnoreCase)) { return }
    $stage = Assert-UnderRoot -Path (Join-Path $serverRoot ('.clippy-stage-' + [Guid]::NewGuid().ToString('N'))) -Root $serverRoot
    $backup = Assert-UnderRoot -Path (Join-Path $serverRoot ('.clippy-backup-' + [Guid]::NewGuid().ToString('N'))) -Root $serverRoot
    Copy-Item -LiteralPath $sourceFull -Destination $stage -Recurse -Force
    try {
        if (Test-Path -LiteralPath $targetFull) { Move-Item -LiteralPath $targetFull -Destination $backup }
        Move-Item -LiteralPath $stage -Destination $targetFull
        if (Test-Path -LiteralPath $backup) { Remove-Item -LiteralPath $backup -Recurse -Force }
    } catch {
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
        if (Test-Path -LiteralPath $targetFull) { Remove-Item -LiteralPath $targetFull -Recurse -Force }
        if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup -Destination $targetFull }
        throw
    }
}

function Resolve-ProfileRoot {
    $configured = $config.DayZLaunch.ProfilesDirectory.ToString()
    if ($configured) { return Resolve-AbsolutePath -Value $configured -Base $serverRoot }
    $argumentText = Get-BaseArgumentText
    $value = Get-LaunchOptionValue -Text $argumentText -Name 'profiles'
    if ($value) { return Resolve-AbsolutePath -Value $value -Base $serverRoot }
    return Join-Path $serverRoot 'profiles'
}

function New-DesiredHostDocument {
    $secrets = Get-OrCreateSecrets
    return [pscustomobject][ordered]@{
        protocolVersion = 1
        bindAddress = $config.StorageHostSettings.BindAddress.ToString()
        port = [int]$config.StorageHostSettings.Port
        apiToken = $secrets.ApiToken.ToString()
        postgresHost = $config.StorageHostSettings.PostgresHost.ToString()
        postgresPort = [int]$config.StorageHostSettings.PostgresPort
        postgresDatabase = $config.StorageHostSettings.PostgresDatabase.ToString()
        postgresUser = $config.StorageHostSettings.PostgresUser.ToString()
        postgresPassword = $secrets.PostgresAppPassword.ToString()
        postgresLibraryPath = $postgresLibpq
        postgresBinDirectory = $postgresBin
        postgresPoolSize = [int]$config.StorageHostSettings.PostgresPoolSize
        postgresConnectTimeoutSeconds = [int]$config.StorageHostSettings.PostgresConnectTimeoutSeconds
        postgresStatementTimeoutMs = [int]$config.StorageHostSettings.PostgresStatementTimeoutMs
        postgresLockTimeoutMs = [int]$config.StorageHostSettings.PostgresLockTimeoutMs
        postgresIdleTransactionTimeoutMs = [int]$config.StorageHostSettings.PostgresIdleTransactionTimeoutMs
        backupDirectory = $config.StorageHostSettings.BackupDirectory.ToString()
        httpThreads = [int]$config.StorageHostSettings.HttpThreads
        maxQueuedRequests = [int]$config.StorageHostSettings.MaxQueuedRequests
        maxBackupFiles = [int]$config.StorageHostSettings.MaxBackupFiles
        terminalRetentionDays = [int]$config.StorageHostSettings.TerminalRetentionDays
        playerTelemetryRetentionDays = [int]$config.AdminPanel.PlayerTelemetryRetentionDays
        playerSnapshotHistoryLimit = [int]$config.AdminPanel.PlayerSnapshotHistoryLimit
        adminAuditRetentionDays = [int]$config.AdminPanel.AdminAuditRetentionDays
        maintenancePruneBatchRows = [int]$config.StorageHostSettings.MaintenancePruneBatchRows
        maintenanceIntervalSeconds = [int]$config.StorageHostSettings.MaintenanceIntervalSeconds
        maxRequestBytes = [int]$config.StorageHostSettings.MaxRequestBytes
        maxItemNodes = [int]$config.StorageHostSettings.MaxItemNodes
        maxPageNodes = [int]$config.StorageHostSettings.MaxPageNodes
        maxItemDepth = [int]$config.StorageHostSettings.MaxItemDepth
    }
}


function New-DesiredAdminDocument {
    $secrets = Get-OrCreateSecrets
    return [pscustomobject][ordered]@{
        protocolVersion = 1
        port = [int]$config.AdminPanel.Port
        idleShutdownMinutes = [int]$config.AdminPanel.IdleShutdownMinutes
        httpThreads = [int]$config.AdminPanel.HttpThreads
        maxQueuedRequests = [int]$config.AdminPanel.MaxQueuedRequests
        maxRequestBytes = [int]$config.AdminPanel.MaxRequestBytes
        storageHostAddress = '127.0.0.1'
        storageHostPort = [int]$config.StorageHostSettings.Port
        storageHostApiToken = $secrets.ApiToken.ToString()
        dayzExecutableName = [IO.Path]::GetFileName($config.ServerExecutable.ToString())
        backupDirectory = (Resolve-AbsolutePath -Value $config.StorageHostSettings.BackupDirectory.ToString() -Base $installedHost)
        exportDirectory = (Join-Path $installedAdmin 'exports')
        enableEditing = ([bool]$config.AdminPanel.EnableEditing -and [bool]$script:adminEditingAvailable)
        maintenanceLockSeconds = [int]$config.AdminPanel.MaintenanceLockSeconds
        postgresHost = '127.0.0.1'
        postgresPort = [int]$config.PostgreSQL.Port
        postgresDatabase = $config.StorageHostSettings.PostgresDatabase.ToString()
        postgresUser = $adminReadRole
        postgresPassword = $secrets.PostgresAdminReadPassword.ToString()
        postgresLibraryPath = $postgresLibpq
        postgresBinDirectory = $postgresBin
        postgresPoolSize = [int]$config.AdminPanel.PostgresPoolSize
        postgresConnectTimeoutSeconds = [int]$config.AdminPanel.PostgresConnectTimeoutSeconds
        postgresStatementTimeoutMs = [int]$config.AdminPanel.PostgresStatementTimeoutMs
        postgresLockTimeoutMs = [int]$config.AdminPanel.PostgresLockTimeoutMs
        postgresIdleTransactionTimeoutMs = [int]$config.AdminPanel.PostgresIdleTransactionTimeoutMs
        postgresWriteUser = $adminEditRole
        postgresWritePassword = $secrets.PostgresAdminEditPassword.ToString()
        postgresWritePoolSize = [int]$config.AdminPanel.PostgresWritePoolSize
        postgresWriteStatementTimeoutMs = [int]$config.AdminPanel.PostgresWriteStatementTimeoutMs
        postgresWriteLockTimeoutMs = [int]$config.AdminPanel.PostgresWriteLockTimeoutMs
        enablePlayerTelemetry = [bool]$config.AdminPanel.EnablePlayerTelemetry
        enablePlayerNetworkTelemetry = [bool]$config.AdminPanel.EnablePlayerNetworkTelemetry
        enablePlayerPositionTelemetry = [bool]$config.AdminPanel.EnablePlayerPositionTelemetry
        playerSnapshotIntervalSeconds = [int]$config.AdminPanel.PlayerSnapshotIntervalSeconds
        playerTelemetryRetentionDays = [int]$config.AdminPanel.PlayerTelemetryRetentionDays
        playerSnapshotHistoryLimit = [int]$config.AdminPanel.PlayerSnapshotHistoryLimit
        adminAuditRetentionDays = [int]$config.AdminPanel.AdminAuditRetentionDays
        enableLivePlayerControl = [bool]$config.AdminPanel.EnableLivePlayerControl
        playerCommandExpirySeconds = [int]$config.AdminPanel.PlayerCommandExpirySeconds
    }
}

function Sync-InstalledAdminConfiguration {
    New-Item -ItemType Directory -Path $installedAdmin -Force | Out-Null
    $document = New-DesiredAdminDocument
    Write-JsonAtomic -Path $installedAdminConfig -Document $document -Depth 20
    Protect-SensitiveFile -Path $installedAdminConfig
    return $document
}

function Test-HostConfigurationChanged {
    param([object]$Desired)
    if (-not (Test-Path -LiteralPath $installedHostConfig -PathType Leaf)) { return $true }
    try {
        $current = Read-JsonFile -Path $installedHostConfig
        $currentJson = $current | ConvertTo-Json -Depth 20 -Compress
        $desiredJson = $Desired | ConvertTo-Json -Depth 20 -Compress
        return $currentJson -ne $desiredJson
    } catch {
        return $true
    }
}

function Resolve-InstalledHostPath {
    param([string]$Value)
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $installedHost $Value))
}

function Invoke-VerifiedHostBackup {
    param([object]$HostDocument)
    $result = Invoke-HostPost -Path '/v1/admin/backup' -HostDocument $HostDocument -TimeoutSeconds 120
    if (-not $result.ok -or -not $result.data.path) { throw 'The storage host did not complete its PostgreSQL backup.' }
    $backupPath = Resolve-InstalledHostPath -Value $result.data.path.ToString()
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf) -or (Get-Item -LiteralPath $backupPath).Length -le 0) { throw "The storage host reported a missing or empty PostgreSQL backup: $backupPath" }
    Write-Host "Verified PostgreSQL backup: $backupPath" -ForegroundColor Green
    return $backupPath
}


function Sync-InstalledConfiguration {
    param([object]$HostDocument = $null)
    if (-not $script:managerState) { Initialize-ManagerState -Commit | Out-Null }
    New-Item -ItemType Directory -Path $installedHost -Force | Out-Null
    if (-not $HostDocument) { $HostDocument = New-DesiredHostDocument }
    $hostDocument = $HostDocument
    Write-JsonAtomic -Path $installedHostConfig -Document $hostDocument -Depth 20
    Protect-SensitiveFile -Path $installedHostConfig

    $profileRoot = Resolve-ProfileRoot
    $settingsDirectory = Join-Path $profileRoot 'ClippyVirtualCargo'
    New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
    $dayZSettings = $config.VirtualCargoSettings | ConvertTo-Json -Depth 50 | ConvertFrom-Json
    $dayZSettings | Add-Member -NotePropertyName ProviderID -NotePropertyValue $script:managerState.EffectiveProviderID -Force
    $requiresExistingMigration = [bool]$script:managerState.RequiresExistingCargoMigration
    $dayZSettings | Add-Member -NotePropertyName EnableExistingCargoMigration -NotePropertyValue $requiresExistingMigration -Force
    $dayZSettings | Add-Member -NotePropertyName EnablePlayerTelemetry -NotePropertyValue ([bool]$config.AdminPanel.EnablePlayerTelemetry) -Force
    $dayZSettings | Add-Member -NotePropertyName EnablePlayerNetworkTelemetry -NotePropertyValue ([bool]$config.AdminPanel.EnablePlayerNetworkTelemetry) -Force
    $dayZSettings | Add-Member -NotePropertyName EnablePlayerPositionTelemetry -NotePropertyValue ([bool]$config.AdminPanel.EnablePlayerPositionTelemetry) -Force
    $dayZSettings | Add-Member -NotePropertyName PlayerSnapshotIntervalSeconds -NotePropertyValue ([int]$config.AdminPanel.PlayerSnapshotIntervalSeconds) -Force
    $dayZSettings | Add-Member -NotePropertyName EnableLivePlayerControl -NotePropertyValue ([bool]$config.AdminPanel.EnableLivePlayerControl) -Force
    $dayZSettings | Add-Member -NotePropertyName PlayerCommandPollIntervalSeconds -NotePropertyValue ([int]$config.AdminPanel.PlayerCommandPollIntervalSeconds) -Force
    $dayZSettings | Add-Member -NotePropertyName PlayerCommandExpirySeconds -NotePropertyValue ([int]$config.AdminPanel.PlayerCommandExpirySeconds) -Force
    # Legacy compatibility fields are written only when an old compatibility PBO is detected.
    if (Test-CompatibilityPboInUse) {
        $legacyMode = if ($requiresExistingMigration) { 'ImportAndRemove' } else { 'Off' }
        $dayZSettings | Add-Member -NotePropertyName ExistingCargoMigrationMode -NotePropertyValue $legacyMode -Force
        $dayZSettings | Add-Member -NotePropertyName MigrationOnlyWhenServerEmpty -NotePropertyValue $false -Force
    }
    $hostUrlAddress = $hostDocument.bindAddress
    if ($hostUrlAddress -eq '::1') { $hostUrlAddress = '[::1]' }
    if ($hostUrlAddress -eq 'localhost') { $hostUrlAddress = '127.0.0.1' }
    $dayZSettings | Add-Member -NotePropertyName HostURL -NotePropertyValue ("http://$hostUrlAddress`:$($hostDocument.port)") -Force
    $dayZSettings | Add-Member -NotePropertyName ApiToken -NotePropertyValue $hostDocument.apiToken -Force
    $settingsPath = Join-Path $settingsDirectory 'Settings.json'
    Write-JsonAtomic -Path $settingsPath -Document $dayZSettings -Depth 50
    return $hostDocument
}

function Install-Payload {
    $dayZ = Get-DayZProcess
    if ($dayZ) { throw "DayZ is running as PID $($dayZ.Id). Stop it cleanly before installing or updating files." }

    Write-JsonAtomic -Path $configPath -Document $config -Depth 50
    Initialize-ManagerState -Commit | Out-Null
    $legacyDatabase = Get-LegacySqliteDatabase
    if ($legacyDatabase) { Write-Host "Detected legacy SQLite data at $legacyDatabase" -ForegroundColor Yellow }

    $oldHostProcess = Get-InstalledHostProcess
    if ($oldHostProcess) {
        Write-Host 'Stopping the installed storage host before the database-engine update.' -ForegroundColor Cyan
        Stop-InstalledHost -AllowForce
    }
    Backup-LegacyHostRuntime -LegacyDatabase $legacyDatabase

    $payloadHostExe = Get-PayloadHostExecutable
    $payloadAdminExe = Get-PayloadAdminExecutable
    Ensure-PostgreSQLInstalled

    if (Test-InstalledCurrentWorkshopMod) {
        Write-Host 'Detected an installed Clippy Workshop PBO. Preserving it instead of replacing it with a compatibility fallback.' -ForegroundColor Green
    } else {
        Copy-DirectoryAtomic -Source $payloadMod -Target $installedMod
    }
    New-Item -ItemType Directory -Path $installedHost -Force | Out-Null
    Copy-Item -LiteralPath $payloadHostExe -Destination $installedHostExe -Force
    New-Item -ItemType Directory -Path $installedAdmin -Force | Out-Null
    Stop-InstalledAdmin
    Copy-Item -LiteralPath $payloadAdminExe -Destination $installedAdminExe -Force
    foreach ($name in @('LICENSE.txt','THIRD-PARTY-NOTICES.md')) {
        $source = Join-Path $payloadHost $name
        if (Test-Path -LiteralPath $source -PathType Leaf) { Copy-Item -LiteralPath $source -Destination $installedHost -Force }
    }

    $serverKeys = Join-Path $serverRoot 'keys'
    New-Item -ItemType Directory -Path $serverKeys -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $installedMod 'keys') -File -Filter '*.bikey' | Copy-Item -Destination $serverKeys -Force

    if (-not $managerScript.Equals($installedManager, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $managerScript -Destination $installedManager -Force }
    if (-not $managerPowerShell.Equals($installedManagerPowerShell, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $managerPowerShell -Destination $installedManagerPowerShell -Force }
    if ((Test-Path -LiteralPath $managerAdminLauncher -PathType Leaf) -and -not $managerAdminLauncher.Equals($installedAdminLauncher, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $managerAdminLauncher -Destination $installedAdminLauncher -Force }
    if (-not (Test-Path -LiteralPath $installedManagerConfig -PathType Leaf)) { Copy-Item -LiteralPath $configPath -Destination $installedManagerConfig -Force }
    $managerReadme = Join-Path $managerRoot 'ClippyVirtualCargoDocs\SERVER-MANAGER-README.md'
    if (-not (Test-Path -LiteralPath $managerReadme -PathType Leaf)) { $managerReadme = Join-Path $managerRoot 'SERVER-MANAGER-README.md' }
    if (Test-Path -LiteralPath $managerReadme -PathType Leaf) { Copy-Item -LiteralPath $managerReadme -Destination (Join-Path $serverRoot 'CLIPPY-SERVER-MANAGER-README.md') -Force }

    $desiredHostDocument = New-DesiredHostDocument
    $hostDocument = Sync-InstalledConfiguration -HostDocument $desiredHostDocument
    Sync-InstalledAdminConfiguration | Out-Null
    Invoke-LegacySqliteMigration -LegacyDatabase $legacyDatabase

    $launcherLabel = if ($baseStartScript) { $baseStartScript } else { 'JSON fallback arguments' }
    Write-Host "Installed or updated $modName with PostgreSQL without editing $launcherLabel" -ForegroundColor Green
    Write-Host "Manager: $installedManager"
    Write-Host "Configuration: $installedManagerConfig"
    return $hostDocument
}


function Expand-BatchVariables {
    param([string]$Text, [Collections.Generic.Dictionary[string,string]]$Variables)
    $expanded = $Text
    for ($pass = 0; $pass -lt 12; $pass++) {
        $before = $expanded
        $expanded = [regex]::Replace($expanded, '%([^%]+)%', {
            param($match)
            $name = $match.Groups[1].Value
            $value = $null
            if ($Variables.TryGetValue($name, [ref]$value)) { return $value }
            $environmentValue = [Environment]::GetEnvironmentVariable($name)
            if ($null -ne $environmentValue) { return $environmentValue }
            return $match.Value
        })
        $expanded = [regex]::Replace($expanded, '!([^!]+)!', {
            param($match)
            $value = $null
            if ($Variables.TryGetValue($match.Groups[1].Value, [ref]$value)) { return $value }
            return $match.Value
        })
        if ($expanded -eq $before) { break }
    }
    return $expanded
}

function Get-LogicalBatchLines {
    param([string]$Path)
    $logical = New-Object System.Collections.Generic.List[string]
    $current = ''
    foreach ($physical in Get-Content -LiteralPath $Path) {
        $line = $physical
        if ($current) { $line = $current + $line.TrimStart() }
        if ($line -match '\^\s*$') {
            $current = [regex]::Replace($line, '\^\s*$', ' ')
            continue
        }
        $logical.Add($line)
        $current = ''
    }
    if ($current) { throw "The base start script ends with an unfinished caret continuation: $Path" }
    return $logical
}

function Assert-SafeArgumentText {
    param([string]$Text)
    if ($Text -match '[\r\n]') { throw 'The generated DayZ argument string contains a newline.' }
    if ($Text -match '%[^%]+%' -or $Text -match '![^!]+!') {
        throw "The DayZ launch arguments contain an unresolved batch variable: $Text"
    }
    if ($Text -match '[&|<>\^]') {
        throw 'The DayZ launch arguments contain a shell control character. Put plain DayZ arguments in DayZLaunch.ArgumentsOverride instead.'
    }
}

function Get-ArgumentsFromBatch {
    param([string]$Path)
    $variables = New-Object 'Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    $variables['~dp0'] = $serverRoot + '\'
    $fileName = [IO.Path]::GetFileName($serverExe)
    foreach ($rawLine in Get-LogicalBatchLines -Path $Path) {
        $trimmed = $rawLine.Trim()
        if (-not $trimmed -or $trimmed -match '^(?i:rem(?:\s|$)|::)') { continue }
        $setMatch = [regex]::Match($trimmed, '^(?i:set)\s+(?!/[ap]\b)(?:"([^"=]+)=(.*)"|([^=\s]+)=(.*))$')
        if ($setMatch.Success) {
            if ($setMatch.Groups[1].Success) {
                $name = $setMatch.Groups[1].Value.Trim()
                $value = $setMatch.Groups[2].Value
            } else {
                $name = $setMatch.Groups[3].Value.Trim()
                $value = $setMatch.Groups[4].Value.TrimEnd()
            }
            $variables[$name] = Expand-BatchVariables -Text $value -Variables $variables
            continue
        }
        $line = Expand-BatchVariables -Text $rawLine -Variables $variables
        $index = $line.IndexOf($fileName, [StringComparison]::OrdinalIgnoreCase)
        if ($index -lt 0) { continue }
        $text = $line.Substring($index + $fileName.Length).TrimStart([char[]]@('"', ' ', "`t"))
        Assert-SafeArgumentText -Text $text
        return $text
    }
    throw "No $fileName launch line was found in $Path. Set DayZLaunch.ArgumentsOverride or DayZLaunch.DefaultArguments."
}

function Get-BaseArgumentText {
    $override = $config.DayZLaunch.ArgumentsOverride.ToString().Trim()
    if ($override) {
        Assert-SafeArgumentText -Text $override
        return $override
    }
    if ($baseStartScript -and (Test-Path -LiteralPath $baseStartScript -PathType Leaf)) {
        return Get-ArgumentsFromBatch -Path $baseStartScript
    }
    $fallback = $config.DayZLaunch.DefaultArguments.ToString().Trim()
    if (-not $fallback) {
        throw 'BaseStartScript is missing and DayZLaunch.DefaultArguments is empty.'
    }
    Assert-SafeArgumentText -Text $fallback
    return $fallback
}

function Format-LaunchOption {
    param([string]$Name, [string]$Value)
    $text = "-$Name=$Value"
    if ($text -match '\s') { return '"' + $text + '"' }
    return $text
}

function Set-LaunchOption {
    param([string]$Text, [string]$Name, [string]$Value)
    $replacement = Format-LaunchOption -Name $Name -Value $Value
    $escapedName = [regex]::Escape($Name)
    $pattern = '(?i)(?<!\S)(?:"-' + $escapedName + '=[^"]*"|-' + $escapedName + '="[^"]*"|-' + $escapedName + '=[^"\s]+)'
    if ([regex]::IsMatch($Text, $pattern)) { return [regex]::Replace($Text, $pattern, $replacement, 1) }
    return ($Text.Trim() + ' ' + $replacement).Trim()
}

function Add-ManagedMods {
    param([string]$Text)
    $quoted = [regex]::Match($Text, '(?i)"-mod=([^"]*)"')
    $plain = [regex]::Match($Text, '(?i)(?<!\S)-mod=(\S+)')
    $existing = ''
    $match = $null
    if ($quoted.Success) { $match = $quoted; $existing = $quoted.Groups[1].Value }
    elseif ($plain.Success) { $match = $plain; $existing = $plain.Groups[1].Value.Trim('"') }
    $mods = New-Object System.Collections.Generic.List[string]
    foreach ($item in @($existing -split ';') + @($config.DayZLaunch.AdditionalMods) + @($modName)) {
        $value = if ($null -eq $item) { '' } else { $item.ToString().Trim() }
        if (-not $value) { continue }
        if (-not ($mods | Where-Object { $_.Equals($value, [StringComparison]::OrdinalIgnoreCase) })) { $mods.Add($value) }
    }
    $replacement = '"-mod=' + ($mods -join ';') + '"'
    if ($match -and $match.Success) {
        return $Text.Substring(0, $match.Index) + $replacement + $Text.Substring($match.Index + $match.Length)
    }
    return ($Text.Trim() + ' ' + $replacement).Trim()
}

function Get-ManagedArgumentText {
    $text = Get-BaseArgumentText
    if ($config.DayZLaunch.ServerConfig.ToString()) { $text = Set-LaunchOption $text 'config' $config.DayZLaunch.ServerConfig.ToString() }
    if ([int]$config.DayZLaunch.ServerPort -gt 0) { $text = Set-LaunchOption $text 'port' $config.DayZLaunch.ServerPort.ToString() }
    if ([int]$config.DayZLaunch.SteamPort -gt 0) { $text = Set-LaunchOption $text 'steamPort' $config.DayZLaunch.SteamPort.ToString() }
    if ([int]$config.DayZLaunch.SteamQueryPort -gt 0) { $text = Set-LaunchOption $text 'steamQueryPort' $config.DayZLaunch.SteamQueryPort.ToString() }
    if ($config.DayZLaunch.ProfilesDirectory.ToString()) { $text = Set-LaunchOption $text 'profiles' $config.DayZLaunch.ProfilesDirectory.ToString() }
    if ([int]$config.DayZLaunch.CpuCount -gt 0) { $text = Set-LaunchOption $text 'cpuCount' $config.DayZLaunch.CpuCount.ToString() }
    if ([int]$config.DayZLaunch.LimitFPS -gt 0) { $text = Set-LaunchOption $text 'limitFPS' $config.DayZLaunch.LimitFPS.ToString() }
    $text = Add-ManagedMods -Text $text
    foreach ($argument in @($config.DayZLaunch.AdditionalArguments)) {
        if ($argument -and $argument.ToString().Trim()) { $text += ' ' + $argument.ToString().Trim() }
    }
    Assert-SafeArgumentText -Text $text
    return $text.Trim()
}

function Get-OptionalValue {
    param([object]$Object, [string]$Name, [object]$Default = $null)
    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $Default
}

function Get-LaunchOptionValue {
    param([string]$Text, [string]$Name)
    $escaped = [regex]::Escape($Name)
    foreach ($pattern in @(
        ('(?i)(?:^|\s)"-' + $escaped + '=([^"\r\n]*)"(?=\s|$)'),
        ('(?i)(?:^|\s)-' + $escaped + '="([^"\r\n]*)"(?=\s|$)'),
        ('(?i)(?:^|\s)-' + $escaped + '=([^\s"]+)(?=\s|$)')
    )) {
        $match = [regex]::Match($Text, $pattern)
        if ($match.Success) { return $match.Groups[1].Value }
    }
    return ''
}

function Remove-DayZConfigComments {
    param([string]$Text)
    $withoutBlocks = [regex]::Replace($Text, '(?s)/\*.*?\*/', '')
    return [regex]::Replace($withoutBlocks, '(?m)//.*$', '')
}

function Get-HiveAnchor {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return '' }
    $rootItem = Get-Item -LiteralPath $Path -Force
    # Save timestamps and file contents change during normal persistence writes.
    # Creation identities for the hive root and stable top-level artifacts do
    # not. Including both catches a wipe that recreates contents in-place while
    # leaving the storage_N directory itself intact.
    $identities = New-Object System.Collections.Generic.List[string]
    $identities.Add('root|directory|' + $rootItem.CreationTimeUtc.Ticks)
    foreach ($relative in @('data','players.db','spawnpoints.bin')) {
        $candidate = Join-Path $Path $relative
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        $item = Get-Item -LiteralPath $candidate -Force
        $kind = if ($item.PSIsContainer) { 'directory' } else { 'file' }
        $identities.Add($relative.ToLowerInvariant() + '|' + $kind + '|' + $item.CreationTimeUtc.Ticks)
    }
    $identityText = (@($identities) | Sort-Object) -join "`n"
    $bytes = [Text.Encoding]::UTF8.GetBytes($identityText)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
}

function Test-HiveHasPersistence {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) { return $false }
    $file = Get-ChildItem -LiteralPath $Path -Recurse -Force -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne '.lock' -and $_.Length -gt 0 } | Select-Object -First 1
    return $null -ne $file
}

function Get-PersistenceInfo {
    $argumentText = Get-ManagedArgumentText
    $serverConfigValue = Get-LaunchOptionValue -Text $argumentText -Name 'config'
    if (-not $serverConfigValue) { $serverConfigValue = 'serverDZ.cfg' }
    $serverConfigPath = Resolve-AbsolutePath -Value $serverConfigValue -Base $serverRoot
    $configText = ''
    if (Test-Path -LiteralPath $serverConfigPath -PathType Leaf) {
        $configText = Remove-DayZConfigComments -Text (Get-Content -LiteralPath $serverConfigPath -Raw)
    }

    $missionOverride = Get-LaunchOptionValue -Text $argumentText -Name 'mission'
    $storageOverride = Get-LaunchOptionValue -Text $argumentText -Name 'storage'
    $configuredPath = $config.Persistence.Path.ToString().Trim()
    $automaticPath = -not $configuredPath -or $configuredPath.Equals('Auto', [StringComparison]::OrdinalIgnoreCase)
    $missionRoot = ''
    $missionTemplate = $config.Persistence.MissionTemplate.ToString().Trim()
    if ($missionOverride -and $automaticPath -and -not $storageOverride) {
        $missionCandidates = New-Object System.Collections.Generic.List[string]
        if ([IO.Path]::IsPathRooted($missionOverride)) {
            $missionCandidates.Add([IO.Path]::GetFullPath($missionOverride).TrimEnd('\'))
        } elseif ($missionOverride.IndexOf('\') -ge 0 -or $missionOverride.IndexOf('/') -ge 0) {
            $missionCandidates.Add((Resolve-AbsolutePath -Value $missionOverride -Base $serverRoot).TrimEnd('\'))
        } else {
            $directMission = [IO.Path]::GetFullPath((Join-Path $serverRoot $missionOverride)).TrimEnd('\')
            $mpMission = [IO.Path]::GetFullPath((Join-Path (Join-Path $serverRoot 'mpmissions') $missionOverride)).TrimEnd('\')
            if (Test-Path -LiteralPath $directMission -PathType Container) { $missionCandidates.Add($directMission) }
            if (Test-Path -LiteralPath $mpMission -PathType Container) { $missionCandidates.Add($mpMission) }
        }
        $existingMissions = @($missionCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container } | Select-Object -Unique)
        if ($existingMissions.Count -ne 1) {
            throw "The -mission value '$missionOverride' did not resolve to one mission directory. Set Persistence.Path explicitly."
        }
        $missionRoot = $existingMissions[0]
        $derivedTemplate = Split-Path -Leaf $missionRoot
        if ($missionTemplate -and -not $missionTemplate.Equals($derivedTemplate, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Persistence.MissionTemplate '$missionTemplate' conflicts with launch option -mission=$missionOverride."
        }
        $missionTemplate = $derivedTemplate
    } elseif ($missionOverride -and -not $missionTemplate) {
        $missionTemplate = Split-Path -Leaf $missionOverride.TrimEnd([char[]]@('\','/'))
    }
    if (-not $missionTemplate -and $configText) {
        $templates = @([regex]::Matches($configText, '(?im)^\s*template\s*=\s*"([^"]+)"\s*;') |
            ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
        if ($templates.Count -eq 1) { $missionTemplate = $templates[0] }
        elseif ($templates.Count -gt 1) { throw 'More than one mission template was found. Set Persistence.MissionTemplate explicitly.' }
    }

    $instanceId = [int]$config.Persistence.InstanceId
    if ($instanceId -le 0 -and $configText) {
        $instanceMatch = [regex]::Match($configText, '(?im)^\s*instanceId\s*=\s*(\d+)\s*;')
        if ($instanceMatch.Success) { $instanceId = [int]$instanceMatch.Groups[1].Value }
    }

    if ($configuredPath -and -not $configuredPath.Equals('Auto', [StringComparison]::OrdinalIgnoreCase)) {
        $hivePath = Resolve-AbsolutePath -Value $configuredPath -Base $serverRoot
    } elseif ($storageOverride) {
        if ($instanceId -le 0) { throw "Could not determine instanceId for -storage=$storageOverride. Set Persistence.InstanceId or Persistence.Path." }
        $storageRoot = Resolve-AbsolutePath -Value $storageOverride -Base $serverRoot
        $hivePath = Join-Path $storageRoot ("storage_$instanceId")
    } elseif ($missionRoot) {
        if ($instanceId -le 0) { throw "Could not determine instanceId for -mission=$missionOverride. Set Persistence.InstanceId or Persistence.Path." }
        $hivePath = Join-Path $missionRoot ("storage_$instanceId")
    } else {
        if (-not $missionTemplate) { throw "Could not determine the active mission template from $serverConfigPath. Set Persistence.MissionTemplate or Persistence.Path." }
        if ($instanceId -le 0) { throw "Could not determine instanceId from $serverConfigPath. Set Persistence.InstanceId or Persistence.Path." }
        $hivePath = Join-Path $serverRoot ("mpmissions\$missionTemplate\storage_$instanceId")
    }
    $hivePath = [IO.Path]::GetFullPath($hivePath).TrimEnd('\')
    return [pscustomobject][ordered]@{
        ServerConfigPath = $serverConfigPath
        MissionTemplate = $missionTemplate
        InstanceId = $instanceId
        HivePath = $hivePath
        HiveExists = (Test-Path -LiteralPath $hivePath -PathType Container)
        HasPersistence = (Test-HiveHasPersistence -Path $hivePath)
        HiveAnchor = (Get-HiveAnchor -Path $hivePath)
    }
}

function Get-LegacyProviderId {
    try {
        $settingsPath = Join-Path (Join-Path (Resolve-ProfileRoot) 'ClippyVirtualCargo') 'Settings.json'
        if (-not (Test-Path -LiteralPath $settingsPath -PathType Leaf)) { return '' }
        $settings = Read-JsonFile -Path $settingsPath
        $provider = Get-OptionalValue -Object $settings -Name 'ProviderID' -Default ''
        if ($provider) { return $provider.ToString().Trim() }
    } catch {
        Write-Warning "The previous virtual-cargo provider ID could not be inspected: $($_.Exception.Message)"
    }
    return ''
}

function New-NamespacedProviderId {
    param([string]$WorldGenerationId)
    $base = $config.VirtualCargoSettings.ProviderID.ToString().Trim().TrimEnd('.')
    if (-not $base) { $base = 'clippy.dayz.virtual-cargo' }
    return $base + '.world.' + $WorldGenerationId.Replace('-', '').ToLowerInvariant()
}

function Get-BackupRoot {
    $value = $config.Persistence.ColdBackupDirectory.ToString().Trim()
    if (-not $value) { $value = 'ClippyStorageHost/migration-backups' }
    return Resolve-AbsolutePath -Value $value -Base $serverRoot
}

function New-ColdPersistenceBackup {
    param([object]$Persistence, [object]$State)
    if (-not $Persistence.HasPersistence) { return $null }
    $source = [IO.Path]::GetFullPath($Persistence.HivePath).TrimEnd('\')
    $backupRoot = [IO.Path]::GetFullPath((Get-BackupRoot)).TrimEnd('\')
    if ($backupRoot.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Persistence.ColdBackupDirectory cannot be inside the active DayZ hive.'
    }
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $stamp = ([DateTime]::Parse($State.FirstDetectedUtc).ToUniversalTime().ToString('yyyyMMdd-HHmmss'))
    $destination = Assert-UnderRoot -Path (Join-Path $backupRoot ($stamp + '-' + $State.WorldGenerationId)) -Root $backupRoot
    $partial = Assert-UnderRoot -Path ($destination + '.partial') -Root $backupRoot
    if (Test-Path -LiteralPath $destination -PathType Container) {
        $manifestPath = Join-Path $destination 'MANIFEST.json'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Existing cold backup has no manifest: $destination" }
        return [pscustomobject]@{ Path = $destination; ManifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash }
    }
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }
    New-Item -ItemType Directory -Path $partial -Force | Out-Null
    Write-Host "Creating cold DayZ persistence backup: $destination" -ForegroundColor Cyan
    $entries = New-Object System.Collections.Generic.List[object]
    try {
        $all = @(Get-ChildItem -LiteralPath $source -Recurse -Force -ErrorAction Stop)
        $reparse = @($all | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
        if ($reparse.Count -gt 0) { throw "The persistence hive contains a reparse point and was not copied: $($reparse[0].FullName)" }
        foreach ($directory in @($all | Where-Object { $_.PSIsContainer })) {
            $relative = $directory.FullName.Substring($source.Length).TrimStart('\')
            if ($relative) { New-Item -ItemType Directory -Path (Join-Path $partial $relative) -Force | Out-Null }
        }
        foreach ($file in @($all | Where-Object { -not $_.PSIsContainer })) {
            $relative = $file.FullName.Substring($source.Length).TrimStart('\')
            $target = Join-Path $partial $relative
            $targetDirectory = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $targetDirectory -PathType Container)) { New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null }
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
            $entries.Add([pscustomobject][ordered]@{
                Path = $relative.Replace('\','/')
                Length = [long]$file.Length
                SHA256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            })
        }
        $manifest = [pscustomobject][ordered]@{
            FormatVersion = 1
            CreatedUtc = [DateTime]::UtcNow.ToString('o')
            SourceHive = $source
            MissionTemplate = $Persistence.MissionTemplate
            InstanceId = $Persistence.InstanceId
            WorldGenerationId = $State.WorldGenerationId
            Files = $entries.ToArray()
        }
        Write-JsonAtomic -Path (Join-Path $partial 'MANIFEST.json') -Document $manifest -Depth 20
        Move-Item -LiteralPath $partial -Destination $destination
    } catch {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force }
        throw
    }
    $finalManifest = Join-Path $destination 'MANIFEST.json'
    return [pscustomobject]@{ Path = $destination; ManifestHash = (Get-FileHash -LiteralPath $finalManifest -Algorithm SHA256).Hash }
}

function New-ManagerStatePlan {
    $persistence = Get-PersistenceInfo
    $now = [DateTime]::UtcNow.ToString('o')
    $previous = $null
    if (Test-Path -LiteralPath $installedManagerState -PathType Leaf) {
        $previous = Read-JsonFile -Path $installedManagerState
    }
    $sameHive = $false
    $worldChanged = $false
    if ($previous) {
        $previousHive = (Get-OptionalValue $previous 'HivePath' '').ToString()
        $sameHive = $previousHive.Equals($persistence.HivePath, [StringComparison]::OrdinalIgnoreCase)
        if ($sameHive) {
            $oldAnchor = (Get-OptionalValue $previous 'HiveAnchor' '').ToString()
            $oldHadPersistence = [bool](Get-OptionalValue $previous 'HadPersistence' $false)
            if ($oldAnchor -and $persistence.HiveAnchor -and $oldAnchor -ne $persistence.HiveAnchor) { $worldChanged = $true }
            if ($oldHadPersistence -and -not $persistence.HasPersistence) { $worldChanged = $true }
        } else {
            $worldChanged = $true
        }
    }

    if (-not $previous -or $worldChanged) {
        $worldGenerationId = [Guid]::NewGuid().ToString()
        $legacyProvider = if (-not $previous -and $persistence.HasPersistence) { Get-LegacyProviderId } else { '' }
        $effectiveProvider = if ($legacyProvider) { $legacyProvider } else { New-NamespacedProviderId -WorldGenerationId $worldGenerationId }
        $classification = if ($persistence.HasPersistence) {
            if ($worldChanged) { 'ExistingNewWorld' } else { 'Existing' }
        } else {
            if ($worldChanged) { 'FreshNewWorld' } else { 'Fresh' }
        }
        $state = [pscustomobject][ordered]@{
            StateVersion = 1
            InstallationId = if ($previous) { (Get-OptionalValue $previous 'InstallationId' ([Guid]::NewGuid().ToString())) } else { [Guid]::NewGuid().ToString() }
            ServerRoot = $serverRoot
            ServerConfigPath = $persistence.ServerConfigPath
            MissionTemplate = $persistence.MissionTemplate
            InstanceId = $persistence.InstanceId
            HivePath = $persistence.HivePath
            HiveAnchor = $persistence.HiveAnchor
            HadPersistence = [bool]$persistence.HasPersistence
            WorldGenerationId = $worldGenerationId
            EffectiveProviderID = $effectiveProvider
            Classification = $classification
            RequiresExistingCargoMigration = [bool]$persistence.HasPersistence
            FirstDetectedUtc = $now
            LastValidatedUtc = $now
            ColdBackupStatus = if ($persistence.HasPersistence) { 'Required' } else { 'NotRequired' }
            ColdBackupPath = ''
            ColdBackupManifestSHA256 = ''
        }
    } else {
        $state = $previous
        $state.ServerRoot = $serverRoot
        $state.ServerConfigPath = $persistence.ServerConfigPath
        $state.MissionTemplate = $persistence.MissionTemplate
        $state.InstanceId = $persistence.InstanceId
        $state.HivePath = $persistence.HivePath
        if (-not (Get-OptionalValue $state 'HiveAnchor' '') -and $persistence.HiveAnchor) { $state.HiveAnchor = $persistence.HiveAnchor }
        if ($persistence.HasPersistence) { $state.HadPersistence = $true }
        $state.Classification = if ([bool](Get-OptionalValue $state 'RequiresExistingCargoMigration' $false)) { 'ManagedExisting' } else { 'ManagedFresh' }
        $state.LastValidatedUtc = $now
    }
    return [pscustomobject]@{ State = $state; Persistence = $persistence }
}

function Initialize-ManagerState {
    param([switch]$Commit)
    $plan = New-ManagerStatePlan
    $state = $plan.State
    if ($Commit) {
        Write-JsonAtomic -Path $installedManagerState -Document $state
        if ([bool]$state.RequiresExistingCargoMigration -and $state.ColdBackupStatus -ne 'Complete') {
            $state.ColdBackupStatus = 'InProgress'
            Write-JsonAtomic -Path $installedManagerState -Document $state
            $backup = New-ColdPersistenceBackup -Persistence $plan.Persistence -State $state
            if (-not $backup) { throw 'Existing persistence was detected but no cold backup was created.' }
            $state.ColdBackupPath = $backup.Path
            $state.ColdBackupManifestSHA256 = $backup.ManifestHash
            $state.ColdBackupStatus = 'Complete'
            Write-JsonAtomic -Path $installedManagerState -Document $state
        }
    }
    $script:managerState = $state
    return $plan
}

function Run-WorkshopUpdater {
    if (-not [bool]$config.WorkshopUpdater.Enabled) { return }
    $script = Resolve-AbsolutePath -Value $config.WorkshopUpdater.Script.ToString() -Base $serverRoot
    if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
        Write-Warning "Configured Workshop updater was not found and was skipped: $script"
        return
    }
    Write-Host 'Running the existing Workshop updater...' -ForegroundColor Cyan
    & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $script
    if ($LASTEXITCODE -ne 0) { throw "Workshop updater failed with exit code $LASTEXITCODE." }
}

function Start-InstalledHost {
    $existing = Get-InstalledHostProcess
    if ($existing) {
        $document = Read-JsonFile -Path $installedHostConfig
        Wait-HostHealthy -HostDocument $document | Out-Null
        return $existing
    }
    $document = Read-JsonFile -Path $installedHostConfig
    $logRoot = Resolve-AbsolutePath -Value $config.Management.LogDirectory.ToString() -Base $serverRoot
    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
    $stdout = Join-Path $logRoot 'ClippyStorageHost.stdout.log'
    $stderr = Join-Path $logRoot 'ClippyStorageHost.stderr.log'
    $process = Start-Process -FilePath $installedHostExe -ArgumentList '--config','ClippyStorageHost.json' -WorkingDirectory $installedHost -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    try { Wait-HostHealthy -HostDocument $document | Out-Null } catch {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        throw
    }
    Write-Host "Storage host healthy on $($document.bindAddress):$($document.port), PID $($process.Id)." -ForegroundColor Green
    return $process
}

function Test-ClippySchemaReady {
    try {
        $database = $config.StorageHostSettings.PostgresDatabase.ToString()
        $secrets = Get-OrCreateSecrets
        $tables = ((Invoke-Psql -Database $database -Sql "SELECT CASE WHEN to_regclass('clippy.storage_containers') IS NOT NULL AND to_regclass('clippy.cargo_roots') IS NOT NULL AND to_regclass('clippy.schema_migrations') IS NOT NULL THEN 1 ELSE 0 END;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        if ($tables -ne '1') { return $false }
        $versionText = ((Invoke-Psql -Database $database -Sql "SELECT COALESCE(max(version),0) FROM clippy.schema_migrations;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        $version = 0
        if (-not [int]::TryParse($versionText, [ref]$version)) { return $false }
        return $version -ge 8
    } catch { return $false }
}

function Test-ClippyEditSchemaReady {
    try {
        $database = $config.StorageHostSettings.PostgresDatabase.ToString()
        $secrets = Get-OrCreateSecrets
        $versionText = ((Invoke-Psql -Database $database -Sql "SELECT COALESCE(max(version),0) FROM clippy.schema_migrations;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        $version = 0
        if (-not [int]::TryParse($versionText, [ref]$version) -or $version -lt 9) { return $false }
        $tables = ((Invoke-Psql -Database $database -Sql "SELECT CASE WHEN to_regclass('clippy.admin_container_locks') IS NOT NULL AND to_regclass('clippy.admin_change_sets') IS NOT NULL AND to_regclass('clippy.admin_quarantine') IS NOT NULL AND to_regclass('clippy.admin_storage_snapshots') IS NOT NULL AND to_regclass('clippy.admin_audit_events') IS NOT NULL THEN 1 ELSE 0 END;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        return $tables -eq '1'
    } catch { return $false }
}

function Test-ItemIndexComplete {
    try {
        $database = $config.StorageHostSettings.PostgresDatabase.ToString()
        $secrets = Get-OrCreateSecrets
        $exists = ((Invoke-Psql -Database $database -Sql "SELECT CASE WHEN to_regclass('clippy.cargo_item_index_state') IS NOT NULL THEN 1 ELSE 0 END;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        if ($exists -ne '1') { return $false }
        $complete = ((Invoke-Psql -Database $database -Sql "SELECT CASE WHEN complete THEN 1 ELSE 0 END FROM clippy.cargo_item_index_state WHERE state_id=1;" -Password $secrets.PostgresAdminPassword.ToString() -TuplesOnly) -join '').Trim()
        return $complete -eq '1'
    } catch { return $false }
}

function Test-StorageRuntimeCurrent {
    $payloadExe = Get-PayloadHostExecutable
    if (-not (Test-Path -LiteralPath $installedHostExe -PathType Leaf)) { return $false }
    return (Get-FileHash -LiteralPath $installedHostExe -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $payloadExe -Algorithm SHA256).Hash
}

function Get-ItemIndexStatus {
    param([object]$HostDocument)
    $result = Invoke-HostPost -Path '/v1/admin/item-index/status' -HostDocument $HostDocument -TimeoutSeconds 15
    if (-not $result.ok -or -not $result.data) { throw 'Cargo item index status query failed.' }
    return $result.data
}

function Complete-ItemIndexBackfill {
    param([object]$HostDocument)
    $status = Get-ItemIndexStatus -HostDocument $HostDocument
    if ([bool]$status.complete) {
        Write-Host 'Cargo item index is already complete.' -ForegroundColor Green
        return
    }

    Write-Host 'Backfilling cargo_item_index in small root batches. DayZ is stopped, so this maintenance cannot compete with gameplay storage.' -ForegroundColor Cyan
    $totalRoots = 0L
    $totalNodes = 0L
    $emptyPasses = 0
    $complete = $false
    while (-not $complete) {
        $batch = Invoke-HostPost -Path '/v1/admin/item-index/rebuild-batch' -HostDocument $HostDocument -Extra @{ root_limit = 4 } -TimeoutSeconds 30
        if (-not $batch.ok -or -not $batch.data) { throw 'Cargo item index backfill batch failed.' }
        $roots = [int64]$batch.data.roots_indexed
        $nodes = [int64]$batch.data.nodes_indexed
        $complete = [bool]$batch.data.complete
        $totalRoots += $roots
        $totalNodes += $nodes
        if ($roots -eq 0 -and -not $complete) {
            $emptyPasses++
            if ($emptyPasses -gt 20) { throw 'Cargo item index backfill made no progress. Check the storage-host log before retrying.' }
            Start-Sleep -Milliseconds 100
        } else {
            $emptyPasses = 0
        }
        if (($totalRoots % 100) -lt 4 -and $roots -gt 0) {
            Write-Host ("Cargo item index progress: " + $totalRoots + " roots, " + $totalNodes + " nodes indexed.") -ForegroundColor DarkCyan
        }
    }
    Write-Host ("Cargo item index backfill complete: " + $totalRoots + " roots, " + $totalNodes + " nodes added in this run.") -ForegroundColor Green
}

function Ensure-AdminRuntimeInstalled {
    $payloadExe = Get-PayloadAdminExecutable
    New-Item -ItemType Directory -Path $installedAdmin -Force | Out-Null
    $needsCopy = -not (Test-Path -LiteralPath $installedAdminExe -PathType Leaf)
    if (-not $needsCopy) {
        $needsCopy = (Get-FileHash -LiteralPath $installedAdminExe -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $payloadExe -Algorithm SHA256).Hash
    }
    if ($needsCopy) {
        Stop-InstalledAdmin
        Copy-Item -LiteralPath $payloadExe -Destination $installedAdminExe -Force
    }
    if ((Test-Path -LiteralPath $managerAdminLauncher -PathType Leaf) -and -not $managerAdminLauncher.Equals($installedAdminLauncher, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $managerAdminLauncher -Destination $installedAdminLauncher -Force
    }
}

function Wait-AdminReady {
    param([int]$Port)
    $deadline = [DateTime]::UtcNow.AddSeconds([int]$config.Management.HealthTimeoutSeconds)
    do {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:$Port/") -TimeoutSec 2
            if ([int]$response.StatusCode -eq 200) { return }
        } catch { }
        Start-Sleep -Milliseconds 200
    } until ([DateTime]::UtcNow -ge $deadline)
    throw "ClippyAdminHost did not become ready on 127.0.0.1:$Port."
}

function Start-ClippyAdminPanel {
    Assert-Configuration
    if (-not [bool]$config.AdminPanel.Enabled) { throw 'AdminPanel.Enabled is false in ClippyServerManager.json.' }

    $dayZ = Get-DayZProcess
    $missingStorageRuntime = -not (Test-Path -LiteralPath $installedHostExe -PathType Leaf) -or -not (Test-Path -LiteralPath $installedHostConfig -PathType Leaf)
    $storageRuntimeCurrent = $false

    if ($missingStorageRuntime) {
        if ($dayZ) { throw 'The Clippy storage runtime is not installed. Stop DayZ once and run the install command before opening the admin panel.' }
        Install-Payload | Out-Null
        $storageRuntimeCurrent = $true
    } else {
        $storageRuntimeCurrent = Test-StorageRuntimeCurrent
        if (-not $storageRuntimeCurrent) {
            if ($dayZ) {
                Write-Warning 'The installed ClippyStorageHost is older than this payload. It will not be replaced while DayZ is running. Nested class search will stay in the safe fallback mode until the server is stopped and the payload is installed.'
                Ensure-PostgreSQLInstalled
                Ensure-AdminRuntimeInstalled
                Sync-InstalledAdminConfiguration | Out-Null
            } else {
                Install-Payload | Out-Null
                $storageRuntimeCurrent = $true
            }
        } else {
            Ensure-PostgreSQLInstalled
            Ensure-AdminRuntimeInstalled
            Sync-InstalledAdminConfiguration | Out-Null
        }
    }

    $schemaReady = Test-ClippySchemaReady
    $startedHostForMaintenance = $false
    if (-not $schemaReady) {
        if ($dayZ) {
            Write-Warning 'PostgreSQL is still on the pre-index schema while DayZ is running. The admin panel will remain read-only and use its bounded fallback search until the next stopped-server install.'
        } elseif ($storageRuntimeCurrent) {
            $hostProcess = Get-InstalledHostProcess
            if (-not $hostProcess) {
                Start-InstalledHost | Out-Null
                $startedHostForMaintenance = $true
            } else {
                $document = Read-JsonFile -Path $installedHostConfig
                Wait-HostHealthy -HostDocument $document | Out-Null
            }
            if (-not (Test-ClippySchemaReady)) { throw 'Clippy PostgreSQL schema did not migrate to version 8.' }
            Ensure-PostgreSQLInstalled
            $schemaReady = $true
        }
    }

    if ($schemaReady -and $storageRuntimeCurrent -and -not (Test-ItemIndexComplete)) {
        if ($dayZ) {
            Write-Warning 'cargo_item_index backfill is incomplete. It is not being rebuilt while DayZ is running. Existing root search and exact item-ID lookup remain available.'
        } else {
            $hostProcess = Get-InstalledHostProcess
            if (-not $hostProcess) {
                Start-InstalledHost | Out-Null
                $startedHostForMaintenance = $true
            }
            $document = Read-JsonFile -Path $installedHostConfig
            Wait-HostHealthy -HostDocument $document | Out-Null
            try {
                Complete-ItemIndexBackfill -HostDocument $document
            } catch {
                Write-Warning ("cargo_item_index backfill did not complete: " + $_.Exception.Message + ". The admin panel will keep using its safe fallback search.")
            }
        }
    }

    $script:adminEditingAvailable = $false
    if (-not [bool]$config.AdminPanel.EnableEditing) {
        Write-Warning 'AdminPanel.EnableEditing is false in the existing ClippyServerManager.json. The panel will open read-only. Set it to true when you are ready to use maintenance-lock editing.'
    }
    if ([bool]$config.AdminPanel.EnableEditing) {
        $editSchemaReady = Test-ClippyEditSchemaReady
        if (-not $editSchemaReady) {
            if ($dayZ) {
                Write-Warning 'Admin editing requires PostgreSQL schema version 11 and the matching 1.0.0 StorageHost. The panel will open read-only until the server is stopped once and the payload is updated.'
            } elseif ($storageRuntimeCurrent) {
                $hostProcess = Get-InstalledHostProcess
                if (-not $hostProcess) {
                    Start-InstalledHost | Out-Null
                    $startedHostForMaintenance = $true
                } else {
                    $document = Read-JsonFile -Path $installedHostConfig
                    Wait-HostHealthy -HostDocument $document | Out-Null
                }
                if (-not (Test-ClippyEditSchemaReady)) { throw 'Clippy PostgreSQL schema did not migrate to version 9 required for safe admin editing.' }
                Ensure-PostgreSQLInstalled
                $editSchemaReady = $true
            }
        }
        if ($editSchemaReady -and $storageRuntimeCurrent) {
            Ensure-PostgreSQLInstalled
            $script:adminEditingAvailable = $true
        }
    }

    if ($startedHostForMaintenance -and -not $dayZ) {
        Stop-InstalledHost -AllowForce
    }

    Ensure-AdminRuntimeInstalled
    Sync-InstalledAdminConfiguration | Out-Null
    Stop-InstalledAdmin

    $token = New-RandomToken
    $bootstrapFile = Join-Path $installedAdmin ('.admin-bootstrap-' + [Guid]::NewGuid().ToString('N') + '.txt')
    Write-TextAtomic -Path $bootstrapFile -Text $token
    Protect-SensitiveFile -Path $bootstrapFile

    $logRoot = Resolve-AbsolutePath -Value $config.Management.LogDirectory.ToString() -Base $serverRoot
    New-Item -ItemType Directory -Path $logRoot -Force | Out-Null
    $stdout = Join-Path $logRoot 'ClippyAdminHost.stdout.log'
    $stderr = Join-Path $logRoot 'ClippyAdminHost.stderr.log'
    $process = $null
    try {
        $adminArguments = @(
            '--config'
            'ClippyAdminHost.json'
            '--bootstrap-file'
            ([IO.Path]::GetFileName($bootstrapFile))
        )
        $process = Start-Process -FilePath $installedAdminExe -ArgumentList $adminArguments -WorkingDirectory $installedAdmin -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        Wait-AdminReady -Port ([int]$config.AdminPanel.Port)
    } catch {
        if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        if (Test-Path -LiteralPath $bootstrapFile -PathType Leaf) { Remove-Item -LiteralPath $bootstrapFile -Force }
        throw
    }

    $url = "http://127.0.0.1:$([int]$config.AdminPanel.Port)/#bootstrap=$token"
    if ($env:CLIPPY_ADMIN_URL_FILE) {
        Write-TextAtomic -Path $env:CLIPPY_ADMIN_URL_FILE -Text ($url + [Environment]::NewLine)
        Protect-SensitiveFile -Path $env:CLIPPY_ADMIN_URL_FILE
    } elseif ([bool]$config.AdminPanel.AutoOpenBrowser) {
        Start-Process $url | Out-Null
    }
    Write-Host "Clippy Admin Panel started on 127.0.0.1:$([int]$config.AdminPanel.Port), PID $($process.Id)." -ForegroundColor Green
    if ($script:adminEditingAvailable) {
        Write-Host 'Admin editing is enabled with maintenance locks, revision checks, audit history, and undo. PostgreSQL remains private on loopback.' -ForegroundColor Green
    } else {
        Write-Host 'Admin Panel is running read-only. PostgreSQL remains private on loopback.' -ForegroundColor Green
    }
}

function Invoke-IntegrityCheck {
    $document = Read-JsonFile -Path $installedHostConfig
    $result = Invoke-HostPost -Path '/v1/admin/integrity' -HostDocument $document

    if (-not $result) {
        throw 'PostgreSQL integrity check returned no response.'
    }

    $okProperty = $result.PSObject.Properties['ok']
    if (-not $okProperty -or -not [bool]$okProperty.Value) {
        throw 'PostgreSQL integrity check failed.'
    }

    $dataProperty = $result.PSObject.Properties['data']
    if (-not $dataProperty -or -not $dataProperty.Value) {
        throw 'PostgreSQL integrity response did not contain data.'
    }
    $data = $dataProperty.Value

    $healthyProperty = $data.PSObject.Properties['healthy']
    if (-not $healthyProperty -or -not [bool]$healthyProperty.Value) {
        throw 'PostgreSQL integrity check reported an unhealthy database.'
    }

    $summary = New-Object System.Collections.Generic.List[string]

    $checksProperty = $data.PSObject.Properties['checks']
    if ($checksProperty -and $checksProperty.Value) {
        foreach ($check in @($checksProperty.Value)) {
            if (-not $check) { continue }

            $nameProperty = $check.PSObject.Properties['check']
            $errorsProperty = $check.PSObject.Properties['errors']

            $name = if ($nameProperty -and $nameProperty.Value) {
                $nameProperty.Value.ToString()
            } else {
                'unnamed_check'
            }

            $errors = 0
            if ($errorsProperty -and $null -ne $errorsProperty.Value) {
                $errors = [int64]$errorsProperty.Value
            }

            if ($errors -eq 0) {
                $summary.Add($name + '=OK')
            } else {
                $summary.Add($name + '=' + $errors + ' error(s)')
            }
        }
    } else {
        # Compatibility with older host builds that returned a string array named results.
        $resultsProperty = $data.PSObject.Properties['results']
        if ($resultsProperty -and $resultsProperty.Value) {
            foreach ($item in @($resultsProperty.Value)) {
                if ($null -ne $item) {
                    $summary.Add($item.ToString())
                }
            }
        }
    }

    if ($summary.Count -eq 0) {
        $summary.Add('healthy')
    }

    Write-Host ('PostgreSQL integrity: ' + ($summary -join ', ')) -ForegroundColor Green
    return $result
}

function Invoke-OnlineBackup {
    $document = Read-JsonFile -Path $installedHostConfig
    $result = Invoke-HostPost -Path '/v1/admin/backup' -HostDocument $document
    if (-not $result.ok) { throw 'PostgreSQL online backup failed.' }
    Write-Host "PostgreSQL backup: $($result.data.path)" -ForegroundColor Green
    return $result
}

function Invoke-PostgresBackupRestore {
    param([string]$BackupFile)

    Assert-Configuration
    if (Get-DayZProcess) { throw 'Refusing to restore PostgreSQL while DayZ is running. Stop DayZ first.' }
    if (-not $BackupFile) { throw 'Specify a Clippy backup filename, for example: ClippyServerManager.ps1 restore-backup ClippyVirtualCargo-1234567890123-89abcdef.dump' }
    $backupName = [IO.Path]::GetFileName($BackupFile.Trim())
    if ($backupName -ne $BackupFile.Trim() -or $backupName -notmatch '^ClippyVirtualCargo-[0-9]{10,20}-[0-9a-fA-F]{8}\.dump$') {
        throw 'Restore accepts only a Clippy-generated backup filename from the configured backup directory.'
    }

    Ensure-PostgreSQLInstalled
    $secrets = Get-OrCreateSecrets
    $dbName = Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresDatabase.ToString() -Label 'PostgresDatabase'
    $appRole = Assert-SafePostgresName -Value $config.StorageHostSettings.PostgresUser.ToString() -Label 'PostgresUser'
    $backupRoot = Resolve-InstalledHostPath -Value $config.StorageHostSettings.BackupDirectory.ToString()
    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    $backupPath = Assert-UnderRoot -Path (Join-Path $backupRoot $backupName) -Root $backupRoot
    if (-not (Test-Path -LiteralPath $backupPath -PathType Leaf) -or (Get-Item -LiteralPath $backupPath).Length -le 0) {
        throw "Restore backup is missing or empty: $backupPath"
    }

    $pgDump = Join-Path $postgresBin 'pg_dump.exe'
    $pgRestore = Join-Path $postgresBin 'pg_restore.exe'
    $connectionArgs = @('--host','127.0.0.1','--port',$postgresPort.ToString())
    Invoke-PostgresTool -Tool $pgRestore -Arguments @('--list',$backupPath) -Password $secrets.PostgresAppPassword.ToString() | Out-Null

    $targetHash = (Get-FileHash -LiteralPath $backupPath -Algorithm SHA256).Hash
    Write-Host ''
    Write-Host 'DANGER: PostgreSQL restore will replace the current Clippy virtual-cargo database.' -ForegroundColor Red
    Write-Host "Target backup: $backupName" -ForegroundColor Yellow
    Write-Host "Target SHA256: $targetHash" -ForegroundColor DarkGray
    Write-Host 'DayZ must remain stopped until this command finishes.' -ForegroundColor Yellow
    $confirmation = Read-Host "Type RESTORE $backupName to continue"
    if ($confirmation -cne "RESTORE $backupName") { throw 'Restore cancelled because the confirmation text did not match exactly.' }

    if (Get-InstalledAdminProcess) {
        Write-Host 'Stopping Clippy Admin Panel for database maintenance...' -ForegroundColor Cyan
        Stop-InstalledAdmin
    }
    if (Get-InstalledHostProcess) {
        Write-Host 'Stopping ClippyStorageHost for database maintenance...' -ForegroundColor Cyan
        Stop-InstalledHost -AllowForce
    }
    if (Get-DayZProcess) { throw 'DayZ started while restore preparation was running. Restore cancelled.' }

    $createdMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $randomSuffix = (New-RandomToken).Substring(0,8)
    $safetyName = "ClippyVirtualCargo-$createdMs-$randomSuffix.dump"
    $safetyPath = Assert-UnderRoot -Path (Join-Path $backupRoot $safetyName) -Root $backupRoot
    $journalPath = Assert-UnderRoot -Path (Join-Path $backupRoot ("restore-$createdMs-$randomSuffix.txt")) -Root $backupRoot

    Write-Host "Creating fresh pre-restore safety backup: $safetyName" -ForegroundColor Cyan
    $dumpArgs = $connectionArgs + @('--username',$appRole,'--dbname',$dbName,'--format=custom','--compress=6','--no-owner','--no-privileges','--file',$safetyPath)
    Invoke-PostgresTool -Tool $pgDump -Arguments $dumpArgs -Password $secrets.PostgresAppPassword.ToString() | Out-Null
    if (-not (Test-Path -LiteralPath $safetyPath -PathType Leaf) -or (Get-Item -LiteralPath $safetyPath).Length -le 0) { throw 'The pre-restore safety backup was not created.' }
    Invoke-PostgresTool -Tool $pgRestore -Arguments @('--list',$safetyPath) -Password $secrets.PostgresAppPassword.ToString() | Out-Null
    $safetyHash = (Get-FileHash -LiteralPath $safetyPath -Algorithm SHA256).Hash

    $journal = @"
Clippy Virtual Cargo PostgreSQL restore
Started UTC: $([DateTime]::UtcNow.ToString('o'))
Target: $backupName
Target SHA256: $targetHash
Safety backup: $safetyName
Safety SHA256: $safetyHash
Database: $dbName
Result: STARTED
"@
    Write-TextAtomic -Path $journalPath -Text $journal

    try {
        Invoke-Psql -Sql "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$dbName' AND pid<>pg_backend_pid();" -Password $secrets.PostgresAdminPassword.ToString() | Out-Null
        if ((Get-DayZProcess) -or (Get-InstalledHostProcess) -or (Get-InstalledAdminProcess)) { throw 'A Clippy/DayZ process restarted during restore preparation. Restore cancelled.' }

        Write-Host 'Restoring PostgreSQL backup...' -ForegroundColor Cyan
        $restoreArgs = $connectionArgs + @('--username',$appRole,'--dbname',$dbName,'--clean','--if-exists','--no-owner','--no-privileges','--exit-on-error',$backupPath)
        Invoke-PostgresTool -Tool $pgRestore -Arguments $restoreArgs -Password $secrets.PostgresAppPassword.ToString() | Out-Null

        # Reapply restricted service/admin role grants that are intentionally omitted from portable pg_dump archives.
        Ensure-PostgreSQLInstalled

        if (-not (Test-Path -LiteralPath $installedHostExe -PathType Leaf)) { throw 'Restore completed, but ClippyStorageHost.exe is missing so post-restore validation could not run.' }
        Write-Host 'Starting StorageHost temporarily for schema migration and integrity validation...' -ForegroundColor Cyan
        Start-InstalledHost | Out-Null
        Invoke-IntegrityCheck | Out-Null
        Stop-InstalledHost -AllowForce

        $finished = $journal.Replace('Result: STARTED','Result: SUCCESS') + "Finished UTC: $([DateTime]::UtcNow.ToString('o'))`r`n"
        Write-TextAtomic -Path $journalPath -Text $finished
        Write-Host "Restore completed and passed the Clippy integrity check. Safety backup: $safetyPath" -ForegroundColor Green
        Write-Host 'StorageHost and DayZ were left stopped. Start the server normally when ready.' -ForegroundColor Green
    } catch {
        try {
            if (Get-InstalledHostProcess) { Stop-InstalledHost -AllowForce }
        } catch { }
        $failed = $journal.Replace('Result: STARTED','Result: FAILED') + "Failed UTC: $([DateTime]::UtcNow.ToString('o'))`r`nError: $($_.Exception.Message)`r`n"
        Write-TextAtomic -Path $journalPath -Text $failed
        throw "PostgreSQL restore failed. The fresh safety backup is $safetyPath. Details were written to $journalPath. $($_.Exception.Message)"
    }
}

function Show-Status {
    $dayZ = Get-DayZProcess
    $hostProcess = Get-InstalledHostProcess
    $adminProcess = Get-InstalledAdminProcess
    Write-Host ('DayZ: ' + $(if ($dayZ) { "running, PID $($dayZ.Id)" } else { 'stopped' }))
    Write-Host ('Storage host: ' + $(if ($hostProcess) { "running, PID $($hostProcess.Id)" } else { 'stopped' }))
    Write-Host ('Admin panel: ' + $(if ($adminProcess) { "running, PID $($adminProcess.Id)" } else { 'stopped' }))
    if ($hostProcess -and (Test-Path -LiteralPath $installedHostConfig -PathType Leaf)) {
        if (-not $script:managerState) { Initialize-ManagerState | Out-Null }
        $document = Read-JsonFile -Path $installedHostConfig
        $health = Invoke-HostPost -Path '/v1/health' -HostDocument $document
        $metrics = Invoke-HostPost -Path '/v1/metrics' -HostDocument $document -Extra @{
            provider_id = $script:managerState.EffectiveProviderID
        }
        Write-Host ('Database healthy: ' + $health.data.database_healthy)
        Write-Host ('Containers: ' + $metrics.data.containers + '; item nodes: ' + $metrics.data.item_nodes + '; incomplete operations: ' + $metrics.data.incomplete_operations + '; incomplete migrations: ' + $metrics.data.incomplete_migrations)
    }
    if (Test-Path -LiteralPath $installedManagerState -PathType Leaf) {
        $state = Read-JsonFile -Path $installedManagerState
        Write-Host ('World generation: ' + $state.WorldGenerationId + '; classification: ' + $state.Classification)
        Write-Host ('Existing cargo migration: ' + $(if ([bool]$state.RequiresExistingCargoMigration) { 'automatic' } else { 'not required' }))
        if ($state.ColdBackupPath) { Write-Host ('Cold persistence backup: ' + $state.ColdBackupPath) }
    }
}

function Start-ManagedServer {
    Assert-Configuration
    if (Get-DayZProcess) { throw 'This DayZ server is already running.' }
    if ([bool]$config.Management.InstallOrUpdatePayloadOnStart) { $hostDocument = Install-Payload } else { $hostDocument = Sync-InstalledConfiguration }
    Run-WorkshopUpdater
    $hostProcess = Start-InstalledHost
    if ((Test-ClippySchemaReady) -and -not (Test-ItemIndexComplete)) {
        if (Test-StorageRuntimeCurrent) {
            try {
                Complete-ItemIndexBackfill -HostDocument $hostDocument
            } catch {
                Write-Warning ("cargo_item_index backfill did not complete: " + $_.Exception.Message + ". DayZ startup will continue and the admin panel will use fallback search until the index can be rebuilt.")
            }
        } else {
            Write-Warning 'cargo_item_index backfill is incomplete, but the installed StorageHost is older than this payload. Backfill is deferred until the current payload is installed.'
        }
    }
    if ([bool]$config.Management.RunIntegrityCheck) { Invoke-IntegrityCheck | Out-Null }
    if ([bool]$config.Management.BackupBeforeServerStart) { Invoke-OnlineBackup | Out-Null }
    $argumentText = Get-ManagedArgumentText
    Write-Host "Launching $serverExe" -ForegroundColor Cyan
    Write-Host "Arguments: $argumentText" -ForegroundColor DarkGray

    $restartCount = 0
    try {
        do {
            $dayZ = Start-Process -FilePath $serverExe -ArgumentList @($argumentText) -WorkingDirectory $serverRoot -PassThru
            Write-Host "DayZ started as PID $($dayZ.Id)." -ForegroundColor Green
            $lastHeartbeat = [DateTime]::UtcNow.AddSeconds(-[int]$config.Management.HeartbeatSeconds)
            while (-not $dayZ.HasExited) {
                Start-Sleep -Seconds 2
                $hostProcess = Get-InstalledHostProcess
                if (-not $hostProcess -and [bool]$config.Management.RestartHostOnFailure) {
                    Write-Warning 'Storage host stopped while DayZ was running. Restarting it.'
                    $hostProcess = Start-InstalledHost
                }
                if (([DateTime]::UtcNow - $lastHeartbeat).TotalSeconds -ge [int]$config.Management.HeartbeatSeconds) {
                    $state = if ($hostProcess) { "host PID $($hostProcess.Id)" } else { 'HOST DOWN' }
                    Write-Host "[Manager] DayZ PID $($dayZ.Id) alive; $state; $([DateTime]::Now.ToString('HH:mm:ss'))." -ForegroundColor DarkGreen
                    $lastHeartbeat = [DateTime]::UtcNow
                }
            }
            $exitCode = $dayZ.ExitCode
            Write-Host "DayZ exited with code $exitCode." -ForegroundColor Yellow
            $restart = $exitCode -ne 0 -and [bool]$config.Management.RestartServerOnCrash -and
                       $restartCount -lt [int]$config.Management.MaximumAutomaticRestarts
            if ($restart) {
                $restartCount++
                Write-Host "Restarting DayZ in $($config.Management.RestartDelaySeconds) seconds ($restartCount/$($config.Management.MaximumAutomaticRestarts))." -ForegroundColor Yellow
                Start-Sleep -Seconds ([int]$config.Management.RestartDelaySeconds)
            }
        } while ($restart)
    } finally {
        if ([bool]$config.Management.BackupAfterServerStops -and (Get-InstalledHostProcess)) {
            try { Invoke-OnlineBackup | Out-Null } catch { Write-Warning "Post-stop backup failed: $($_.Exception.Message)" }
        }
        if ([bool]$config.Management.StopHostAfterServerStops) {
            try { Stop-InstalledHost -AllowForce } catch { Write-Warning $_.Exception.Message }
        }
    }
    if ($exitCode -ne 0) { exit $exitCode }
}

switch ($command) {
    'start' { Start-ManagedServer }
    'install' {
        Assert-Configuration
        Install-Payload | Out-Null
    }
    'status' { Show-Status }
    'admin' { Start-ClippyAdminPanel }
    'check' {
        if (-not (Get-InstalledHostProcess)) { Start-InstalledHost | Out-Null }
        Invoke-IntegrityCheck | Out-Null
    }
    'backup' {
        if (-not (Get-InstalledHostProcess)) { Start-InstalledHost | Out-Null }
        Invoke-OnlineBackup | Out-Null
    }
    'restore-backup' {
        $restoreFile = if ($Value) { $Value } elseif ($env:CLIPPY_RESTORE_FILE) { $env:CLIPPY_RESTORE_FILE } else { '' }
        Invoke-PostgresBackupRestore -BackupFile $restoreFile
    }
    'stop-host' {
        if (Get-DayZProcess) { throw 'Refusing to stop the storage host while DayZ is running.' }
        Stop-InstalledHost -AllowForce
        Write-Host 'Storage host stopped.' -ForegroundColor Green
    }
    'validate' {
        Assert-Configuration
        $arguments = Get-ManagedArgumentText
        $plan = Initialize-ManagerState
        Write-Host 'Configuration and payload validation passed.' -ForegroundColor Green
        Write-Host "Server root: $serverRoot"
        Write-Host "Host port: $($config.StorageHostSettings.Port)"
        Write-Host "Admin port: $($config.AdminPanel.Port)"
        Write-Host "Persistence: $($plan.Persistence.HivePath)"
        Write-Host "Detection: $($plan.State.Classification)"
        Write-Host ('Automatic existing-cargo migration: ' + $(if ([bool]$plan.State.RequiresExistingCargoMigration) { 'required' } else { 'not required' }))
        Write-Host "Generated arguments: $arguments"
    }
    'help' {
        Write-Host 'Commands: start, install, admin, status, check, backup, restore-backup, stop-host, validate, help'
        Write-Host 'Edit ClippyServerManager.json to change server, port, database, launch, persistence, and backup settings.'
    }
    default { throw "Unknown manager command '$command'. Use help for the command list." }
}
