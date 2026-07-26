#!/usr/bin/env bash
# Baut cdripper für Windows in einer MSYS2-UCRT64-Umgebung.
#
# Warum nicht MSVC/vcpkg (scripts/build-windows.ps1, jetzt Altlast):
# libcdio, libcdio-paranoia und libdiscid gibt es in vcpkg schlicht nicht.
# Das Manifest listete sie seit Mai 2026, und jeder Windows-Lauf scheiterte
# folgerichtig beim Auflösen der Abhängigkeiten — der Windows-Port war nie
# übersetzt worden. MSYS2 liefert alle drei als fertiges Paket.
#
# Voraussetzungen (einmalig, in MSYS2 UCRT64):
#   pacman -S --needed \
#     mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,pkgconf} \
#     mingw-w64-ucrt-x86_64-{libcdio,libcdio-paranoia,libdiscid} \
#     mingw-w64-ucrt-x86_64-{qt6-base,qt6-tools,nlohmann-json,curl,flac}
#
# Ausgabe:
#   build-windows/cdripper.exe  samt allen benötigten DLLs und Qt-Plugins
#   build-windows/cdripper-<version>-windows-x64.zip
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(awk '/constexpr const char\* VERSION =/{ split($0,a,"\""); print a[2]; exit }' engine.h)
BUILD="$SRC/build-windows"
DIST="$BUILD/dist"

echo ">>> cdripper $VERSION → Windows x64 (MSYS2/UCRT64)"

if [ -z "${MSYSTEM:-}" ]; then
  echo "FEHLER: Läuft nicht in MSYS2. Die UCRT64-Shell starten." >&2
  exit 1
fi
if [ "${MSYSTEM}" != "UCRT64" ]; then
  echo "WARNUNG: MSYSTEM=$MSYSTEM (erwartet UCRT64) — Paketnamen könnten abweichen." >&2
fi

# Fehlende Pakete früh und gesammelt melden, statt CMake einzeln
# scheitern zu lassen.
missing=""
for p in libcdio libcdio-paranoia libdiscid qt6-base qt6-tools \
         nlohmann-json curl flac; do
  pacman -Qq "mingw-w64-ucrt-x86_64-$p" >/dev/null 2>&1 || missing="$missing $p"
done
if [ -n "$missing" ]; then
  echo "FEHLER: Diese Pakete fehlen:$missing" >&2
  echo "  pacman -S --needed$(for p in $missing; do printf ' mingw-w64-ucrt-x86_64-%s' "$p"; done)" >&2
  exit 1
fi

cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"
cmake --build "$BUILD" --target cdripper_tests -j"$(nproc)"

echo ">>> Unit-Tests"
"$BUILD/cdripper_tests.exe"

# ── Auslieferungsverzeichnis füllen ───────────────────────────────────────
# Die EXE allein läuft auf keinem fremden Rechner: Sie hängt an den
# MinGW-Laufzeit- und Qt-DLLs aus /ucrt64/bin. Wir lesen die Abhängigkeiten
# aus, statt eine Liste zu pflegen, die bei jedem Qt-Update veraltet.
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/cdripper.exe" "$DIST/"

echo ">>> DLL-Abhängigkeiten auflösen"
# Auf einen Pfadpräfix wie /ucrt64/ darf man sich nicht verlassen: ldd gibt je
# nach MSYS2-Installation mal /ucrt64/bin/..., mal C:/msys64/ucrt64/bin/... aus
# — in der CI liegt die Umgebung unter D:\a\_temp\msys64. Deshalb wird
# ausgeschlossen statt eingeschlossen: alles mitnehmen, was NICHT aus dem
# Windows-Systemverzeichnis kommt. Und die Pipe darf nicht leer laufen: mit
# `set -e` beendet ein grep ohne Treffer das ganze Skript.
copy_deps() {
  local target="$1"
  local deps
  deps=$(ldd "$target" 2>/dev/null | awk '{print $3}' \
         | grep -viE '/(WINDOWS|Windows|windows)/' | grep -i '\.dll$' || true)
  [ -z "$deps" ] && return 0
  local dll base
  while IFS= read -r dll; do
    [ -z "$dll" ] && continue
    [ -f "$dll" ] || continue
    base=$(basename "$dll")
    if [ ! -f "$DIST/$base" ]; then
      cp "$dll" "$DIST/"
      copy_deps "$dll"          # Qt6Gui zieht selbst wieder Bibliotheken nach
    fi
  done <<< "$deps"              # kein Pipe-Subshell: Rekursion braucht den Zustand
  return 0
}
copy_deps "$DIST/cdripper.exe"
echo "    $(find "$DIST" -maxdepth 1 -name '*.dll' | wc -l | tr -d ' ') DLLs"

# Qt-Plugins: kommen nicht über ldd, weil sie zur Laufzeit geladen werden.
# Ohne das Plattform-Plugin startet die Anwendung mit "could not find or
# load the Qt platform plugin windows" und sonst nichts.
echo ">>> Qt-Plugins"
QT_PLUGINS=$(qmake6 -query QT_INSTALL_PLUGINS 2>/dev/null || echo /ucrt64/share/qt6/plugins)
for grp in platforms styles imageformats iconengines tls; do
  if [ -d "$QT_PLUGINS/$grp" ]; then
    mkdir -p "$DIST/$grp"
    cp "$QT_PLUGINS/$grp"/*.dll "$DIST/$grp/" 2>/dev/null || true
    # Auch die Plugins brauchen ihre eigenen Bibliotheken.
    for pl in "$DIST/$grp"/*.dll; do
      [ -f "$pl" ] && copy_deps "$pl"
    done
  fi
done

# Qt sucht die Plugins relativ zur EXE, sobald eine qt.conf danebenliegt.
cat > "$DIST/qt.conf" <<'EOF'
[Paths]
Plugins = .
EOF

# CA-Bundle: OpenSSL aus MSYS2 sucht seinen Zertifikatsspeicher relativ zur
# eigenen DLL — aus einem beliebigen Installationsverzeichnis heraus geht das
# ins Leere, und JEDER HTTPS-Aufruf scheitert. main.cpp setzt SSL_CERT_FILE
# auf diese Datei neben der EXE.
if [ -f /ucrt64/ssl/certs/ca-bundle.crt ]; then
  cp /ucrt64/ssl/certs/ca-bundle.crt "$DIST/curl-ca-bundle.crt"
elif [ -f /usr/ssl/certs/ca-bundle.crt ]; then
  cp /usr/ssl/certs/ca-bundle.crt "$DIST/curl-ca-bundle.crt"
else
  echo "WARNUNG: kein CA-Bundle gefunden — HTTPS wird zur Laufzeit scheitern." >&2
fi

# Externe Werkzeuge (flac, metaflac) mitgeben, damit das Encodieren ohne
# separate Installation läuft.
for t in flac metaflac; do
  if [ -f "/ucrt64/bin/$t.exe" ]; then
    cp "/ucrt64/bin/$t.exe" "$DIST/"
    copy_deps "$DIST/$t.exe"
  else
    echo "    Hinweis: $t.exe nicht gefunden — Encoden braucht es zur Laufzeit." >&2
  fi
done

echo ">>> Paket schnüren"
ZIP="$BUILD/cdripper-$VERSION-windows-x64.zip"
rm -f "$ZIP"
( cd "$DIST" && zip -qr "$ZIP" . )

echo
echo "Fertig."
echo "  EXE:    $DIST/cdripper.exe"
echo "  Paket:  $ZIP"
echo "  Dateien: $(find "$DIST" -type f | wc -l | tr -d ' ')"
