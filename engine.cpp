// engine.cpp — Implementierung des Qt-freien Kerns.
#include "engine.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <linux/cdrom.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <discid/discid.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <cdio/cdio.h>
#include <cdio/device.h>
#include <cdio/cdtext.h>
#include <cdio/paranoia/cdda.h>
#include <cdio/paranoia/paranoia.h>
}

using json = nlohmann::json;

namespace cdr {

// ───────────────────────────── Helfer ─────────────────────────────────────────

static std::string trim(std::string s) {
    auto sp = [](unsigned char c) { return std::isspace(c); };
    while (!s.empty() && sp(s.front())) s.erase(s.begin());
    while (!s.empty() && sp(s.back()))  s.pop_back();
    return s;
}

std::string sanitize(const std::string& in) {
    std::string o;
    o.reserve(in.size());
    for (unsigned char c : in) {
        if (c == '/' || c == '\\') o += '-';
        else if (c == 0 || c < 0x20) o += ' ';
        else if (c == ':' || c == '*' || c == '?' || c == '"' ||
                 c == '<' || c == '>' || c == '|') o += '_';
        else o += static_cast<char>(c);
    }
    o = trim(o);
    while (!o.empty() && o.back() == '.') o.pop_back();
    o = trim(o);
    if (o.empty()) o = "Unknown";
    if (o.size() > 120) o.resize(120);
    return o;
}

static std::string two(int n) { char b[16]; std::snprintf(b, sizeof b, "%02d", n); return b; }

// CD-TEXT ist (für westliche Releases) ISO-8859-1. libcdio liefert die Bytes
// roh; mit flac --no-utf8-convert würden Nicht-ASCII-Bytes ungültiges UTF-8.
// Latin-1 mappt 1:1 auf U+0080..U+00FF → triviale 2-Byte-UTF-8-Kodierung.
// Bereits gültiges UTF-8 (selten in CD-TEXT) wird nicht angefasst.
bool is_valid_utf8(const std::string& s) {
    int need = 0;
    for (unsigned char c : s) {
        if (need == 0) {
            if      (c < 0x80) continue;
            else if ((c >> 5) == 0x6) need = 1;
            else if ((c >> 4) == 0xE) need = 2;
            else if ((c >> 3) == 0x1E) need = 3;
            else return false;
        } else {
            if ((c >> 6) != 0x2) return false;
            --need;
        }
    }
    return need == 0;
}
std::string latin1_to_utf8(const std::string& s) {
    bool has_high = false;
    for (unsigned char c : s) if (c >= 0x80) { has_high = true; break; }
    if (!has_high || is_valid_utf8(s)) return s;
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c < 0x80) o += static_cast<char>(c);
        else { o += static_cast<char>(0xC0 | (c >> 6));
               o += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return o;
}

// Sampler-Heuristik: Album-Artist „Various…" oder ≥2 verschiedene Track-Artists.
bool looks_compilation(const Album& a) {
    std::string low;
    for (char c : a.artist) low += (char)std::tolower((unsigned char)c);
    if (low.find("various") != std::string::npos) return true;
    std::string first;
    for (const auto& t : a.tracks) {
        if (t.artist.empty()) continue;
        if (first.empty()) first = t.artist;
        else if (t.artist != first) return true;
    }
    return false;
}

std::string Album::year() const {
    return date.size() >= 4 ? date.substr(0, 4) : std::string();
}

std::vector<std::string> Album::folder_segments() const {
    std::vector<std::string> v{ sanitize(artist) };
    std::string y = year();
    v.push_back(y.empty() ? sanitize(title) : sanitize(title) + " (" + y + ")");
    return v;
}

// ───────────────────────────── Config ─────────────────────────────────────────

std::string default_config_path() {
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x)
        return std::string(x) + "/cdripper/config.ini";
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.config/cdripper/config.ini";
    return "./cdripper.ini";
}

static std::string g_cfg_path;          // aktiver Config-Pfad (von main gesetzt)
void set_config_path(const std::string& p) { g_cfg_path = p; }
std::string config_dir() {
    const std::string& base = g_cfg_path.empty() ? default_config_path()
                                                 : g_cfg_path;
    return fs::path(base).parent_path().string();
}
std::string profile_path(const std::string& name) {
    if (name.empty()) return default_config_path();
    return (fs::path(config_dir()) / "profiles" / (name + ".ini")).string();
}
std::vector<std::string> list_profiles() {
    std::vector<std::string> v;
    std::error_code ec;
    fs::path d = fs::path(config_dir()) / "profiles";
    for (auto it = fs::directory_iterator(d, ec);
         !ec && it != fs::directory_iterator(); ++it)
        if (it->path().extension() == ".ini")
            v.push_back(it->path().stem().string());
    std::sort(v.begin(), v.end());
    return v;
}
std::string active_profile() {
    std::ifstream f(fs::path(config_dir()) / "active_profile");
    std::string s;
    std::getline(f, s);
    return trim(s);
}
bool set_active_profile(const std::string& name) {
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::ofstream f(fs::path(config_dir()) / "active_profile",
                    std::ios::trunc);
    if (!f) return false;
    f << name << "\n";
    return true;
}

Config load_config(const std::string& path) {
    Config c;
    std::ifstream f(path);
    if (f) {
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if      (k == "device")        c.device       = v;
            else if (k == "nextcloud_url") c.nextcloud_url = v;
            else if (k == "webdav_user")   c.webdav_user   = v;
            else if (k == "webdav_pass")   c.webdav_pass   = v;
            else if (k == "music_root")    c.music_root    = v;
            else if (k == "tmpdir")        c.tmpdir        = v;
            else if (k == "musicbrainz_useragent") c.mb_useragent = v;
            else if (k == "acoustid_key")  c.acoustid_key  = v;
            else if (k == "read_speed")    c.read_speed = std::atoi(v.c_str());
            else if (k == "upload_retries") c.upload_retries = std::atoi(v.c_str());
            else if (k == "replaygain")    c.replaygain =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "audio_format")  c.audio_format  = v;
            else if (k == "audio_quality") c.audio_quality = std::atoi(v.c_str());
            else if (k == "preflight")     c.preflight =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "scan_density")  c.scan_density =
                std::atoi(v.c_str());
            else if (k == "recovery_budget_min") c.recovery_budget_min =
                std::atoi(v.c_str());
            else if (k == "jukebox")       c.jukebox =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "fast_rip")      c.fast_rip =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "accuraterip")   c.accuraterip =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "read_offset")   c.read_offset = std::atoi(v.c_str());
            else if (k == "auto_eject")    c.auto_eject =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "chime")         c.chime =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "lyrics")        c.lyrics =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "overwrite_existing") c.overwrite_existing =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "upload_backend") c.upload_backend = v;
            else if (k == "local_base")    c.local_base    = v;
            else if (k == "ssh_host")      c.ssh_host       = v;
            else if (k == "ssh_user")      c.ssh_user       = v;
            else if (k == "ssh_base")      c.ssh_base       = v;
            else if (k == "ssh_port")      c.ssh_port = std::atoi(v.c_str());
            else if (k == "smb_url")       c.smb_url        = v;
            else if (k == "smb_user")      c.smb_user       = v;
            else if (k == "smb_pass")      c.smb_pass       = v;
            else if (k == "registry_url")  c.registry_url   = v;
            else if (k == "registry_submit") c.registry_submit =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "registry_stats") c.registry_stats =
                (v == "1" || v == "true" || v == "yes");
            else if (k == "registry_condition") c.registry_condition =
                (v == "1" || v == "true" || v == "yes");
        }
    }
    if (const char* p = std::getenv("CDRIPPER_WEBDAV_PASS")) c.webdav_pass = p;
    if (const char* u = std::getenv("CDRIPPER_WEBDAV_USER")) c.webdav_user = u;
    if (const char* d = std::getenv("CDRIPPER_DEVICE"))      c.device      = d;
    if (const char* s = std::getenv("CDRIPPER_SMB_PASS"))    c.smb_pass    = s;
    return c;
}

bool save_config(const Config& c, const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    auto b = [](bool v) { return v ? "true" : "false"; };
    f << "# cdripper config — von der GUI geschrieben\n"
      << "device = "         << c.device       << "\n"
      << "read_speed = "     << c.read_speed   << "\n"
      << "tmpdir = "         << c.tmpdir       << "\n"
      << "musicbrainz_useragent = " << c.mb_useragent << "\n"
      << "acoustid_key = "   << c.acoustid_key << "\n"
      << "replaygain = "     << b(c.replaygain) << "\n"
      << "audio_format = "   << c.audio_format  << "\n"
      << "audio_quality = "  << c.audio_quality << "\n"
      << "preflight = "      << b(c.preflight)  << "\n"
      << "scan_density = "   << c.scan_density  << "\n"
      << "recovery_budget_min = " << c.recovery_budget_min << "\n"
      << "jukebox = "        << b(c.jukebox)    << "\n"
      << "fast_rip = "       << b(c.fast_rip)   << "\n"
      << "accuraterip = "    << b(c.accuraterip) << "\n"
      << "read_offset = "    << c.read_offset   << "\n"
      << "auto_eject = "     << b(c.auto_eject) << "\n"
      << "chime = "          << b(c.chime)      << "\n"
      << "lyrics = "         << b(c.lyrics)     << "\n"
      << "overwrite_existing = " << b(c.overwrite_existing) << "\n"
      << "upload_retries = " << c.upload_retries << "\n"
      << "music_root = "     << c.music_root   << "\n"
      << "\n# Upload-Backend: webdav | local | ssh | smb\n"
      << "upload_backend = " << c.upload_backend << "\n"
      << "nextcloud_url = "  << c.nextcloud_url << "\n"
      << "webdav_user = "    << c.webdav_user  << "\n"
      << "webdav_pass = "    << c.webdav_pass  << "\n"
      << "local_base = "     << c.local_base   << "\n"
      << "ssh_host = "       << c.ssh_host     << "\n"
      << "ssh_user = "       << c.ssh_user     << "\n"
      << "ssh_base = "       << c.ssh_base     << "\n"
      << "ssh_port = "       << c.ssh_port     << "\n"
      << "smb_url = "        << c.smb_url      << "\n"
      << "smb_user = "       << c.smb_user     << "\n"
      << "smb_pass = "       << c.smb_pass     << "\n"
      << "\n# Offset-Registry (T5) — leer = aus; submit/stats Opt-in\n"
      << "registry_url = "    << c.registry_url << "\n"
      << "registry_submit = " << b(c.registry_submit) << "\n"
      << "registry_stats = "  << b(c.registry_stats)  << "\n"
      << "registry_condition = " << b(c.registry_condition) << "\n";
    f.close();
    ::chmod(path.c_str(), 0600);   // enthält Passwörter
    return true;
}

// ── Rip-Session: Worker-Subprozess + Watchdog ──────────────────────────────────

RipResult rip_session(const std::string& device, const std::string& workdir,
                      int speed, bool fast, int stall_secs,
                      const std::function<void(const std::string&)>& onEvent,
                      const std::function<bool()>& stop,
                      const std::string& plan_csv, int track_budget_secs) {
    int pf[2];
    if (::pipe(pf) != 0) return RipResult::Fatal;
    std::string defcsv = plan_csv.empty() ? "-" : plan_csv;
    pid_t pid = ::fork();
    if (pid < 0) { ::close(pf[0]); ::close(pf[1]); return RipResult::Fatal; }
    if (pid == 0) {
        ::dup2(pf[1], STDOUT_FILENO);
        ::close(pf[0]); ::close(pf[1]);
        std::string sp = std::to_string(speed);
        ::execl("/proc/self/exe", "cdripper", "--rip-worker",
                device.c_str(), workdir.c_str(), sp.c_str(),
                fast ? "1" : "0", defcsv.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(pf[1]);
    int fd = pf[0];
    std::string buf;
    std::time_t last = std::time(nullptr);
    std::time_t trkStart = 0;                 // 0 = gerade kein Track aktiv
    RipResult res = RipResult::Ok;
    bool sawFatal = false;
    for (;;) {
        if (stop && stop()) { ::kill(pid, SIGKILL);
                              res = RipResult::Aborted; break; }
        // Per-Track-Zeitbudget: ein einzelner Track der trotz Fortschritt
        // (Stall-Watchdog greift dann NICHT) endlos grindet → wie Hänger
        // behandeln, damit Skip-ahead mit dem Rest weitermacht.
        if (track_budget_secs > 0 && trkStart &&
            std::time(nullptr) - trkStart > track_budget_secs) {
            ::kill(pid, SIGKILL);
            res = RipResult::Stalled;
            break;
        }
        struct pollfd p { fd, POLLIN, 0 };
        int pr = ::poll(&p, 1, 1000);
        if (pr > 0) {
            char t[8192];
            ssize_t k = ::read(fd, t, sizeof t);
            if (k > 0) {
                buf.append(t, (size_t)k);
                last = std::time(nullptr);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string ln = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (ln.rfind("FATAL", 0) == 0) sawFatal = true;
                    if (ln.rfind("RIPSTART ", 0) == 0)
                        trkStart = std::time(nullptr);
                    else if (ln.rfind("RIPDONE ", 0) == 0 ||
                             ln.rfind("RIPERR ", 0) == 0)
                        trkStart = 0;
                    if (onEvent) onEvent(ln);
                }
            } else if (k == 0) {
                break;                       // EOF — Worker fertig
            }
        } else if (pr == 0) {
            if (std::time(nullptr) - last > stall_secs) {
                ::kill(pid, SIGKILL);
                res = RipResult::Stalled;
                break;
            }
        }
    }
    ::close(fd);
    int st = 0;
    for (int i = 0; i < 50; ++i) {           // nicht ewig auf D-State warten
        pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid || r < 0) break;
        ::usleep(100000);
    }
    if (res == RipResult::Ok) {
        if (sawFatal) res = RipResult::Fatal;
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0 &&
                 WEXITSTATUS(st) != 2) res = RipResult::Fatal;
    }
    return res;
}

