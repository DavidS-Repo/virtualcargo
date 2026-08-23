[CmdletBinding()]
param(
    [switch]$Force,
    [int]$DayZTimeoutSeconds = 45,
    [int]$HostTimeoutSeconds = 15,
    [int]$PostgresTimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $elevatedArguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + [IO.Path]::GetFullPath($PSCommandPath) + '"'))
    if ($Force) { $elevatedArguments += '-Force' }
    $child = Start-Process -FilePath 'powershell.exe' -ArgumentList $elevatedArguments -Verb RunAs -Wait -PassThru
    exit $child.ExitCode
}

$root = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$managerConfigPath = Join-Path $root 'ClippyServerManager.json'
$config = $null
if (Test-Path -LiteralPath $managerConfigPath -PathType Leaf) {
    $config = Get-Content -LiteralPath $managerConfigPath -Raw | ConvertFrom-Json
}

function Resolve-ConfiguredPath {
    param([string]$Value, [string]$Base)
    if (-not $Value -or $Value.Trim() -eq '' -or $Value.Trim() -eq 'Auto') { return [IO.Path]::GetFullPath($Base) }
    $expanded = [Environment]::ExpandEnvironmentVariables($Value.Trim())
    if ([IO.Path]::IsPathRooted($expanded)) { return [IO.Path]::GetFullPath($expanded) }
    return [IO.Path]::GetFullPath((Join-Path $Base $expanded))
}

$serverRoot = Resolve-ConfiguredPath -Value $(if ($config) { [string]$config.ServerRoot } else { 'Auto' }) -Base $root
$serverExecutableName = if ($config -and $config.ServerExecutable) { [string]$config.ServerExecutable } else { 'DayZServer_x64.exe' }
$serverExecutable = Resolve-ConfiguredPath -Value $serverExecutableName -Base $serverRoot
$hostExecutable = Join-Path $serverRoot 'ClippyStorageHost\ClippyStorageHost.exe'
$adminExecutable = Join-Path $serverRoot 'ClippyAdmin\ClippyAdminHost.exe'
$hostConfigPath = Join-Path $serverRoot 'ClippyStorageHost\ClippyStorageHost.json'
$postgresServiceName = if ($config -and $config.PostgreSQL -and $config.PostgreSQL.ServiceName) { [string]$config.PostgreSQL.ServiceName } else { 'ClippyPostgreSQL18' }

function Get-ExactProcess {
    param([string]$ExecutablePath)
    if (-not (Test-Path -LiteralPath $ExecutablePath -PathType Leaf)) { return $null }
    $expected = [IO.Path]::GetFullPath($ExecutablePath)
    $name = [IO.Path]::GetFileName($expected)
    foreach ($candidate in @(Get-CimInstance Win32_Process -Filter "Name='$name'" -ErrorAction SilentlyContinue)) {
        $matchesPath = $candidate.ExecutablePath -and [IO.Path]::GetFullPath($candidate.ExecutablePath).Equals($expected, [StringComparison]::OrdinalIgnoreCase)
        if ($matchesPath) { return Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue }
    }
    return $null
}

function Wait-ProcessStopped {
    param([System.Diagnostics.Process]$Process, [int]$TimeoutSeconds, [string]$Label)
    if (-not $Process) { return $true }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ($Process.HasExited) { return $true }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)
    Write-Warning "$Label did not exit within $TimeoutSeconds seconds."
    return $false
}

function Stop-ProcessCleanly {
    param([System.Diagnostics.Process]$Process, [string]$Label, [int]$TimeoutSeconds)
    if (-not $Process) { return $true }
    Write-Host "Stopping $Label (PID $($Process.Id))..." -ForegroundColor Cyan
    try {
        if ($Process.MainWindowHandle -ne 0) { [void]$Process.CloseMainWindow() }
    } catch { }
    if (Wait-ProcessStopped -Process $Process -TimeoutSeconds $TimeoutSeconds -Label $Label) { return $true }
    if (-not $Force) {
        throw "$Label is still running. Re-run with -Force only after confirming a forced stop is acceptable."
    }
    Write-Warning "Forcing $Label (PID $($Process.Id))."
    Stop-Process -Id $Process.Id -Force -ErrorAction Stop
    return (Wait-ProcessStopped -Process $Process -TimeoutSeconds 10 -Label $Label)
}

