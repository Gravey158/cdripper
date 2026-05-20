#!/usr/bin/env bash
# Baut cdripper als AppImage (eine portable Datei).
#
# Läuft in der distrobox `ripper` auf Bazzite (Fedora 42). Erwartet:
#   ~/tools/linuxdeploy  (+ linuxdeploy-plugin-qt) als symlinks ohne .AppImage
#
# Bündelt:
#   - cdripper-Binary (Release)
#   - Qt6-Bibliotheken via linuxdeploy-plugin-qt
#   - Externe CLI-Tools (flac/opusenc/lame/metaflac/rsgain/fpcalc/eject)
#     plus deren lib-Deps (linuxdeploy zieht sie nach)
#
# Ausgabe: cdripper-<VERSION>-x86_64.AppImage im Repo-Root.
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(awk '/constexpr const char\* VERSION =/{ split($0,a,"\""); print a[2]; exit }' engine.h)
echo ">>> cdripper $VERSION → AppImage"

PATH="$HOME/tools:$PATH"
command -v linuxdeploy >/dev/null \
  || { echo "linuxdeploy fehlt in ~/tools — siehe Containerfile/Memory"; exit 1; }

# Sauberer Release-Build (cdripper target).
cmake --build build -j4 --target cdripper --clean-first

APPDIR="$SRC/build/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/applications" \
         "$APPDIR/usr/share/icons/hicolor/256x256/apps"

# Haupt-Binary
cp -v build/cdripper "$APPDIR/usr/bin/cdripper"

# Externe CLI-Tools mitbündeln. linuxdeploy zieht ihre .so-Deps nach.
# (ssh + smbclient bewusst NICHT — zu groß, selten genutzt; User
# installiert die bei Bedarf auf Distro-Ebene.)
for tool in flac opusenc lame metaflac rsgain fpcalc eject; do
  if t=$(command -v "$tool" 2>/dev/null); then
    cp -v "$t" "$APPDIR/usr/bin/"
  else
    echo "  warn: $tool nicht gefunden — wird NICHT gebündelt"
  fi
done

# .desktop-Eintrag
cat > "$APPDIR/usr/share/applications/cdripper.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=CD Ripper
Comment=Audio-CD → FLAC → Navidrome
Exec=cdripper --gui
Icon=cdripper
Categories=AudioVideo;Audio;
Terminal=false
EOF

# Icon: vorhandenes nehmen, sonst SVG-Platzhalter (blau-grauer Disc-Look,
# passt zum App-Theme #2979ff/#262b33).
ICON_PNG="$APPDIR/usr/share/icons/hicolor/256x256/apps/cdripper.png"
if [ -f "$SRC/icon/cdripper.png" ]; then
  cp -v "$SRC/icon/cdripper.png" "$ICON_PNG"
else
  cat > /tmp/cdripper.svg <<'SVG'
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
  if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 256 -h 256 /tmp/cdripper.svg -o "$ICON_PNG"
  elif command -v convert >/dev/null 2>&1; then
    convert -background none -size 256x256 /tmp/cdripper.svg "$ICON_PNG"
  else
    # 1x1 PNG-Notnagel; linuxdeploy ist beim Format streng (PNG/SVG)
    base64 -d <<'B64' > "$ICON_PNG"
iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=
B64
  fi
fi
cp -v "$ICON_PNG" "$APPDIR/cdripper.png"

# linuxdeploy + qt-plugin: Libs einsammeln + AppImage erzeugen.
# OUTPUT-Override → vorhersagbarer Dateiname (sonst CD_Ripper-... aus
# .desktop-Name).
export ARCH=x86_64
export LINUXDEPLOY_OUTPUT_VERSION="$VERSION"        # neuer Env-Name
export LDAI_OUTPUT="cdripper-${VERSION}-x86_64.AppImage"
# Fedora 42 nutzt SHT_RELR (modern relative relocations, glibc 2.36+);
# linuxdeploys gebündelter `strip` kennt das Format nicht und bricht
# zwischendrin silent ab. NO_STRIP=1 überspringt das Strippen ganz
# (Bibliotheken bleiben dann unstripped → AppImage etwas größer, egal).
export NO_STRIP=true
# libcrypt: Fedoras libxcrypt nutzt GLIBC_PRIVATE-Symbole, die je nach
# Distro andere ABI haben → wenn gebündelt mit Fedoras Version, crasht's
# beim Init auf Debian. NICHT mitbündeln, System-libcrypt soll gewinnen.
linuxdeploy \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/cdripper" \
  --desktop-file "$APPDIR/usr/share/applications/cdripper.desktop" \
  --icon-file "$APPDIR/cdripper.png" \
  --exclude-library libcrypt.so.2 \
  --plugin qt \
  --output appimage

echo
echo "Fertig:"
ls -lh "$LDAI_OUTPUT" 2>/dev/null || ls -lh cdripper-*x86_64.AppImage 2>/dev/null
