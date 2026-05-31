// engine.h — Qt-freier Kern: Disc lesen, Metadaten, Rippen, FLAC, WebDAV.
// Keine UI-/Logging-Kopplung — alles läuft über Callbacks / Rückgabewerte.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

namespace cdr {

// Versionsschema MAJOR.MINOR.PATCH:
//   MAJOR  großer Meilenstein
//   MINOR  mittleres Feature / größerer Bugfix
//   PATCH  kleiner Bugfix / kleine Änderung
// Bei jeder veröffentlichten Änderung hier hochzählen und im lokalen
// Git-Repo einen passenden Tag setzen (git tag -a vX.Y.Z).
constexpr const char* VERSION = "1.8.3";

struct Config {
    std::string device        = "/dev/sr0";   // primäres/Single-Laufwerk
    // T7: optionale Mehrfach-Laufwerksliste (parallel im CLI). Leer →
    // genau { device } (Single-Drive = exakter Subset, unverändert).
    std::vector<std::string> devices;
    std::vector<std::string> device_list() const {
        return devices.empty() ? std::vector<std::string>{ device }
                               : devices;
    }
    std::string nextcloud_url = "https://n1-k58z.x2-pandora.de";
    std::string webdav_user   = "cabanskid";
    std::string webdav_pass;            // NIE im Code — Config-Datei/Env
    std::string music_root    = "Music";
    std::string tmpdir        = "/tmp/cdripper";
    std::string mb_useragent  = "athena-cdripper/1.0 ( gravemind158@gmail.com )";
    // AcoustID/Chromaprint API-Key (kostenlos: acoustid.org/new-application).
    // Leer = akustischer Fingerprint-Fallback aus.
    std::string acoustid_key;
    bool        dry_run       = false;  // rippen/encoden, aber nicht hochladen
    int         read_speed    = 0;      // 0 = Laufwerks-Default; kleiner (z.B.
                                        // 4/8) = bessere Recovery zerkratzter CDs
    int         upload_retries = 3;     // Versuche pro PUT/MKCOL bei Netz-Blip
    bool        replaygain    = true;   // rsgain Track-Gain/R128-Tags schreiben
    // Audio-Encoder. format: flac|opus|mp3. quality ist format-spezifisch:
    // flac = Kompressionsstufe 0–8 (8 = --best); opus = kbit/s VBR;
    // mp3 = LAME-VBR-Stufe 0–9 (0 ≈ V0 ~245k, 9 = klein). Per Preset gesetzt.
    std::string audio_format  = "flac";
    int         audio_quality = 8;
    // Preflight: vor dem Rip einen schnellen Disc-Quality-Scan und davon
    // Speed/Modus automatisch ableiten (zerkratzt → langsam+volle paranoia).
    bool        preflight     = true;
    int         scan_density  = 6;      // Stichproben/Track (Karten-Granularität)
    // Recovery-Zeitbudget pro Track in Minuten (0 = aus): ein zerkratzter
    // Track der trotz Fortschritt endlos grindet wird nach dieser Zeit
    // übersprungen (Skip-ahead) statt den Rip ewig zu blockieren.
    int         recovery_budget_min = 5;
    bool        jukebox       = false;  // Auto-Rip sobald Disc erkannt (GUI)
    bool        fast_rip      = false;  // erst schnell lesen, FULL nur bei Fehler
    bool        accuraterip   = true;   // CRC gegen AccurateRip-DB prüfen
    int         read_offset   = 0;      // Drive-Sample-Offset (AccurateRip)
    bool        auto_eject    = true;   // CD nach Fertig auswerfen
    bool        chime         = false;  // Ton bei Disc fertig
    bool        lyrics        = true;   // synced Lyrics (LRCLIB) → .lrc-Sidecar
    bool        overwrite_existing = false;  // sonst vorhandene Tracks skippen

