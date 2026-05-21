# Baut cdripper nativ auf Windows mit MSVC + vcpkg + CMake.
#
# Voraussetzungen (in der Windows-VM, einmalig — siehe windows/SETUP.md):
#   1. Visual Studio 2022 Build Tools mit C++ workload
#   2. vcpkg unter C:\vcpkg, bootstrapped:
#        git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
#        C:\vcpkg\bootstrap-vcpkg.bat
#        C:\vcpkg\vcpkg integrate install
#   3. CMake (kommt mit VS Build Tools) im PATH
#   4. Git, PowerShell 7+
#
# Output:
#   build-windows\cdripper.exe            (eigentlicher Build)
#   build-windows\<deps>.dll              (Qt, libcdio, …)
#   tools\flac\bin\flac.exe etc.          (von fetch-cli-tools.ps1)
#
# Anschließend scripts\build-msi.ps1 für den WiX-Installer.

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$src = (Get-Location).Path

# Version aus engine.h ableiten — selbe Quelle wie alle anderen Pipelines.
$version = (Select-String -Path engine.h `
    -Pattern 'VERSION = "([0-9]+\.[0-9]+\.[0-9]+)"' `
    | Select-Object -First 1).Matches.Groups[1].Value
Write-Host ">>> cdripper $version → Windows x64"

# vcpkg
$vcpkgRoot = $env:VCPKG_ROOT
if (-not $vcpkgRoot) { $vcpkgRoot = "C:\vcpkg" }
$toolchain = "$vcpkgRoot\scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain nicht gefunden ($toolchain). Setze `$env:VCPKG_ROOT oder installiere vcpkg unter C:\vcpkg."
}

# Externe CLI-Tools holen (idempotent — überspringt vorhandene Downloads).
& "$PSScriptRoot\fetch-cli-tools.ps1"

# Configure + Build
$build = "$src\build-windows"
cmake -S $src -B $build `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE=$toolchain `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_INSTALL_PREFIX="$build\install"

cmake --build $build --config Release -j

# windeployqt: Qt-DLLs ins Build-Verzeichnis kopieren
$qtBin = "$vcpkgRoot\installed\x64-windows\tools\Qt6\bin"
if (Test-Path "$qtBin\windeployqt.exe") {
    Write-Host ">>> windeployqt"
    & "$qtBin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler `
        --no-quick-import "$build\Release\cdripper.exe"
}

Write-Host
Write-Host "Fertig: $build\Release\cdripper.exe"
Write-Host "Test:  $build\Release\cdripper.exe --help"
