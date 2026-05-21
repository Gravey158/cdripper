# Holt die externen Encoder-CLI-Tools (flac, metaflac, opusenc, lame, rsgain,
# fpcalc) als statisch verlinkte Windows-Binaries direkt von Upstream-
# Releases und legt sie in tools/ ab. WiX nimmt das tools/ später ins .msi.
#
# Hintergrund:
#   - vcpkg liefert nur libFLAC, libopus, libmp3lame etc. — nicht die
#     CLIs flac.exe/opusenc.exe/lame.exe.
#   - opus-tools, rsgain, chromaprint-fpcalc haben eigene Windows-Builds.
#   - Wir bündeln das alles ins MSI-Installer-Bundle damit cdripper's
#     pipeline.cpp std::system("flac …") auf Anhieb funktioniert.

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$tools = "tools"
New-Item -ItemType Directory -Force -Path $tools | Out-Null

function Get-Zip([string]$url, [string]$dest, [string]$archive) {
    if (Test-Path $archive) { return }
    Write-Host ">>> Lade $url"
    Invoke-WebRequest -Uri $url -OutFile "$tools\$archive"
    Expand-Archive -Path "$tools\$archive" -DestinationPath $dest -Force
}

# FLAC (flac.exe + metaflac.exe) — Xiph upstream
Get-Zip "https://ftp.osuosl.org/pub/xiph/releases/flac/flac-1.4.3-win.zip" `
        "$tools\flac" "flac.zip"

# Opus-Tools (opusenc.exe / opusdec.exe / opusinfo.exe)
Get-Zip "https://archive.mozilla.org/pub/opus/win32/opus-tools-0.2-opus-1.3.zip" `
        "$tools\opus-tools" "opus-tools.zip"

# LAME — RareWares hostet die kanonischen Windows-Builds
Get-Zip "https://www.rarewares.org/files/mp3/lame-3.100-x64.zip" `
        "$tools\lame" "lame.zip"

# rsgain — Releases haben Windows-Builds
Get-Zip "https://github.com/complexlogic/rsgain/releases/download/v3.6.1/rsgain-3.6.1-win64.zip" `
        "$tools\rsgain" "rsgain.zip"

# Chromaprint/fpcalc — Releases haben Windows-Builds
Get-Zip "https://github.com/acoustid/chromaprint/releases/download/v1.5.1/chromaprint-fpcalc-1.5.1-windows-x86_64.zip" `
        "$tools\chromaprint" "chromaprint.zip"

Write-Host
Write-Host "Tools nach $tools/ entpackt:"
Get-ChildItem -Recurse $tools -Include *.exe | Select-Object FullName
