#!/usr/bin/env bash
# CD-Ripper-GUI starten — rootful podman, damit /dev/sr0 (echtes root) geht,
# Wayland-Socket + Config-Verzeichnis (r/w) durchgereicht. Letzteres, damit
# drive_offsets.ini / Profile auf dem Host persistieren (nicht container-intern).
# Voraussetzung: Image `localhost/cdripper` rootful gebaut (siehe Containerfile).
set -euo pipefail
mkdir -p "$HOME/.config/cdripper"
# Persistentes Daten-Verzeichnis (rotierendes cdripper.log) — sonst landet
# das Log container-intern und ist nach jedem GUI-Schließen weg (--rm).
mkdir -p "$HOME/.local/share/cdripper"

exec sudo podman run --rm --replace --name cdripper-gui \
  --privileged --device /dev/sr0 --net=host \
  -e XDG_RUNTIME_DIR=/run/user/"$(id -u)" \
  -e WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
  -e QT_QPA_PLATFORM=wayland \
  -e XDG_DATA_HOME=/data \
  -v /run/user/"$(id -u)":/run/user/"$(id -u)" \
  -v "$HOME/.config/cdripper:/cfg:Z" \
  -v "$HOME/.local/share/cdripper:/data/cdripper:Z" \
  localhost/cdripper --gui --config /cfg/config.ini