// Bewusst eine eigene, schlanke Kopie der rip_session-Watchdog-Plumbing
// statt die produktiv-bewährte rip_session zu refactoren (Risiko-Minimierung;
// die Poll/Stall/D-State-Logik ist identisch, nur das exec-Argv differiert).
RipResult probe_session(const std::string& device, int stall_secs,
                         const std::function<void(const std::string&)>& onEvent,
                         const std::function<bool()>& stop,
                         int start_track, int density) {
    int pf[2];
    if (::pipe(pf) != 0) return RipResult::Fatal;
    std::string sst = std::to_string(start_track < 1 ? 1 : start_track);
    std::string sden = std::to_string(density < 1 ? 6 : density);
    pid_t pid = ::fork();
    if (pid < 0) { ::close(pf[0]); ::close(pf[1]); return RipResult::Fatal; }
    if (pid == 0) {
        ::dup2(pf[1], STDOUT_FILENO);
        ::close(pf[0]); ::close(pf[1]);
        ::execl("/proc/self/exe", "cdripper", "--probe-worker",
                device.c_str(), sst.c_str(), sden.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(pf[1]);
    int fd = pf[0];
    std::string buf;
    std::time_t last = std::time(nullptr);
    RipResult res = RipResult::Ok;
    bool sawFatal = false;
    for (;;) {
        if (stop && stop()) { ::kill(pid, SIGKILL);
                              res = RipResult::Aborted; break; }
        struct pollfd p { fd, POLLIN, 0 };
        int pr = ::poll(&p, 1, 1000);
        if (pr > 0) {
            char t[8192];
            ssize_t k = ::read(fd, t, sizeof t);
            if (k > 0) {
                buf.append(t, (size_t)k);
                last = std::time(nullptr);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string ln = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (ln.rfind("FATAL", 0) == 0) sawFatal = true;
                    if (onEvent) onEvent(ln);
                }
            } else if (k == 0) {
                break;
            }
        } else if (pr == 0) {
            if (std::time(nullptr) - last > stall_secs) {
                ::kill(pid, SIGKILL);
                res = RipResult::Stalled;
                break;
            }
        }
    }
    ::close(fd);
    int st = 0;
    for (int i = 0; i < 50; ++i) {
        pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid || r < 0) break;
        ::usleep(100000);
    }
    if (res == RipResult::Ok) {
        if (sawFatal) res = RipResult::Fatal;
        else if (WIFEXITED(st) && WEXITSTATUS(st) != 0) res = RipResult::Fatal;
    }
    return res;
}

ProbeResult probe_classify(const std::vector<ProbeRaw>& raw, int ntracks,
                           int disc_end, const std::vector<int>& hung,
                           int skips, RipResult rr) {
    ProbeResult r;
    r.completed = (rr == RipResult::Ok);
    // Karte IMMER aus den gesammelten Stichproben bauen — auch bei Stall:
    // genau dann zeigt sie, WO es hing (die letzte gelesene Position).
    std::vector<long> good;
    for (auto& s : raw) if (s.ok) good.push_back(s.ms);
    long med = 0;                          // global — nur fürs Detail-Label
    if (!good.empty()) {
        std::vector<long> g = good;
        std::sort(g.begin(), g.end());
        med = g[g.size() / 2];
    }
    // „langsam" RELATIV ZU DEN NACHBARN statt global: der Laufwerk-
    // Anlauf am Disc-Anfang (Spin-up) ist ein glatter Abfall über viele
    // Stichproben → lokal konsistent → KEIN Flag (sonst wird eine saubere
    // CD fälschlich „grenzwertig"). Ein Kratzer ist ein Zeit-Spike
    // gegenüber den unmittelbaren Nachbarn → Flag bleibt.
    const int WIN = 4;                      // gute Proben je Seite
    auto local_med = [&](int gp) -> long {
        int a = std::max(0, gp - WIN);
        int b = std::min((int)good.size() - 1, gp + WIN);
        std::vector<long> w(good.begin() + a, good.begin() + b + 1);
        std::sort(w.begin(), w.end());
        return w.empty() ? med : w[w.size() / 2];
    };
    std::vector<int> status(raw.size(), 0);
    {
        int gp = -1;                        // Position in good[] (Reihenfolge)
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i].ok == 0) { status[i] = 2; continue; }
            ++gp;
            long base = (good.size() < 5) ? med : local_med(gp);
            long lthr = std::max<long>(base * 3, base + 300);
            status[i] = (raw[i].ms > lthr) ? 1 : 0;
        }
    }
    for (size_t i = 0; i < raw.size(); ++i) {
        const auto& s = raw[i];
        int stt = status[i];
        r.map.push_back({ s.lba, stt });
        ++r.samples;
        if (stt != 0) ++r.problems;
        if (r.samples == 1) { r.lba_min = r.lba_max = s.lba; }
        else { if (s.lba < r.lba_min) r.lba_min = s.lba;
               if (s.lba > r.lba_max) r.lba_max = s.lba; }
    }
    int errs = 0; for (auto& s : raw) if (s.ok == 0) ++errs;
    // Pro-Track-Verdikt aus den Stichproben des jeweiligen Tracks.
    int maxtrk = 0; for (auto& s : raw) maxtrk = std::max(maxtrk, s.trk);
    // ntr auf die echte Trackzahl deckeln (kein Phantom-Track jenseits TOC).
    int ntr = ntracks > 0 ? ntracks
            : std::max(maxtrk, hung.empty() ? 0 : hung.back());
    r.track_status.assign(ntr + 1, -1);
    for (size_t i = 0; i < raw.size(); ++i) {
        const auto& s = raw[i];
        if (s.trk >= 1 && s.trk <= ntr)
            r.track_status[s.trk] =
                std::max(r.track_status[s.trk], status[i]);
    }
    bool gave_up = (rr != RipResult::Ok && rr != RipResult::Aborted);
    for (int h : hung) if (h >= 1 && h <= ntr) r.track_status[h] = 2; // defekt
    for (int t = 1; t <= ntr; ++t) {
        if (r.track_status[t] != -1) continue;
        // unbeprobt: vor maxtrk = liegt zwischen Stichproben → rippbar;
        // dahinter & Scan gab auf = nie erreicht → ungescannt.
        r.track_status[t] = (gave_up && t > maxtrk) ? 3 : 0;
    }
    // Hänger-Tracks zusätzlich als rote Markierung auf die Karte (geschätzte
    // Radialposition, da unbeprobt — disc_end·track/ntracks).
    if (disc_end > 0 && ntr > 0)
        for (int h : hung)
            r.map.push_back({ (int)((long long)disc_end * h / ntr), 2 });
    if (disc_end > r.lba_max) r.lba_max = disc_end;   // echte Geometrie
    if (r.samples) r.lba_min = 0;                      // Disc-Anfang = innen

    double score = r.samples ? (double)r.problems / r.samples : 0.0;
    bool any_defect = !hung.empty();
    if (any_defect || errs > 0 || score >= 0.10) r.quality = DiscQuality::Bad;
    else if (score > 0.0)                         r.quality = DiscQuality::Marginal;
    else                                          r.quality = DiscQuality::Clean;

    std::string base = std::to_string(r.problems) + "/" +
        std::to_string(r.samples) + " auffällig" +
        (errs ? ", " + std::to_string(errs) + " unlesbar" : "") +
        " (Basis " + std::to_string(med) + " ms)";
    if (!hung.empty()) {
        std::string hl;
        for (size_t i = 0; i < hung.size(); ++i)
            hl += (i ? "," : "") + std::to_string(hung[i]);
        base += " · Hänger umgangen, Track(s) " + hl + " defekt";
        if (gave_up) base += " · Scan nach " + std::to_string(skips) +
                             " Sprüngen gestoppt (Rest ungescannt)";
    } else if (rr == RipResult::Aborted) {
        base = "Scan abgebrochen (Teil-Karte) — " + base;
    }
    r.detail = base;
    return r;
}

DamageReport classify_damage(const std::vector<ProbeSample>& defects,
                             int lba_min, int lba_max, int ntracks) {
    DamageReport d;
    // Defekte einsammeln, je LBA der schlimmste Status, sortiert.
    std::map<int,int> bad;
    for (auto& s : defects)
        if (s.status >= 1) { int& v = bad[s.lba]; if (s.status > v) v = s.status; }
    d.bad_sectors = (int)bad.size();
    if (bad.empty()) {
        d.kind = DamageReport::None;
        d.headline = "Keine defekten Sektoren erfasst — sauberer Rip.";
        return d;
    }
    const int    spanDisc = std::max(1, lba_max - lba_min);
    const int    CLUSTER_GAP = 16;     // nur ECHT benachbarte Sektoren = eine
                                       // Zone (radiale Furche trifft ~1/Umdr.
                                       // mit guten Sektoren dazwischen → bleibt
                                       // bewusst zerlegt, nicht zu Block fusion)
    const int    BIG_SPAN    = 150;    // ~2 s zusammenhängend = umlaufend
    // Zusammenhängende Schadenszonen bilden.
    struct Cl { int lo, hi, cnt, hard; };
    std::vector<Cl> cl;
    for (auto& kv : bad) {
        int lba = kv.first, st = kv.second;
        if (cl.empty() || lba - cl.back().hi > CLUSTER_GAP)
            cl.push_back({ lba, lba, 0, 0 });
        cl.back().hi = lba; cl.back().cnt++;
        if (st >= 2) cl.back().hard++;
    }
    d.clusters = (int)cl.size();
    int oLo = bad.begin()->first, oHi = bad.rbegin()->first;
    int overall = std::max(1, oHi - oLo + 1);
    std::vector<int> sizes; int big = 0; long deepWorst = -1;
    for (auto& k : cl) {
        sizes.push_back(k.cnt);
        int sp = k.hi - k.lo + 1;
        if (sp >= BIG_SPAN) ++big;
        // tiefe Gouge: großer Block, fast nur Total-Verlust, dicht.
        if (sp >= 80 && k.cnt >= 0.6 * sp &&
            k.hard >= 0.8 * k.cnt) deepWorst = std::max<long>(deepWorst, sp);
    }
    std::sort(sizes.begin(), sizes.end());
    int medSize = sizes[sizes.size() / 2];
    // Mediane Lücke zwischen Clustern (≈ Sektoren/Umdrehung bei radial).
    std::vector<int> gaps;
    for (size_t i = 1; i < cl.size(); ++i)
        gaps.push_back(cl[i].lo - cl[i-1].hi);
    int medGap = gaps.empty() ? 0
               : (std::sort(gaps.begin(), gaps.end()),
                  gaps[gaps.size() / 2]);
    auto trk = [&](int lba){
        if (ntracks <= 0) return 0;
        int t = 1 + (int)((long long)(lba - lba_min) * ntracks / spanDisc);
        return t < 1 ? 1 : (t > ntracks ? ntracks : t);
    };
    // Füllgrad über die gesamte Schadensspanne: dicht = solider Block
    // (Wisch/Gouge), dünn = periodische Einzeltreffer (radiale Furche).
    double fill = (double)d.bad_sectors / overall;
    bool isGouge   = deepWorst > 0;
    // Radiale Furche: viele winzige Zonen, dünn gefüllt, regelmäßig
    // ~1/Umdrehung (Lücke wenige Dutzend Sektoren), über weite Spanne.
    bool isScratch = !isGouge && (int)cl.size() >= 8 && medSize <= 4 &&
                     fill <= 0.15 && medGap >= 4 && medGap <= 96 &&
                     overall >= 8 * (medGap + medSize);
    // Umlaufender Wisch/Schmutz → dichte, konzentrierte Zone(n).
    bool isScuff   = !isGouge && !isScratch && fill >= 0.35 &&
                     ((int)cl.size() <= 4 || big >= 1);
    std::string where = ntracks > 0
        ? " (~Track " + std::to_string(trk(oLo)) +
          (trk(oHi) != trk(oLo) ? "–" + std::to_string(trk(oHi)) : "") + ")"
        : "";
    std::string base = std::to_string(d.bad_sectors) +
        " defekte Sektoren in " + std::to_string(d.clusters) +
        " Zone(n)" + where + " — ";
    if (isGouge) {
        d.kind = DamageReport::Gouge;
        d.headline = base + "tiefe Pit-Beschädigung (Total-Verlust, lokal).";
        d.advice = "Physikalisches Limit — der jetzige Best-Effort-Rip ist "
                   "vermutlich das Maximum; anderes Laufwerk kann minimal "
                   "mehr holen, Politur hilft hier nicht.";
    } else if (isScratch) {
        d.kind = DamageReport::Scratch;
        d.headline = base + "radialer Kratzer (quert viele Windungen).";
        d.advice = "Vorsichtig RADIAL polieren (Mitte→Rand, nicht im Kreis), "
                   "dann neu rippen.";
    } else if (isScuff) {
        d.kind = DamageReport::Scuff;
        d.headline = base + "umlaufender Wisch / Schmutz / Fingerabdruck.";
        d.advice = "Disc mit weichem Tuch von der Mitte nach außen reinigen "
                   "und neu rippen — danach meist voll lesbar.";
    } else {
        d.kind = DamageReport::Mixed;
        d.headline = base + "verstreute Einzelfehler, kein klares Muster.";
        d.advice = "Reinigen und neu rippen; bei Bestand anderes Laufwerk.";
    }
    return d;
}