    // Upload-Backend: webdav | local | ssh | smb. music_root ist überall der
    // Unterordner unter der jeweiligen Basis.
    std::string upload_backend = "webdav";
    std::string local_base;             // local:  Zielbasis (z.B. /mnt/music)
    std::string ssh_host;               // ssh:    host
    std::string ssh_user;               // ssh:    user (leer = aktueller)
    std::string ssh_base;               // ssh:    Basispfad auf dem Host
    int         ssh_port      = 22;
    std::string smb_url;                // smb:    smb://host/share/Basis
    std::string smb_user;               // smb:    user (leer = guest)
    std::string smb_pass;               // smb:    Passwort (Secret!)

    // Offset-Registry (T5) — Cluster-App or1-9c4k. Leer = komplett aus.
    // Privacy: beide submit-Flags standardmäßig AUS (Opt-in).
    std::string registry_url;           // z.B. https://or1-9c4k.x2-pandora.de
    bool        registry_submit = false;  // eigenen kalibrierten Offset teilen
    bool        registry_stats  = false;  // anonyme Rip-Statistik melden
    bool        registry_condition = false; // Album-Zustand + Cover-Thumb für
                                             // den CD-Zensus teilen (opt-in!)
};

Config load_config(const std::string& path);   // Datei + Env (Env gewinnt)
bool save_config(const Config& c, const std::string& path);  // schreibt config.ini
std::string default_config_path();

// ── Profile (benannte Config-Sets) ─────────────────────────────────────────────
void set_config_path(const std::string& path);  // bestimmt config_dir()
std::string config_dir();                       // Verzeichnis der config.ini
std::vector<std::string> list_profiles();       // Namen unter profiles/
std::string active_profile();                   // "" = Standard (config.ini)
bool set_active_profile(const std::string& name);
std::string profile_path(const std::string& name);  // ""→default_config_path()

struct Track {
    int         number = 0;
    std::string title;
    std::string artist;          // pro Track (V.A.), sonst = Album-Artist
    std::string mb_track_id;
    std::string mb_artist_id;    // MUSICBRAINZ_ARTISTID (T2)
};

struct Album {
    std::string artist;
    std::string title;
    std::string date;            // "1997" oder "1997-08-12"
    std::string mb_release_id;
    std::string mb_artist_id;    // MUSICBRAINZ_ALBUMARTISTID (T2)
    std::vector<std::string> genres;   // primär zuerst (T2)
    std::string label;           // Plattenfirma (T2)
    std::string catalogno;       // Katalognummer (T2)
    std::string barcode;         // Barcode/EAN (T2)
    std::string country;         // Veröffentlichungsland (ISO, z.B. "DE")
    std::string originaldate;    // Erstveröffentlichung (release-group, T2)
    int         disc_number = 1;
    int         disc_total  = 1;
    bool        compilation = false;  // → COMPILATION=1 (Sampler/Various)
    std::vector<Track> tracks;
    fs::path    cover_jpg;       // leer = kein Cover

