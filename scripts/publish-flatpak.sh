#!/usr/bin/env bash
# Pusht das lokale Flatpak-OSTree-Repo (build-flatpak/repo) auf das Cluster-
# Repo flatpak.x2-pandora.de via kubectl cp in den nginx-Pod.
#
# Voraussetzung: build-flatpak/repo/ existiert (build-flatpak.sh gelaufen).
#
# Hintergrund: flatpak.x2-pandora.de mountet das PVC `flatpak-repo-data` als
# /repo im nginx-unprivileged-Pod (uid 101). RWO-PVC → nur ein Mounter,
# also direkt in den laufenden nginx-Pod kopieren statt zweiten Pod
# anzulegen. nginx selbst hat read-only-Mountpoint nicht gesetzt — /repo
# ist beschreibbar von uid 101.
set -euo pipefail

cd "$(dirname "$0")/.."
SRC=$(pwd)
LOCAL_REPO=$SRC/build-flatpak/repo
NS=flatpak-repo

[ -d "$LOCAL_REPO" ] || {
  echo "FEHLER: $LOCAL_REPO existiert nicht — erst build-flatpak.sh laufen lassen."
  exit 1
}

POD=$(kubectl -n "$NS" get pod -l app=flatpak-repo -o jsonpath='{.items[0].metadata.name}')
[ -n "$POD" ] || { echo "FEHLER: kein flatpak-repo-Pod gefunden"; exit 1; }

echo ">>> Push lokales OSTree-Repo nach $NS/$POD:/repo/"

# Wenn /repo komplett leer (Erst-Upload), Tar-Stream einfach hochkopieren.
# Bei Updates: rsync-mäßig via tar mit --delete-after wäre netter, aber
# kubectl cp kann das nicht — wir kopieren alles, OSTree-Objects sind
# content-addressed und überschreiben sich byte-identisch wieder.
tar -C "$LOCAL_REPO" -cf - . | \
  kubectl -n "$NS" exec -i "$POD" -- tar -C /repo -xf -

echo
echo ">>> Verifikation"
kubectl -n "$NS" exec "$POD" -- ls /repo/ | head
echo
SUM=$(kubectl -n "$NS" exec "$POD" -- find /repo -type f | wc -l)
echo "Objekt-Count im Cluster-Repo: $SUM"

echo
echo ">>> Extern-Check"
curl -sI https://flatpak.x2-pandora.de/config | head -3
curl -s https://flatpak.x2-pandora.de/config 2>/dev/null | head -5 || true

echo
echo "Fertig."
echo "  Remote-Add (auf Test-Maschine):"
echo "    flatpak --user remote-add --no-gpg-verify \\"
echo "      cdripper-athena https://flatpak.x2-pandora.de/"
echo "  Install:"
echo "    flatpak --user install -y cdripper-athena io.github.gravey158.cdripper"
echo "  Run:"
echo "    flatpak run io.github.gravey158.cdripper"