ProbeResult disc_probe(const std::string& device,
                        const std::function<bool()>& stop,
                        const std::function<void(int,int,long)>& onSample,
                        int density,
                        const std::function<void(int)>& onCursor) {
    // Rohdaten erst sammeln, DANN relativ klassifizieren: jede Stichprobe
    // trägt konstanten Seek-/Spin-Overhead → ein fester ms-Schwellwert
    // würde bei langsamem Laufwerk ALLES als „langsam" melden (keine
    // Lokalisierung). Stattdessen: Median = disc-eigene Basislinie,
    // nur deutliche Ausreißer + echte Lesefehler (NULL) zählen.
    std::vector<ProbeRaw> raw;
    int disc_end = 0;                          // Leadout-LBA (echtes Disc-Ende)
    int ntracks = 0;
    // Lauf-Median der bisher guten Lesezeiten → PROVISORISCHE Live-
    // Einstufung pro Stichprobe (0 ok·1 langsam·2 Fehler), damit der Ring
    // schon WÄHREND des Scans rot/orange wird. Die endgültige, exakte
    // Einfärbung macht probe_classify am Schluss (Median über ALLE Proben).
    std::vector<long> rgood;
    auto onEv = [&](const std::string& ln) {
        if (ln.rfind("PROBE ", 0) == 0) {
            int trk = 0, lba = 0, ok = 0; long ms = 0;
            if (std::sscanf(ln.c_str(), "PROBE %d %d %d %ld",
                            &trk, &lba, &ok, &ms) == 4) {
                raw.push_back({ trk, lba, ok, ms });
                int st = 0;
                if (ok == 0) st = 2;
                else {
                    rgood.push_back(ms);
                    if (rgood.size() >= 4) {
                        std::vector<long> g = rgood;
                        std::sort(g.begin(), g.end());
                        long m = g[g.size() / 2];
                        if (ms > std::max<long>(m * 3, m + 300)) st = 1;
                    }
                }
                if (onSample) onSample(lba, st, ms);   // 2. Arg = Live-Status
            }
        } else if (ln.rfind("PCUR ", 0) == 0) {
            if (onCursor) onCursor(std::atoi(ln.c_str() + 5));
        } else if (ln.rfind("PEND ", 0) == 0) {
            disc_end = std::atoi(ln.c_str() + 5);
        } else if (ln.rfind("PTOC ", 0) == 0) {
            ntracks = std::atoi(ln.c_str() + 5);
        }
    };
    // Skip-ahead-Orchestrierung: hängt das Laufwerk (D-State), wird der
    // Track als defekt verbucht, kurz auf Erholung gewartet (der D-State-
    // Orphan gibt /dev/sr0 erst nach Kernel-Timeout frei — KEIN Drive-Open
    // hier, das würde im Scan-Thread selbst hängen), dann beim nächsten
    // Track weitergescannt. Begrenzt, sonst läuft eine tote Disc ewig.
    std::vector<int> hung;                     // Tracks mit D-State-Hänger
    int start = 1, skips = 0;
    const int MAX_SKIPS = 4;
    RipResult rr = RipResult::Ok;
    for (;;) {
        rr = probe_session(device, 30, onEv, stop, start, density);
        if (rr == RipResult::Ok || rr == RipResult::Aborted) break;
        int lasttrk = 0;
        for (auto& s : raw) lasttrk = std::max(lasttrk, s.trk);
        int hungtrk = std::max(start, lasttrk + 1);
        if (ntracks > 0 && hungtrk > ntracks) hungtrk = ntracks; // kein Phantom
        if (std::find(hung.begin(), hung.end(), hungtrk) == hung.end())
            hung.push_back(hungtrk);
        // Hänger SOFORT live als roten Marker zeigen (geschätzte Radial-
        // position), nicht erst am Schluss aus probe_classify.
        if (onSample && disc_end > 0 && ntracks > 0)
            onSample((int)((long long)disc_end * hungtrk / ntracks), 2, 0);
        if (++skips > MAX_SKIPS) break;
        if (ntracks > 0 && hungtrk >= ntracks) break;
        if (stop && stop()) break;
        start = hungtrk + 1;                   // diesen Track überspringen
        for (int i = 0; i < 45 && !(stop && stop()); ++i) ::sleep(1);
        if (stop && stop()) break;
    }
    return probe_classify(raw, ntracks, disc_end, hung, skips, rr);
}

std::string scan_svg(const ProbeResult& r) {
    const double C = 200, R = 180, ri = R * 0.32, ro = R * 0.92;
    std::ostringstream s;
    s << "<svg xmlns='http://www.w3.org/2000/svg' width='400' height='400' "
         "viewBox='0 0 400 400'>\n"
         "<rect width='400' height='400' fill='#1e2127'/>\n"
         "<circle cx='200' cy='200' r='180' fill='#2b2f37'/>\n";
    if (r.map.empty() || r.lba_max <= r.lba_min) {
        s << "<text x='200' y='205' fill='#9aa0aa' font-family='sans-serif' "
             "font-size='16' text-anchor='middle'>kein Scan</text>\n";
    } else {
        auto m = r.map;
        std::sort(m.begin(), m.end(),
                  [](const ProbeSample& a, const ProbeSample& b){
                      return a.lba < b.lba; });
        auto rad = [&](int lba){
            double n = (double)(lba - r.lba_min) /
                       (double)(r.lba_max - r.lba_min);
            return ri + n * (ro - ri);
        };
        auto col = [](int st){
            return st == 2 ? "#c0392b" : st == 1 ? "#e0a83e" : "#27ae60";
        };
        for (size_t i = 0; i < m.size(); ++i) {
            double r0 = rad(m[i].lba);
            double r1 = (i + 1 < m.size()) ? rad(m[i + 1].lba) : ro;
            double w  = std::max(2.0, r1 - r0);
            int st = (i + 1 < m.size())
                   ? std::max(m[i].status, m[i + 1].status) : m[i].status;
            s << "<circle cx='200' cy='200' r='" << (r0 + r1) / 2.0
              << "' fill='none' stroke='" << col(st)
              << "' stroke-width='" << w << "'/>\n";
        }
    }
    s << "<circle cx='200' cy='200' r='" << ri * 0.55
      << "' fill='#1e2127'/>\n"
      << "<circle cx='200' cy='200' r='" << R
      << "' fill='none' stroke='#3a3f4b'/>\n"
      << "<circle cx='200' cy='200' r='" << ro
      << "' fill='none' stroke='#3a3f4b'/>\n"
      << "<text x='200' y='392' fill='#9aa0aa' font-family='sans-serif' "
         "font-size='12' text-anchor='middle'>"
         "aussen=Disc-Rand · gruen ok · gelb langsam · rot Lesefehler"
         "</text>\n</svg>\n";
    return s.str();
}

// ── Archiv / Zustands-Log ──────────────────────────────────────────────────────
std::string archive_path() { return config_dir() + "/archive.json"; }

std::vector<ArchiveEntry> load_archive() {
    std::vector<ArchiveEntry> v;
    std::ifstream f(archive_path());
    if (!f) return v;
    json j;
    try { f >> j; } catch (...) { return v; }
    if (!j.is_array()) return v;
    for (auto& o : j) {
        ArchiveEntry e;
        e.ts             = o.value("ts", (long)0);
        e.disc_id        = o.value("disc_id", std::string());
        e.artist         = o.value("artist", std::string());
        e.title          = o.value("title", std::string());
        e.year           = o.value("year", std::string());
        e.tracks         = o.value("tracks", 0);
        e.kind           = o.value("kind", std::string("rip"));
        e.format         = o.value("format", std::string());
        e.ar_ok          = o.value("ar_ok", 0);
        e.ar_total       = o.value("ar_total", 0);
        e.damaged_tracks = o.value("damaged", 0);
        e.quality        = o.value("quality", 0);
        e.scan_completed = o.value("scan_completed", true);
        e.outcome        = o.value("outcome", std::string());
        e.lba_min        = o.value("lba_min", 0);
        e.lba_max        = o.value("lba_max", 0);
        if (o.contains("map") && o["map"].is_array())
            for (auto& m : o["map"])
                e.map.push_back({ m.value("l", 0), m.value("s", 0) });
        if (o.contains("tstatus") && o["tstatus"].is_array())
            for (auto& t : o["tstatus"])
                e.track_status.push_back(t.is_number() ? (int)t : -1);
        v.push_back(std::move(e));
    }
    return v;
}

static json entry_to_json(const ArchiveEntry& e) {
    json m = json::array();
    for (const auto& s : e.map)
        m.push_back(json{ {"l", s.lba}, {"s", s.status} });
    json ts = json::array();
    for (int s : e.track_status) ts.push_back(s);
    return json{
        {"ts", e.ts}, {"disc_id", e.disc_id}, {"artist", e.artist},
        {"title", e.title}, {"year", e.year}, {"tracks", e.tracks},
        {"kind", e.kind}, {"format", e.format}, {"ar_ok", e.ar_ok},
        {"ar_total", e.ar_total}, {"damaged", e.damaged_tracks},
        {"quality", e.quality}, {"scan_completed", e.scan_completed},
        {"outcome", e.outcome}, {"lba_min", e.lba_min},
        {"lba_max", e.lba_max}, {"map", m}, {"tstatus", ts} };
}

bool append_archive(const ArchiveEntry& e) {
    json j = json::array();
    {
        std::ifstream f(archive_path());
        if (f) { try { f >> j; } catch (...) { j = json::array(); }
                 if (!j.is_array()) j = json::array(); }
    }
    j.push_back(entry_to_json(e));
    std::error_code ec;
    fs::create_directories(config_dir(), ec);
    std::string tmp = archive_path() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << j.dump(1);
    }
    fs::rename(tmp, archive_path(), ec);
    return !ec;
}

// ───────────────────────────── HTTP ───────────────────────────────────────────

void curl_global_setup()    { curl_global_init(CURL_GLOBAL_DEFAULT); }
void curl_global_teardown() { curl_global_cleanup(); }

static size_t w_str(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n); return s * n;
}
static size_t w_file(char* p, size_t s, size_t n, void* u) {
    return std::fwrite(p, s, n, static_cast<FILE*>(u)) * s;
}

static std::optional<std::string> http_get(const std::string& url,
                                           const std::string& ua, long& code) {
    CURL* c = curl_easy_init();
    if (!c) return std::nullopt;
    std::string body;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, w_str);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return std::nullopt;
    return body;
}

static bool http_download(const std::string& url, const std::string& ua,
                          const fs::path& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    FILE* fp = std::fopen(out.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(c); return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, w_file);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, fp);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    std::fclose(fp);
    curl_easy_cleanup(c);
    bool ok = (rc == CURLE_OK && code >= 200 && code < 300);
    if (!ok) { std::error_code ec; fs::remove(out, ec); }
    return ok;
}

static std::string esc(CURL* c, const std::string& s) {
    char* e = curl_easy_escape(c, s.c_str(), (int)s.size());
    std::string r = e ? e : s;
    if (e) curl_free(e);
    return r;
}

static std::optional<std::string> http_post_json(const std::string& url,
                                                 const std::string& body,
                                                 const std::string& ua,
                                                 long& code) {
    CURL* c = curl_easy_init();
    if (!c) return std::nullopt;
    std::string resp;
    struct curl_slist* hdr =
        curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, w_str);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return std::nullopt;
    return resp;
}

