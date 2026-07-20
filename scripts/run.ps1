param(
    [int]$MemoryMb = 256,
    [int]$TimeoutSeconds = 0,
    [switch]$NoBuild,
    [switch]$Headless,
    [switch]$BootScript,
    [switch]$Recovery
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"
$EspDir = Join-Path $BuildDir "esp"
$Qemu = (Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue)
if (-not $Qemu) {
    $candidate = "C:\Program Files\qemu\qemu-system-x86_64.exe"
    if (Test-Path $candidate) { $Qemu = Get-Item $candidate }
}
if (-not $Qemu) { throw "qemu-system-x86_64.exe was not found." }

$QemuRoot = Split-Path -Parent $Qemu.Source
$OvmfCode = Join-Path $QemuRoot "share\edk2-x86_64-code.fd"
$OvmfVarsTemplate = Join-Path $QemuRoot "share\edk2-i386-vars.fd"
if (-not (Test-Path $OvmfCode)) { throw "OVMF code image not found at $OvmfCode" }
if (-not (Test-Path $OvmfVarsTemplate)) { throw "OVMF vars template not found at $OvmfVarsTemplate" }

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build.ps1")
}

if ($Headless -and $TimeoutSeconds -le 0) {
    $TimeoutSeconds = 360
}

$Vars = Join-Path $BuildDir "OVMF_VARS.fd"
Copy-Item $OvmfVarsTemplate $Vars -Force

$SerialLog = Join-Path $BuildDir "qemu-serial.log"
$BootDir = Join-Path $EspDir "boot"
$BootScriptMarker = Join-Path $BootDir "runtests"
$RecoveryMarker = Join-Path $BootDir "recovery"
$DiskImage = Join-Path $BuildDir "out\image\kernel.img"
New-Item -ItemType Directory -Force -Path $BootDir | Out-Null
$ForceBootScript = $BootScript -or $env:HYBRID_BOOT_SCRIPT -eq "1"
$ForceRecovery = $Recovery -or $env:HYBRID_RECOVERY_BOOT -eq "1"
$PreserveBootMarker = $env:HYBRID_PRESERVE_BOOT_MARKER -eq "1"
if ($ForceBootScript) {
    Set-Content -Path $BootScriptMarker -Value "1" -NoNewline -Encoding ASCII
} elseif ((-not $PreserveBootMarker) -and (Test-Path $BootScriptMarker)) {
    Remove-Item $BootScriptMarker -Force
}
if ($ForceRecovery) {
    Set-Content -Path $RecoveryMarker -Value "1" -NoNewline -Encoding ASCII
} elseif ((-not $PreserveBootMarker) -and (Test-Path $RecoveryMarker)) {
    Remove-Item $RecoveryMarker -Force
}

$Python = (Get-Command py.exe -ErrorAction SilentlyContinue)
if (-not $Python) { throw "py.exe was not found." }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DiskImage) | Out-Null
& $Python.Source -B (Join-Path $RepoRoot "tools\make_fat_image.py") $EspDir $DiskImage
if ($LASTEXITCODE -ne 0) { throw "Failed to build FAT disk image $DiskImage" }
& $Python.Source -B (Join-Path $RepoRoot "tools\image_report.py") $EspDir $DiskImage
if ($LASTEXITCODE -ne 0) { throw "Failed to report FAT disk image $DiskImage" }

function Stop-StaleQemuForWorkspace {
    $escapedLog = [regex]::Escape($SerialLog)
    $escapedEsp = [regex]::Escape($EspDir)
    $escapedImage = [regex]::Escape($DiskImage)
    Get-CimInstance Win32_Process -Filter "name = 'qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -match $escapedLog -or $_.CommandLine -match $escapedEsp -or $_.CommandLine -match $escapedImage } |
        ForEach-Object {
            Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        }
}

Stop-StaleQemuForWorkspace
if (Test-Path $SerialLog) {
    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        try {
            Remove-Item $SerialLog -Force
            break
        } catch {
            if ($attempt -eq 9) { throw }
            Start-Sleep -Milliseconds 200
            Stop-StaleQemuForWorkspace
        }
    }
}

$args = @(
    "-machine", "q35",
    "-cpu", "qemu64,+apic",
    "-smp", "2",
    "-m", "$MemoryMb",
    "-drive", "if=pflash,format=raw,readonly=on,file=$OvmfCode",
    "-drive", "if=pflash,format=raw,file=$Vars",
    "-drive", "format=raw,file=$DiskImage",
    "-serial", "file:$SerialLog",
    "-display", $(if ($Headless) { "none" } else { "gtk" }),
    "-no-reboot"
)

function Quote-Arg([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + ($Value -replace '"', '\"') + '"'
}

$proc = $null
try {
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Qemu.Source
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.Arguments = ($args | ForEach-Object { Quote-Arg $_ }) -join " "
    if (-not $Headless) {
        Write-Host "Starting interactive QEMU. Use the QEMU window for keyboard input after the ianos> prompt."
        Write-Host "Close the QEMU window to end the run. Serial log: $SerialLog"
    }
    $proc = [System.Diagnostics.Process]::Start($psi)
    if ($TimeoutSeconds -gt 0) {
        if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }
    } else {
        $proc.WaitForExit()
    }
} finally {
    if ($proc -and -not $proc.HasExited) {
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit()
    }
}

Write-Host "QEMU run complete. Serial log: $SerialLog"
