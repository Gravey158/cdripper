// pipeline.cpp — 3-stufiges Fließband mit Per-Track-State.
#include "pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>

namespace cdr {

const char* state_label(TrackState s) {
    switch (s) {
        case TrackState::Pending:   return "wartet";
        case TrackState::Ripping:   return "rippt";
        case TrackState::Ripped:    return "gerippt";
        case TrackState::Encoding:  return "encodiert";
        case TrackState::Uploading: return "lädt hoch";
        case TrackState::Done:      return "fertig";
        case TrackState::Failed:    return "FEHLER";
    }
    return "?";
}

// ── Stage (closable queue) ─────────────────────────────────────────────────────
void Stage::push(int v) {
    { std::lock_guard<std::mutex> l(m_); q_.push(v); }
    cv_.notify_one();
}
std::optional<int> Stage::pop() {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait(l, [&] { return !q_.empty() || closed_; });
    if (q_.empty()) return std::nullopt;
    int v = q_.front(); q_.pop();
    return v;
}
void Stage::close() {
    { std::lock_guard<std::mutex> l(m_); closed_ = true; }
    cv_.notify_all();
}

// ── Helfer ─────────────────────────────────────────────────────────────────────
static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

Pipeline::Pipeline(Config cfg, Callbacks cb)
    : cfg_(std::move(cfg)), cb_(std::move(cb)) {}

Album Pipeline::album_copy() {
    std::lock_guard<std::mutex> l(album_mu_);
    return album_;
}
void Pipeline::set_track_title(int idx, const std::string& t) {
    std::lock_guard<std::mutex> l(album_mu_);
    if (idx >= 1 && idx <= (int)album_.tracks.size()) album_.tracks[idx - 1].title = t;
}
void Pipeline::set_track_artist(int idx, const std::string& a) {
    std::lock_guard<std::mutex> l(album_mu_);
    if (idx >= 1 && idx <= (int)album_.tracks.size()) album_.tracks[idx - 1].artist = a;
}
void Pipeline::set_album(const std::string& artist, const std::string& title,
                         const std::string& year) {
    std::lock_guard<std::mutex> l(album_mu_);
    album_.artist = artist;
    album_.title  = title;
    album_.date   = year;
}
void Pipeline::set_cover(const std::string& path) {
    std::lock_guard<std::mutex> l(album_mu_);
    album_.cover_jpg = path;
}

void Pipeline::emit_progress(bool force) {
    double n = now_s();
    {
        std::lock_guard<std::mutex> l(prog_mu_);
        if (!force && n - last_emit_ < 0.5) return;
        last_emit_ = n;
    }
    double elapsed = (rip_start_ > 0) ? n - rip_start_ : 0;
    long tot = sectors_total_.load(), dn = sectors_done_.load();
    double frac = tot > 0 ? (double)dn / tot : 0;
    double eta  = (frac > 0.02) ? elapsed * (1.0 - frac) / frac : -1;
    if (cb_.onProgress)
        cb_.onProgress(elapsed, eta, ripped_.load(), uploaded_.load(),
                       total_tracks_.load());
    if (cb_.onMetrics) {
        double rip_mbps = (elapsed > 0.5)
            ? (double)dn * 2352.0 / elapsed / 1e6 : 0;
        long eus = enc_usec_.load(), uus = up_usec_.load();
        double enc_mbps = eus > 0
            ? (double)enc_bytes_.load() / ((double)eus / 1e6) / 1e6 : 0;
        double up_mbps  = uus > 0
            ? (double)up_bytes_.load()  / ((double)uus / 1e6) / 1e6 : 0;
        cb_.onMetrics(rip_mbps, enc_mbps, up_mbps);
    }
}

// ── Disc-Loop ──────────────────────────────────────────────────────────────────
void Pipeline::run(const std::atomic<bool>& stop, bool once) {
    std::unique_ptr<Drive> drv;
    try {
        drv = std::make_unique<Drive>(cfg_.device);
    } catch (const std::exception& e) {
        if (cb_.onFatal) cb_.onFatal(e.what());
        return;
    }
    std::string last_id;
    while (!stop.load()) {
        if (!wait_for_new_disc(*drv, stop, last_id)) break;
        if (stop.load()) break;
        try {
            process_disc(*drv, stop, last_id);
        } catch (const std::exception& e) {
            if (cb_.onDiscDone) cb_.onDiscDone(false, e.what());
            if (cb_.onLog) cb_.onLog(std::string("Fehler: ") + e.what());
        }
        if (cfg_.auto_eject) {
            if (cb_.onLog) cb_.onLog("Werfe CD aus.");
            eject_device(cfg_.device);   // frisch + robust (Lock+Retry),
        }                                // nicht über den alten drv-fd
        if (once) break;
    }
}

// Wartet auf eine NEUE Disc. Wichtig: pro Poll ein FRISCH geöffnetes
// Laufwerk — ein einmal (bei run()-Start) geöffneter fd bekommt einen
// Medienwechsel nach Auto-Eject oft NICHT mit (Kernel bindet den
// Media-Change-Status an ein frisches open) → sonst „hängt ewig, neue
// CD wird ignoriert". `drv` wird hier bewusst nicht mehr benutzt.
bool Pipeline::wait_for_new_disc(Drive& /*drv*/, const std::atomic<bool>& stop,
                                 const std::string& last_id) {
    bool saw_empty = last_id.empty();
    if (cb_.onWaiting) cb_.onWaiting("Lege die (nächste) Audio-CD ein …");
    while (!stop.load()) {
        bool ready = false, audio = false;
        try {
            Drive d(cfg_.device);                 // jeder Poll frisch
            ready = d.disc_ready();
            audio = ready && d.has_audio();
        } catch (...) { ready = false; }          // kurz nicht offen → retry
        if (!ready) { saw_empty = true; }
        else if (saw_empty && audio) {
            std::string id = probe_disc_id(cfg_.device);
            if (id.empty()) { std::this_thread::sleep_for(std::chrono::seconds(2));
                              continue; }
            if (id == last_id) {
                if (cb_.onLog) cb_.onLog("Gleiche CD erkannt — werfe erneut aus.");
                eject_device(cfg_.device);
                saw_empty = false;
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            return true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

// ── Eine Disc durch das Fließband ──────────────────────────────────────────────
void Pipeline::process_disc(Drive& drv, const std::atomic<bool>& stop,
                            std::string& last_id) {
    DiscIdent ident = read_disc_ident(cfg_.device);
    if (cb_.onDiscIdent) cb_.onDiscIdent(ident);
    if (cb_.onLog) cb_.onLog("Disc-ID " + ident.id);
    if (ident.reconstructed && cb_.onLog)
        cb_.onLog("Hinweis: TOC war nicht direkt lesbar — Audio-TOC "
                  "rekonstruiert (evtl. Copy-Control/Kopierschutz). "
                  "Falls der Rip scheitert, anderes Laufwerk probieren.");

    // Metadaten-Fallback-Kette (mit Release-Picker bei Mehrdeutigkeit)
    std::optional<Album> a;
    {
        std::string mbsrc;
        auto cands = mb_release_candidates(ident.id, cfg_.mb_useragent,
                                           ident.toc, &mbsrc);
        if (!cands.empty()) {
            int pick = 0;
            if (cands.size() > 1 && cb_.onChooseRelease) {
                std::vector<std::string> lab;
                for (const auto& c : cands) {
                    // Unterscheidbar machen: Land · Datum · Label/Kat-Nr —
                    // sonst stehen N optisch identische Zeilen da.
                    std::string s = c.artist + " — " + c.title;
                    std::string meta;
                    auto add = [&](const std::string& x) {
                        if (x.empty()) return;
                        meta += (meta.empty() ? "  ·  " : " · ") + x; };
                    add(c.country.empty() ? "" : "[" + c.country + "]");
                    add(c.date.empty() ? c.year() : c.date);
                    add(c.label);
                    add(c.catalogno);
                    s += meta + "  [" + std::to_string(c.tracks.size()) +
                         " Tracks]" + (c.compilation ? " [VA]" : "");
                    lab.push_back(s);
                }
                pick = cb_.onChooseRelease(lab, 0);
                if (pick < 0 || pick >= (int)cands.size()) pick = 0;
            }
            a = cands[pick];
            if (cb_.onLog)
                cb_.onLog("Metadaten: MusicBrainz [" +
                    (mbsrc.empty() ? std::string("?") : mbsrc) + "] — " +
                    std::to_string(cands.size()) + " Release(s), gewählt #" +
                    std::to_string(pick + 1) + " (" + a->artist + " — " +
                    a->title + ", " + std::to_string(a->tracks.size()) +
                    " Tracks).");
        }
    }
    if (a) { /* MusicBrainz ok (exakt / TOC-Fuzzy / CD-Stub) */ }
    else if ((a = cddb_lookup(ident.toc, cfg_.mb_useragent))) {
        if (cb_.onLog) cb_.onLog("Metadaten aus gnudb/CDDB.");
    }
    else {
        a = cdtext_lookup(cfg_.device, ident.toc_tracks);
        if (a) { if (cb_.onLog) cb_.onLog("Metadaten aus CD-TEXT."); }
        else {
            if (!ident.submission_url.empty() && cb_.onLog)
                cb_.onLog("Unbekannt — MB-Eintrag: " + ident.submission_url);
            a = placeholder_album(ident.toc_tracks);
            if (cb_.onLog) cb_.onLog("Keine Metadaten (auch nicht via "
                "CDDB) — Platzhalter. Tipp: Aktion-Menue 'Titel manuell "
                "suchen' oder 'per Klang erkennen', oder Tabelle von "
                "Hand korrigieren.");
        }
    }

    // Rip-Stufe läuft über Worker-Subprozess + Watchdog (T10): das
    // libcdio-Open (cdio_cddap_open) kann auf mancher Firmware hart im
    // D-State hängen und würde sonst die ganze App einfrieren.
    // rip_session() kapselt das wie bei --calibrate; TOC/Offsets/Progress
    // kommen über das stdout-Zeilenprotokoll des Workers.
    int  rip_speed = cfg_.read_speed;
    bool rip_fast  = cfg_.fast_rip;

    // Preflight: schneller Disc-Quality-Scan → Speed/Modus automatisch.
    // pr hier deklariert (nicht im if), damit es am Ende für den
    // Archiv-Eintrag (gespeicherte Scan-Visualisierung) verfügbar ist.
    ProbeResult pr;
    bool pr_done = false;
    // Frischer Standalone-Scan dieser Sitzung für diese Disc da? → den
    // übernehmen und den (auf zähen Discs minutenlangen) Preflight-Scan
    // KOMPLETT überspringen. Genau die Erwartung wenn man eben manuell
    // gescannt (z.B. nach dem Reinigen) und dann rippt.
    std::vector<int> presScan;
    if (cb_.scannedTrackStatus) presScan = cb_.scannedTrackStatus(ident.id);
    // Fallback: kein In-Memory-Session-Status (z.B. Scan in anderer
    // Sitzung, oder Callback-Timing) → jüngsten persistierten Scan
    // DIESER Disc aus dem Archiv ziehen. So landen bekannte Hänger-
    // Tracks zuverlässig im 'r'-Plan (zuletzt), statt früh den Rip zu
    // blockieren.
    if (presScan.empty() && !ident.id.empty()) {
        long best = -1;
        for (const auto& a : load_archive())
            if ((a.kind == "scan" || a.kind == "rip") &&
                a.disc_id == ident.id &&
                !a.track_status.empty() && a.ts >= best) {
                best = a.ts; presScan = a.track_status;
            }
        if (!presScan.empty() && cb_.onLog)
            cb_.onLog("Letzten gespeicherten Zustand dieser Disc übernommen "
                      "(Archiv: Scan + gelernte Rip-Hänger) — kein "
                      "erneuter Preflight-Scan.");
    }
    if (!presScan.empty()) {
        pr.track_status = presScan;
        pr.quality = DiscQuality::Marginal;     // grob; Plan steuert per-Track
        for (int s : presScan) if (s == 2) pr.quality = DiscQuality::Bad;
        pr_done = true;
        rip_fast = false; rip_speed = 0;        // Basis; Plan überschreibt
        if (cb_.onLog) cb_.onLog("Vorheriger Scan dieser Disc übernommen "
            "— kein erneuter Preflight-Scan.");
    } else if (cfg_.preflight) {
        if (cb_.onLog) cb_.onLog("Preflight: scanne Disc-Qualität …");
        pr = disc_probe(cfg_.device, [&]{ return stop.load(); }, {},
                        cfg_.scan_density);
        pr_done = true;
        switch (pr.quality) {
            case DiscQuality::Clean:
                rip_fast = true; rip_speed = 0;
                if (cb_.onLog) cb_.onLog("Preflight: Disc sauber (" +
                    pr.detail + ") → Schnell-Rip, volle Speed.");
                break;
            case DiscQuality::Marginal:
                rip_fast = false; rip_speed = 0;
                if (cb_.onLog) cb_.onLog("Preflight: Disc grenzwertig (" +
                    pr.detail + ") → volle paranoia, volle Speed.");
                break;
            case DiscQuality::Bad:
                rip_fast = false; rip_speed = 4;
                if (cb_.onLog) cb_.onLog("⚠ Preflight: Disc stark "
                    "zerkratzt (" + pr.detail + ") → volle paranoia, "
                    "4× Speed. Disc reinigen empfohlen; Aussetzer / "
                    "Track-Verlust möglich.");
                break;
        }
    }
    if (rip_fast && cb_.onLog)
        cb_.onLog("Schnell-Rip aktiv (FULL nur bei Lesefehlern).");
    if (rip_speed > 0 && cb_.onLog)
        cb_.onLog("Lese-Speed " + std::to_string(rip_speed) +
                  "x (Kratzer-Recovery).");
    // AccurateRip-Offset: pro Laufwerk gespeichert, sonst manueller Fallback.
    std::string ar_drive = drive_id(cfg_.device);
    int ar_offset = cfg_.read_offset;
    bool ar_calibrated = false;
    bool ar_from_registry = false;
    if (auto o = lookup_drive_offset(ar_drive)) {
        ar_offset = *o; ar_calibrated = true;
    } else if (!cfg_.registry_url.empty()) {
        // Kein lokaler Eintrag → Konsens aus der Offset-Registry ziehen
        // (nur Lookup; immer aktiv wenn registry_url gesetzt — kein Upload).
        if (auto o = registry_lookup_offset(cfg_.registry_url, ar_drive,
                                            cfg_.mb_useragent)) {
            ar_offset = *o; ar_calibrated = true; ar_from_registry = true;
        }
    }
    if (cfg_.accuraterip && cb_.onLog)
        cb_.onLog("AccurateRip-Laufwerk: \"" + ar_drive + "\" — Offset " +
                  std::to_string(ar_offset) +
                  (ar_from_registry ? " (Registry-Konsens)."
                   : ar_calibrated  ? " (kalibriert)."
                   : " (NICHT kalibriert — Settings → Kalibrieren)."));

    // Album initial aus den Metadaten; die Track-Liste wird an die echte
    // TOC-Anzahl angeglichen, sobald der Worker die TOC gemeldet hat.
    {
        std::lock_guard<std::mutex> l(album_mu_);
        album_ = *a;
    }
    Album snap = album_copy();
    fs::path work = fs::path(cfg_.tmpdir) /
                    sanitize(snap.artist + " - " + snap.title);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) throw std::runtime_error("Temp-Ordner: " + work.string());

    // Cover (best effort) — bewusst VOR rip_session: ein Netz-Fetch im
    // Event-Callback würde den Worker-stdout-Reader blockieren
    // (Pipe-Stau + Watchdog-Fehlalarm).
    {
        fs::path cov;
        if (fetch_cover(snap.mb_release_id, cfg_.mb_useragent, work, cov)) {
            std::lock_guard<std::mutex> l(album_mu_);
            album_.cover_jpg = cov;
            if (cb_.onCover) cb_.onCover(cov);
            if (cb_.onLog) cb_.onLog("Cover geladen.");
        } else if (cb_.onLog) cb_.onLog("Kein Cover gefunden.");
    }

    Stage enc_q, up_q;
    std::atomic<int>  failed{0};
    std::atomic<bool> dirs_ok{false};
    auto stopfn = [&] { return stop.load(); };

    // Vom Worker-Protokoll gefüllter Zustand (erst TOC, dann Rip-Events).
    int n = 0;
    std::vector<int> ar_offs; int ar_leadout = 0;
    std::map<int, long> nsec;          // idx → Sektoren (für Fortschritt)
    long done_acc = 0;                 // kumulativ fertig-gerippte Sektoren
    bool toc_done = false;
    bool fatal = false; std::string fatalmsg;

    // Stufe 1 (Rippen) ist KEIN In-Process-Thread mehr — sie wird weiter
    // unten von rip_session() (Worker+Watchdog) über onEvent getrieben und
    // füttert enc_q. enc_q wird nach rip_session() geschlossen.

    // Stufe 2: FLAC-Encode (parallel zum Rippen der Folgetracks)
    std::thread enc_th([&] {
        while (auto idx = enc_q.pop()) {
            if (stop.load()) continue;
            Album al = album_copy();           // Live-Edits greifen hier
            const Track& tr = al.tracks[*idx - 1];
            if (cb_.onTrack) cb_.onTrack(*idx, TrackState::Encoding, 0, "");
            try {
                fs::path wav = work / (std::to_string(*idx) + ".wav");
                std::error_code se;
                long wsz = (long)fs::file_size(wav, se);
                double t0 = now_s();
                fs::path enc = encode_audio(wav, al, tr, cfg_);
                enc_usec_ += (long)((now_s() - t0) * 1e6);
                if (wsz > 0) enc_bytes_ += wsz;
                if (cfg_.accuraterip) {
                    uint32_t c1 = 0, c2 = 0;
                    ar_crc_file(wav, *idx == 1, *idx == n,
                                ar_offset, c1, c2);
                    std::lock_guard<std::mutex> l(ar_mu_);
                    if (*idx >= 1 && *idx <= (int)ar_crcs_.size())
                        ar_crcs_[*idx - 1] = { c1, c2 };
                }
                if (cfg_.replaygain) {
                    if (!apply_replaygain(enc) && cb_.onLog)
                        cb_.onLog("Track " + std::to_string(*idx) +
                                  ": ReplayGain übersprungen (rsgain fehlt?).");
                }
                if (cfg_.lyrics) {                          // T3: .lrc-Sidecar
                    int dur = wsz > 0 ? (int)(wsz / (44100.0 * 4)) : 0;
                    std::string lrc = fetch_synced_lyrics(
                        tr.artist, tr.title, al.title, dur, cfg_.mb_useragent);
                    if (!lrc.empty()) {
                        std::ofstream lf(work / (std::to_string(*idx) + ".lrc"));
                        lf << lrc;
                    }
                }
                std::error_code e;
                fs::remove(wav, e);
                up_q.push(*idx);
            } catch (const std::exception& e) {
                ++failed;
                if (cb_.onTrack)
                    cb_.onTrack(*idx, TrackState::Failed, 0, e.what());
                if (cb_.onLog)
                    cb_.onLog("Track " + std::to_string(*idx) +
                              " Encode-Fehler: " + e.what());
            }
        }
        up_q.close();
    });

    // Stufe 3: WebDAV-Upload (parallel zu Rip+Encode)
    std::thread up_th([&] {
        std::unique_ptr<Uploader> dav;
        try { dav = make_uploader(cfg_); }
        catch (const std::exception& e) {
            if (cb_.onLog)
                cb_.onLog(std::string("Upload-Backend-Fehler: ") + e.what());
            while (auto idx = up_q.pop()) {
                ++failed;
                if (cb_.onTrack)
                    cb_.onTrack(*idx, TrackState::Failed, 0, e.what());
            }
            return;
        }
        // snap ist erst nach TOC-complete final → dir_segs lazy bauen.
        // Das erste up_q-Item kommt garantiert nach der TOC, also ist
        // snap beim ersten ensure_segs() bereits gesetzt.
        std::vector<std::string> dir_segs;
        auto ensure_segs = [&] {
            if (dir_segs.empty()) {
                dir_segs.push_back(cfg_.music_root);
                for (auto& s : snap.folder_segments())
                    dir_segs.push_back(s);
            }
        };
        const std::string ext = audio_ext(cfg_);   // flac|opus|mp3
        bool cover_done = false;
        // P2a: transienter Netz-/WebDAV-Fehler ≠ Track verloren.
        auto with_retry = [&](const std::function<void()>& fn,
                              const char* what, int who) {
            int att = cfg_.upload_retries < 1 ? 1 : cfg_.upload_retries;
            for (int k = 1; ; ++k) {
                try { fn(); return; }
                catch (const std::exception& ex) {
                    if (k >= att) throw;
                    if (cb_.onLog)
                        cb_.onLog("Track " + std::to_string(who) + " " + what +
                                  " Versuch " + std::to_string(k) +
                                  " fehlgeschlagen (" + ex.what() +
                                  ") — retry…");
                    std::this_thread::sleep_for(std::chrono::seconds(2 * k));
                }
            }
        };
        while (auto idx = up_q.pop()) {
            if (stop.load()) continue;
            ensure_segs();
            Album al = album_copy();
            const Track& tr = al.tracks[*idx - 1];
            // Box-Set: Disc-Präfix vermeidet Dateinamen-Kollision (Disc1 01
            // vs Disc2 01) im selben Albumordner.
            std::string pfx = (al.disc_total > 1)
                ? (std::to_string(al.disc_number) + "-") : std::string();
            std::string fn = pfx + (tr.number < 10 ? "0" : "") +
                             std::to_string(tr.number) + " - " +
                             sanitize(tr.title) + "." + ext;
            fs::path flac = work / (std::to_string(*idx) + "." + ext);
            if (cb_.onTrack) cb_.onTrack(*idx, TrackState::Uploading, 0, "");
            try {
                if (cfg_.dry_run) {
                    if (cb_.onLog) cb_.onLog("[dry] " + fn);
                } else {
                    auto segs = dir_segs; segs.push_back(fn);
                    if (!cfg_.overwrite_existing && dav->exists(segs)) {
                        // T4: Track existiert schon am Ziel → überspringen,
                        // fehlende Tracks (Box-Set/Resume) kommen trotzdem.
                        if (cb_.onLog)
                            cb_.onLog("Track " + std::to_string(*idx) +
                                      " existiert bereits — übersprungen.");
                        std::error_code de;
                        fs::remove(flac, de);
                        fs::remove(work / (std::to_string(*idx) + ".lrc"), de);
                        ++uploaded_;
                        if (cb_.onTrack)
                            cb_.onTrack(*idx, TrackState::Done, 1,
                                        "bereits vorhanden");
                        emit_progress(true);
                        continue;
                    }
                    if (!dirs_ok.load()) {
                        with_retry([&]{ dav->ensure_dirs(dir_segs); },
                                   "MKCOL", *idx);
                        dirs_ok = true;
                    }
                    std::error_code fe;
                    long fsz = (long)fs::file_size(flac, fe);
                    double t0 = now_s();
                    with_retry([&]{ dav->put(flac, segs); }, "PUT", *idx);
                    up_usec_ += (long)((now_s() - t0) * 1e6);
                    if (fsz > 0) up_bytes_ += fsz;
                    fs::path lrcp = work / (std::to_string(*idx) + ".lrc");
                    std::error_code le;
                    if (fs::exists(lrcp, le)) {           // T3: .lrc mit hoch
                        auto ls = dir_segs;
                        ls.push_back(fn.substr(0, fn.rfind('.')) + ".lrc");
                        try { with_retry([&]{ dav->put(lrcp, ls); },
                                         "PUT-lrc", *idx); } catch (...) {}
                        fs::remove(lrcp, le);
                    }
                    if (!cover_done && !al.cover_jpg.empty()) {
                        auto cs = dir_segs; cs.push_back("cover.jpg");
                        try { dav->put(al.cover_jpg, cs); } catch (...) {}
                        cover_done = true;
                    }
                }
                std::error_code e; fs::remove(flac, e);
                ++uploaded_;
                if (cb_.onTrack) cb_.onTrack(*idx, TrackState::Done, 1, "");
                emit_progress(true);
            } catch (const std::exception& e) {
                ++failed;
                if (cb_.onTrack)
                    cb_.onTrack(*idx, TrackState::Failed, 0, e.what());
                if (cb_.onLog)
                    cb_.onLog("Track " + std::to_string(*idx) +
                              " Upload-Fehler: " + e.what());
            }
        }
    });

    // Pre-Rip-Existenz-Check (unten befüllt, vor rip_session): Tracks die
    // schon am Ziel liegen → gar nicht erst rippen (Plan-Code 's').
    std::vector<int> preexist;
    // Rip-Skip-ahead: welche Tracks sind „erledigt" (RIPDONE/RIPERR) und
    // welcher lief zuletzt an (RIPSTART) — für Hänger-Erkennung.
    std::vector<int> rip_settled;
    std::vector<int> rip_failed;             // Rip-Stufe: Hänger/Lesefehler
    int last_started = 0;
    int rip_done_cnt = 0;                     // erledigte rippbare Tracks
    int rippable_total = -1;                  // n minus vor-übersprungene
    auto is_settled = [&](int t){
        for (int x : rip_settled) if (x == t) return true; return false; };

    // ── Rip-Stufe über Worker+Watchdog (T10) ──────────────────────────
    // enc_th/up_th laufen bereits und blockieren auf leeren Queues; der
    // Event-Callback füllt nach der TOC die Pipeline und schiebt fertige
    // Tracks in enc_q. Der Callback macht NICHTS Blockierendes (kein Netz,
    // kein langer I/O) — sonst staut der Worker-stdout-Pipe.
    auto onEvent = [&](const std::string& ln) {
        if (ln.rfind("TOC ", 0) == 0) {
            n = std::atoi(ln.c_str() + 4);
        } else if (ln.rfind("LEN ", 0) == 0) {
            int i = 0, ct = 0, ns = 0;
            if (std::sscanf(ln.c_str(), "LEN %d %d %d", &i, &ct, &ns) == 3)
                nsec[i] = ns;
        } else if (ln.rfind("AR", 0) == 0 &&
                   (ln.size() == 2 || ln[2] == ' ')) {
            std::vector<int> v; const char* p = ln.c_str() + 2;
            while (*p) { while (*p == ' ') ++p; if (!*p) break;
                         v.push_back(std::atoi(p));
                         while (*p && *p != ' ') ++p; }
            if (v.size() >= 2) { ar_leadout = v.back(); v.pop_back();
                                 ar_offs = v; }
            // TOC vollständig (Worker sendet TOC→LEN*→AR vor dem ersten
            // Rip) → Album angleichen, Zähler initialisieren, scharf.
            if (!toc_done) {
                toc_done = true;
                if (n <= 0) n = (int)nsec.size();
                long tot = 0; for (auto& kv : nsec) tot += kv.second;
                {
                    std::lock_guard<std::mutex> l(album_mu_);
                    while ((int)album_.tracks.size() < n) {
                        Track t; t.number = (int)album_.tracks.size() + 1;
                        t.title = "Track " +
                            std::string(t.number < 10 ? "0" : "") +
                            std::to_string(t.number);
                        t.artist = album_.artist;
                        album_.tracks.push_back(std::move(t));
                    }
                    if ((int)album_.tracks.size() > n)
                        album_.tracks.resize(n);
                }
                snap = album_copy();
                if (cb_.onAlbum) cb_.onAlbum(snap);
                // B4: Speicher-Preflight (nicht blockierend, nur Warnung).
                {
                    long long est = tot * 2352LL;          // WAV-Rohgröße
                    long long flac_est = (long long)(est * 0.6);
                    long long tf = fs_free_bytes(cfg_.tmpdir);
                    if (tf >= 0 && tf < est / 3 && cb_.onLog)
                        cb_.onLog("⚠ Wenig Platz in tmpdir (" +
                            std::to_string(tf / (1024 * 1024)) +
                            " MB frei) — Rip könnte scheitern.");
                    if (cfg_.upload_backend == "local" &&
                        !cfg_.local_base.empty()) {
                        long long lf = fs_free_bytes(cfg_.local_base);
                        if (lf >= 0 && lf < flac_est && cb_.onLog)
                            cb_.onLog("⚠ Zielspeicher knapp (" +
                                std::to_string(lf / (1024 * 1024)) +
                                " MB frei, ~" +
                                std::to_string(flac_est / (1024 * 1024)) +
                                " MB nötig).");
                    }
                }
                sectors_total_ = tot;
                sectors_done_  = 0;
                ripped_ = 0; uploaded_ = 0;
                total_tracks_ = n;
                enc_bytes_ = 0; enc_usec_ = 0;
                up_bytes_ = 0; up_usec_ = 0;
                { std::lock_guard<std::mutex> l(damaged_mu_);
                  damaged_.clear(); }
                { std::lock_guard<std::mutex> l(ar_mu_);
                  ar_crcs_.assign(n, { 0u, 0u }); }
                rip_start_ = now_s();
                emit_progress(true);
                for (int i = 1; i <= n; ++i)
                    if (cb_.onTrack)
                        cb_.onTrack(i, TrackState::Pending, 0, "");
                // Pre-Rip-Existenz: schon vorhandene Tracks sofort als
                // fertig markieren (werden nicht gerippt) → Progress 100%.
                for (int t : preexist) if (t >= 1 && t <= n) {
                    if (cb_.onTrack)
                        cb_.onTrack(t, TrackState::Done, 1,
                                    "bereits vorhanden — übersprungen");
                    ++ripped_; ++uploaded_;
                }
                emit_progress(true);
                // Live-Disc-Scan: LBA-Bereich setzen + Grafik leeren.
                if (cb_.onDiscScanInit && !ar_offs.empty())
                    cb_.onDiscScanInit(ar_offs.front(), ar_leadout);
                // GANZE Disc sofort vorfärben (sonst zeigt die Live-Karte
                // nur die paar tatsächlich gerippten Tracks → „nur ein
                // Viertel gefüllt"): bereits vorhandene = grün/fertig,
                // sonst das Scan-Verdikt. RIPDONE überschreibt je Band.
                if (cb_.onDiscScanCell && !ar_offs.empty())
                    for (int t = 1; t <= n && t <= (int)ar_offs.size(); ++t) {
                        int cell = 0;
                        bool pre = std::find(preexist.begin(),
                            preexist.end(), t) != preexist.end();
                        if (!pre && t < (int)pr.track_status.size()) {
                            int s = pr.track_status[t];
                            cell = s == 2 ? 2 : (s == 1 || s == 3) ? 1 : 0;
                        }
                        cb_.onDiscScanCell(ar_offs[t - 1], cell);
                    }
            }
        } else if (ln.rfind("DEFECT ", 0) == 0) {
            int lba = 0, sev = 0;
            if (std::sscanf(ln.c_str(), "DEFECT %d %d", &lba, &sev) == 2 &&
                cb_.onDiscScanCell)
                cb_.onDiscScanCell(lba, sev >= 2 ? 2 : 1);
        } else if (ln.rfind("RIPSTART ", 0) == 0) {
            int idx = std::atoi(ln.c_str() + 9);
            last_started = idx;
            if (cb_.onTrack) cb_.onTrack(idx, TrackState::Ripping, 0, "");
        } else if (ln.rfind("RIP ", 0) == 0) {
            int idx = 0, pm = 0;
            if (std::sscanf(ln.c_str(), "RIP %d %d", &idx, &pm) == 2) {
                double f = pm / 1000.0;
                long ns = nsec.count(idx) ? nsec[idx] : 0;
                sectors_done_ = done_acc + (long)(f * ns);
                if (cb_.onTrack)
                    cb_.onTrack(idx, TrackState::Ripping, f, "");
                // Live-Ring = MONOTONER Gesamtfortschritt über alle
                // rippbaren Tracks (nicht physische Sektor-Position →
                // kein Springen/Overflow bei scan-adaptiver Reihenfolge).
                if (rippable_total < 0) {
                    rippable_total = n;
                    for (int p : preexist)
                        if (p >= 1 && p <= n) --rippable_total;
                    if (rippable_total < 1) rippable_total = 1;
                }
                if (cb_.onRipProgress) {
                    double fr = ((double)rip_done_cnt + f) / rippable_total;
                    cb_.onRipProgress(fr > 1.0 ? 1.0 : fr);
                }
                emit_progress(false);
            }
        } else if (ln.rfind("RIPDONE ", 0) == 0) {
            int idx = 0; long bad = 0;
            std::sscanf(ln.c_str(), "RIPDONE %d %ld", &idx, &bad);
            done_acc += nsec.count(idx) ? nsec[idx] : 0;
            sectors_done_ = done_acc;
            ++ripped_;
            std::string warn;
            if (bad > 0) {
                warn = "⚠ " + std::to_string(bad) +
                       " unlesbare Sektoren — evtl. Aussetzer";
                if (cb_.onLog)
                    cb_.onLog("Track " + std::to_string(idx) + ": " +
                              std::to_string(bad) + " nicht lesbare "
                              "Sektoren (zerkratzte CD?) — hörbare "
                              "Aussetzer möglich.");
                std::lock_guard<std::mutex> l(damaged_mu_);
                damaged_.push_back({ idx, bad });
            }
            // Defekt-Band rot markieren (bleibt über dem Fortschritts-
            // Overlay sichtbar). „Sauber" braucht keine Zelle mehr — das
            // deckt der monotone Fortschritts-Indikator ab.
            if (bad > 0 && cb_.onDiscScanCell && idx >= 1 &&
                idx <= (int)ar_offs.size())
                cb_.onDiscScanCell(ar_offs[idx - 1], 2);
            ++rip_done_cnt;
            if (cb_.onRipProgress && rippable_total > 0)
                cb_.onRipProgress((double)rip_done_cnt / rippable_total > 1.0
                    ? 1.0 : (double)rip_done_cnt / rippable_total);
            if (cb_.onTrack) cb_.onTrack(idx, TrackState::Ripped, 1, warn);
            emit_progress(true);
            rip_settled.push_back(idx);
            enc_q.push(idx);
        } else if (ln.rfind("RIPERR ", 0) == 0) {
            int idx = 0; char msg[256] = { 0 };
            std::sscanf(ln.c_str(), "RIPERR %d %255[^\n]", &idx, msg);
            ++failed;
            rip_settled.push_back(idx);          // erledigt (Fehler, kein Hang)
            rip_failed.push_back(idx);           // → persistent als defekt
            if (cb_.onDiscScanCell && idx >= 1 && idx <= (int)ar_offs.size())
                cb_.onDiscScanCell(ar_offs[idx - 1], 2);   // rot bleibt
            ++rip_done_cnt;
            if (cb_.onRipProgress && rippable_total > 0)
                cb_.onRipProgress((double)rip_done_cnt / rippable_total > 1.0
                    ? 1.0 : (double)rip_done_cnt / rippable_total);
            if (cb_.onTrack)
                cb_.onTrack(idx, TrackState::Failed, 0, msg);
            if (cb_.onLog)
                cb_.onLog("Track " + std::to_string(idx) +
                          " Rip-Fehler: " + msg);
        } else if (ln.rfind("FATAL ", 0) == 0) {
            fatal = true; fatalmsg = ln.substr(6);
        }
    };

    // Scan-adaptiver Rip-Plan: pro Track Speed/Modus aus dem Preflight-
    // Scan (automatisch, kein manueller Scan nötig) — 0 rippbar→f schnell,
    // 1 langsam→c (max,paranoia), 2 defekt→r (4×,paranoia, ans Ende),
    // 3 ungescannt→c. Dazu der GUI-Hint (manueller Scan) → erzwingt r.
    // Leerer Plan ⇒ Rip exakt wie bisher (T10, Basis-Speed/Modus).
    std::string plan; std::vector<int> defer_log;
    std::map<int,char> pc;                       // Track→Code (für Resume)
    auto serialize_plan = [&]{
        std::string s;
        for (auto& kv : pc)
            s += (s.empty() ? "" : ",") +
                 std::to_string(kv.first) + ":" + kv.second;
        return s;
    };
    {
        if (pr_done)
            for (int t = 1; t < (int)pr.track_status.size(); ++t) {
                int s = pr.track_status[t];
                pc[t] = s == 0 ? 'f' : s == 2 ? 'r'
                      : s == 3 ? 'c' : s == 1 ? 'c' : 'd';
            }
        if (cb_.ripDeferTracks)
            for (int t : cb_.ripDeferTracks(ident.id)) pc[t] = 'r';
        // Pre-Rip-Existenz-Check: Track schon am Ziel → 's' (gar nicht
        // rippen). Spart Rip+Encode (auf zähen Discs enorm) + vermeidet
        // erneutes D-State-Risiko auf bereits gesicherten Tracks.
        if (!cfg_.dry_run && !cfg_.overwrite_existing) {
            try {
                auto up = make_uploader(cfg_);
                std::vector<std::string> base = { cfg_.music_root };
                for (auto& s : snap.folder_segments()) base.push_back(s);
                std::string ex = audio_ext(cfg_);
                for (int t = 1; t <= (int)snap.tracks.size(); ++t) {
                    const Track& tr = snap.tracks[t - 1];
                    std::string pfx = (snap.disc_total > 1)
                        ? (std::to_string(snap.disc_number) + "-")
                        : std::string();
                    std::string fn = pfx + (tr.number < 10 ? "0" : "") +
                        std::to_string(tr.number) + " - " +
                        sanitize(tr.title) + "." + ex;
                    auto segs = base; segs.push_back(fn);
                    if (up->exists(segs)) {
                        pc[t] = 's';
                        preexist.push_back(t);
                    }
                }
            } catch (...) { /* Uploader-Fehler → up_th-T4 fängt es noch */ }
            if (!preexist.empty() && cb_.onLog) {
                std::string pl;
                for (size_t i = 0; i < preexist.size(); ++i)
                    pl += (i ? "," : "") + std::to_string(preexist[i]);
                cb_.onLog("Bereits am Ziel — übersprungen (nicht gerippt): "
                          "Track(s) " + pl);
            }
        }
        for (auto& kv : pc) {
            plan += (plan.empty() ? "" : ",") +
                    std::to_string(kv.first) + ":" + kv.second;
            if (kv.second == 'r') defer_log.push_back(kv.first);
        }
    }
    if (cb_.onLog && !plan.empty()) {
        std::string dl;
        for (size_t i = 0; i < defer_log.size(); ++i)
            dl += (i ? "," : "") + std::to_string(defer_log[i]);
        cb_.onLog(std::string("Scan-adaptiver Rip aktiv") +
            (dl.empty() ? "." : " — Hänger-Track(s) " + dl +
             " zuletzt + 4×/paranoia, gute Tracks schnell."));
    }
    // Rip-Skip-ahead: hängt das Laufwerk an einem Track (Watchdog killt
    // den Worker), wird der Track als defekt verbucht, ~45 s auf Erholung
    // gewartet und mit den RESTLICHEN Tracks weitergemacht — statt dass
    // ein kaputter Track den ganzen Rip beendet. enc/up-Threads laufen
    // über alle Runs (enc_q.close erst NACH der Schleife).
    RipResult rr = RipResult::Ok;
    int rskips = 0; const int MAX_RSKIPS = 3;
    for (;;) {
        rr = rip_session(cfg_.device, work.string(),
                         rip_speed, rip_fast, 90, onEvent, stopfn,
                         serialize_plan(),
                         cfg_.recovery_budget_min > 0
                             ? cfg_.recovery_budget_min * 60 : 0);
        if (rr != RipResult::Stalled) break;     // Ok/Aborted/Fatal → fertig
        int hung = (last_started > 0 && !is_settled(last_started))
                       ? last_started : 0;
        if (hung > 0) {
            ++failed;
            rip_settled.push_back(hung);
            rip_failed.push_back(hung);          // → persistent als defekt
            if (cb_.onTrack)
                cb_.onTrack(hung, TrackState::Failed, 0,
                            "Laufwerk hing — übersprungen");
            if (cb_.onLog)
                cb_.onLog("⚠ Track " + std::to_string(hung) + " hat das "
                    "Laufwerk gehängt — übersprungen, weiter mit dem Rest.");
        }
        for (int t : rip_settled) pc[t] = 's';   // erledigte/hängende → skip
        bool more = false;
        for (int t = 1; t <= n; ++t) {
            auto it = pc.find(t);                 // KEIN pc[t] (würde
            char c = (it == pc.end()) ? 'd' : it->second;  // '\0' einfügen
            if (!is_settled(t) && c != 's' &&     // → Plan-Korruption)
                std::find(preexist.begin(), preexist.end(), t) ==
                    preexist.end()) { more = true; break; }
        }
        if (!more) break;
        if (++rskips > MAX_RSKIPS) {
            if (cb_.onLog) cb_.onLog("Rip nach " + std::to_string(rskips) +
                " Hänger-Skips gestoppt — Rest nicht rippbar.");
            break;
        }
        if (stop.load()) break;
        if (cb_.onLog) cb_.onLog("Laufwerk-Erholung … (~45 s) dann weiter "
                                 "mit den restlichen Tracks.");
        for (int i = 0; i < 45 && !stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (stop.load()) break;
    }
    enc_q.close();          // keine weiteren Rip-Ergebnisse mehr
    enc_th.join();
    up_th.join();

    if (rr == RipResult::Stalled && cb_.onLog)
        cb_.onLog("⚠ Laufwerk hängt (kein Fortschritt > 90 s) — Worker "
                  "abgebrochen. Bitte Laufwerk zurücksetzen/neu anstecken.");
    if ((rr == RipResult::Fatal || fatal) && cb_.onLog)
        cb_.onLog("Rip fehlgeschlagen: " +
                  (fatalmsg.empty() ? std::string("unbekannt") : fatalmsg));
    if (rr == RipResult::Stalled || rr == RipResult::Fatal || fatal)
        if (failed.load() == 0) ++failed;   // erzwingt ok=false unten

    // AccurateRip-Abgleich (advisory; ändert nichts, nur Info).
    std::vector<ArMatch> ar_res;
    int  ar_ok = 0;          // wieviele Tracks gegen die AR-DB bestätigt
    bool ar_in_db = false;   // Disc war überhaupt in der AR-DB
    if (cfg_.accuraterip && !stop.load() && !ar_offs.empty()) {
        ArIds ids = ar_ids_from_toc(ar_offs, ar_leadout);
        std::vector<std::pair<uint32_t, uint32_t>> crcs;
        { std::lock_guard<std::mutex> l(ar_mu_); crcs = ar_crcs_; }
        auto res = ar_lookup(ids, crcs, cfg_.mb_useragent);
        ar_res = res;
        if (res.empty()) {
            if (cb_.onLog)
                cb_.onLog("AccurateRip: Disc nicht in der DB (oder offline).");
        } else {
            ar_in_db = true;
            int ripped_cnt = 0;          // Tracks die DIESEN Lauf frisch
                                         // gerippt wurden (CRC != 0)
            for (const auto& m : res) {
                bool ripped = m.track >= 1 &&
                    m.track <= (int)crcs.size() &&
                    (crcs[m.track - 1].first || crcs[m.track - 1].second);
                bool good = m.v1 || m.v2;
                if (ripped) ++ripped_cnt;
                if (good) ++ar_ok;
                // Übersprungene/fehlgeschlagene Tracks NICHT als „kein
                // Match (read_offset?)" verleumden — sie wurden gar nicht
                // gerippt, da gibt es nichts abzugleichen. Deren Status
                // (z.B. „bereits vorhanden") so stehen lassen.
                if (cb_.onTrack && (ripped || good))
                    cb_.onTrack(m.track, TrackState::Done, 1.0,
                        good ? ("AccurateRip ✓ conf " +
                                std::to_string(m.confidence))
                             : "AccurateRip: kein DB-Vergleich (Rip ok)");
            }
            if (cb_.onLog) {
                std::string msg = "AccurateRip: " + std::to_string(ar_ok) +
                    "/" + std::to_string((int)res.size()) + " Tracks bestätigt";
                if (ar_ok > 0)              msg += ".";
                else if (ripped_cnt == 0)   msg += " — keine frisch "
                    "gerippten Tracks (alle übersprungen/fehlgeschlagen), "
                    "daher kein Abgleich. Offset unverändert kalibriert.";
                else                        msg += " — read_offset evtl. "
                    "unkalibriert (experimentell).";
                cb_.onLog(msg);
            }
        }
    }

    // T5: bewiesen-korrekten Offset an die Registry zurückspielen. Nur wenn
    // (a) Opt-in, (b) lokal kalibriert (nicht selbst aus der Registry), und
    // (c) AccurateRip die Disc bei genau diesem Offset bestätigt hat — so
    // landet nie ein falscher Offset im Konsens.
    if (cfg_.registry_submit && !cfg_.registry_url.empty() &&
        ar_calibrated && !ar_from_registry && ar_ok > 0) {
        if (registry_submit_offset(cfg_.registry_url, ar_drive, ar_offset,
                                   cfg_.mb_useragent) && cb_.onLog)
            cb_.onLog("Offset " + std::to_string(ar_offset) +
                      " an die Registry gemeldet.");
    }

    // B1: EAC-Style Rip-Report neben das Album legen (best effort).
    if (!stop.load()) {
        try {
            std::time_t tt = std::time(nullptr);
            char ts[32]; std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S",
                                       std::localtime(&tt));
            std::ostringstream r;
            r << "cdripper " << VERSION << " — Rip-Report\n"
              << "Datum    : " << ts << "\n"
              << "Disc-ID  : " << ident.id << "\n"
              << "Laufwerk : " << ar_drive << "  (Offset " << ar_offset
              << (ar_calibrated ? ", kalibriert)" : ", NICHT kalibriert)") << "\n"
              << "Album    : " << snap.artist << " — " << snap.title;
            if (!snap.year().empty()) r << " (" << snap.year() << ")";
            r << (snap.compilation ? "   [Compilation]\n" : "\n")
              << "Tracks   : " << n << "\n"
              << "----------------------------------------\n";
            std::map<int, long> dmgmap;
            { std::lock_guard<std::mutex> l(damaged_mu_);
              for (auto& d : damaged_) dmgmap[d.first] = d.second; }
            Album a2 = album_copy();
            for (int i = 1; i <= n; ++i) {
                std::string title = (i <= (int)a2.tracks.size())
                    ? a2.tracks[i - 1].title : "";
                r << (i < 10 ? " " : "") << i << "  " << title << "\n     ";
                bool arknown = false;
                for (const auto& m : ar_res)
                    if (m.track == i) {
                        arknown = true;
                        r << (m.v1 || m.v2
                              ? "AccurateRip OK (conf " +
                                std::to_string(m.confidence) + ")"
                              : "AccurateRip: kein DB-Vergleich");
                        break;
                    }
                if (!arknown) r << "AccurateRip: n/a";
                auto it = dmgmap.find(i);
                if (it != dmgmap.end())
                    r << "  ·  " << it->second << " unlesbare Sektoren";
                r << "\n";
            }
            fs::path rp = work / "rip-report.txt";
            { std::ofstream rf(rp); rf << r.str(); }
            auto up = make_uploader(cfg_);
            std::vector<std::string> segs = { cfg_.music_root };
            for (auto& s : snap.folder_segments()) segs.push_back(s);
            up->ensure_dirs(segs);
            segs.push_back(sanitize(snap.artist + " - " + snap.title) +
                           ".cdripper.log");
            up->put(rp, segs);
            if (cb_.onLog) cb_.onLog("Rip-Report abgelegt.");

            // (A) Lokaler Zustands-Sidecar: condition.txt (+ disc-scan.svg
            // wenn ein Preflight-Scan vorliegt) ins Albumverzeichnis.
            {
                auto album_segs = [&]{
                    std::vector<std::string> v = { cfg_.music_root };
                    for (auto& s : snap.folder_segments()) v.push_back(s);
                    return v;
                };
                const char* qn = !pr_done ? "ohne Preflight-Scan"
                    : pr.quality == DiscQuality::Clean ? "sauber"
                    : pr.quality == DiscQuality::Marginal ? "grenzwertig"
                    : "stark zerkratzt";
                int dmgn; { std::lock_guard<std::mutex> l(damaged_mu_);
                            dmgn = (int)damaged_.size(); }
                std::ostringstream cs;
                cs << "cdripper — Disc-Zustand\n"
                   << "Album    : " << snap.artist << " — " << snap.title;
                if (!snap.year().empty()) cs << " (" << snap.year() << ")";
                cs << "\nDisc-ID  : " << ident.id
                   << "\nZustand  : " << qn;
                if (pr_done && !pr.completed)
                    cs << " (Scan unvollständig — Laufwerk hing)";
                bool ab = stop.load();
                bool okk = (failed.load() == 0) && !ab;
                cs << "\nAccurateRip: " << ar_ok << "/" << (int)ar_res.size()
                   << "\nBeschädigte Tracks: " << dmgn
                   << "\nErgebnis : "
                   << (ab ? "abgebrochen" : (okk ? "ok" : "fehler"))
                   << "\n";
                fs::path cp = work / "disc-condition.txt";
                { std::ofstream cf(cp); cf << cs.str(); }
                auto cseg = album_segs();
                cseg.push_back(sanitize(snap.artist + " - " + snap.title) +
                               " - disc-condition.txt");
                try { up->put(cp, cseg); } catch (...) {}
                if (pr_done && !pr.map.empty()) {
                    fs::path sp = work / "disc-scan.svg";
                    { std::ofstream sf(sp); sf << scan_svg(pr); }
                    auto sseg = album_segs();
                    sseg.push_back(sanitize(snap.artist + " - " +
                                   snap.title) + " - disc-scan.svg");
                    try { up->put(sp, sseg); } catch (...) {}
                }
                if (cb_.onLog) cb_.onLog("Zustands-Sidecar abgelegt.");
            }
        } catch (const std::exception& e) {
            if (cb_.onLog)
                cb_.onLog(std::string("Rip-Report übersprungen: ") + e.what());
        }
    }

    std::error_code e2;
    fs::remove_all(work, e2);

    bool aborted = stop.load();
    bool ok = (failed.load() == 0) && !aborted;
    if (ok) last_id = ident.id;
    emit_progress(true);

    // T5: anonyme Rip-Statistik melden (Opt-in). Kein Album-/Track-Bezug —
    // nur Laufwerksmodell, Version, ob AR bestätigt, kurzer Fehlergrund.
    // Abbruch durch den Nutzer ist kein Rip-Ergebnis → nicht melden.
    if (cfg_.registry_stats && !cfg_.registry_url.empty() && !aborted) {
        std::string err = ok ? "" :
            (failed.load() > 0 ? "tracks_failed"
             : !ar_in_db        ? "not_in_ar_db"
             : "");
        registry_submit_stat(cfg_.registry_url, ar_drive, VERSION,
                              ar_ok > 0, err, cfg_.mb_useragent);
    }

    std::string dmg;
    {
        std::lock_guard<std::mutex> l(damaged_mu_);
        if (!aborted && !damaged_.empty()) {
            dmg = "  ⚠ Lesefehler: ";
            for (size_t k = 0; k < damaged_.size(); ++k) {
                dmg += "Track " + std::to_string(damaged_[k].first) + " (" +
                       std::to_string(damaged_[k].second) + ")";
                if (k + 1 < damaged_.size()) dmg += ", ";
            }
            dmg += " — Disc reinigen & mit read_speed=4 neu rippen.";
        }
    }
    // Archiv-/Zustands-Eintrag (jeder Rip eine Zeile, auch Fehler/Abbruch
    // — gerade die sind als Zustands-Doku wertvoll). Best effort.
    {
        ArchiveEntry ae;
        ae.ts      = (long)std::time(nullptr);
        ae.disc_id = ident.id;
        ae.artist  = snap.artist;
        ae.title   = snap.title;
        ae.year    = snap.year();
        ae.tracks  = n;
        ae.kind    = "rip";
        ae.format  = cfg_.audio_format;
        ae.ar_ok   = ar_ok;
        ae.ar_total = (int)ar_res.size();
        { std::lock_guard<std::mutex> l(damaged_mu_);
          ae.damaged_tracks = (int)damaged_.size(); }
        ae.outcome = aborted ? "abgebrochen" : (ok ? "ok" : "fehler");
        if (pr_done) {
            ae.quality        = (int)pr.quality;
            ae.scan_completed = pr.completed;
            ae.lba_min        = pr.lba_min;
            ae.lba_max        = pr.lba_max;
            ae.map            = pr.map;
        } else {
            ae.scan_completed = false;          // kein Preflight gelaufen
        }
        // Persistierter Pro-Track-Status fürs Archiv-Reuse: Scan-Verdikt
        // als Basis, dann was der RIP SELBST gelernt hat — autoritativ:
        // erfolgreich gerippt / schon vorhanden = ok, Hänger/Lesefehler =
        // defekt. So defert ein erneuter Rip dieser Disc die kaputten
        // Tracks sofort ans Ende (gute zuerst sicher im Kasten).
        {
            int m = (int)snap.tracks.size();
            if (n > m) m = n;
            if ((int)pr.track_status.size() - 1 > m)
                m = (int)pr.track_status.size() - 1;
            for (int t : rip_settled) m = std::max(m, t);
            std::vector<int> ts(m + 1, -1);
            for (int t = 1; t <= m && t < (int)pr.track_status.size(); ++t)
                ts[t] = pr.track_status[t];
            for (int t : rip_settled)
                if (t >= 1 && t <= m) ts[t] = 0;        // gerippt → ok
            for (int t : preexist)
                if (t >= 1 && t <= m) ts[t] = 0;        // schon da → ok
            for (int t : rip_failed)
                if (t >= 1 && t <= m) ts[t] = 2;        // defekt (autoritativ)
            ae.track_status = ts;
        }
        if (!append_archive(ae) && cb_.onLog)
            cb_.onLog("Archiv-Eintrag konnte nicht geschrieben werden.");

        // CD-Zensus (opt-in, Default AUS): Album-Zustand + Cover-Thumb
        // an die Registry. Holt das Thumbnail selbst (CAA front-250).
        if (cfg_.registry_condition && !cfg_.registry_url.empty()) {
            if (registry_submit_condition(cfg_.registry_url, ae.disc_id,
                    ae.artist, ae.title, ae.year, snap.mb_release_id,
                    ae.quality, ae.ar_ok, ae.ar_total, ae.damaged_tracks,
                    "rip", cfg_.mb_useragent) && cb_.onLog)
                cb_.onLog("CD-Zustand an den Zensus gemeldet.");
        }
    }

    if (cb_.onDiscDone)
        cb_.onDiscDone(ok, (aborted ? std::string("Abgebrochen")
                       : ok ? (snap.artist + " — " + snap.title +
                               " vollständig hochgeladen.")
                            : (std::to_string(failed.load()) +
                               " Track(s) fehlgeschlagen.")) + dmg);
}

} // namespace cdr