static std::optional<std::string> http_post_form(const std::string& url,
        const std::string& body, const std::string& ua, long& code) {
    CURL* c = curl_easy_init();
    if (!c) return std::nullopt;
    std::string resp;
    struct curl_slist* hdr = curl_slist_append(nullptr,
        "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, ua.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, w_str);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) return std::nullopt;
    return resp;
}

static std::string reg_base(const std::string& u) {
    std::string b = u;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}

// ───────────────────────────── MusicBrainz ────────────────────────────────────

// nlohmann value("k","") wirft type_error.302, wenn der Key zwar
// EXISTIERT, aber null ist (häufig bei AcoustID/CDDB/Such-Antworten:
// "title":null usw.). Immer null-sicher als String lesen.
static std::string jstr(const json& j, const char* k) {
    if (!j.is_object()) return std::string();
    auto it = j.find(k);
    if (it == j.end() || !it->is_string()) return std::string();
    return it->get<std::string>();
}
static std::string ac_join(const json& ac) {
    std::string s;
    if (!ac.is_array()) return s;
    for (const auto& e : ac) { s += jstr(e, "name"); s += jstr(e, "joinphrase"); }
    return trim(s);
}
static std::string ac_first_artist_id(const json& ac) {
    if (ac.is_array() && !ac.empty() && ac[0].contains("artist"))
        return jstr(ac[0]["artist"], "id");
    return "";
}
// Top-N MB-Genres (nach count) aus einem genres-Array.
static std::vector<std::string> top_genres(const json& g, size_t n) {
    std::vector<std::pair<int, std::string>> v;
    if (g.is_array())
        for (const auto& e : g)
            v.push_back({ e.is_object() && e.contains("count") &&
                              e["count"].is_number()
                              ? e["count"].get<int>() : 0,
                          jstr(e, "name") });
    std::stable_sort(v.begin(), v.end(),
        [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<std::string> r;
    for (auto& p : v) { if (p.second.empty()) continue;
                        r.push_back(p.second); if (r.size() >= n) break; }
    return r;
}

static std::optional<Album> parse_release(const json& rel, const json& med) {
    Album a;
    { std::string ti = jstr(rel, "title");
      a.title = ti.empty() ? "Unknown Album" : ti; }
    a.date          = jstr(rel, "date");
    a.mb_release_id = jstr(rel, "id");
    a.country       = jstr(rel, "country");
    if (a.country.empty() && rel.contains("release-events") &&
        rel["release-events"].is_array() && !rel["release-events"].empty()) {
        const json& ev = rel["release-events"][0];
        if (ev.contains("area") && ev["area"].is_object()) {
            const json& ar = ev["area"];
            if (ar.contains("iso-3166-1-codes") &&
                ar["iso-3166-1-codes"].is_array() &&
                !ar["iso-3166-1-codes"].empty() &&
                ar["iso-3166-1-codes"][0].is_string())
                a.country = ar["iso-3166-1-codes"][0].get<std::string>();
            else
                a.country = jstr(ar, "name");
        }
    }
    if (rel.contains("artist-credit")) {
        a.artist = ac_join(rel["artist-credit"]);
        a.mb_artist_id = ac_first_artist_id(rel["artist-credit"]);
    }
    if (a.artist.empty()) a.artist = "Various Artists";
    a.disc_number = med.value("position", 1);
    a.disc_total  = rel.contains("media") ? (int)rel["media"].size() : 1;
    // T2: Genre/Label/Barcode/ORIGINALDATE
    a.barcode = jstr(rel, "barcode");
    if (rel.contains("label-info") && rel["label-info"].is_array() &&
        !rel["label-info"].empty()) {
        const auto& li = rel["label-info"][0];
        if (li.contains("label")) a.label = jstr(li["label"], "name");
        a.catalogno = jstr(li, "catalog-number");
    }
    if (rel.contains("release-group")) {
        const auto& rg = rel["release-group"];
        a.originaldate = jstr(rg, "first-release-date");
        if (rg.contains("genres")) a.genres = top_genres(rg["genres"], 3);
    }
    if (a.genres.empty() && rel.contains("genres"))
        a.genres = top_genres(rel["genres"], 3);
    if (med.contains("tracks"))
        for (const auto& tr : med["tracks"]) {
            Track t;
            t.number = tr.is_object() && tr.contains("position") &&
                       tr["position"].is_number()
                       ? tr["position"].get<int>()
                       : (int)a.tracks.size() + 1;
            t.title  = jstr(tr, "title");
            if (t.title.empty() && tr.contains("recording"))
                t.title = jstr(tr["recording"], "title");
            if (t.title.empty()) t.title = "Track " + two(t.number);
            if (tr.contains("artist-credit")) t.artist = ac_join(tr["artist-credit"]);
            if (t.artist.empty() && tr.contains("recording") &&
                tr["recording"].contains("artist-credit"))
                t.artist = ac_join(tr["recording"]["artist-credit"]);
            if (t.artist.empty()) t.artist = a.artist;
            if (tr.contains("artist-credit"))
                t.mb_artist_id = ac_first_artist_id(tr["artist-credit"]);
            if (t.mb_artist_id.empty() && tr.contains("recording") &&
                tr["recording"].contains("artist-credit"))
                t.mb_artist_id =
                    ac_first_artist_id(tr["recording"]["artist-credit"]);
            if (t.mb_artist_id.empty()) t.mb_artist_id = a.mb_artist_id;
            if (tr.contains("recording")) t.mb_track_id = jstr(tr["recording"], "id");
            a.tracks.push_back(std::move(t));
        }
    if (a.tracks.empty()) return std::nullopt;
    a.compilation = looks_compilation(a);
    return a;
}

// Findet das Medium einer Release, dessen Disc unsere discid trägt (sonst [0]).
static const json* medium_for(const json& rel, const std::string& discid) {
    if (!rel.contains("media")) return nullptr;
    for (const auto& m : rel["media"]) {
        if (!m.contains("discs")) continue;
        for (const auto& d : m["discs"])
            if (jstr(d, "id") == discid) return &m;
    }
    return rel["media"].empty() ? nullptr : &rel["media"][0];
}

// Fuzzy: kein Disc-ID-Treffer → Medium mit passender Trackzahl wählen.
static const json* medium_by_tracks(const json& rel, int ntr) {
    if (!rel.contains("media") || rel["media"].empty()) return nullptr;
    for (const auto& m : rel["media"]) {
        int tc = (m.contains("tracks") && m["tracks"].is_array())
                 ? (int)m["tracks"].size()
                 : (m.contains("track-count") && m["track-count"].is_number()
                    ? m["track-count"].get<int>() : -1);
        if (ntr > 0 && tc == ntr) return &m;
    }
    return &rel["media"][0];
}

// MusicBrainz „CD stub": nutzergemeldete, inoffizielle Tracklist (kein
// Release-Match). Besser als Platzhalter — Artist/Titel/Tracktitel.
static std::optional<Album> parse_cdstub(const json& j) {
    if (!j.contains("cdstub")) return std::nullopt;
    const json& s = j["cdstub"];
    Album a;
    a.artist = jstr(s, "artist");
    a.title  = jstr(s, "title");
    a.barcode = jstr(s, "barcode");
    if (s.contains("tracks") && s["tracks"].is_array()) {
        int n = 1;
        for (const auto& t : s["tracks"]) {
            Track tr;
            tr.number = n++;
            tr.title  = jstr(t, "title");
            if (tr.title.empty()) tr.title = "Track " + two(tr.number);
            tr.artist = jstr(t, "artist");
            if (tr.artist.empty()) tr.artist = a.artist;
            a.tracks.push_back(std::move(tr));
        }
    }
    if (a.tracks.empty() || (a.artist.empty() && a.title.empty()))
        return std::nullopt;
    a.compilation = looks_compilation(a);
    return a;
}

std::vector<Album> mb_release_candidates(const std::string& discid,
                                         const std::string& ua,
                                         const std::string& toc,
                                         std::string* source) {
    std::vector<Album> out;
    auto setsrc = [&](const char* s) { if (source) *source = s; };
    // 1) Exakter Disc-ID-Lookup.
    std::string url = "https://musicbrainz.org/ws/2/discid/" + discid +
                      "?fmt=json&inc=recordings+artist-credits+genres+"
                      "labels+release-groups";
    long code = 0;
    auto body = http_get(url, ua, code);
    ::sleep(1);
    if (body && code == 200) {
        json j;
        try { j = json::parse(*body); } catch (...) { j = json(); }
        if (j.contains("releases")) {
            for (const auto& r : j["releases"]) {
                const json* m = medium_for(r, discid);
                if (!m) continue;
                if (auto a = parse_release(r, *m)) out.push_back(std::move(*a));
            }
            std::stable_sort(out.begin(), out.end(),   // exakte Treffer vorn
                [&](const Album& x, const Album&) {
                    for (const auto& r : j["releases"]) {
                        if (jstr(r, "id") != x.mb_release_id) continue;
                        if (!r.contains("media")) return false;
                        for (const auto& md : r["media"])
                            if (md.contains("discs"))
                                for (const auto& d : md["discs"])
                                    if (jstr(d, "id") == discid)
                                        return true;
                    }
                    return false;
                });
            if (!out.empty()) setsrc("exakte Disc-ID");
        }
    }
    // 2) Fuzzy: keine exakte Disc-ID in MB → TOC-Suche (findet die Release
    //    auch, wenn diese Pressung nie als Disc-ID eingereicht wurde).
    if (out.empty() && !toc.empty()) {
        std::string enc;
        for (char c : toc) enc += (c == ' ' ? '+' : c);
        std::string furl =
            "https://musicbrainz.org/ws/2/discid/-?toc=" + enc +
            "&fmt=json&inc=recordings+artist-credits+genres+"
            "labels+release-groups";
        long fc = 0;
        auto fb = http_get(furl, ua, fc);
        ::sleep(1);
        if (fb && fc == 200) {
            json fj;
            try { fj = json::parse(*fb); } catch (...) { fj = json(); }
            int first = 0, last = 0;
            std::sscanf(toc.c_str(), "%d %d", &first, &last);
            int ntr = (first > 0 && last >= first) ? last - first + 1 : 0;
            if (fj.contains("releases"))
                for (const auto& r : fj["releases"]) {
                    const json* m = medium_by_tracks(r, ntr);
                    if (!m) continue;
                    if (auto a = parse_release(r, *m))
                        out.push_back(std::move(*a));
                }
            if (!out.empty()) setsrc("TOC-Fuzzy");
            if (out.empty())
                if (auto cs = parse_cdstub(fj)) {
                    out.push_back(std::move(*cs));
                    setsrc("CD-Stub");
                }
        }
    }
    return out;
}

std::optional<Album> mb_lookup(const std::string& discid, const std::string& ua,
                               const std::string& toc, std::string* source) {
    auto v = mb_release_candidates(discid, ua, toc, source);
    if (v.empty()) return std::nullopt;
    return v.front();
}

// ── gnudb / CDDB (FreeDB-Protokoll über den TOC-String) ────────────────────────
std::optional<Album> cddb_lookup(const std::string& toc, const std::string& ua) {
    if (toc.empty()) return std::nullopt;
    std::vector<int> v;
    { std::istringstream is(toc); int x; while (is >> x) v.push_back(x); }
    if (v.size() < 4) return std::nullopt;
    int first = v[0], last = v[1], leadout = v[2];
    int ntr = last - first + 1;
    if (ntr <= 0 || (int)v.size() < 3 + ntr) return std::nullopt;
    std::vector<int> offs(v.begin() + 3, v.begin() + 3 + ntr);  // LBA-Sektoren
    ArIds ids = ar_ids_from_toc(offs, leadout);                 // .cddb (FreeDB)
    char hexid[16];
    std::snprintf(hexid, sizeof hexid, "%08x", ids.cddb);
    const std::string hello =
        "&hello=cdripper+localhost+cdripper+1.0&proto=6";
    // CDDB-Frame-Offsets = LBA + 150; Disc-Länge = (Leadout+150)/75 Sekunden.
    std::string q = std::string("cmd=cddb+query+") + hexid + "+" +
                    std::to_string(ntr);
    for (int o : offs) q += "+" + std::to_string(o + 150);
    q += "+" + std::to_string((leadout + 150) / 75) + hello;
    long code = 0;
    auto body =
        http_get("https://gnudb.gnudb.org/~cddb/cddb.cgi?" + q, ua, code);
    ::sleep(1);
    if (!body || code != 200) return std::nullopt;
    std::string cat, did;
    {
        std::istringstream is(*body);
        std::string line; std::getline(is, line);
        int rc = std::atoi(line.c_str());
        if (rc == 200) {
            std::istringstream ls(line);
            std::string cs; ls >> cs >> cat >> did;
        } else if (rc == 211 || rc == 210) {
            std::string l;
            while (std::getline(is, l)) {
                if (!l.empty() && l.back() == '\r') l.pop_back();
                if (l == ".") break;
                std::istringstream ls(l); ls >> cat >> did; break;
            }
        }
    }
    if (cat.empty() || did.empty()) return std::nullopt;
    auto rd = http_get("https://gnudb.gnudb.org/~cddb/cddb.cgi?cmd=cddb+read+" +
                       cat + "+" + did + hello, ua, code);
    ::sleep(1);
    if (!rd || code != 200) return std::nullopt;
    auto u8 = [](std::string s) {
        return is_valid_utf8(s) ? s : latin1_to_utf8(s); };
    Album a;
    std::map<int, std::string> tt;
    std::istringstream is(*rd);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("DTITLE=", 0) == 0) {
            std::string d = line.substr(7);
            auto p = d.find(" / ");
            if (p != std::string::npos) {
                a.artist += d.substr(0, p);
                a.title  += d.substr(p + 3);
            } else a.title += d;
        } else if (line.rfind("DYEAR=", 0) == 0) {
            a.date = line.substr(6);
        } else if (line.rfind("TTITLE", 0) == 0) {
            auto eq = line.find('=');
            if (eq != std::string::npos && eq > 6) {
                int n = std::atoi(line.substr(6, eq - 6).c_str());
                tt[n] += line.substr(eq + 1);
            }
        }
    }
    a.artist = u8(a.artist);
    a.title  = u8(a.title);
    if (a.artist.empty() && a.title.empty()) return std::nullopt;
    for (int i = 0; i < ntr; ++i) {
        Track t;
        t.number = i + 1;
        t.title  = tt.count(i) ? u8(tt[i]) : ("Track " + two(i + 1));
        t.artist = a.artist;
        a.tracks.push_back(std::move(t));
    }
    if (a.tracks.empty()) return std::nullopt;
    a.compilation = looks_compilation(a);
    return a;
}

// ── Manuelle MusicBrainz-Textsuche ─────────────────────────────────────────────
std::vector<ReleaseHit> mb_search_releases(const std::string& artist,
                                           const std::string& title,
                                           const std::string& ua) {
    std::vector<ReleaseHit> out;
    std::string lucene;
    if (!title.empty())  lucene += "release:\"" + title + "\"";
    if (!artist.empty()) {
        if (!lucene.empty()) lucene += " AND ";
        lucene += "artist:\"" + artist + "\"";
    }
    if (lucene.empty()) return out;
    CURL* c = curl_easy_init();
    if (!c) return out;
    std::string q = esc(c, lucene);
    curl_easy_cleanup(c);
    long code = 0;
    auto body = http_get("https://musicbrainz.org/ws/2/release/?query=" + q +
                         "&fmt=json&limit=15", ua, code);
    ::sleep(1);
    if (!body || code != 200) return out;
    json j;
    try { j = json::parse(*body); } catch (...) { return out; }
    if (!j.contains("releases")) return out;
    for (const auto& r : j["releases"]) {
        ReleaseHit h;
        h.mbid  = jstr(r, "id");
        h.title = jstr(r, "title");
        if (r.contains("artist-credit")) h.artist = ac_join(r["artist-credit"]);
        h.date    = jstr(r, "date");
        h.country = jstr(r, "country");
        if (r.contains("media") && r["media"].is_array() &&
            !r["media"].empty()) {
            const auto& m0 = r["media"][0];
            if (m0.contains("track-count") && m0["track-count"].is_number())
                h.tracks = m0["track-count"].get<int>();
        }
        if (!h.mbid.empty()) out.push_back(std::move(h));
    }
    return out;
}

std::optional<Album> mb_release_by_id(const std::string& mbid, int want_tracks,
                                      const std::string& ua) {
    if (mbid.empty()) return std::nullopt;
    long code = 0;
    auto body = http_get("https://musicbrainz.org/ws/2/release/" + mbid +
        "?fmt=json&inc=recordings+artist-credits+genres+labels+"
        "release-groups", ua, code);
    ::sleep(1);
    if (!body || code != 200) return std::nullopt;
    json r;
    try { r = json::parse(*body); } catch (...) { return std::nullopt; }
    const json* m = medium_by_tracks(r, want_tracks);
    if (!m) return std::nullopt;
    return parse_release(r, *m);
}

// ── AcoustID / Chromaprint (Erkennung am Klang) ────────────────────────────────
std::optional<AcoustIdHit> acoustid_identify(const fs::path& audio,
                                             int duration_sec,
                                             const std::string& key,
                                             const std::string& ua) {
    if (key.empty()) return std::nullopt;
    // fpcalc liefert {"duration":N,"fingerprint":"..."}. Der Pfad wird von
    // uns erzeugt (keine Shell-Sonderzeichen) → einfache Quotes genügen.
    std::string cmd = "fpcalc -json '" + audio.string() + "' 2>/dev/null";
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return std::nullopt;
    std::string fpjson;
    char b[8192]; size_t n;
    while ((n = std::fread(b, 1, sizeof b, p)) > 0) fpjson.append(b, n);
    int prc = ::pclose(p);
    if (prc != 0 || fpjson.empty()) return std::nullopt;
    json fj;
    try { fj = json::parse(fpjson); } catch (...) { return std::nullopt; }
    std::string fp = jstr(fj, "fingerprint");
    int dur = duration_sec;
    if (dur <= 0 && fj.contains("duration") && fj["duration"].is_number())
        dur = (int)fj["duration"].get<double>();
    if (fp.empty() || dur <= 0) return std::nullopt;
    CURL* c = curl_easy_init();
    if (!c) return std::nullopt;
    std::string form = "client=" + esc(c, key) +
                       "&duration=" + std::to_string(dur) +
                       "&meta=recordings+releases" +
                       "&fingerprint=" + esc(c, fp);
    curl_easy_cleanup(c);
    long code = 0;
    auto resp = http_post_form("https://api.acoustid.org/v2/lookup",
                               form, ua, code);
    if (!resp || code != 200) return std::nullopt;
    json aj;
    try { aj = json::parse(*resp); } catch (...) { return std::nullopt; }
    if (jstr(aj, "status") != "ok" || !aj.contains("results") ||
        !aj["results"].is_array())
        return std::nullopt;
    for (const auto& res : aj["results"]) {
        AcoustIdHit h;
        if (res.contains("score") && res["score"].is_number())
            h.score = res["score"].get<double>();
        if (res.contains("recordings") && res["recordings"].is_array())
            for (const auto& rec : res["recordings"]) {
                if (h.recording.empty()) h.recording = jstr(rec, "title");
                if (h.artist.empty() && rec.contains("artists") &&
                    rec["artists"].is_array() && !rec["artists"].empty())
                    h.artist = jstr(rec["artists"][0], "name");
                if (rec.contains("releases") && rec["releases"].is_array() &&
                    !rec["releases"].empty())
                    h.mb_release_id = jstr(rec["releases"][0], "id");
                if (!h.mb_release_id.empty()) break;
            }
        if (!h.mb_release_id.empty() || !h.recording.empty()) return h;
    }
    return std::nullopt;
}

bool fetch_url(const std::string& url, const std::string& ua,
               const std::string& out) {
    return http_download(url, ua, fs::path(out));
}

std::vector<std::string> caa_image_urls(const std::string& rid,
                                        const std::string& ua) {
    std::vector<std::string> v;
    if (rid.empty()) return v;
    long code = 0;
    auto body = http_get("https://coverartarchive.org/release/" + rid, ua, code);
    if (!body || code != 200) return v;
    try {
        json j = json::parse(*body);
        if (j.contains("images") && j["images"].is_array())
        for (const auto& img : j["images"]) {
            std::string u = jstr(img, "image");
            if (u.empty() && img.contains("thumbnails"))
                u = jstr(img["thumbnails"], "large");
            if (!u.empty()) v.push_back(u);
        }
    } catch (...) {}
    return v;
}

std::optional<Album> cdtext_lookup(const std::string& device, int n) {
    CdIo_t* cdio = cdio_open(device.c_str(), DRIVER_UNKNOWN);
    if (!cdio) return std::nullopt;
    cdtext_t* ct = cdio_get_cdtext(cdio);
    if (!ct) { cdio_destroy(cdio); return std::nullopt; }
    auto fld = [&](track_t t, cdtext_field_t f) -> std::string {
        const char* v = cdtext_get_const(ct, f, t);
        return v ? latin1_to_utf8(trim(v)) : std::string();
    };
    Album a;
    a.artist = fld(0, CDTEXT_FIELD_PERFORMER);
    a.title  = fld(0, CDTEXT_FIELD_TITLE);
    if (a.artist.empty() && a.title.empty()) { cdio_destroy(cdio); return std::nullopt; }
    if (a.artist.empty()) a.artist = "Unknown Artist";
    if (a.title.empty())  a.title  = "Unknown Album";
    for (int i = 1; i <= n; ++i) {
        Track t;
        t.number = i;
        t.title  = fld(i, CDTEXT_FIELD_TITLE);
        if (t.title.empty()) t.title = "Track " + two(i);
        t.artist = fld(i, CDTEXT_FIELD_PERFORMER);
        if (t.artist.empty()) t.artist = a.artist;
        a.tracks.push_back(std::move(t));
    }
    cdio_destroy(cdio);
    a.compilation = looks_compilation(a);
    return a;
}

Album placeholder_album(int n) {
    Album a;
    a.artist = "Unknown Artist";
    a.title  = "Unknown Album";
    for (int i = 1; i <= n; ++i)
        a.tracks.push_back(Track{ i, "Track " + two(i), a.artist, "" });
    return a;
}

bool fetch_cover(const std::string& rid, const std::string& ua,
                 const fs::path& dir, fs::path& out) {
    if (rid.empty()) return false;
    fs::path cov = dir / "cover.jpg";
    const std::string base = "https://coverartarchive.org/release/" + rid;
    // Größer zuerst: 1200px (guter Kompromiss), dann Original, dann 500px.
    if (http_download(base + "/front-1200", ua, cov) ||
        http_download(base + "/front",      ua, cov) ||
        http_download(base + "/front-500",  ua, cov)) {
        out = cov;
        return true;
    }
    return false;
}

std::string fetch_synced_lyrics(const std::string& artist,
                                const std::string& title,
                                const std::string& album,
                                int dur, const std::string& ua) {
    if (artist.empty() || title.empty()) return "";
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string url = "https://lrclib.net/api/get?artist_name=" +
        esc(c, artist) + "&track_name=" + esc(c, title) +
        "&album_name=" + esc(c, album) +
        "&duration=" + std::to_string(dur);
    curl_easy_cleanup(c);
    long code = 0;
    auto body = http_get(url, ua, code);
    if (!body || code != 200) return "";
    try {
        json j = json::parse(*body);
        std::string s = j.value("syncedLyrics", "");
        return s;                       // nur synced (mit Zeitstempeln)
    } catch (...) { return ""; }
}

// ───────────────────────────── Disc-Ident ─────────────────────────────────────

// Toleranter TOC-Fallback für kopiergeschützte / multisession Discs
// (Copy Control / Cactus Data Shield — der unbeschriebene Außenring ist
// eine gefälschte 2. Daten-Session, die libdiscid beim TOC-Lesen abbrechen
// lässt). Das einfachere libcdio kann die Audio-Tracks der 1. Session oft
// trotzdem aufzählen; daraus die TOC rekonstruieren und discid_put()
// füttern → echte MusicBrainz-Disc-ID + TOC-String für die Metadaten-Kette.
static bool libcdio_audio_toc(const std::string& dev,
                              int& first, int& last, int offsets[100]) {
    CdIo_t* c = cdio_open(dev.c_str(), DRIVER_UNKNOWN);
    if (!c) return false;
    track_t ft = cdio_get_first_track_num(c);
    track_t nt = cdio_get_num_tracks(c);
    if (ft == CDIO_INVALID_TRACK || nt == CDIO_INVALID_TRACK || nt == 0) {
        cdio_destroy(c); return false;
    }
    int n = 0, last_audio_end = 0;
    for (track_t t = ft; t < ft + nt && n < 99; ++t) {
        if (cdio_get_track_format(c, t) != TRACK_FORMAT_AUDIO) continue;
        lba_t lba = cdio_get_track_lba(c, t);          // = LSN + 150
        if (lba == CDIO_INVALID_LBA) continue;
        offsets[++n] = (int)lba;
        lsn_t ll = cdio_get_track_last_lsn(c, t);
        if (ll != CDIO_INVALID_LSN) last_audio_end = (int)ll + 150 + 1;
    }
    lba_t lo = cdio_get_track_lba(c, CDIO_CDROM_LEADOUT_TRACK);
    cdio_destroy(c);
    if (n < 1) return false;
    offsets[0] = (lo != CDIO_INVALID_LBA && (int)lo > offsets[n])
                 ? (int)lo : last_audio_end;
    if (offsets[0] <= offsets[n]) offsets[0] = offsets[n] + 300;  // Notnagel
    first = 1; last = n;
    return true;
}

// 0 = Fehler, 1 = normal (libdiscid), 2 = rekonstruiert (Copy-Control-Pfad)
static int discid_read_tolerant(DiscId* d, const std::string& dev) {
    if (discid_read_sparse(d, dev.c_str(), 0)) return 1;
    int first, last, offs[100];
    if (!libcdio_audio_toc(dev, first, last, offs)) return 0;
    return discid_put(d, first, last, offs) ? 2 : 0;
}

DiscIdent read_disc_ident(const std::string& device) {
    DiscId* d = discid_new();
    int mode = discid_read_tolerant(d, device);
    if (mode == 0) {
        std::string e = discid_get_error_msg(d) ? discid_get_error_msg(d) : "?";
        discid_free(d);
        throw std::runtime_error("Disc-ID lesen fehlgeschlagen: " + e);
    }
    DiscIdent r;
    r.reconstructed  = (mode == 2);
    r.id             = discid_get_id(d) ? discid_get_id(d) : "";
    r.submission_url = discid_get_submission_url(d) ? discid_get_submission_url(d) : "";
    int f = discid_get_first_track_num(d), l = discid_get_last_track_num(d);
    r.toc_tracks = (l >= f) ? (l - f + 1) : 0;
    if (const char* t = discid_get_toc_string(d)) r.toc = t;  // Fuzzy-Fallback
    discid_free(d);
    return r;
}

std::string probe_disc_id(const std::string& device) noexcept {
    DiscId* d = discid_new();
    std::string id;
    if (discid_read_tolerant(d, device) && discid_get_id(d))
        id = discid_get_id(d);
    discid_free(d);
    return id;
}

// ───────────────────────────── Ripper ─────────────────────────────────────────

static int para_mode_full() { return PARANOIA_MODE_FULL ^ PARANOIA_MODE_NEVERSKIP; }
static int para_mode_fast() { return PARANOIA_MODE_OVERLAP ^ PARANOIA_MODE_NEVERSKIP; }

Ripper::Ripper(const std::string& device, int speed, bool fast)
    : fast_(fast) {
    cdrom_drive_t* dr = cdio_cddap_identify(device.c_str(), 0, nullptr);
    if (!dr) throw std::runtime_error("cdio_cddap_identify fehlgeschlagen");
    if (cdio_cddap_open(dr) != 0) {
        cdio_cddap_close(dr);
        throw std::runtime_error("cdio_cddap_open fehlgeschlagen");
    }
    if (speed > 0)
        speed_applied_ = (cdio_cddap_speed_set(dr, speed) == 0);
    cdrom_paranoia_t* p = cdio_paranoia_init(dr);
    if (!p) { cdio_cddap_close(dr); throw std::runtime_error("paranoia_init"); }
    cdio_paranoia_modeset(p, fast_ ? para_mode_fast() : para_mode_full());
    drive_ = dr;
    para_  = p;

    track_t n = cdio_cddap_tracks(dr);
    int idx = 0;
    for (track_t t = 1; t <= n; ++t) {
        if (!cdio_cddap_track_audiop(dr, t)) continue;
        lsn_t lo = cdio_cddap_track_firstsector(dr, t);
        lsn_t hi = cdio_cddap_track_lastsector(dr, t);
        if (lo < 0 || hi < lo) continue;
        int sec = (int)(hi - lo + 1);
        tracks_.push_back(AudioTrack{ ++idx, (int)t, sec });
        total_sectors_ += sec;
    }
    if (tracks_.empty()) {
        cdio_paranoia_free(p);
        cdio_cddap_close(dr);
        throw std::runtime_error("keine Audio-Tracks auf der Disc");
    }
}

Ripper::~Ripper() {
    if (para_)  cdio_paranoia_free(static_cast<cdrom_paranoia_t*>(para_));
    if (drive_) cdio_cddap_close(static_cast<cdrom_drive_t*>(drive_));
}

static void wav_header(FILE* f, uint32_t bytes) {
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(36 + bytes);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(16);
    w16(1); w16(2); w32(44100); w32(44100 * 4); w16(4); w16(16);
    std::fwrite("data", 1, 4, f); w32(bytes);
}

// Paranoia ruft den Callback synchron im selben Thread → thread_local Zähler.
static thread_local long g_para_skips   = 0;   // unlesbar/übersprungen = Aussetzer
static thread_local long g_para_softerr = 0;   // Jitter-Fixups (meist unhörbar)
// Präzise Defekt-Karte: der Rip-Loop setzt g_cur_lba je Sektor; para_cb
// hängt (LBA, Severity) an — coalesced, gleicher LBA behält max Severity.
// Severity 2 = verlorene/unlesbare Samples (hörbar), 1 = recovered.
static thread_local long g_cur_lba = 0;
static thread_local std::vector<std::pair<int,int>> g_defects;
static void para_cb(long /*inpos*/, paranoia_cb_mode_t mode) {
    int sev = 0;
    switch (mode) {
        case PARANOIA_CB_SKIP:              ++g_para_skips;   sev = 2; break;
        case PARANOIA_CB_READERR:
        case PARANOIA_CB_FIXUP_DROPPED:
        case PARANOIA_CB_FIXUP_DUPED:       ++g_para_softerr; sev = 2; break;
        // NUR echte „paranoia musste hart arbeiten"-Events → gelb.
        // OVERLAP/DRIFT/FIXUP_EDGE/FIXUP_ATOM sind Routine-Jitterabgleich
        // (feuern bei JEDER vollen-paranoia-Lesung, auch auf sauberen
        // Discs) → bewusst NICHT als Defekt werten (sonst alles gelb).
        case PARANOIA_CB_REPAIR:
        case PARANOIA_CB_SCRATCH:
        case PARANOIA_CB_BACKOFF:            sev = 1; break;
        default: break;
    }
    if (sev == 0) return;
    if (!g_defects.empty() && g_defects.back().first == (int)g_cur_lba) {
        if (sev > g_defects.back().second) g_defects.back().second = sev;
    } else {
        g_defects.push_back({ (int)g_cur_lba, sev });
    }
}

void Ripper::set_speed(int speed) {
    auto* dr = static_cast<cdrom_drive_t*>(drive_);
    // speed<=0 → so schnell wie möglich (großer Wert, Drive clamped auf Max).
    speed_applied_ = (cdio_cddap_speed_set(dr, speed > 0 ? speed : 1000) == 0);
}

void Ripper::set_fast(bool fast) {
    fast_ = fast;
    cdio_paranoia_modeset(static_cast<cdrom_paranoia_t*>(para_),
                          fast_ ? para_mode_fast() : para_mode_full());
}

long Ripper::rip(const AudioTrack& at, const fs::path& wav,
                 const std::function<void(double)>& progress,
                 const std::function<bool()>& stop,
                 const std::function<void(int,int)>& defect) {
    auto* dr = static_cast<cdrom_drive_t*>(drive_);
    auto* p  = static_cast<cdrom_paranoia_t*>(para_);
    lsn_t lo = cdio_cddap_track_firstsector(dr, at.cd_track);
    lsn_t hi = cdio_cddap_track_lastsector(dr, at.cd_track);
    if (lo < 0 || hi < lo) throw std::runtime_error("ungültige Sektoren");
    uint32_t nsec = (uint32_t)(hi - lo + 1);

    auto one_pass = [&]() {
        FILE* out = std::fopen(wav.c_str(), "wb");
        if (!out)
            throw std::runtime_error("kann WAV nicht schreiben: " + wav.string());
        wav_header(out, nsec * CDIO_CD_FRAMESIZE_RAW);
        cdio_paranoia_seek(p, lo, SEEK_SET);
        g_para_skips = 0;
        g_para_softerr = 0;
        g_defects.clear();
        auto flush_defects = [&]{
            if (!defect || g_defects.empty()) return;
            for (auto& d : g_defects) defect(d.first, d.second);
            g_defects.clear();
        };
        uint32_t done = 0;
        for (lsn_t s = lo; s <= hi; ++s) {
            if (stop && stop()) {
                std::fclose(out);
                std::error_code ec; fs::remove(wav, ec);
                throw std::runtime_error("abgebrochen");
            }
            g_cur_lba = (long)s;                 // Position für para_cb
            int16_t* buf = cdio_paranoia_read(p, para_cb);
            if (!buf) {
                const char* err = cdio_cddap_errors(dr);
                std::fclose(out);
                throw std::runtime_error(std::string("Leseabbruch") +
                    (err ? std::string(": ") + err : ""));
            }
            std::fwrite(buf, 1, CDIO_CD_FRAMESIZE_RAW, out);
            if (++done % 64 == 0) {
                if (progress) progress((double)done / nsec);
                flush_defects();
            }
        }
        flush_defects();
        std::fclose(out);
        if (progress) progress(1.0);
    };

    one_pass();
    // Schnell-Modus: bei Lesefehlern denselben Track in FULL nachziehen.
    if (fast_ && g_para_skips > 0) {
        cdio_paranoia_modeset(p, para_mode_full());
        one_pass();
        cdio_paranoia_modeset(p, para_mode_fast());   // zurück für Folgetracks
    }
    return g_para_skips;
}

// ───────────────────────────── AccurateRip ────────────────────────────────────

void Ripper::ar_toc(std::vector<int>& offs, int& leadout) const {
    auto* dr = static_cast<cdrom_drive_t*>(drive_);
    offs.clear();
    int last_hi = 0;
    for (const auto& at : tracks_) {
        lsn_t lo = cdio_cddap_track_firstsector(dr, at.cd_track);
        lsn_t hi = cdio_cddap_track_lastsector(dr, at.cd_track);
        offs.push_back((int)lo);            // rohe LBA (AccurateRip id1/id2)
        last_hi = (int)hi;
    }
    leadout = last_hi + 1;                   // Leadout-LBA (kein +150!)
}

void Ripper::probe(int per_track, int sect,
                   const std::function<void(int,int,bool,long)>& emit,
                   const std::function<bool()>& stop, int from_track,
                   const std::function<void(int)>& onSeek) {
    auto* dr = static_cast<cdrom_drive_t*>(drive_);
    auto* p  = static_cast<cdrom_paranoia_t*>(para_);
    // Roh lesen: keine Recovery → ein kaputter Sektor liefert SCHNELL NULL
    // statt minutenlangem paranoia-Grinden. Echte D-State-Hänger (Treiber)
    // fängt der Watchdog im Eltern-Prozess ab.
    cdio_paranoia_modeset(p, PARANOIA_MODE_DISABLE);
    if (per_track < 1) per_track = 1;
    if (sect < 1) sect = 1;
    for (const auto& at : tracks_) {
        if (at.index < from_track) continue;     // Skip-ahead nach Hänger
        if (stop && stop()) return;
        lsn_t lo = cdio_cddap_track_firstsector(dr, at.cd_track);
        lsn_t hi = cdio_cddap_track_lastsector(dr, at.cd_track);
        if (lo < 0 || hi < lo) continue;
        for (int k = 0; k < per_track; ++k) {
            if (stop && stop()) return;
            double frac = (double)(k + 1) / (per_track + 1);
            int lba = (int)lo + (int)(frac * (double)(hi - lo));
            if (onSeek) onSeek(lba);             // Echtzeit-Cursor VOR dem Read
            cdio_paranoia_seek(p, lba, SEEK_SET);
            auto t0 = std::chrono::steady_clock::now();
            bool bad = false;
            for (int s = 0; s < sect; ++s) {
                int16_t* buf = cdio_paranoia_read(p, para_cb);
                if (!buf) { bad = true; break; }
            }
            long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            emit(at.index, lba, !bad, ms);
        }
    }
}

static int sum_digits(int n) { int s = 0; while (n > 0) { s += n % 10; n /= 10; }
                               return s; }

ArIds ar_ids_from_toc(const std::vector<int>& offs, int leadout) {
    ArIds r;
    r.ntracks = (int)offs.size();
    uint32_t id1 = 0, id2 = 0;
    int n = 0;
    for (size_t i = 0; i < offs.size(); ++i) {
        id1 += (uint32_t)offs[i];                          // rohe LBA
        id2 += (uint32_t)(offs[i] ? offs[i] : 1) * (uint32_t)(i + 1);
        n   += sum_digits((offs[i] + 150) / 75);           // CDDB: +150 (Sek.)
    }
    id1 += (uint32_t)leadout;
    id2 += (uint32_t)leadout * (uint32_t)(r.ntracks + 1);
    int total = (leadout + 150) / 75 -
                (offs.empty() ? 0 : (offs[0] + 150) / 75);
    r.cddb = (uint32_t)(((n % 255) << 24) | (total << 8) | r.ntracks);
    r.id1 = id1;
    r.id2 = id2;
    return r;
}

std::string ArIds::url() const {
    char b[160];
    std::snprintf(b, sizeof b,
        "http://www.accuraterip.com/accuraterip/%x/%x/%x/"
        "dBAR-%03d-%08x-%08x-%08x.bin",
        id1 & 0xF, (id1 >> 4) & 0xF, (id1 >> 8) & 0xF,
        ntracks, id1, id2, cddb);
    return b;
}

void ar_crc_file(const fs::path& wav, bool first, bool last, int offset,
                  uint32_t& v1, uint32_t& v2) {
    v1 = 0; v2 = 0;
    std::ifstream f(wav, std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 44) return;
    f.seekg(44, std::ios::beg);
    size_t nwords = (size_t)((sz - 44) / 4);
    std::vector<uint32_t> w(nwords);
    f.read(reinterpret_cast<char*>(w.data()), (std::streamsize)nwords * 4);
    const size_t trim = 5 * 588;            // 5 Frames Disc-Rand-Trimmung
    size_t lo = first ? trim : 0;
    size_t hi = (last && nwords > trim) ? nwords - trim : nwords;
    uint64_t a1 = 0, a2 = 0;
    for (size_t k = lo; k < hi; ++k) {
        long src = (long)k + offset;
        uint32_t s = (src >= 0 && (size_t)src < nwords) ? w[src] : 0u;
        uint64_t mult = (uint64_t)(k + 1);
        uint64_t prod = (uint64_t)s * mult;
        a1 += (uint32_t)(s * (uint32_t)mult);
        a2 += (uint32_t)(prod & 0xffffffffu) + (uint32_t)(prod >> 32);
    }
    v1 = (uint32_t)a1;
    v2 = (uint32_t)a2;
}

std::vector<ArMatch> ar_lookup(
    const ArIds& ids,
    const std::vector<std::pair<uint32_t, uint32_t>>& crcs,
    const std::string& ua) {
    std::vector<ArMatch> out;
    long code = 0;
    auto body = http_get(ids.url(), ua, code);
    if (!body || code != 200 || body->size() < 13) return out;
    const unsigned char* p = (const unsigned char*)body->data();
    size_t len = body->size(), pos = 0;
    auto u32 = [&](size_t o) {
        return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) |
               ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
    };
    int nt = ids.ntracks;
    std::vector<ArMatch> acc(nt, ArMatch{ 0, 0, false, false });
    for (int i = 0; i < nt; ++i) acc[i].track = i + 1;
    while (pos + 13 <= len) {
        int tc = p[pos];
        pos += 13;                          // 1 trackcount + 3×u32 header
        if (tc <= 0 || pos + (size_t)tc * 9 > len) break;
        for (int t = 0; t < tc; ++t) {
            int conf       = p[pos];
            uint32_t crc   = u32(pos + 1);
            pos += 9;                        // conf + crc + crc450
            if (t < nt && t < (int)crcs.size()) {
                if (crc != 0 && crc == crcs[t].first) {
                    acc[t].v1 = true; acc[t].confidence += conf;
                }
                if (crc != 0 && crc == crcs[t].second) {
                    acc[t].v2 = true; acc[t].confidence += conf;
                }
            }
        }
    }
    out = acc;
    return out;
}