function Invoke-HostRequest {
    param([string]$Path, [int]$TimeoutSeconds)
    if (-not (Test-Path -LiteralPath $hostConfigPath -PathType Leaf)) { return $false }
    $document = Get-Content -LiteralPath $hostConfigPath -Raw | ConvertFrom-Json
    $address = [string]$document.bindAddress
    if ($address -eq 'localhost') { $address = '127.0.0.1' }
    if ($address -eq '::1') { $address = '[::1]' }
    $body = @{ api_token = [string]$document.apiToken; request_id = [Guid]::NewGuid().ToString('N') } | ConvertTo-Json -Depth 10
    try {
        Invoke-RestMethod -Method Post -Uri ("http://$address`:$([int]$document.port)$Path") -ContentType 'application/json' -Body $body -TimeoutSec $TimeoutSeconds | Out-Null
        return $true
    } catch {
        Write-Warning "StorageHost request $Path was unavailable: $($_.Exception.Message)"
        return $false
    }
}

Write-Host 'Clippy coordinated shutdown' -ForegroundColor Cyan
Write-Host "Server root: $serverRoot" -ForegroundColor DarkGray

$dayZ = Get-ExactProcess -ExecutablePath $serverExecutable
if ($dayZ) {
    if (-not (Stop-ProcessCleanly -Process $dayZ -Label 'DayZServer' -TimeoutSeconds $DayZTimeoutSeconds)) {
        throw 'DayZServer did not stop.'
    }
} else {
    Write-Host 'DayZServer is already stopped.' -ForegroundColor DarkGray
}

$admin = Get-ExactProcess -ExecutablePath $adminExecutable
if ($admin) {
    [void](Stop-ProcessCleanly -Process $admin -Label 'ClippyAdminHost' -TimeoutSeconds $HostTimeoutSeconds)
} else {
    Write-Host 'ClippyAdminHost is already stopped.' -ForegroundColor DarkGray
}

$hostProcess = Get-ExactProcess -ExecutablePath $hostExecutable
if ($hostProcess) {
    if (Invoke-HostRequest -Path '/v1/admin/backup' -TimeoutSeconds $HostTimeoutSeconds) {
        Write-Host 'StorageHost online backup completed.' -ForegroundColor Green
    }
    [void](Invoke-HostRequest -Path '/v1/admin/shutdown' -TimeoutSeconds 5)
    if (-not (Wait-ProcessStopped -Process $hostProcess -TimeoutSeconds $HostTimeoutSeconds -Label 'ClippyStorageHost')) {
        if (-not $Force) { throw 'ClippyStorageHost did not stop cleanly. Re-run with -Force only after checking the host log.' }
        Write-Warning "Forcing ClippyStorageHost (PID $($hostProcess.Id))."
        Stop-Process -Id $hostProcess.Id -Force -ErrorAction Stop
    }
} else {
    Write-Host 'ClippyStorageHost is already stopped.' -ForegroundColor DarkGray
}

$service = Get-Service -Name $postgresServiceName -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    Write-Host "Stopping PostgreSQL service $postgresServiceName..." -ForegroundColor Cyan
    Stop-Service -Name $postgresServiceName -ErrorAction Stop
    (Get-Service -Name $postgresServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds($PostgresTimeoutSeconds))
} elseif ($service) {
    Write-Host "PostgreSQL service $postgresServiceName is already stopped." -ForegroundColor DarkGray
} else {
    Write-Warning "PostgreSQL service $postgresServiceName was not found; no PostgreSQL process was terminated."
}

$remaining = @(
    (Get-ExactProcess -ExecutablePath $serverExecutable),
    (Get-ExactProcess -ExecutablePath $adminExecutable),
    (Get-ExactProcess -ExecutablePath $hostExecutable)
) | Where-Object { $_ }
if ($remaining.Count -gt 0) {
    throw 'One or more targeted Clippy/DayZ processes are still running.'
}

Write-Host 'Clippy, DayZ, and PostgreSQL shutdown completed.' -ForegroundColor Green
