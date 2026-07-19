param(
    [int]$MemoryMb = 256,
    [int]$TimeoutSeconds = 0,
    [switch]$NoBuild,
    [switch]$Headless,
    [switch]$BootScript
)

$ErrorActionPreference = "Stop"
$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Launcher = Join-Path $ScriptRoot "scripts\run.ps1"

& $Launcher -MemoryMb $MemoryMb -TimeoutSeconds $TimeoutSeconds -NoBuild:$NoBuild -Headless:$Headless -BootScript:$BootScript
