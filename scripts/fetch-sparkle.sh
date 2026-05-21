#!/usr/bin/env bash
# Lädt Sparkle.framework als pre-built Bundle nach mac/. Sparkle ist eine
# Objective-C-Library, wir ziehen sie nicht aus Quelle — die offiziellen
# Releases sind signiert + getestet.
#
# Output: mac/Sparkle.framework/   (gitignored — siehe .gitignore)
#         mac/sparkle-version       (Versions-Marker)
#
# Run: scripts/fetch-sparkle.sh    (idempotent — re-runs nur wenn Version
#                                   sich ändert)
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
SPARKLE_VERSION="2.6.4"
SPARKLE_URL="https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz"
DEST="$SRC/mac/Sparkle.framework"
MARKER="$SRC/mac/sparkle-version"

if [ -d "$DEST" ] && [ "$(cat "$MARKER" 2>/dev/null)" = "$SPARKLE_VERSION" ]; then
    echo "Sparkle $SPARKLE_VERSION schon da: $DEST"
    exit 0
fi

rm -rf "$DEST" "$SRC/mac/.sparkle-tmp"
mkdir -p "$SRC/mac/.sparkle-tmp"
echo ">>> Lade Sparkle $SPARKLE_VERSION"
curl -fsSL "$SPARKLE_URL" | tar -xJ -C "$SRC/mac/.sparkle-tmp"

# Sparkle-Tarball entpackt nach Sparkle/ — Framework darin.
cp -R "$SRC/mac/.sparkle-tmp/Sparkle.framework" "$DEST"
echo "$SPARKLE_VERSION" > "$MARKER"

# Zusatz: Sparkle-CLI-Tools (generate_keys, sign_update) — zum
# Release-Signieren auf Dev-Maschine. Liegen unter bin/ im Tarball.
mkdir -p "$SRC/mac/sparkle-tools"
cp -R "$SRC/mac/.sparkle-tmp/bin/"* "$SRC/mac/sparkle-tools/" 2>/dev/null || true

rm -rf "$SRC/mac/.sparkle-tmp"
echo "Sparkle $SPARKLE_VERSION fertig: $DEST"
echo "Tools verfügbar in: $SRC/mac/sparkle-tools/"
echo
echo "Erst-Setup:"
echo "  $SRC/mac/sparkle-tools/generate_keys"
echo "    → erzeugt EdDSA-Keypair, public key in stdout."
echo "    Private key landet im macOS Keychain (Service: \"https://sparkle-project.org\")."
echo "  Den Public-Key in mac/Info.plist.in unter SUPublicEDKey eintragen."
