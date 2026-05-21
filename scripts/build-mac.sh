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

echo
ls -lh "$SRC/build-mac/cdripper"
echo
echo "Test:  $SRC/build-mac/cdripper --help"
