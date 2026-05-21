#!/usr/bin/env bash
# Veröffentlicht einen Mac-Release: signiert das .dmg mit Sparkle, hängt es
# als <item> an die AppCast-XML, pusht beides ins Cluster-Repo, patcht den
# Homebrew-Cask. Nicht idempotent — re-runs ergänzen Items.
#
# Voraussetzungen:
#   - scripts/build-mac.sh ist gelaufen → build-mac/cdripper-VERSION-*.dmg
#   - mac/sparkle-tools/ (scripts/fetch-sparkle.sh hat sie eingerichtet)
#   - EdDSA-Private-Key im Keychain (mac/sparkle-tools/generate_keys hat ihn
#     beim Erst-Setup angelegt)
#   - kubectl Zugang zum athena-Cluster
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(awk '/constexpr const char\* VERSION =/{ split($0,a,"\""); print a[2]; exit }' engine.h)
DMG="$SRC/build-mac/cdripper-${VERSION}-macos-x86_64.dmg"
APPCAST="$SRC/mac/appcast.xml"
CASK="$SRC/homebrew/Casks/cdripper.rb"

[ -f "$DMG" ] || { echo "FEHLER: $DMG fehlt — build-mac.sh laufen lassen."; exit 1; }
[ -x "$SRC/mac/sparkle-tools/sign_update" ] || \
  { echo "FEHLER: Sparkle-Tools fehlen — scripts/fetch-sparkle.sh laufen lassen."; exit 1; }

echo ">>> Sign DMG mit Sparkle EdDSA"
SIG_LINE=$("$SRC/mac/sparkle-tools/sign_update" "$DMG")
SIG=$(awk -F'"' '/sparkle:edSignature/{print $2}' <<< "$SIG_LINE")
LEN=$(awk -F'"' '/length=/{print $4}' <<< "$SIG_LINE")
SHA=$(shasum -a 256 "$DMG" | awk '{print $1}')
echo "  Signature: ${SIG:0:32}..."
echo "  Länge:     $LEN"
echo "  SHA256:    ${SHA:0:32}..."

echo
echo ">>> Patch AppCast (mac/appcast.xml — Item für $VERSION ergänzen)"
if grep -q "<sparkle:version>${VERSION}</sparkle:version>" "$APPCAST"; then
    echo "  $VERSION-Item bereits in AppCast — skip."
else
    # Python-Insert nach <language>en</language>. BSD-awk wirft bei
    # multi-line String-Vars „newline in string", deshalb Python.
    PUBDATE=$(date -R) VERSION="$VERSION" LEN="$LEN" SIG="$SIG" \
    python3 - "$APPCAST" <<'PY'
import os, sys, pathlib
p = pathlib.Path(sys.argv[1])
pubdate, version, length, sig = (os.environ[k] for k in ("PUBDATE","VERSION","LEN","SIG"))
item = f"""
        <item>
            <title>Version {version}</title>
            <pubDate>{pubdate}</pubDate>
            <sparkle:version>{version}</sparkle:version>
            <sparkle:shortVersionString>{version}</sparkle:shortVersionString>
            <sparkle:minimumSystemVersion>14.0</sparkle:minimumSystemVersion>
            <description><![CDATA[<p>See git log v{version} for changes.</p>]]></description>
            <enclosure
                url="https://flatpak.x2-pandora.de/cdripper/cdripper-{version}-macos-x86_64.dmg"
                sparkle:version="{version}"
                sparkle:shortVersionString="{version}"
                length="{length}"
                type="application/octet-stream"
                sparkle:edSignature="{sig}" />
        </item>"""
text = p.read_text()
marker = "<language>en</language>"
i = text.find(marker)
if i < 0:
    sys.exit(f"Marker '{marker}' nicht in {p}")
i += len(marker)
p.write_text(text[:i] + item + text[i:])
PY
fi

echo
echo ">>> Patch Homebrew-Cask"
sed -i.bak \
    -e "s/^  version \".*\"/  version \"${VERSION}\"/" \
    -e "s/^  sha256 \".*\"/  sha256 \"${SHA}\"/" \
    "$CASK"
rm -f "$CASK.bak"

echo
echo ">>> Cluster-Push (DMG + AppCast)"
POD=$(kubectl -n flatpak-repo get pod -l app=flatpak-repo -o jsonpath='{.items[0].metadata.name}')
kubectl -n flatpak-repo exec "$POD" -- mkdir -p /repo/cdripper
kubectl -n flatpak-repo cp "$DMG"     "${POD}":/repo/cdripper/
kubectl -n flatpak-repo cp "$APPCAST" "${POD}":/repo/cdripper/

echo
echo ">>> Verifikation"
curl -sI https://flatpak.x2-pandora.de/cdripper/cdripper-${VERSION}-macos-x86_64.dmg | head -3

echo
echo "Fertig."
echo "  AppCast: https://flatpak.x2-pandora.de/cdripper/appcast.xml"
echo "  DMG:     https://flatpak.x2-pandora.de/cdripper/cdripper-${VERSION}-macos-x86_64.dmg"
echo "  Cask gepatcht — homebrew-cdripper-Repo separat syncen + pushen:"
echo "    cp homebrew/Casks/cdripper.rb ~/projects/homebrew-cdripper/Casks/"
echo "    cd ~/projects/homebrew-cdripper && git add . && git commit -m \"${VERSION}\" && git push"
