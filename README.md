# cdripper

> No-nonsense audio CD ripper. FLAC out, MusicBrainz in, AccurateRip-verified.

`cdripper` rippt Audio-CDs sample-genau via `libcdio_paranoia`, holt
Metadaten und Cover aus MusicBrainz + Cover Art Archive, verifiziert jeden
Track gegen die öffentliche AccurateRip-Datenbank, schreibt FLAC mit
vollständigen Vorbis-Comment-Tags + embedded Cover + synced Lyrics von
LRCLib, und lädt fertige Alben direkt auf einen WebDAV-Server hoch
(Nextcloud-getestet — Navidrome zieht es dann automatisch).

Qt6-GUI für interaktiv, headless CLI für Batch-Ripping ganzer Schwiegerpapa-
Kistenbestände. Ein Multi-Drive-Modus rippt parallel auf mehreren
Laufwerken in dieselbe Albumstruktur. Per-Disc-Quality-Scan klassifiziert
Tracks live als clean/marginal/bad und passt Lese-Speed + paranoia-Modus
pro Track automatisch an.

## Install

### Linux (Flatpak)

```sh
flatpak --user remote-add --no-gpg-verify cdripper-athena https://flatpak.x2-pandora.de/
flatpak --user install -y cdripper-athena io.github.gravey158.cdripper
flatpak run io.github.gravey158.cdripper
```

Funktioniert auf Fedora, Debian, Ubuntu, Arch — überall wo `flatpak`
läuft. Runtime: `org.kde.Platform//6.7` zieht sich automatisch von
Flathub (~600 MB Erst-Pull, dann cached).

### macOS (Homebrew Cask, Intel + Apple Silicon)

```sh
brew tap gravey158/cdripper
brew install --cask cdripper

# einmalig wegen ad-hoc-Signatur (kein Apple Developer ID):
sudo xattr -cr /Applications/cdripper.app

open -a cdripper
```

macOS Sonoma 14+ erforderlich. Updates kommen via `brew upgrade --cask
cdripper` ODER über das integrierte Sparkle (24h-Auto-Check, Public-EdDSA-
verifiziert).

### Windows (in Arbeit — v1.7.9 ist Code-Pipeline-fertig)

WinGet + .msi-Installer kommen mit dem ersten echten Windows-Build. Bis
dahin: `git clone` + `scripts/build-windows.ps1` in einer VS-2022-
Build-Tools-Umgebung (siehe `windows/SETUP.md`).

## Bauen aus Quelle

### Linux

```sh
# Distro-Deps (Debian/Ubuntu)
sudo apt install qt6-base-dev libcdio-dev libcdio-paranoia-dev \
                 libdiscid-dev libcurl4-openssl-dev nlohmann-json3-dev \
                 cmake pkg-config build-essential

cmake -B build && cmake --build build -j
./build/cdripper --help
```

Encoder-Tools (`flac`, `opusenc`, `lame`, `rsgain`, `fpcalc`) müssen
zusätzlich im `$PATH` sein — `apt install flac opus-tools lame rsgain
libchromaprint-tools` deckt das ab.

### macOS

```sh
brew install cmake pkg-config libcdio libcdio-paranoia libdiscid \
             nlohmann-json qt@6 flac opus-tools lame taglib libebur128 \
             fmt inih chromaprint ffmpeg curl

./scripts/build-mac.sh
# build-mac/cdripper.app
```

### Windows

Siehe [`windows/SETUP.md`](windows/SETUP.md).

## Architektur

```text
                  ┌───────────┐    ┌─────────────┐    ┌──────────┐
   physische CD → │  Rip      │ →  │  Encode +    │ → │ Upload   │ → Nextcloud / WebDAV
   /dev/sr0      │ (worker+   │    │  Tag (FLAC/  │    │ (retry)  │
                  │ watchdog) │    │   Opus/MP3)  │    └──────────┘
                  └─────┬─────┘    └─────┬────────┘
                        │                │
                  ┌─────▼──────────────────────────────┐
                  │  MusicBrainz / Cover Art Archive /  │
                  │  AccurateRip / LRCLib / AcoustID    │
                  │  (Metadata + CRC-Verifikation +     │
                  │   Lyrics + Fingerprint-Fallback)    │
                  └─────────────────────────────────────┘
```

Drei-Stufen-Pipeline, jede Stufe in eigenem Thread; Worker-Rip-Prozess
ist watchdog-überwacht (90s-Stall → SIGKILL, Skip-ahead-Logik) damit ein
hängender Track nicht den ganzen Rip kostet.

## Konfiguration

`~/.config/cdripper/config.ini` mit Profil-Support (mehrere benannte Sets):

```ini
[default]
device          = /dev/sr0          # Linux. Mac: /dev/disk2. Win: \\.\D:
audio_format    = flac              # flac | opus | mp3
output_dir      = ~/Music
upload_backend  = webdav            # webdav | local | ssh | smb
webdav_url      = https://nextcloud.example/remote.php/dav/files/USER/Music
webdav_user     = USER
webdav_pass     = ...               # chmod 600
fast_rip        = true              # OVERLAP-Mode, eskaliert bei Fehler
preflight       = true              # Pre-Rip-Quality-Scan
auto_eject      = true
registry_url    = ...               # Optional: AR-Offset-Registry
registry_submit = false             # Opt-in
```

Kein Cover-fetch oder Upload ohne explizite Config-Werte. Passwörter NIE
in plaintext-Commits — Profile speichern chmod 600.

## Hardware-Empfehlung

- Externes USB-3-Optisches Laufwerk mit Slot-Loading oder Tray
- Pioneer BDR-XD08UMB-S, ASUS ZenDrive U9M, oder Apple USB SuperDrive
  (limited TOC-Support, aber gut genug für 99% der CDs)
- AccurateRip-Offset wird automatisch kalibriert (siehe `--calibrate`)

## Lizenz

MIT — siehe [LICENSE](LICENSE).

## Mitmachen

Issues + PRs willkommen unter
[github.com/Gravey158/cdripper/issues](https://github.com/Gravey158/cdripper/issues).
Vor PRs lokal `cmake --build build --target cdripper_tests && ./build/cdripper_tests`
laufen lassen — die Unit-Tests covern die kritische probe_classify/AR-CRC-
Logik.
