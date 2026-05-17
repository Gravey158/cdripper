# Laufzeit-Image für den CD-Ripper (GUI + CLI).
# Deps vorinstalliert → Start ohne dnf-Wartezeit.
#
# Bauen (rootful, weil der GUI-Lauf für /dev/sr0 echten root braucht):
#   sudo podman build -t localhost/cdripper -f Containerfile .
# Voraussetzung: ./build/cdripper wurde vorher gebaut (siehe CMakeLists.txt /
# Build in der Fedora-distrobox `ripper`).
FROM registry.fedoraproject.org/fedora:42

RUN dnf -y install \
        qt6-qtbase-gui qt6-qtwayland \
        libcdio libcdio-paranoia libdiscid libcurl \
        flac opus-tools lame eject rsgain \
        chromaprint-tools \
        openssh-clients samba-client libnotify \
 && dnf clean all

# ENTRYPOINT-Prozess sourcet kein /etc/profile → ohne das kein UTF-8-Locale.
# Defense-in-depth zusätzlich zu flac --no-utf8-convert.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

COPY build/cdripper /usr/local/bin/cdripper
ENTRYPOINT ["/usr/local/bin/cdripper"]
