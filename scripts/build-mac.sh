#!/usr/bin/env bash
# Baut cdripper nativ auf macOS (Intel ODER Apple Silicon) gegen
# Homebrew-Deps. Output: build-mac/cdripper (CLI+GUI in einem Binary).
#
# Voraussetzungen (einmalig):
#   brew install cmake pkg-config libcdio libdiscid nlohmann-json qt@6 \
#                flac opus-tools lame taglib libebur128 fmt inih chromaprint \
#                ffmpeg curl
#
# Hinweis: Mac-Build ist in v1.7.3 noch experimentell.
#   - CDROM-Ioctls (eject/disc-status) sind auf Mac stubbed → kein Auswurf
#     (kommt mit cdio_eject_media-Refactor)
#   - Default-Device /dev/sr0 ist Linux-spezifisch; auf Mac per
#     `cdripper --device /dev/disk2 ...` (siehe `cdripper --help` /
#     `diskutil list`)
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(awk '/constexpr const char\* VERSION =/{ split($0,a,"\""); print a[2]; exit }' engine.h)
echo ">>> cdripper $VERSION → macOS-native"

# Sparkle: idempotent fetchen (~10MB Framework + sign-Tools).
"$SRC/scripts/fetch-sparkle.sh"

# brew --prefix lifert /usr/local (Intel) bzw. /opt/homebrew (Apple Silicon).
BREW_PREFIX=$(brew --prefix)
QT_PREFIX=$(brew --prefix qt@6)
CURL_PREFIX=$(brew --prefix curl)
echo "  brew  : $BREW_PREFIX"
echo "  qt@6  : $QT_PREFIX"
echo "  curl  : $CURL_PREFIX"

# pkg-config soll auch Curl + Qt sehen (Curl ist keg-only).
export PKG_CONFIG_PATH="$BREW_PREFIX/lib/pkgconfig:$CURL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="$BREW_PREFIX/bin:$PATH"

cmake -S "$SRC" -B "$SRC/build-mac" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX;$CURL_PREFIX;$BREW_PREFIX" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0

cmake --build "$SRC/build-mac" -j"$(sysctl -n hw.logicalcpu)"

# .app-Bundle finalisieren mit macdeployqt — bündelt Qt6-Frameworks
# in cdripper.app/Contents/Frameworks/.
APP="$SRC/build-mac/cdripper.app"
if [ -d "$APP" ]; then
  # Sparkle.framework ins Bundle kopieren VOR macdeployqt — sonst meckert
  # macdeployqt über fehlende Frameworks beim Auto-Plugin-Discovery.
  if [ -d "$SRC/mac/Sparkle.framework" ]; then
    mkdir -p "$APP/Contents/Frameworks"
    cp -R "$SRC/mac/Sparkle.framework" "$APP/Contents/Frameworks/"
    echo ">>> Sparkle.framework kopiert"
  fi

  echo
  echo ">>> macdeployqt → Qt + Plugins bündeln"
  "$QT_PREFIX/bin/macdeployqt" "$APP" -verbose=1 2>&1 | tail -8

  # Workaround macdeployqt-Quirk: einige Qt-Transitive (brotli, sharpyuv,
  # …) haben rpath @loader_path/../lib im Header. macdeployqt kopiert sie
  # nach Frameworks/ statt lib/. Symlink schließt die Lücke.
  if [ ! -L "$APP/Contents/lib" ]; then
    ln -s Frameworks "$APP/Contents/lib"
  fi

  # Ad-hoc-codesign — modernes macOS verlangt mindestens das, selbst
  # wenn wir keine Apple-Developer-ID einsetzen. Ersetzt durch echte
  # Signatur sobald Dennis das Skipping ändert.
  echo
  echo ">>> Ad-hoc-codesign (--sign -)"
  codesign --force --deep --sign - "$APP" 2>&1 | tail -3
fi

# CPack → DragNDrop-.dmg
echo
echo ">>> cpack → DragNDrop .dmg"
(cd "$SRC/build-mac" && cpack -G DragNDrop 2>&1 | tail -5)

echo
ls -lh "$SRC/build-mac/cdripper.app/Contents/MacOS/cdripper" 2>/dev/null
ls -lh "$SRC/build-mac/"cdripper-*-macos-*.dmg 2>/dev/null
echo
echo "Test:  open $APP   # GUI"
echo "       $APP/Contents/MacOS/cdripper --help   # CLI"
