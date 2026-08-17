Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$managerScript = [IO.Path]::GetFullPath($env:CLIPPY_MANAGER_SCRIPT)
$managerPowerShell = [IO.Path]::GetFullPath($MyInvocation.MyCommand.Path)
$managerRoot = [IO.Path]::GetFullPath($env:CLIPPY_MANAGER_ROOT).TrimEnd('\')
$command = $env:CLIPPY_MANAGER_COMMAND.ToLowerInvariant()
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
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $Base $Value))
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
  "ConfigVersion": 5,
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
    "MaintenancePruneBatchRows": 500, "MaintenanceIntervalSeconds": 300,
    "MaxRequestBytes": 2097152, "MaxItemNodes": 4096,
    "MaxPageNodes": 256, "MaxItemDepth": 16
  },
  "PostgreSQL": {
    "Version": "18.4-1", "Port": 27816, "ServiceName": "ClippyPostgreSQL18",
    "InstallDirectory": "ClippyPostgreSQL/18", "DataDirectory": "ClippyPostgreSQL/data",
    "InstallerUrl": "https://get.enterprisedb.com/postgresql/postgresql-18.4-1-windows-x64.exe",
    "InstallerSHA256": "44B8187D2DB7E866495952D8260A1D7252CBB5125843142E1F0BF30115D23279"
  },
  "VirtualCargoSettings": {
    "Version": 5, "Enabled": true, "ConnectionTimeoutSeconds": 5,
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
    $initial.ConfigVersion = 5
    $initial.VirtualCargoSettings.Version = 5
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
$payloadSettings = Join-Path $payloadRoot 'ServerProfileTemplate\ClippyVirtualCargo\Settings.json'
$installedMod = Join-Path $serverRoot $modName
$installedHost = Join-Path $serverRoot 'ClippyStorageHost'
$installedHostExe = Join-Path $installedHost 'ClippyStorageHost.exe'
$installedHostConfig = Join-Path $installedHost 'ClippyStorageHost.json'
$installedManager = Join-Path $serverRoot 'START-CLIPPY-SERVER.bat'
$installedManagerPowerShell = Join-Path $serverRoot 'ClippyServerManager.ps1'
$installedManagerConfig = Join-Path $serverRoot 'ClippyServerManager.json'
$installedManagerState = Join-Path $serverRoot 'ClippyServerManager.state.json'
$installedSecrets = Join-Path $installedHost '.clippy-secrets.json'
$postgresInstall = Resolve-AbsolutePath -Value $config.PostgreSQL.InstallDirectory.ToString() -Base $serverRoot
$postgresData = Resolve-AbsolutePath -Value $config.PostgreSQL.DataDirectory.ToString() -Base $serverRoot
$postgresBin = Join-Path $postgresInstall 'bin'
$postgresPsql = Join-Path $postgresBin 'psql.exe'
$postgresCreatedb = Join-Path $postgresBin 'createdb.exe'
$postgresLibpq = Join-Path $postgresBin 'libpq.dll'
$postgresServiceName = $config.PostgreSQL.ServiceName.ToString()
$postgresPort = [int]$config.PostgreSQL.Port
$payloadHostExe = Join-Path $payloadHost 'ClippyStorageHost.exe'
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

    foreach ($pair in @(@('API token',$apiToken), @('PostgreSQL application password',$appPassword), @('PostgreSQL administrator password',$adminPassword))) {
        if ($pair[1].Length -lt 32 -or $pair[1].Length -gt 256) { throw "$($pair[0]) must contain 32 to 256 characters." }
    }
    $document = [pscustomobject][ordered]@{
        Version = 1
        ApiToken = $apiToken
        PostgresAppPassword = $appPassword
        PostgresAdminPassword = $adminPassword
    }
    Write-JsonAtomic -Path $installedSecrets -Document $document -Depth 10
    Protect-SensitiveFile -Path $installedSecrets
    $script:secrets = $document
    return $document
}

