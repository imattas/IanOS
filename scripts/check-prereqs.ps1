$ErrorActionPreference = "Stop"

$checks = @(
    @{ Name = "clang++"; Path = "C:\Program Files\LLVM\bin\clang++.exe" },
    @{ Name = "ld.lld"; Path = "C:\Program Files\LLVM\bin\ld.lld.exe" },
    @{ Name = "lld-link"; Path = "C:\Program Files\LLVM\bin\lld-link.exe" },
    @{ Name = "cmake"; Path = "C:\Program Files\CMake\bin\cmake.exe" },
    @{ Name = "qemu-system-x86_64"; Path = "C:\Program Files\qemu\qemu-system-x86_64.exe" },
    @{ Name = "nasm"; Path = "$env:LOCALAPPDATA\bin\NASM\nasm.exe" },
    @{ Name = "py"; Path = "$env:LOCALAPPDATA\Programs\Python\Launcher\py.exe" }
)

foreach ($check in $checks) {
    $cmd = Get-Command "$($check.Name).exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        Write-Host "$($check.Name): $($cmd.Source)"
    } elseif (Test-Path $check.Path) {
        Write-Host "$($check.Name): $($check.Path)"
    } else {
        throw "$($check.Name) not found"
    }
}

$ovmf = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
if (-not (Test-Path $ovmf)) { throw "OVMF not found at $ovmf" }
Write-Host "OVMF: $ovmf"