std::vector<std::vector<uint32_t>> ar_db_crcs(const ArIds& ids,
                                              const std::string& ua) {
    std::vector<std::vector<uint32_t>> per(ids.ntracks);
    long code = 0;
    auto body = http_get(ids.url(), ua, code);
    if (!body || code != 200 || body->size() < 13) return per;
    const unsigned char* p = (const unsigned char*)body->data();
    size_t len = body->size(), pos = 0;
    auto u32 = [&](size_t o) {
        return (uint32_t)p[o] | ((uint32_t)p[o + 1] << 8) |
               ((uint32_t)p[o + 2] << 16) | ((uint32_t)p[o + 3] << 24);
    };
    while (pos + 13 <= len) {
        int tc = p[pos];
        pos += 13;
        if (tc <= 0 || pos + (size_t)tc * 9 > len) break;
        for (int t = 0; t < tc; ++t) {
            uint32_t crc = u32(pos + 1);
            pos += 9;
            if (t < ids.ntracks && crc != 0) per[t].push_back(crc);
        }
    }
    return per;
}

// ── Laufwerks-Identität & Offset-Store ─────────────────────────────────────────

HwInfo drive_hwinfo(const std::string& device) {
    HwInfo h;
    CdIo_t* c = cdio_open(device.c_str(), DRIVER_UNKNOWN);
    if (!c) return h;
    cdio_hwinfo_t hw;
    if (cdio_get_hwinfo(c, &hw)) {
        h.vendor   = trim(hw.psz_vendor);
        h.model    = trim(hw.psz_model);
        h.revision = trim(hw.psz_revision);
        h.ok = !(h.vendor.empty() && h.model.empty());
    }
    cdio_destroy(c);
    return h;
}