function Get-PostgresService {
    return Get-Service -Name $postgresServiceName -ErrorAction SilentlyContinue
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
        if (Test-Path -LiteralPath $postgresInstall) {
            $entries = @(Get-ChildItem -LiteralPath $postgresInstall -Force -ErrorAction SilentlyContinue)
            if ($entries.Count -gt 0) { throw "PostgreSQL install directory exists without the expected Windows service: $postgresInstall" }
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
            '--serviceaccount','clippy_pg_svc','--servicepassword',$secrets.PostgresAdminPassword.ToString(),
            '--enable_acledit','1',
            '--enable-components','server,commandlinetools',
            '--create_shortcuts','0'
        )
        Write-Host 'Installing the private PostgreSQL instance...' -ForegroundColor Cyan
        & $installer @installArgs
        if ($LASTEXITCODE -ne 0) { throw "PostgreSQL installer failed with exit code $LASTEXITCODE." }
        Assert-PostgresServiceOwnership
        if (-not (Get-PostgresService)) { throw "PostgreSQL installer did not create service '$postgresServiceName'." }
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
    $expectedVersion = '0.5.0'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifest = Read-JsonFile -Path $manifestPath
        $expectedHash = $manifest.HostExeSHA256.ToString().Trim().ToUpperInvariant()
        if ($expectedHash -notmatch '^[0-9A-F]{64}$') { throw 'Payload HostExeSHA256 is missing or invalid.' }
        if ((Get-FileHash -LiteralPath $payloadHostExe -Algorithm SHA256).Hash -ne $expectedHash) {
            throw 'Prebuilt ClippyStorageHost.exe hash does not match the release manifest.'
        }
        $expectedVersion = $manifest.Version.ToString()
    }
    $help = & $payloadHostExe '--help' 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0 -or (($help | Out-String) -notmatch [regex]::Escape("ClippyStorageHost $expectedVersion"))) {
        throw 'Prebuilt ClippyStorageHost.exe failed its version smoke test.'
    }
    return $payloadHostExe
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
    if ($manifest.WorkshopID.ToString() -ne $workshopId -or $manifest.Version.ToString() -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { throw 'Staged payload identity is invalid.' }
    $signatureFile = $manifest.SignatureFile.ToString()
    $keyFile = $manifest.KeyFile.ToString()
    if ($signatureFile -notmatch '^[A-Za-z0-9_.-]+\.bisign$' -or $keyFile -notmatch '^[A-Za-z0-9_.-]+\.bikey$') { throw 'Staged payload signature or public-key filename is invalid.' }
    $checks = @{
        (Join-Path $payloadRoot "$modName\addons\clippy_virtual_cargo.pbo") = $manifest.PboSHA256.ToString()
        (Join-Path $payloadRoot "$modName\addons\$signatureFile") = $manifest.SignatureSHA256.ToString()
        (Join-Path $payloadRoot "$modName\keys\$keyFile") = $manifest.KeySHA256.ToString()
        (Join-Path $payloadRoot 'ServerProfileTemplate\ClippyVirtualCargo\Settings.json') = $manifest.SettingsSHA256.ToString()
        (Join-Path $payloadRoot 'ClippyStorageHost\ClippyStorageHost.exe') = $manifest.HostExeSHA256.ToString()
        $managerScript = $manifest.ManagerSHA256.ToString()
        $managerPowerShell = $manifest.ManagerPowerShellSHA256.ToString()
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
    $releaseVersion = (Read-JsonFile -Path $manifestPath).Version.ToString()
    if ($releaseVersion -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$') { return $false }
    if ($workshopBuild.Version.ToString() -ne $releaseVersion) { return $false }
    $expectedPboHash = $workshopBuild.PboSHA256.ToString().Trim().ToUpperInvariant()
    if ($expectedPboHash -notmatch '^[0-9A-F]{64}$') { return $false }
    return (Get-FileHash -LiteralPath $installedPbo -Algorithm SHA256).Hash -eq $expectedPboHash
}

function Assert-Configuration {
    foreach ($required in @($serverRoot, $serverExe, $payloadMod, $payloadHost, $payloadHostExe,
            (Join-Path $payloadMod 'addons\clippy_virtual_cargo.pbo'), $payloadSettings)) {
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
    if ([int]$config.ConfigVersion -ne 5) { throw 'Unsupported ClippyServerManager.json ConfigVersion.' }
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
        maintenancePruneBatchRows = [int]$config.StorageHostSettings.MaintenancePruneBatchRows
        maintenanceIntervalSeconds = [int]$config.StorageHostSettings.MaintenanceIntervalSeconds
        maxRequestBytes = [int]$config.StorageHostSettings.MaxRequestBytes
        maxItemNodes = [int]$config.StorageHostSettings.MaxItemNodes
        maxPageNodes = [int]$config.StorageHostSettings.MaxPageNodes
        maxItemDepth = [int]$config.StorageHostSettings.MaxItemDepth
    }
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
    Ensure-PostgreSQLInstalled

    if (Test-InstalledCurrentWorkshopMod) {
        Write-Host 'Detected an installed 0.5.0 Workshop PBO. Preserving it instead of replacing it with the signed compatibility fallback.' -ForegroundColor Green
    } else {
        Copy-DirectoryAtomic -Source $payloadMod -Target $installedMod
    }
    New-Item -ItemType Directory -Path $installedHost -Force | Out-Null
    Copy-Item -LiteralPath $payloadHostExe -Destination $installedHostExe -Force
    foreach ($name in @('LICENSE.txt','THIRD-PARTY-NOTICES.md')) {
        $source = Join-Path $payloadHost $name
        if (Test-Path -LiteralPath $source -PathType Leaf) { Copy-Item -LiteralPath $source -Destination $installedHost -Force }
    }

    $serverKeys = Join-Path $serverRoot 'keys'
    New-Item -ItemType Directory -Path $serverKeys -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $installedMod 'keys') -File -Filter '*.bikey' | Copy-Item -Destination $serverKeys -Force

    if (-not $managerScript.Equals($installedManager, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $managerScript -Destination $installedManager -Force }
    if (-not $managerPowerShell.Equals($installedManagerPowerShell, [StringComparison]::OrdinalIgnoreCase)) { Copy-Item -LiteralPath $managerPowerShell -Destination $installedManagerPowerShell -Force }
    if (-not (Test-Path -LiteralPath $installedManagerConfig -PathType Leaf)) { Copy-Item -LiteralPath $configPath -Destination $installedManagerConfig -Force }
    $managerReadme = Join-Path $managerRoot 'ClippyVirtualCargoDocs\SERVER-MANAGER-README.md'
    if (-not (Test-Path -LiteralPath $managerReadme -PathType Leaf)) { $managerReadme = Join-Path $managerRoot 'SERVER-MANAGER-README.md' }
    if (Test-Path -LiteralPath $managerReadme -PathType Leaf) { Copy-Item -LiteralPath $managerReadme -Destination (Join-Path $serverRoot 'CLIPPY-SERVER-MANAGER-README.md') -Force }

    $desiredHostDocument = New-DesiredHostDocument
    $hostDocument = Sync-InstalledConfiguration -HostDocument $desiredHostDocument
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

function Invoke-IntegrityCheck {
    $document = Read-JsonFile -Path $installedHostConfig
    $result = Invoke-HostPost -Path '/v1/admin/integrity' -HostDocument $document
    if (-not $result.ok -or -not $result.data.healthy) { throw 'PostgreSQL integrity check failed.' }
    Write-Host ('PostgreSQL integrity: ' + ($result.data.results -join ', ')) -ForegroundColor Green
    return $result
}

function Invoke-OnlineBackup {
    $document = Read-JsonFile -Path $installedHostConfig
    $result = Invoke-HostPost -Path '/v1/admin/backup' -HostDocument $document
    if (-not $result.ok) { throw 'PostgreSQL online backup failed.' }
    Write-Host "PostgreSQL backup: $($result.data.path)" -ForegroundColor Green
    return $result
}

function Show-Status {
    $dayZ = Get-DayZProcess
    $hostProcess = Get-InstalledHostProcess
    Write-Host ('DayZ: ' + $(if ($dayZ) { "running, PID $($dayZ.Id)" } else { 'stopped' }))
    Write-Host ('Storage host: ' + $(if ($hostProcess) { "running, PID $($hostProcess.Id)" } else { 'stopped' }))
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
    'check' {
        if (-not (Get-InstalledHostProcess)) { Start-InstalledHost | Out-Null }
        Invoke-IntegrityCheck | Out-Null
    }
    'backup' {
        if (-not (Get-InstalledHostProcess)) { Start-InstalledHost | Out-Null }
        Invoke-OnlineBackup | Out-Null
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
        Write-Host "Persistence: $($plan.Persistence.HivePath)"
        Write-Host "Detection: $($plan.State.Classification)"
        Write-Host ('Automatic existing-cargo migration: ' + $(if ([bool]$plan.State.RequiresExistingCargoMigration) { 'required' } else { 'not required' }))
        Write-Host "Generated arguments: $arguments"
    }
    'help' {
        Write-Host 'Commands: start, install, status, check, backup, stop-host, validate, help'
        Write-Host 'Edit ClippyServerManager.json to change server, port, database, launch, persistence, and backup settings.'
    }
    default { throw "Unknown manager command '$command'. Use help for the command list." }
}
