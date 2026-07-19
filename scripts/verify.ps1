param(
    [int]$TimeoutSeconds = 45
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Python = (Get-Command py.exe -ErrorAction Stop).Source
$SerialLog = Join-Path $RepoRoot "build\qemu-serial.log"

& (Join-Path $PSScriptRoot "check-prereqs.ps1")
& (Join-Path $PSScriptRoot "build.ps1")
& (Join-Path $PSScriptRoot "run.ps1") -TimeoutSeconds $TimeoutSeconds -NoBuild -Headless -BootScript
& $Python -B (Join-Path $RepoRoot "tools\validate_boot_log.py") $SerialLog
if ($LASTEXITCODE -ne 0) { throw "Boot log validation failed with exit code $LASTEXITCODE" }

$qemu = Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue
if ($qemu) {
    $qemu | Stop-Process -Force
    throw "QEMU process was still running after verification"
}

Write-Host "IanOS verification passed."
