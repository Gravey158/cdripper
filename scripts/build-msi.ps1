# Baut den .msi-Installer aus build-windows\Release\ + tools\.
# Voraussetzung: WiX Toolset v6 installiert via dotnet tool:
#   dotnet tool install --global wix
# (Erstmal mit dotnet SDK 8+; siehe windows\SETUP.md.)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$version = (Select-String -Path engine.h `
    -Pattern 'VERSION = "([0-9]+\.[0-9]+\.[0-9]+)"' `
    | Select-Object -First 1).Matches.Groups[1].Value
$buildDir = "build-windows\Release"
$toolsDir = "tools"
$wxs = "windows\cdripper.wxs"
$genFiles = "windows\generated-files.wxs"
$genTools = "windows\generated-tools.wxs"
$out = "build-windows\cdripper-$version-windows-x64.msi"

if (-not (Test-Path "$buildDir\cdripper.exe")) {
    throw "$buildDir\cdripper.exe fehlt — build-windows.ps1 zuerst laufen lassen."
}

# WiX heat: dynamisches Harvesting der build-Output-Dateien als Component-
# Group. Das spart manuelles Pflegen aller bundled DLLs/Plugins.
Write-Host ">>> heat: $buildDir → $genFiles"
& wix heat dir $buildDir -nologo `
    -cg cdripperFiles -dr INSTALLFOLDER -gg -ke -sfrag -srd `
    -var var.SourceDir -out $genFiles

Write-Host ">>> heat: $toolsDir → $genTools"
& wix heat dir $toolsDir -nologo `
    -cg cdripperTools -dr ToolsFolder -gg -ke -sfrag -srd `
    -var var.ToolsDir -out $genTools

Write-Host ">>> wix build"
& wix build $wxs $genFiles $genTools `
    -d "Version=$version" `
    -d "SourceDir=$buildDir" `
    -d "ToolsDir=$toolsDir" `
    -o $out

Write-Host
Write-Host "Fertig: $out"
Write-Host "  SHA256: $((Get-FileHash $out -Algorithm SHA256).Hash)"
