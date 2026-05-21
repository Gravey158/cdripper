# Veröffentlicht eine Windows-Release: pusht das .msi via scp auf
# flatpak.x2-pandora.de/cdripper/ + patcht das WinGet-Manifest mit der
# konkreten Version + SHA256 (Manifest danach manuell als PR gegen
# microsoft/winget-pkgs einreichen via `wingetcreate submit`).
#
# Voraussetzungen:
#   - scripts/build-windows.ps1 + scripts/build-msi.ps1 sind durch
#   - scp/ssh konfiguriert in $HOME\.ssh\config (athena-cluster key)
#   - wingetcreate installiert (für PR-Submission): siehe windows/SETUP.md

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$version = (Select-String -Path engine.h `
    -Pattern 'VERSION = "([0-9]+\.[0-9]+\.[0-9]+)"' `
    | Select-Object -First 1).Matches.Groups[1].Value
$msi = "build-windows\cdripper-$version-windows-x64.msi"
if (-not (Test-Path $msi)) { throw "$msi fehlt — build-msi.ps1 zuerst." }

$sha = (Get-FileHash $msi -Algorithm SHA256).Hash
Write-Host ">>> cdripper $version → Windows-Release"
Write-Host "  MSI:    $msi"
Write-Host "  SHA256: $sha"

# Cluster-Upload via scp. Annahme: ~/.ssh/config hat einen Host-Alias
# "athena-flatpak-repo" der via kubectl-exec-Wrapper (oder ProxyCommand)
# in den Pod schreibt. Alternativ: vorher per scp auf das VPS, dann von
# dort weiter. Hier rein der scp-Aufruf — Konfig macht der User einmal.
Write-Host
Write-Host ">>> Upload nach flatpak.x2-pandora.de/cdripper/"
scp $msi athena-flatpak-repo:/repo/cdripper/

# WinGet-Manifest patchen (in-place auf einer Kopie unter
# windows/winget-out/ — Quelle bleibt template).
$out = "windows\winget-out"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
Copy-Item -Recurse "windows\winget" $out

Get-ChildItem $out -Filter "*.yaml" | ForEach-Object {
    (Get-Content $_.FullName -Raw) `
        -replace '__VERSION__', $version `
        -replace '__SHA256__', $sha `
    | Set-Content -Path $_.FullName -NoNewline
}
# Versions-Manifest braucht echte Version (statt "0.0.0" Default)
(Get-Content "$out\Gravey158.cdripper.yaml" -Raw) `
    -replace 'PackageVersion: 0\.0\.0', "PackageVersion: $version" `
| Set-Content "$out\Gravey158.cdripper.yaml" -NoNewline

Write-Host
Write-Host "WinGet-Manifest in $out/ — Submission via:"
Write-Host "  wingetcreate submit -t <github_pat> $out"
Write-Host
Write-Host "Manuelle Cluster-Verifikation:"
Write-Host "  curl -sI https://flatpak.x2-pandora.de/cdripper/cdripper-$version-windows-x64.msi"
