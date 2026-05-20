#!/usr/bin/env bash
# Baut cdripper als Flatpak gegen die KDE-Platform 6.7-Runtime.
#
# Läuft in der distrobox `ripper` auf Bazzite. Voraussetzungen:
#   - flatpak-builder (dnf install flatpak-builder)
#   - org.kde.Platform//6.7 + org.kde.Sdk//6.7 (flatpak --user install)
#
# Output:
#   - Lokales OSTree-Repo:  build-flatpak/repo/  (zum Push auf flatpak.x2-pandora.de)
#   - Test-Bundle:          build-flatpak/cdripper-<VERSION>.flatpak
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(awk '/constexpr const char\* VERSION =/{ split($0,a,"\""); print a[2]; exit }' engine.h)
APP_ID=io.github.gravey158.cdripper

echo ">>> cdripper $VERSION → Flatpak ($APP_ID)"

# Icon-PNG aus dem AppImage-SVG generieren, falls noch nicht vorhanden.
# (Manifest verweist auf flatpak/cdripper.png.)
if [ ! -f "$SRC/flatpak/cdripper.png" ]; then
  cat > /tmp/cdripper-flatpak.svg <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <defs>
    <radialGradient id="g" cx="50%" cy="50%" r="55%">
      <stop offset="0%" stop-color="#4fc3f7"/>
      <stop offset="60%" stop-color="#2979ff"/>
      <stop offset="100%" stop-color="#1f63d6"/>
    </radialGradient>
  </defs>
  <circle cx="128" cy="128" r="120" fill="url(#g)"/>
  <circle cx="128" cy="128" r="60" fill="#262b33"/>
  <circle cx="128" cy="128" r="20" fill="#1e2127"/>
</svg>
SVG
  rsvg-convert -w 256 -h 256 /tmp/cdripper-flatpak.svg -o "$SRC/flatpak/cdripper.png"
  echo "  Icon generiert: flatpak/cdripper.png"
fi

# Build-Verzeichnis. flatpak-builder cached zwischen Runs in build-flatpak/.flatpak-builder/.
BUILD=$SRC/build-flatpak
REPO=$BUILD/repo
mkdir -p "$BUILD"

echo
echo ">>> flatpak-builder run"
flatpak-builder \
  --user \
  --force-clean \
  --install-deps-from=flathub \
  --repo="$REPO" \
  --state-dir="$BUILD/.flatpak-builder" \
  "$BUILD/build" \
  "$SRC/flatpak/$APP_ID.yml"

echo
echo ">>> Bundle erzeugen"
BUNDLE="$BUILD/cdripper-${VERSION}.flatpak"
flatpak build-bundle "$REPO" "$BUNDLE" "$APP_ID" master

ls -lh "$BUNDLE"
echo
echo "Fertig."
echo "  Test-Install:  flatpak --user install --reinstall $BUNDLE"
echo "  Repo-Pfad:     $REPO"
echo "  Push auf Cluster:  kubectl -n flatpak-repo cp <repo-content> ...:/repo/"
