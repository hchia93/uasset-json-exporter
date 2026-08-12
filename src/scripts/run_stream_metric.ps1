# Launches an unattended editor running stream_metric_probe.py and waits for the result JSON.
# Never saves the level, the AlwaysLoaded to Dynamic swap stays in memory.
#
# The probe needs to know which level to open as the persistent one. Pass -PersistentLevel, or let
# this script derive it by running AuditLevelTopology first.
#
# Requires: no UnrealEditor already running.

param(
    [string]$PersistentLevel = "",
    [string]$ScanDir = "/Game",
    [switch]$SkipExport,
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = "Stop"

# <Project>/Plugins/UAssetWorkbench/scripts/this.ps1
$ProjectRoot = Resolve-Path "$PSScriptRoot\..\..\.."
$Uproject = Get-ChildItem -Path $ProjectRoot -Filter *.uproject -File | Select-Object -First 1
if ($null -eq $Uproject) {
    Write-Error "No .uproject found under $ProjectRoot"
}

$ProbeScript = Join-Path $PSScriptRoot "stream_metric_probe.py"
$ResultPath = Join-Path $ProjectRoot "Saved\StreamMetric\stream_metric_result.json"
$EditorLog = Join-Path $ProjectRoot "Saved\StreamMetric\probe_editor.log"
$TopologyReport = Join-Path $ProjectRoot "Intermediate\AuditLevelTopology\stream_metric_topology.json"

if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) {
    Write-Error "UnrealEditor is running, close it first."
}

$Assoc = (Get-Content $Uproject.FullName -Raw | ConvertFrom-Json).EngineAssociation
$EnginePath = (Get-ItemProperty "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Assoc").InstalledDirectory
$EditorExe = Join-Path $EnginePath "Engine\Binaries\Win64\UnrealEditor.exe"
$EditorCmdExe = Join-Path $EnginePath "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

# Topology always runs, the target's sublevel list is what drives the LevelExport step.
Write-Output "Running AuditLevelTopology..."
New-Item -ItemType Directory -Force (Split-Path $TopologyReport) | Out-Null
& $EditorCmdExe $Uproject.FullName -run=AuditLevelTopology "-scandir=$ScanDir" "-report=$TopologyReport" -unattended -nullrhi -nosplash | Out-Null
if (-not (Test-Path $TopologyReport)) {
    Write-Error "AuditLevelTopology produced no report at $TopologyReport"
}

$Topology = Get-Content $TopologyReport -Raw | ConvertFrom-Json

if ($PersistentLevel -eq "") {
    $Hosts = @($Topology.results | Where-Object { $_.role -eq "PersistentHost" })
    if ($Hosts.Count -ne 1) {
        Write-Error "topology lists $($Hosts.Count) PersistentHost levels, pass -PersistentLevel to pick one: $(($Hosts | ForEach-Object { $_.level }) -join ', ')"
    }
    $PersistentLevel = $Hosts[0].level
    Write-Output "Derived persistent level: $PersistentLevel"
}

$env:UAW_PERSISTENT_LEVEL = $PersistentLevel
$env:UAW_TOPOLOGY_REPORT = $TopologyReport

$Target = $Topology.results | Where-Object { $_.level -eq $PersistentLevel }
if ($null -eq $Target) {
    Write-Error "$PersistentLevel is not in the topology report, check -ScanDir"
}

$Sublevels = @($Target.streaming_levels)
if ($Sublevels.Count -eq 0) {
    Write-Error "$PersistentLevel streams no sublevels, nothing to measure"
}

if (-not $SkipExport) {
    Write-Output "Exporting $($Sublevels.Count) sublevel(s) for scale data..."
    & $EditorCmdExe $Uproject.FullName -run=LevelExport "-assets=$($Sublevels -join ',')" -unattended -nullrhi -nosplash | Out-Null
}

if (Test-Path $ResultPath) { Remove-Item $ResultPath -Force }
New-Item -ItemType Directory -Force (Split-Path $ResultPath) | Out-Null

$EditorArgs = @(
    "`"$($Uproject.FullName)`"",
    "-ExecCmds=`"py $ProbeScript`"",
    "-nosplash",
    "-unattended",
    # unfocused editor throttles to ~1fps, which kills streaming progress and frame metrics
    "-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False",
    "-abslog=`"$EditorLog`""
)
Write-Output "Launching probe editor..."
$Proc = Start-Process -FilePath $EditorExe -ArgumentList $EditorArgs -PassThru

$Elapsed = 0
while ($Elapsed -lt $TimeoutSec) {
    Start-Sleep -Seconds 10
    $Elapsed += 10
    if ($Proc.HasExited) { break }
    if (Test-Path $ResultPath) {
        try {
            $Status = (Get-Content $ResultPath -Raw | ConvertFrom-Json).status
            if ($Status -ne "running") { break }
        } catch {}
    }
}

if (-not $Proc.HasExited) {
    Write-Output "Waiting for editor to exit..."
    if (-not $Proc.WaitForExit(60000)) {
        Write-Output "Force killing probe editor (timeout)."
        Stop-Process -Id $Proc.Id -Force -Confirm:$false
    }
}

if (-not (Test-Path $ResultPath)) {
    Write-Output "No result file produced, check $EditorLog"
    exit 1
}

Write-Output "=== RESULT ==="
& python (Join-Path $PSScriptRoot "stream_metric_report.py") --project $ProjectRoot
if ($LASTEXITCODE -ne 0) {
    Write-Output "Report step failed, raw timings are at $ResultPath"
    exit 1
}