std::vector<std::string> list_optical_devices() {
    std::vector<std::string> v;
    char** d = cdio_get_devices(DRIVER_DEVICE);
    if (d) {
        for (int i = 0; d[i]; ++i)
            if (d[i][0]) v.push_back(d[i]);
        cdio_free_device_list(d);
    }
    return v;
}

std::string drive_id(const std::string& device) {
    HwInfo h = drive_hwinfo(device);
    std::string id = h.ok ? trim(h.vendor + " " + h.model) : "unknown-drive";
    if (id.empty()) id = "unknown-drive";
    for (char& ch : id)
        if (ch == '=' || ch == '\n' || ch == '\r') ch = '_';
    return id;
}

std::string drive_offsets_path() {
    return (fs::path(config_dir()) / "drive_offsets.ini").string();
}

static std::map<std::string, int> load_offsets() {
    std::map<std::string, int> m;
    std::ifstream f(drive_offsets_path());
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.rfind('=');
        if (eq == std::string::npos) continue;
        m[trim(line.substr(0, eq))] = std::atoi(trim(line.substr(eq + 1)).c_str());
    }
    return m;
}

std::optional<int> lookup_drive_offset(const std::string& did) {
    auto m = load_offsets();
    auto it = m.find(did);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

bool save_drive_offset(const std::string& did, int offset) {
    auto m = load_offsets();
    m[did] = offset;
    std::string path = drive_offsets_path();
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# cdripper — per-Laufwerk AccurateRip Read-Offset (Samples)\n";
    for (const auto& kv : m) f << kv.first << " = " << kv.second << "\n";
    return true;
}

bool delete_drive_offset(const std::string& did) {
    auto m = load_offsets();
    if (!m.erase(did)) return false;
    std::ofstream f(drive_offsets_path(), std::ios::trunc);
    if (!f) return false;
    f << "# cdripper — per-Laufwerk AccurateRip Read-Offset (Samples)\n";
    for (const auto& kv : m) f << kv.first << " = " << kv.second << "\n";
    return true;
}

std::vector<DriveOffset> list_drive_offsets() {
    std::vector<DriveOffset> v;
    for (const auto& kv : load_offsets())
        v.push_back({ kv.first, kv.second });
    return v;
}

// ── Offset-Registry-Client (T5) ────────────────────────────────────────────────
std::optional<int> registry_lookup_offset(const std::string& base_url,
                                          const std::string& model,
                                          const std::string& ua) {
    if (base_url.empty() || model.empty()) return std::nullopt;
    CURL* c = curl_easy_init();
    if (!c) return std::nullopt;
    std::string url = reg_base(base_url) + "/api/offset?model=" + esc(c, model);
    curl_easy_cleanup(c);
    long code = 0;
    auto body = http_get(url, ua, code);
    if (!body || code != 200) return std::nullopt;
    try {
        json j = json::parse(*body);
        if (j.contains("offset") && j["offset"].is_number_integer())
            return j["offset"].get<int>();
    } catch (...) {}
    return std::nullopt;
}

bool registry_submit_offset(const std::string& base_url,
                            const std::string& model, int offset,
                            const std::string& ua) {
    if (base_url.empty() || model.empty()) return false;
    json j = {{"model", model}, {"offset", offset}, {"source", "cdripper"}};
    long code = 0;
    auto r = http_post_json(reg_base(base_url) + "/api/offset",
                            j.dump(), ua, code);
    return r.has_value() && code == 200;
}

bool registry_submit_stat(const std::string& base_url,
                          const std::string& model, const std::string& version,
                          bool ar_match, const std::string& err,
                          const std::string& ua) {
    if (base_url.empty()) return false;
    json j = {{"drive_model", model}, {"version", version},
              {"ar_match", ar_match}, {"err", err}};
    long code = 0;
    auto r = http_post_json(reg_base(base_url) + "/api/stat/ripped",
                            j.dump(), ua, code);
    return r.has_value() && code == 200;
}

std::string b64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (unsigned char)in[i] << 16 |
                     (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        o += T[(n >> 18) & 63]; o += T[(n >> 12) & 63];
        o += T[(n >> 6) & 63];  o += T[n & 63];
    }
    if (i < in.size()) {
        unsigned n = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) n |= (unsigned char)in[i + 1] << 8;
        o += T[(n >> 18) & 63]; o += T[(n >> 12) & 63];
        o += (i + 1 < in.size()) ? T[(n >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

bool registry_submit_condition(const std::string& base_url,
                               const std::string& disc_id,
                               const std::string& artist,
                               const std::string& title,
                               const std::string& year,
                               const std::string& mb_release_id,
                               int quality, int ar_ok, int ar_total,
                               int damaged, const std::string& kind,
                               const std::string& ua) {
    if (base_url.empty() || disc_id.empty()) return false;
    std::string thumb;                          // CAA front-250, klein
    if (!mb_release_id.empty()) {
        long c = 0;
        auto img = http_get("https://coverartarchive.org/release/" +
                            mb_release_id + "/front-250", ua, c);
        if (img && c == 200 && !img->empty() && img->size() < 400000)
            thumb = b64(*img);
    }
    json j = {{"disc_id", disc_id}, {"artist", artist}, {"title", title},
              {"year", year}, {"quality", quality}, {"ar_ok", ar_ok},
              {"ar_total", ar_total}, {"damaged", damaged}, {"kind", kind}};
    if (!thumb.empty()) j["thumb_b64"] = thumb;
    long code = 0;
    auto r = http_post_json(reg_base(base_url) + "/api/condition",
                            j.dump(), ua, code);
    return r.has_value() && code == 200;
}

long long fs_free_bytes(const std::string& path) {
    std::error_code ec;
    auto si = fs::space(fs::path(path), ec);
    if (ec) return -1;
    return (long long)si.available;
}

static std::string log_dir() {
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
        return std::string(x) + "/cdripper";
    if (const char* h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.local/share/cdripper";
    return "/tmp/cdripper-logs";
}

void log_to_file(const std::string& line) {
    try {
        std::string dir = log_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path lp = fs::path(dir) / "cdripper.log";
        auto sz = fs::file_size(lp, ec);
        if (!ec && sz > 2 * 1024 * 1024) {
            fs::rename(lp, fs::path(dir) / "cdripper.log.1", ec);
        }
        std::ofstream f(lp, std::ios::app);
        if (!f) return;
        std::time_t tt = std::time(nullptr);
        char ts[32];
        std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S",
                      std::localtime(&tt));
        f << "[" << ts << "] " << line << "\n";
    } catch (...) {}
}

// ───────────────────────────── Drive (ioctl) ──────────────────────────────────

Drive::Drive(const std::string& p) : path_(p) {
    fd_ = ::open(p.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd_ < 0)
        throw std::runtime_error("Kann Laufwerk nicht öffnen: " + p + " (" +
                                 std::strerror(errno) + ")");
}
Drive::~Drive()              { if (fd_ >= 0) ::close(fd_); }
int  Drive::raw_status() const { return ::ioctl(fd_, CDROM_DRIVE_STATUS, 0); }
bool Drive::disc_ready() const { return raw_status() == CDS_DISC_OK; }
bool Drive::has_audio() const {
    int t = ::ioctl(fd_, CDROM_DISC_STATUS, 0);
    return t == CDS_AUDIO || t == CDS_MIXED;
}
// Robustes Auswerfen: libcdio/paranoia sperrt während Rip/Scan die Klappe
// (CDROM_LOCKDOOR) — ein hängengebliebener Lock lässt CDROMEJECT mit EBUSY
// ins Leere laufen (Tray bleibt zu, scheinbar „passiert nichts"). Daher
// erst ENTsperren, dann auswerfen, mit kurzer Retry-Schleife (das Laufwerk
// braucht nach einem Lauf evtl. einen Moment, bis es das Kommando annimmt).
static bool do_eject_fd(int fd) {
    ::ioctl(fd, CDROM_LOCKDOOR, 0);          // Klappe entriegeln (Lock weg)
    for (int i = 0; i < 4; ++i) {
        if (::ioctl(fd, CDROMEJECT, 0) == 0) return true;
        ::usleep(400 * 1000);
        ::ioctl(fd, CDROM_LOCKDOOR, 0);
    }
    return false;
}
void Drive::eject() const    { do_eject_fd(fd_); }

bool eject_device(const std::string& dev) {
    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    bool ok = do_eject_fd(fd);
    ::close(fd);
    return ok;
}
bool load_tray(const std::string& dev) {
    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    int r = ::ioctl(fd, CDROMCLOSETRAY, 0);
    ::close(fd);
    return r == 0;
}

// ───────────────────────────── FLAC ───────────────────────────────────────────

static int run(const std::vector<std::string>& argv) {
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(a[0], a.data()); _exit(127); }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Gemeinsamer Tag-Satz (Vorbis-Comment-Stil KEY=VALUE) für FLAC & Opus.
// Mehrfaches GENRE = Multi-Genre. Werte sind bereits UTF-8.
static std::vector<std::pair<std::string, std::string>>
track_tags(const Album& al, const Track& tr) {
    std::vector<std::pair<std::string, std::string>> t = {
        { "TITLE",       tr.title },
        { "ARTIST",      tr.artist },
        { "ALBUM",       al.title },
        { "ALBUMARTIST", al.artist },
        { "TRACKNUMBER", std::to_string(tr.number) },
        { "TRACKTOTAL",  std::to_string((int)al.tracks.size()) },
    };
    if (!al.year().empty())    t.push_back({ "DATE", al.year() });
    if (al.compilation)        t.push_back({ "COMPILATION", "1" });
    if (al.disc_total > 1) {
        t.push_back({ "DISCNUMBER", std::to_string(al.disc_number) });
        t.push_back({ "DISCTOTAL",  std::to_string(al.disc_total) });
    }
    if (!al.mb_release_id.empty())
        t.push_back({ "MUSICBRAINZ_ALBUMID", al.mb_release_id });
    if (!tr.mb_track_id.empty())
        t.push_back({ "MUSICBRAINZ_TRACKID", tr.mb_track_id });
    for (const auto& g : al.genres) t.push_back({ "GENRE", g });
    if (!al.originaldate.empty()) {
        t.push_back({ "ORIGINALDATE", al.originaldate });
        t.push_back({ "ORIGINALYEAR", al.originaldate.substr(0, 4) });
    }
    if (!al.label.empty())     t.push_back({ "LABEL", al.label });
    if (!al.catalogno.empty()) t.push_back({ "CATALOGNUMBER", al.catalogno });
    if (!al.barcode.empty())   t.push_back({ "BARCODE", al.barcode });
    if (!al.mb_artist_id.empty())
        t.push_back({ "MUSICBRAINZ_ALBUMARTISTID", al.mb_artist_id });
    if (!tr.mb_artist_id.empty())
        t.push_back({ "MUSICBRAINZ_ARTISTID", tr.mb_artist_id });
    return t;
}

fs::path encode_flac(const fs::path& wav, const Album& al, const Track& tr,
                     int compression) {
    fs::path out = wav; out.replace_extension(".flac");
    if (compression < 0) compression = 0;
    if (compression > 8) compression = 8;
    // --no-utf8-convert: Tag-Werte sind schon UTF-8 verbatim schreiben; ohne
    // das mappt flac ohne UTF-8-Locale jedes Nicht-ASCII-Byte auf '#'.
    std::vector<std::string> a = {
        "flac", "-" + std::to_string(compression), "--silent",
        "--no-utf8-convert", "-f", "-o", out.string(),
    };
    for (const auto& kv : track_tags(al, tr)) {
        a.push_back("-T"); a.push_back(kv.first + "=" + kv.second);
    }
    if (!al.cover_jpg.empty()) {
        a.push_back("--picture");
        a.push_back("3||image/jpeg||" + al.cover_jpg.string());
    }
    a.push_back(wav.string());
    if (run(a) != 0) throw std::runtime_error("flac-Encode fehlgeschlagen");
    return out;
}

static fs::path encode_opus(const fs::path& wav, const Album& al,
                            const Track& tr, int kbps) {
    fs::path out = wav; out.replace_extension(".opus");
    if (kbps <= 0) kbps = 128;
    std::vector<std::string> a = {
        "opusenc", "--quiet", "--bitrate", std::to_string(kbps),
    };
    for (const auto& kv : track_tags(al, tr)) {
        a.push_back("--comment"); a.push_back(kv.first + "=" + kv.second);
    }
    if (!al.cover_jpg.empty()) {
        a.push_back("--picture"); a.push_back(al.cover_jpg.string());
    }
    a.push_back(wav.string());
    a.push_back(out.string());
    if (run(a) != 0) throw std::runtime_error("opus-Encode fehlgeschlagen");
    return out;
}

static fs::path encode_mp3(const fs::path& wav, const Album& al,
                           const Track& tr, int vq) {
    fs::path out = wav; out.replace_extension(".mp3");
    if (vq < 0) vq = 0;
    if (vq > 9) vq = 9;
    // LAME-VBR (-V). ID3v2 für die Standardfelder; MusicBrainz-IDs lässt MP3
    // bewusst weg (ID3-Mapping nicht-standard, lame-CLI dafür zu limitiert).
    std::vector<std::string> a = {
        "lame", "--quiet", "-V", std::to_string(vq), "--add-id3v2",
        "--tt", tr.title, "--ta", tr.artist, "--tl", al.title,
        "--tn", std::to_string(tr.number) + "/" +
                std::to_string((int)al.tracks.size()),
    };
    if (!al.year().empty())   { a.push_back("--ty"); a.push_back(al.year()); }
    if (!al.genres.empty())   { a.push_back("--tg"); a.push_back(al.genres[0]); }
    a.push_back("--tv"); a.push_back("TPE2=" + al.artist);          // AlbumArtist
    if (al.compilation) { a.push_back("--tv"); a.push_back("TCMP=1"); }
    if (al.disc_total > 1) {
        a.push_back("--tv");
        a.push_back("TPOS=" + std::to_string(al.disc_number) + "/" +
                    std::to_string(al.disc_total));
    }
    if (!al.cover_jpg.empty()) {
        a.push_back("--ti"); a.push_back(al.cover_jpg.string());
    }
    a.push_back(wav.string());
    a.push_back(out.string());
    if (run(a) != 0) throw std::runtime_error("mp3-Encode fehlgeschlagen");
    return out;
}

std::string audio_ext(const Config& c) {
    if (c.audio_format == "opus") return "opus";
    if (c.audio_format == "mp3")  return "mp3";
    return "flac";
}

fs::path encode_audio(const fs::path& wav, const Album& al, const Track& tr,
                      const Config& c) {
    if (c.audio_format == "opus") return encode_opus(wav, al, tr, c.audio_quality);
    if (c.audio_format == "mp3")  return encode_mp3(wav, al, tr, c.audio_quality);
    return encode_flac(wav, al, tr, c.audio_quality);
}

bool apply_replaygain(const fs::path& flac) {
    // rsgain custom -s i  → nur Track-Gain/R128, Tags in-place schreiben.
    return run({ "rsgain", "custom", "-s", "i", flac.string() }) == 0;
}

// ───────────────────────────── WebDAV ─────────────────────────────────────────

WebDav::WebDav(const Config& c) : cfg_(c) {
    std::string u = c.nextcloud_url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    base_ = u + "/remote.php/dav/files/" + c.webdav_user;
}

static std::string url_for(const std::string& base, CURL* c,
                           const std::vector<std::string>& segs) {
    std::string u = base;
    for (const auto& s : segs) u += "/" + esc(c, s);
    return u;
}

static long mkcol(const Config& cfg, const std::string& url) {
    CURL* c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "MKCOL");
    curl_easy_setopt(c, CURLOPT_USERPWD, (cfg.webdav_user + ":" + cfg.webdav_pass).c_str());
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return code;
}

void WebDav::ensure_dirs(const std::vector<std::string>& segs) {
    CURL* c = curl_easy_init();
    std::vector<std::string> acc;
    for (const auto& s : segs) {
        acc.push_back(s);
        long code = mkcol(cfg_, url_for(base_, c, acc));
        if (code != 201 && code != 405 && code != 301)
            throw std::runtime_error("MKCOL '" + s + "' HTTP " + std::to_string(code));
    }
    curl_easy_cleanup(c);
}

void WebDav::put(const fs::path& local, const std::vector<std::string>& segs) {
    CURL* c = curl_easy_init();
    std::string url = url_for(base_, c, segs);
    FILE* fp = std::fopen(local.c_str(), "rb");
    if (!fp) { curl_easy_cleanup(c);
               throw std::runtime_error("PUT: lokal nicht lesbar: " + local.string()); }
    std::error_code ec;
    auto sz = (curl_off_t)fs::file_size(local, ec);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(c, CURLOPT_READDATA, fp);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, sz);
    curl_easy_setopt(c, CURLOPT_USERPWD, (cfg_.webdav_user + ":" + cfg_.webdav_pass).c_str());
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    std::fclose(fp);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code < 200 || code >= 300)
        throw std::runtime_error("PUT HTTP " + std::to_string(code) + " (" +
                                 curl_easy_strerror(rc) + ")");
}

