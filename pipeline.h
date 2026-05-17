// pipeline.h — Fließband: Rip → Encode → Upload laufen parallel (3 Threads).
// Während Track k gerippt wird, werden k-1..1 encodiert/hochgeladen.
// Qt-frei: Status fließt über std::function-Callbacks raus.
#pragma once

#include "engine.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace cdr {

enum class TrackState { Pending, Ripping, Ripped, Encoding, Uploading, Done, Failed };
const char* state_label(TrackState s);

struct Callbacks {
    std::function<void(const std::string&)>                 onWaiting;     // "lege CD ein"
    std::function<void(const DiscIdent&)>                    onDiscIdent;
    std::function<void(const Album&)>                        onAlbum;       // Tabelle bauen
    std::function<void(const fs::path&)>                     onCover;
    // idx 1-basiert, frac 0..1 nur bei Ripping/Uploading sinnvoll
    std::function<void(int, TrackState, double, const std::string&)> onTrack;
    // elapsedSec, etaSec (-1 = unbekannt), rippedDone, uploadedDone, total
    std::function<void(double, double, int, int, int)>       onProgress;
    std::function<void(const std::string&)>                  onLog;
    std::function<void(bool, const std::string&)>            onDiscDone;    // success, msg
    std::function<void(const std::string&)>                  onFatal;
    // Durchsatz MB/s: rip, encode, upload (je 0 = noch unbekannt)
    std::function<void(double, double, double)>              onMetrics;
    // Mehrere MB-Releases: Labels + Default-Index → gewählter Index.
    // Nicht gesetzt ⇒ Default (CLI/headless).
    std::function<int(const std::vector<std::string>&, int)> onChooseRelease;
    // Live-Disc-Scan (Hauptfenster): Init setzt LBA-Bereich + leert die
    // Ring-Grafik; Cell färbt eine Position (status 0 ok·1 langsam·2 Fehler).
    std::function<void(int lbaMin, int lbaMax)>              onDiscScanInit;
    std::function<void(int lba, int status)>                onDiscScanCell;
    // Echtzeit-Lese-Cursor beim Rip (wie der separate Live-Scan): wandernde
    // Position, an der das Laufwerk gerade liest.
    std::function<void(int lba)>                            onDiscScanCursor;
    // Rip-Gesamtfortschritt 0..1 über ALLE rippbaren Tracks. Eigene Farbe,
    // überschreibt die Scan-Vorfärbung von innen nach außen („bis hierhin
    // gerippt"). Monoton — unabhängig von der physischen Rip-Reihenfolge.
    std::function<void(double frac)>                        onRipProgress;
    // Scan-geführter Rip: gibt für die Disc-ID die Track-Indizes zurück,
    // die ZULETZT gerippt werden sollen (bekannte Hänger). Leer = normal.
    std::function<std::vector<int>(const std::string& discId)> ripDeferTracks;
    // Frischer Standalone-Scan DIESER Sitzung für die Disc-ID: voller
    // Pro-Track-Status (Index 1..N: 0 ok·1 langsam·2 defekt·3 ungescannt).
    // Nicht leer ⇒ Rip übernimmt ihn und ÜBERSPRINGT den eigenen Preflight.
    std::function<std::vector<int>(const std::string& discId)> scannedTrackStatus;
};

// Closable MPSC-Queue für Track-Indizes zwischen den Stufen.
class Stage {
public:
    void push(int v);
    std::optional<int> pop();   // nullopt = geschlossen und leer
    void close();
private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<int> q_;
    bool closed_ = false;
};

class Pipeline {
public:
    Pipeline(Config cfg, Callbacks cb);

    // Loop: warten → eine Disc verarbeiten → auswerfen → wiederholen.
    // Läuft bis stop()==true. `once`=true → nach einer Disc beenden.
    void run(const std::atomic<bool>& stop, bool once);

    // GUI-Live-Edit: vor dem Encode dieses Tracks gesetzte Werte greifen.
    void set_track_title(int idx, const std::string& title);
    void set_track_artist(int idx, const std::string& artist);
    void set_album(const std::string& artist, const std::string& title,
                   const std::string& year);
    void set_cover(const std::string& path);   // GUI: Cover manuell ersetzen

private:
    bool wait_for_new_disc(Drive& drv, const std::atomic<bool>& stop,
                           const std::string& last_id);
    void process_disc(Drive& drv, const std::atomic<bool>& stop,
                      std::string& last_id);
    Album album_copy();
    void  emit_progress(bool force);

    Config    cfg_;
    Callbacks cb_;

    std::mutex album_mu_;
    Album      album_;

    // Fortschritt/ETA
    std::mutex     prog_mu_;
    std::atomic<long> sectors_total_{0};
    std::atomic<long> sectors_done_{0};
    std::atomic<int>  ripped_{0};
    std::atomic<int>  uploaded_{0};
    std::atomic<int>  total_tracks_{0};
    double            rip_start_ = 0;
    double            last_emit_ = 0;

    // Durchsatz-Akkus (P1d)
    std::atomic<long> enc_bytes_{0};
    std::atomic<long> enc_usec_{0};
    std::atomic<long> up_bytes_{0};
    std::atomic<long> up_usec_{0};

    // Lesefehler-Sammlung pro Disc (P1c)
    std::mutex                              damaged_mu_;
    std::vector<std::pair<int, long>>       damaged_;   // {track_idx, bad_sectors}

    // AccurateRip (P4b): je Track (v1,v2)-CRC, vom enc-Thread gefüllt
    std::mutex                                       ar_mu_;
    std::vector<std::pair<uint32_t, uint32_t>>       ar_crcs_;
};

} // namespace cdr
