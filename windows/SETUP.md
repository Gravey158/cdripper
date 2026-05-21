# Windows-VM Build-Setup für cdripper

Einmaliges Setup in der Windows 11 VM. Alle nachfolgenden Releases laufen
dann über `scripts/build-windows.ps1` + `scripts/build-msi.ps1` ohne
weitere Konfiguration.

## 1. Tools installieren (in einer Admin-PowerShell)

```powershell
# Chocolatey als Paket-Manager
Set-ExecutionPolicy Bypass -Scope Process -Force
iex ((New-Object System.Net.WebClient).DownloadString('https://community.chocolatey.org/install.ps1'))

# Build-Tools (~6 GB Download)
choco install -y visualstudio2022buildtools `
    --params "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
choco install -y cmake git python3 7zip

# WiX 6 (für .msi-Build) — über dotnet SDK
choco install -y dotnet-sdk
dotnet tool install --global wix
```

## 2. vcpkg-Bootstrap

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg integrate install
[Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "User")
```

vcpkg-Deps werden bei erstem `cmake -DCMAKE_TOOLCHAIN_FILE=…` automatisch
aus `vcpkg.json` resolvt (Manifest-Modus). Erstbau dauert lange, weil
Qt6 + libcdio + ffmpeg compiliert werden müssen (~30–90 Min je nach
Maschine; danach gecacht).

## 3. cdripper-Source ziehen

```powershell
cd $HOME
git clone https://github.com/Gravey158/cdripper.git
cd cdripper
```

## 4. Erster Build

```powershell
.\scripts\build-windows.ps1
.\scripts\build-msi.ps1
```

Output: `build-windows\Release\cdripper.exe` (+ Qt-DLLs) und
`build-windows\cdripper-<VERSION>-windows-x64.msi`.

## 5. CD-Drive an die VM durchreichen

In UTM-VM-Settings → Drives → New Drive → External (Host-USB):
- Apple USB SuperDrive / externes Laufwerk anschließen
- VM-Settings öffnen, "USB Drives" → das Laufwerk durchreichen
- Inside Windows: erscheint als `\\.\D:` (oder anderer Laufwerksbuchstabe)
- cdripper aufrufen: `cdripper.exe --device \\.\D: --cli --once`

## 6. Release-Workflow (jeder Release)

```powershell
git pull
.\scripts\build-windows.ps1
.\scripts\build-msi.ps1
# Upload .msi auf flatpak.x2-pandora.de/cdripper/ (via scp/rsync,
# parallel zur Mac-DMG-Pipeline) — Skript folgt in v1.7.9.
```