bool WebDav::exists(const std::vector<std::string>& segs) {
    CURL* c = curl_easy_init();
    std::string url = url_for(base_, c, segs);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);            // HEAD
    curl_easy_setopt(c, CURLOPT_USERPWD,
                     (cfg_.webdav_user + ":" + cfg_.webdav_pass).c_str());
    curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && code >= 200 && code < 400;  // 404 = nicht da
}

// ── Local / SSH / SMB Uploader ─────────────────────────────────────────────────

static std::string shq(const std::string& s) {     // POSIX single-quote
    std::string o = "'";
    for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
    o += "'";
    return o;
}

void LocalUploader::ensure_dirs(const std::vector<std::string>& segs) {
    fs::path p = base_;
    for (const auto& s : segs) p /= s;
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) throw std::runtime_error("local mkdir: " + p.string() +
                                     " (" + ec.message() + ")");
}
void LocalUploader::put(const fs::path& local,
                        const std::vector<std::string>& segs) {
    fs::path p = base_;
    for (const auto& s : segs) p /= s;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::copy_file(local, p, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("local copy → " + p.string() +
                                     " (" + ec.message() + ")");
}
bool LocalUploader::exists(const std::vector<std::string>& segs) {
    fs::path p = base_;
    for (const auto& s : segs) p /= s;
    std::error_code ec;
    return fs::exists(p, ec);
}

SshUploader::SshUploader(const Config& c)
    : host_(c.ssh_host), base_(c.ssh_base), port_(c.ssh_port) {
    if (host_.empty()) throw std::runtime_error("ssh_host nicht gesetzt");
    sshtarget_ = c.ssh_user.empty() ? host_ : (c.ssh_user + "@" + host_);
}
static std::string join_segs(const std::string& base,
                             const std::vector<std::string>& segs, size_t upto) {
    std::string p = base;
    for (size_t i = 0; i < upto; ++i) { if (!p.empty()) p += "/"; p += segs[i]; }
    return p;
}
void SshUploader::ensure_dirs(const std::vector<std::string>& segs) {
    std::string remote = join_segs(base_, segs, segs.size());
    if (run({ "ssh", "-p", std::to_string(port_), "-o", "BatchMode=yes",
              sshtarget_, "mkdir -p " + shq(remote) }) != 0)
        throw std::runtime_error("ssh mkdir -p fehlgeschlagen: " + remote);
}
void SshUploader::put(const fs::path& local,
                      const std::vector<std::string>& segs) {
    std::string remote = join_segs(base_, segs, segs.size());
    if (run({ "scp", "-P", std::to_string(port_), "-o", "BatchMode=yes",
              "-q", local.string(),
              sshtarget_ + ":" + shq(remote) }) != 0)
        throw std::runtime_error("scp fehlgeschlagen → " + remote);
}
bool SshUploader::exists(const std::vector<std::string>& segs) {
    std::string remote = join_segs(base_, segs, segs.size());
    return run({ "ssh", "-p", std::to_string(port_), "-o", "BatchMode=yes",
                 sshtarget_, "test -e " + shq(remote) }) == 0;
}

SmbUploader::SmbUploader(const Config& c) {
    std::string u = c.smb_url;
    const std::string pfx = "smb://";
    if (u.rfind(pfx, 0) == 0) u = u.substr(pfx.size());
    std::vector<std::string> parts;
    std::string cur;
    for (char ch : u) { if (ch == '/') { if (!cur.empty()) parts.push_back(cur);
                                          cur.clear(); }
                        else cur += ch; }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.size() < 2)
        throw std::runtime_error("smb_url muss smb://host/share/… sein");
    service_ = "//" + parts[0] + "/" + parts[1];
    for (size_t i = 2; i < parts.size(); ++i) base_segs_.push_back(parts[i]);
    if (!c.smb_user.empty())
        auth_ = c.smb_user + "%" + c.smb_pass;
}
static int smb_run(const std::string& service, const std::string& auth,
                   const std::string& cmds) {
    std::vector<std::string> a = { "smbclient", service };
    if (auth.empty()) a.push_back("-N");
    else { a.push_back("-U"); a.push_back(auth); }
    a.push_back("-c"); a.push_back(cmds);
    return run(a);
}
void SmbUploader::ensure_dirs(const std::vector<std::string>& segs) {
    std::vector<std::string> all = base_segs_;
    for (const auto& s : segs) all.push_back(s);
    std::string cmds;
    for (const auto& s : all)
        cmds += "mkdir \"" + s + "\"; cd \"" + s + "\"; ";
    // mkdir-on-exists liefert nonzero → best effort, nicht werfen.
    smb_run(service_, auth_, cmds);
}
void SmbUploader::put(const fs::path& local,
                      const std::vector<std::string>& segs) {
    std::vector<std::string> all = base_segs_;
    for (const auto& s : segs) all.push_back(s);     // letztes = Dateiname
    if (all.empty()) throw std::runtime_error("smb put: leerer Pfad");
    std::string fname = all.back();
    std::string cmds;
    for (size_t i = 0; i + 1 < all.size(); ++i)
        cmds += "cd \"" + all[i] + "\"; ";
    cmds += "put \"" + local.string() + "\" \"" + fname + "\"";
    if (smb_run(service_, auth_, cmds) != 0)
        throw std::runtime_error("smbclient put fehlgeschlagen: " + fname);
}
bool SmbUploader::exists(const std::vector<std::string>& segs) {
    std::vector<std::string> all = base_segs_;
    for (const auto& s : segs) all.push_back(s);
    if (all.empty()) return false;
    std::string fname = all.back();
    std::string cmds;
    for (size_t i = 0; i + 1 < all.size(); ++i)
        cmds += "cd \"" + all[i] + "\"; ";
    cmds += "ls \"" + fname + "\"";
    return smb_run(service_, auth_, cmds) == 0;   // ls vorhandener Datei = 0
}

std::unique_ptr<Uploader> make_uploader(const Config& c) {
    const std::string& b = c.upload_backend;
    if (b == "webdav" || b.empty())
        return std::make_unique<WebDav>(c);
    if (b == "local") {
        if (c.local_base.empty())
            throw std::runtime_error("local_base nicht gesetzt");
        return std::make_unique<LocalUploader>(c.local_base);
    }
    if (b == "ssh")  return std::make_unique<SshUploader>(c);
    if (b == "smb")  return std::make_unique<SmbUploader>(c);
    throw std::runtime_error("unbekanntes upload_backend: " + b);
}

} // namespace cdr
