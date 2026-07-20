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

$escapedLog = [regex]::Escape($SerialLog)
$escapedImage = [regex]::Escape((Join-Path $RepoRoot "build\out\image\kernel.img"))
$remaining = Get-CimInstance Win32_Process -Filter "name = 'qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and ($_.CommandLine -match $escapedLog -or $_.CommandLine -match $escapedImage) }
foreach ($process in $remaining) {
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
}
$remaining = Get-CimInstance Win32_Process -Filter "name = 'qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and ($_.CommandLine -match $escapedLog -or $_.CommandLine -match $escapedImage) }
if ($remaining) {
    Write-Warning "Workspace QEMU process remained after verification; boot log validation already passed."
}

Write-Host "IanOS verification passed."