    std::string year() const;
    // "Artist/Album (Jahr)" als einzelne Pfadsegmente (schon sanitisiert)
    std::vector<std::string> folder_segments() const;
};

std::string sanitize(const std::string& in);
bool is_valid_utf8(const std::string& s);          // (für Tests exponiert)
std::string latin1_to_utf8(const std::string& s);
std::string b64(const std::string& in);            // Base64 (für Tests exponiert)
bool looks_compilation(const Album& a);

// ── Disc-Identifikation ────────────────────────────────────────────────────────
struct DiscIdent {
    std::string id;              // MusicBrainz Disc-ID
    std::string submission_url;  // MB-Eintrags-URL bei Unbekannt
    int         toc_tracks = 0;  // Track-Anzahl laut TOC
    bool        reconstructed = false;  // TOC via libcdio-Fallback geholt
                                        // (libdiscid scheiterte → evtl.
                                        // Copy-Control/kopiergeschützt)
    std::string toc;             // libdiscid-TOC-String (für Fuzzy-Lookup,
                                 // wenn die exakte Disc-ID nicht in MB ist)
};
// Wirft std::runtime_error wenn keine lesbare Disc.
DiscIdent read_disc_ident(const std::string& device);
// Non-throwing: gibt MB-Disc-ID zurück oder "" (für die Warteschleife).
std::string probe_disc_id(const std::string& device) noexcept;

// ── Metadaten-Quellen (Fallback-Kette wird vom Pipeline orchestriert) ──────────
std::optional<Album> mb_lookup(const std::string& discid, const std::string& ua,
                               const std::string& toc = "",
                               std::string* source = nullptr);
// Alle passenden Releases (Disc-ID-Treffer zuerst) — für den Release-Picker.
// toc (libdiscid-TOC-String) optional: schlägt der exakte Disc-ID-Lookup
// fehl, wird per MusicBrainz-TOC-Fuzzy-Suche nachgeschlagen (findet die
// Release auch, wenn diese Pressung nie als Disc-ID eingetragen wurde).
// source (optional): welche Stufe traf — "exakte Disc-ID" | "TOC-Fuzzy" |
// "CD-Stub" (für die Log-Nachvollziehbarkeit der Erkennungskette).
std::vector<Album> mb_release_candidates(const std::string& discid,
                                         const std::string& ua,
                                         const std::string& toc = "",
                                         std::string* source = nullptr);
// Cover-Art-Archive-Bild-URLs einer Release (für die Cover-Galerie).
std::vector<std::string> caa_image_urls(const std::string& mb_release_id,
                                        const std::string& ua);
// Lädt eine URL nach `out`. true bei Erfolg (für die GUI-Galerie).
bool fetch_url(const std::string& url, const std::string& ua,
               const std::string& out);
std::optional<Album> cdtext_lookup(const std::string& device, int n_audio_tracks);

// gnudb/CDDB-Lookup über den TOC-String (FreeDB-Protokoll). Findet
// Mainstream-Alben, die nicht in MusicBrainz als Disc-ID stehen. "" → nichts.
std::optional<Album> cddb_lookup(const std::string& toc, const std::string& ua);

// Manuelle MusicBrainz-Textsuche: Release-Kandidaten zu Interpret/Album
// (ohne Tracklist — die zieht mb_release_by_id für die gewählte Release).
struct ReleaseHit { std::string mbid, artist, title, date, country; int tracks = 0; };
std::vector<ReleaseHit> mb_search_releases(const std::string& artist,
                                           const std::string& title,
                                           const std::string& ua);
// Volle Release per MBID (Medium nach Wunsch-Trackzahl, sonst erstes).
std::optional<Album> mb_release_by_id(const std::string& mbid,
                                      int want_tracks, const std::string& ua);

// AcoustID/Chromaprint: identifiziert eine Audiodatei (WAV) am Klang.
// Braucht `fpcalc` im PATH + AcoustID-API-Key. Liefert bei Treffer die
// beste MusicBrainz-Release-MBID (+ Recording-Titel/Artist als Fallback).
struct AcoustIdHit { std::string mb_release_id, recording, artist; double score = 0; };
std::optional<AcoustIdHit> acoustid_identify(const fs::path& audio,
                                             int duration_sec,
                                             const std::string& key,
                                             const std::string& ua);
Album placeholder_album(int n_audio_tracks);   // "Unknown Artist / Track NN"

// Lädt Cover-Art vom Cover Art Archive nach <dir>/cover.jpg. true bei Erfolg.
bool fetch_cover(const std::string& mb_release_id, const std::string& ua,
                 const fs::path& dir, fs::path& out);

// Synced Lyrics (LRC) von LRCLIB; "" wenn nichts gefunden. Best effort.
std::string fetch_synced_lyrics(const std::string& artist,
                                const std::string& title,
                                const std::string& album,
                                int duration_sec, const std::string& ua);

// ── Laufwerk / Ripping ─────────────────────────────────────────────────────────
struct AudioTrack { int index; int cd_track; int n_sectors; };

// Hält das Laufwerk offen, rippt Tracks sequenziell (Drive ist single-reader).
class Ripper {
public:
    // speed: 0 = Laufwerks-Default, sonst Lese-Speed (z.B. 8) für bessere
    // Fehlerkorrektur bei zerkratzten Discs. Wirft bei Fehler.
    // fast=true: erst schneller paranoia-OVERLAP-Pass; nur Tracks mit
    // Lesefehlern werden in FULL nachgezogen (~3× schneller bei sauberen CDs).
    explicit Ripper(const std::string& device, int speed = 0,
                    bool fast = false);
    ~Ripper();
    const std::vector<AudioTrack>& tracks() const { return tracks_; }
    long total_sectors() const { return total_sectors_; }
    bool speed_applied() const { return speed_applied_; }
    // Rippt einen Audiotrack nach wav. progress(frac 0..1) periodisch.
    // stop()==true bricht sauber ab (wirft runtime_error "abgebrochen").
    // Rückgabe: Anzahl unlesbarer/übersprungener Sektoren (0 = sauber);
    // >0 ⇒ hörbare Aussetzer wahrscheinlich (zerkratzte CD).
    // Pro-Track umschaltbar (scan-adaptiver Rip): Lese-Speed + Modus
    // zwischen rip()-Aufrufen ändern. speed 0 = Laufwerks-Max.
    void set_speed(int speed);
    void set_fast(bool fast);
    // defect(lba, severity 1|2): periodisch gemeldete Defekt-Positionen
    // (paranoia-Recovery/Fehler) für die präzise Live-Karte. Optional.
    long rip(const AudioTrack& t, const fs::path& wav,
             const std::function<void(double)>& progress,
             const std::function<bool()>& stop,
             const std::function<void(int,int)>& defect = {});
private:
    void* drive_ = nullptr;      // cdrom_drive_t*
    void* para_  = nullptr;      // cdrom_paranoia_t*
    std::vector<AudioTrack> tracks_;
    long total_sectors_ = 0;
    bool speed_applied_ = false;
    bool fast_ = false;
public:
    // AccurateRip-TOC: Frame-Offsets (LBA+150) je Audiotrack + Leadout.
    void ar_toc(std::vector<int>& offsets_frames, int& leadout_frames) const;
    // Preflight-Quality-Scan: liest pro Track `per_track` kleine Stichproben
    // (`sect` Sektoren) ROH (paranoia AUS → Fehler kommen schnell statt
    // minutenlangem Grinden; echte D-State-Hänger fängt der Watchdog im
    // Eltern-Prozess). emit(track, lba, ok, ms) je Stichprobe.
    // onSeek(lba): VOR jeder (mehrsekündigen) Stichprobe → Echtzeit-Cursor.
    void probe(int per_track, int sect,
               const std::function<void(int,int,bool,long)>& emit,
               const std::function<bool()>& stop, int from_track = 1,
               const std::function<void(int)>& onSeek = {});
};

// ── AccurateRip ────────────────────────────────────────────────────────────────
struct ArIds {
    uint32_t id1 = 0, id2 = 0, cddb = 0;
    int ntracks = 0;
    std::string url() const;     // .bin-URL in der AccurateRip-DB
};
ArIds ar_ids_from_toc(const std::vector<int>& offsets_frames,
                      int leadout_frames);

// AR-v1+v2-CRC eines Tracks aus der WAV (offset in Samples; first/last für
// die 5-Frame-Trimmung an Disc-Anfang/-Ende).
void ar_crc_file(const fs::path& wav, bool first, bool last, int offset,
                 uint32_t& v1, uint32_t& v2);

// Alle bekannten CRCs (über alle Pressungen) je Track — für Offset-Kalibrierung.
std::vector<std::vector<uint32_t>> ar_db_crcs(const ArIds& ids,
                                              const std::string& ua);

// ── Laufwerks-Identität & per-Laufwerk-Offset-Store ────────────────────────────
// Stabiler Schlüssel pro Laufwerk (Vendor+Model via libcdio-Hwinfo).
std::string drive_id(const std::string& device);
struct HwInfo { std::string vendor, model, revision; bool ok = false; };
HwInfo drive_hwinfo(const std::string& device);   // Detailfelder fürs UI
// Datei (neben der Config) mit Zeilen "<drive-id> = <offset>".
std::string drive_offsets_path();
std::optional<int> lookup_drive_offset(const std::string& drive_id);
bool save_drive_offset(const std::string& drive_id, int offset);
bool delete_drive_offset(const std::string& drive_id);
struct DriveOffset { std::string id; int offset; };
std::vector<DriveOffset> list_drive_offsets();

// Im System erkannte optische Laufwerke (Gerätepfade, z.B. /dev/sr0).
// Leer = keins gefunden (dann Fallback auf konfiguriertes Gerät).
std::vector<std::string> list_optical_devices();

// ── Offset-Registry-Client (T5) ────────────────────────────────────────────────
// Cluster-App (or1-9c4k.x2-pandora.de). Alles best-effort, nie werfend; bei
// leerer base_url No-Op. drive_model = drive_id() (Vendor+Model, maschinen-
// übergreifend stabil → portabel zwischen Laufwerken/Rechnern).
std::optional<int> registry_lookup_offset(const std::string& base_url,
                                          const std::string& drive_model,
                                          const std::string& ua);
bool registry_submit_offset(const std::string& base_url,
                            const std::string& drive_model, int offset,
                            const std::string& ua);
bool registry_submit_stat(const std::string& base_url,
                          const std::string& drive_model,
                          const std::string& version, bool ar_match,
                          const std::string& err, const std::string& ua);

// CD-Zustands-Zensus (opt-in). Holt selbst das Cover-Thumbnail (CAA
// front-250 via mb_release_id, leer = keins) und POSTet /api/condition.
// quality: 0 clean·1 marginal·2 bad. Best-effort, nie werfend.
bool registry_submit_condition(const std::string& base_url,
                               const std::string& disc_id,
                               const std::string& artist,
                               const std::string& title,
                               const std::string& year,
                               const std::string& mb_release_id,
                               int quality, int ar_ok, int ar_total,
                               int damaged, const std::string& kind,
                               const std::string& ua);

// Laufwerksklappe direkt steuern (ioctl). Best effort.
bool eject_device(const std::string& device);
bool load_tray(const std::string& device);

// Freier Speicher des Dateisystems von `path` in Bytes; -1 = unbekannt.
long long fs_free_bytes(const std::string& path);
// Hängt `line` zeitgestempelt an ~/.local/share/cdripper/cdripper.log
// (Rotation bei >2 MB → .1). Best effort, nie werfend.
void log_to_file(const std::string& line);

struct ArMatch { int track; int confidence; bool v1; bool v2; };
// Holt die .bin, vergleicht je Track gegen unsere (v1,v2). Leerer Vektor =
// Disc nicht in AccurateRip / Netzfehler.
std::vector<ArMatch> ar_lookup(
    const ArIds& ids,
    const std::vector<std::pair<uint32_t, uint32_t>>& crcs,
    const std::string& ua);

// ── Plattform-Abstraktion: Subprozess-Worker ──────────────────────────────────
// cdripper startet sich selbst als Worker-Subprozess via `--rip-worker`/
// `--probe-worker` (siehe main.cpp). WorkerSession kapselt fork+exec+pipe+
// poll auf POSIX bzw. CreateProcessW+CreatePipe+PeekNamedPipe auf Windows
// (Windows-Pfad implementiert; native Build-Verifikation gegen ein echtes
// Laufwerk steht noch aus).
//
// Lifecycle: ctor spawnt das Kind; spawned() prüft Erfolg; read_line()
// liefert die nächste vollständige Zeile aus stdout (oder leer = Timeout,
// nullopt = EOF). kill() killt das Kind hart; wait_exit() reaped + liefert
// Exit-Code (0..255), -1 bei Signal/Abnormal/Wait-Fehler.
class WorkerSession {
public:
    WorkerSession(const std::string& self_exe,
                  const std::vector<std::string>& args);
    ~WorkerSession();
    WorkerSession(const WorkerSession&) = delete;
    WorkerSession& operator=(const WorkerSession&) = delete;

