param(
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"
$CMake = (Get-Command cmake.exe -ErrorAction Stop).Source
$Nasm = (Get-Command nasm.exe -ErrorAction SilentlyContinue)
if (-not $Nasm) {
    $candidate = Join-Path $env:LOCALAPPDATA "bin\NASM\nasm.exe"
    if (Test-Path $candidate) { $Nasm = Get-Item $candidate }
}
if (-not $Nasm) { throw "NASM was not found on PATH or in the expected local install path." }

& $CMake -S $RepoRoot -B $BuildDir -G Ninja "-DCMAKE_ASM_NASM_COMPILER=$($Nasm.Source)" "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
$BootDir = Join-Path $BuildDir "esp\boot"
foreach ($Marker in @("runtests", "recovery")) {
    $MarkerPath = Join-Path $BootDir $Marker
    if (Test-Path $MarkerPath) { Remove-Item $MarkerPath -Force }
}
& $CMake --build $BuildDir --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
& $CMake --build $BuildDir --target test --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "CMake test target failed with exit code $LASTEXITCODE" }

$OutDir = Join-Path $BuildDir "out"
$Image = Join-Path $OutDir "image\kernel.img"
if (-not (Test-Path $Image)) { throw "Expected final image was not created at $Image" }
$LegacyImage = Join-Path $OutDir "kernel.img"
if (Test-Path $LegacyImage) { Remove-Item $LegacyImage -Force }
Write-Host "Final image: $Image"