    bool spawned() const { return spawned_; }
    std::optional<std::string> read_line(int timeout_ms);
    void kill();
    int  wait_exit();
private:
#ifdef _WIN32
    void* h_process_ = nullptr;
    void* h_stdout_ = nullptr;
#else
    int pid_ = -1;
    int fd_  = -1;
#endif
    std::string buf_;
    bool spawned_ = false;
    bool killed_  = false;   // kill() gesetzt → wait_exit() liefert -1 (wie SIGKILL)
};

// Absoluter Pfad des laufenden Binaries — Linux: /proc/self/exe (immer
// gültig nach chdir); Mac: _NSGetExecutablePath + realpath; Windows:
// GetModuleFileNameW. Leerer String bei Fehler.
std::string self_exe_path();

// Laufwerksklappe / Disc-Erkennung (ioctl).
class Drive {
public:
    explicit Drive(const std::string& path);
    ~Drive();
    bool disc_ready() const;
    bool has_audio() const;
    int  raw_status() const;
    void eject() const;
    const std::string& path() const { return path_; }
private:
    std::string path_;
    int fd_ = -1;
};

// ── FLAC-Encode (flac-Binary, kein Shell — execvp mit argv) ────────────────────
// Wirft bei Fehler. Gibt den Pfad der erzeugten .flac zurück.
fs::path encode_flac(const fs::path& wav, const Album& al, const Track& tr,
                     int compression = 8);
// Dispatcht anhand c.audio_format auf flac/opus/mp3, schreibt alle Tags +
// Cover, gibt den erzeugten Pfad zurück. Wirft bei Encode-Fehler.
fs::path encode_audio(const fs::path& wav, const Album& al, const Track& tr,
                      const Config& c);
// Dateiendung des aktiven Formats ohne Punkt ("flac"|"opus"|"mp3").
std::string audio_ext(const Config& c);

// Schreibt ReplayGain-2.0/R128 *Track*-Gain-Tags in die FLAC (rsgain, in-place).
// Track- statt Album-Gain ist für Sampler korrekt (jeder Song anderes Album).
// Best effort: false wenn rsgain fehlt/fehlschlägt (kein Wurf).
bool apply_replaygain(const fs::path& flac);

// ── Upload-Backends ────────────────────────────────────────────────────────────
// Alle Methoden werfen std::runtime_error bei Fehler (Pipeline retry'd).
struct Uploader {
    virtual ~Uploader() = default;
    virtual void ensure_dirs(const std::vector<std::string>& dir_segs) = 0;
    virtual void put(const fs::path& local,
                     const std::vector<std::string>& path_segs) = 0;
    // true wenn die Zieldatei schon existiert (Duplikat-Erkennung, T4).
    virtual bool exists(const std::vector<std::string>& path_segs) = 0;
};

class WebDav : public Uploader {            // Nextcloud / generisches WebDAV
public:
    explicit WebDav(const Config& c);
    void ensure_dirs(const std::vector<std::string>&) override;
    void put(const fs::path&, const std::vector<std::string>&) override;
    bool exists(const std::vector<std::string>&) override;
private:
    const Config& cfg_;
    std::string base_;   // …/remote.php/dav/files/<user>
};

class LocalUploader : public Uploader {     // lokaler Pfad / gemounteter Share
public:
    explicit LocalUploader(std::string base) : base_(std::move(base)) {}
    void ensure_dirs(const std::vector<std::string>&) override;
    void put(const fs::path&, const std::vector<std::string>&) override;
    bool exists(const std::vector<std::string>&) override;
private:
    std::string base_;
};

class SshUploader : public Uploader {       // ssh: mkdir -p + scp
public:
    explicit SshUploader(const Config& c);
    void ensure_dirs(const std::vector<std::string>&) override;
    void put(const fs::path&, const std::vector<std::string>&) override;
    bool exists(const std::vector<std::string>&) override;
private:
    std::string host_, base_, sshtarget_;
    int port_;
};

class SmbUploader : public Uploader {       // smb://host/share/Basis via smbclient
public:
    explicit SmbUploader(const Config& c);
    void ensure_dirs(const std::vector<std::string>&) override;
    void put(const fs::path&, const std::vector<std::string>&) override;
    bool exists(const std::vector<std::string>&) override;
private:
    std::string service_;                 // //host/share
    std::vector<std::string> base_segs_;  // Pfad-Segmente unter dem Share
    std::string auth_;                    // user%pass  oder leer (=guest)
};

// Wählt anhand cfg.upload_backend; wirft bei unbekanntem/fehlkonfiguriertem.
std::unique_ptr<Uploader> make_uploader(const Config& c);

// ── Stall-resistentes Rippen via Worker-Subprozess + Watchdog (T8) ─────────────
enum class RipResult { Ok, Stalled, Aborted, Fatal };
// Startet `/proc/self/exe --rip-worker DEV WORK SPEED FAST`, liest dessen
// stdout-Protokoll zeilenweise und ruft onEvent(line) je Zeile. Kommt
// stall_secs lang KEINE Zeile (Laufwerk-D-State-Hänger), wird das Kind
// SIGKILL't und Stalled zurückgegeben — der Aufrufer bleibt responsiv.
// track_budget_secs > 0: ein EINZELNER Track, der länger als das Budget
// braucht (zerkratzte Disc grindet zwar mit Fortschritt, aber endlos),
// wird ebenfalls als Stalled abgebrochen → Skip-ahead macht weiter.
RipResult rip_session(const std::string& device, const std::string& workdir,
                      int speed, bool fast, int stall_secs,
                      const std::function<void(const std::string&)>& onEvent,
                      const std::function<bool()>& stop,
                      const std::string& plan_csv = std::string(),
                      int track_budget_secs = 0);

// ── Preflight Disc-Quality-Scan ────────────────────────────────────────────────
// Wie rip_session, aber Worker = `--probe-worker DEV`; Watchdog-geschützt
// (eine zerkratzte Disc kann auch den Scan in den D-State ziehen).
RipResult probe_session(const std::string& device, int stall_secs,
                         const std::function<void(const std::string&)>& onEvent,
                         const std::function<bool()>& stop,
                         int start_track = 1, int density = 6);

enum class DiscQuality { Clean, Marginal, Bad };
struct ProbeSample { int lba; int status; };   // status: 0 ok, 1 langsam, 2 Fehler
// Rohe Scan-Stichprobe (eine Lese-Messung). trk 1-basiert, ok 0=Fehler,
// ms = Lesedauer. Eingang von probe_classify (für Tests deterministisch).
struct ProbeRaw { int trk; int lba; int ok; long ms; };
struct ProbeResult {
    DiscQuality quality = DiscQuality::Clean;
    int samples = 0, problems = 0;             // problems = langsam+Fehler
    int lba_min = 0, lba_max = 0;              // für Radius-Normierung (Ring-Grafik)
    bool completed = false;                    // false = Scan stalled/fatal
    std::vector<ProbeSample> map;              // pro Stichprobe (für Visualisierung)
    // Pro-Track-Verdikt (Index = Track-Nr, 1-basiert; [0] ungenutzt):
    // 0 rippbar · 1 langsam · 2 defekt · 3 ungescannt (hinter dem Hänger).
    std::vector<int> track_status;
    std::string detail;
};
// Führt den Scan aus + klassifiziert. Bei Stall/Fatal → Bad (completed=false).
// onSample(lba, ok 0|1, ms): pro Stichprobe live (für Live-Visualisierung).
ProbeResult disc_probe(const std::string& device,
                        const std::function<bool()>& stop,
                        const std::function<void(int,int,long)>& onSample = {},
                        int density = 6,
                        const std::function<void(int)>& onCursor = {});

// Reine, deterministische Klassifikation der gesammelten Roh-Stichproben
// (aus disc_probe extrahiert → unit-testbar ohne Laufwerk). Median = disc-
// eigene Basislinie; nur deutliche Ausreißer + echte Lesefehler zählen.
// hung = Tracks mit D-State-Hänger, skips = Skip-ahead-Sprünge, rr =
// Endergebnis des (letzten) Scan-Subprozesses.
ProbeResult probe_classify(const std::vector<ProbeRaw>& raw, int ntracks,
                           int disc_end, const std::vector<int>& hung,
                           int skips, RipResult rr);

// ── Schadensform-Diagnose ──────────────────────────────────────────────────────
// Aus dem SEKTORGENAUEN Defekt-LBA-Muster (Rip-Defektmap) die physische
// Schadensform rekonstruieren — eine CD ist eine Spirale konstanten
// Spurabstands, daher korreliert das LBA-Muster mit der Geometrie:
//  • umlaufender Wisch/Schmutz  → ein zusammenhängender LBA-Block
//  • radialer Kratzer           → viele winzige Cluster, ~1/Umdrehung
//  • tiefe Pit-Beschädigung     → dichter kompakter Block, fast nur Verlust
// Rein & deterministisch (unit-testbar, kein Laufwerk).
struct DamageReport {
    // DriveHang = Laufwerk hat sich an Tracks hart aufgehängt (D-State),
    // bevor Paranoia die Schäden kartieren konnte → Karte ist leer/dünn,
    // das ist Laufwerks-Physik, kein per-Defektmuster diagnostizierbar.
    enum Kind { None, Scuff, Scratch, Gouge, Mixed, DriveHang };
    Kind        kind = None;
    int         bad_sectors = 0;       // Anzahl defekter Sektoren
    int         clusters    = 0;       // zusammenhängende Schadenszonen
    std::string headline;              // Klartext-Diagnose
    std::string advice;                // Handlungsempfehlung
};
// hung_tracks = Anzahl Tracks, die das Laufwerk hängen ließen / fehlschlugen
// (Rip-Pfad: rip_failed.size(); Scan-Pfad: 0). Bei Hängern ohne kartierbares
// Muster → DriveHang statt irreführendem „verstreute Einzelfehler".
DamageReport classify_damage(const std::vector<ProbeSample>& defects,
                             int lba_min, int lba_max, int ntracks,
                             int hung_tracks = 0);

// ── Archiv / Zustands-Log (JSON neben der Config) ──────────────────────────────
// Jeder Rip UND jeder Standalone-Scan = ein Eintrag (Verlauf, mehrfach pro
// Disc erlaubt → Zustands-Entwicklung über Reinigung sichtbar).
struct ArchiveEntry {
    long        ts = 0;            // Unix-Zeit
    std::string disc_id;          // MusicBrainz Disc-ID (Erkennung)
    std::string artist, title, year;
    int         tracks = 0;
    std::string kind   = "rip";   // "rip" | "scan"
    std::string format;           // flac/opus/mp3 (bei rip)
    int         ar_ok = 0, ar_total = 0;       // AccurateRip X/N
    int         damaged_tracks = 0;
    int         quality = 0;      // 0 clean · 1 marginal · 2 bad
    bool        scan_completed = true;
    std::string outcome;          // "ok" | "fehler" | "abgebrochen" | "scan"
    int         lba_min = 0, lba_max = 0;
    std::vector<ProbeSample> map; // gespeicherte Scan-Visualisierung
    // Pro-Track-Verdikt des Scans (Index = Track-Nr, 1-basiert; [0] leer).
    // Persistiert, damit ein späterer Rip denselben Discs den Scan-Plan
    // auch dann übernimmt, wenn der In-Memory-Session-Status fehlt.
    std::vector<int> track_status;
};
// Polar-Ring-Visualisierung als SVG (reiner Text, Qt-frei) — für den
// lokalen Sidecar im Albumverzeichnis. Leere map → Hinweis-SVG.
std::string scan_svg(const ProbeResult& r);

std::string archive_path();                       // config_dir()/archive.json
std::vector<ArchiveEntry> load_archive();         // neueste zuletzt
bool append_archive(const ArchiveEntry& e);       // lädt + hängt an + schreibt

void curl_global_setup();
void curl_global_teardown();

} // namespace cdr
