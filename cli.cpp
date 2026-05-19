// cli.cpp — Headless-Modus: gleiche Pipeline, Status auf stderr.
#include "pipeline.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> g_stop{false};
double g_mr = 0, g_me = 0, g_mu = 0;   // letzte Durchsatz-Metriken (MB/s)
void on_sig(int) { g_stop = true; }

std::string mmss(double s) {
    if (s < 0) return "—";
    int t = (int)(s + 0.5);
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}
}

// Callbacks für ein Laufwerk. tag leer = Single-Drive (exakt das alte
// Verhalten, inkl. \r-Statuszeilen). tag gesetzt = Multi-Drive: jede
// Meldung mit „[tag] " geprefixt und \n-terminiert (kein \r-Cursor-
// Trick, weil mehrere Laufwerke dasselbe Terminal teilen).
static cdr::Callbacks make_cli_cb(const std::string& tag) {
    const bool m = !tag.empty();
    const std::string p = m ? "[" + tag + "] " : std::string();
    cdr::Callbacks cb;
    cb.onWaiting = [p](const std::string& s) {
        std::fprintf(stderr, "\n%s⏏  %s\n", p.c_str(), s.c_str());
    };
    cb.onDiscIdent = [p](const cdr::DiscIdent& d) {
        std::fprintf(stderr, "%s[disc] %s (%d Tracks laut TOC)\n",
                     p.c_str(), d.id.c_str(), d.toc_tracks);
    };
    cb.onAlbum = [p](const cdr::Album& a) {
        std::fprintf(stderr, "%s[album] %s — %s (%s), %zu Tracks\n",
                     p.c_str(), a.artist.c_str(), a.title.c_str(),
                     a.year().c_str(), a.tracks.size());
    };
    cb.onCover = [p](const fs::path& fp) {
        std::fprintf(stderr, "%s[cover] %s\n", p.c_str(),
                     fp.string().c_str());
    };
    cb.onTrack = [p, m](int i, cdr::TrackState s, double f,
                        const std::string& msg) {
        const char* nl = m ? "\n" : "\r";
        if (!m && (s == cdr::TrackState::Ripping ||
                   s == cdr::TrackState::Uploading))
            std::fprintf(stderr, "\r[t%02d] %-9s %3d%%   ",
                         i, cdr::state_label(s), (int)(f * 100));
        else if (s == cdr::TrackState::Ripping ||
                 s == cdr::TrackState::Uploading)
            std::fprintf(stderr, "%s[t%02d] %-9s %3d%%\n",
                         p.c_str(), i, cdr::state_label(s),
                         (int)(f * 100));
        else
            std::fprintf(stderr, "%s%s[t%02d] %-9s %s%s\n",
                         m ? "" : "\r", p.c_str(), i,
                         cdr::state_label(s),
                         (s == cdr::TrackState::Done ? "✓" : ""),
                         msg.empty() ? "" : (" — " + msg).c_str());
        (void)nl; std::fflush(stderr);
    };
    cb.onMetrics = [](double r, double en, double up) {
        g_mr = r; g_me = en; g_mu = up;
    };
    cb.onProgress = [p, m](double e, double eta, int r, int u, int t) {
        if (m)   // Multi: keine MB/s (g_m* sind global/geteilt) + \n
            std::fprintf(stderr,
                "%s[zeit %s · rest ~%s · rip %d/%d · up %d/%d]\n",
                p.c_str(), mmss(e).c_str(), mmss(eta).c_str(),
                r, t, u, t);
        else
            std::fprintf(stderr,
                "\r[zeit %s · rest ~%s · rip %d/%d · up %d/%d · "
                "R %.1f E %.1f U %.1f MB/s]   ",
                mmss(e).c_str(), mmss(eta).c_str(), r, t, u, t,
                g_mr, g_me, g_mu);
        std::fflush(stderr);
    };
    cb.onLog = [p](const std::string& l) {
        std::fprintf(stderr, "\n%s[log] %s\n", p.c_str(), l.c_str());
        cdr::log_to_file(p + l);
    };
    cb.onDiscDone = [p](bool ok, const std::string& msg) {
        std::fprintf(stderr, "\n%s%s %s\n", p.c_str(),
                     ok ? "[OK]" : "[FEHLER]", msg.c_str());
        cdr::log_to_file(p + (ok ? "[OK] " : "[FEHLER] ") + msg);
    };
    cb.onFatal = [p](const std::string& msg) {
        std::fprintf(stderr, "\n%s[FATAL] %s\n", p.c_str(), msg.c_str());
    };
    return cb;
}

int run_cli(const cdr::Config& cfg, bool once) {
    std::signal(SIGINT,  on_sig);
    std::signal(SIGTERM, on_sig);

    std::vector<std::string> devs = cfg.device_list();
    if (devs.size() <= 1) {                       // Single-Drive (unverändert)
        cdr::Config c = cfg;
        if (!devs.empty()) c.device = devs[0];
        cdr::Pipeline pl(c, make_cli_cb(""));
        pl.run(g_stop, once);
    } else {                                      // T7: parallel
        std::fprintf(stderr,
            "[multi] %zu Laufwerke parallel — je eine Disc pro Laufwerk "
            "einlegen (NICHT dieselbe Disc in zwei Laufwerke).\n",
            devs.size());
        std::vector<std::thread> th;
        for (const auto& d : devs) {
            th.emplace_back([&cfg, d, once] {
                cdr::Config c = cfg;
                c.device = d;
                c.devices.clear();          // Kind-Pipeline = Single
                std::string tag = d;
                auto sl = tag.find_last_of('/');
                if (sl != std::string::npos) tag = tag.substr(sl + 1);
                cdr::Pipeline pl(c, make_cli_cb(tag));
                pl.run(g_stop, once);
            });
        }
        for (auto& t : th) t.join();
    }
    std::fprintf(stderr, "\ncdripper beendet.\n");
    return 0;
}

// ── Kalibrierung: Drive-Offset gegen AccurateRip ermitteln & speichern ─────────
namespace {
uint32_t v1_buf(const std::vector<uint32_t>& w, bool first, bool last, int off) {
    const size_t trim = 5 * 588, nn = w.size();
    size_t lo = first ? trim : 0;
    size_t hi = (last && nn > trim) ? nn - trim : nn;
    uint64_t a = 0;
    for (size_t k = lo; k < hi; ++k) {
        long s = (long)k + off;
        uint32_t v = (s >= 0 && (size_t)s < nn) ? w[s] : 0u;
        a += (uint32_t)(v * (uint32_t)(k + 1));
    }
    return (uint32_t)a;
}
std::vector<uint32_t> load_wav(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint32_t> w;
    if (!f) return w;
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz <= 44) return w;
    f.seekg(44, std::ios::beg);
    w.resize((size_t)((sz - 44) / 4));
    f.read(reinterpret_cast<char*>(w.data()), (std::streamsize)w.size() * 4);
    return w;
}
bool in(const std::vector<uint32_t>& v, uint32_t x) {
    for (uint32_t e : v) if (e == x) return true;
    return false;
}
}

int run_calibrate(const cdr::Config& cfg) {
    std::signal(SIGINT, on_sig);

    // WICHTIG: erst auf „Disc bereit" warten (wie der Pipeline-Pfad), bevor
    // irgendein libcdio-Open passiert — sonst hängt cdio_cddap_open hart (D),
    // solange das Laufwerk noch die TOC liest.
    std::fprintf(stderr, "Warte auf eingelegte Audio-CD …\n");
    try {
        cdr::Drive drv(cfg.device);
        bool ready = false;
        for (int s = 0; s < 90 && !g_stop; ++s) {
            if (drv.disc_ready() && drv.has_audio()) { ready = true; break; }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!ready) {
            std::fprintf(stderr,
                "Keine lesbare Audio-CD im Laufwerk (Timeout). Abbruch.\n");
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Laufwerk-Fehler: %s\n", e.what());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));   // kurz settlen

    std::string did = cdr::drive_id(cfg.device);
    std::fprintf(stderr, "Laufwerk erkannt: \"%s\"\n", did.c_str());
    if (auto o = cdr::lookup_drive_offset(did))
        std::fprintf(stderr, "(bereits kalibriert: Offset %d — wird neu ermittelt)\n", *o);

    fs::path dir = fs::path(cfg.tmpdir) / "calib";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    std::fprintf(stderr, "Rippe Disc einmal (Schnell-Modus, Worker+Watchdog) …\n");
    std::vector<cdr::AudioTrack> tracks;
    std::vector<int> offs; int leadout = 0; int n = 0;
    bool fatal = false; std::string fatalmsg;
    auto onEv = [&](const std::string& ln) {
        if (ln.rfind("TOC ", 0) == 0) n = std::atoi(ln.c_str() + 4);
        else if (ln.rfind("LEN ", 0) == 0) {
            int i, ct, ns;
            if (std::sscanf(ln.c_str(), "LEN %d %d %d", &i, &ct, &ns) == 3)
                tracks.push_back(cdr::AudioTrack{ i, ct, ns });
        } else if (ln.rfind("AR", 0) == 0) {
            std::vector<int> v; const char* p = ln.c_str() + 2;
            while (*p) { while (*p == ' ') ++p; if (!*p) break;
                         v.push_back(std::atoi(p));
                         while (*p && *p != ' ') ++p; }
            if (v.size() >= 2) { leadout = v.back(); v.pop_back(); offs = v; }
        } else if (ln.rfind("RIPSTART ", 0) == 0) {
            std::fprintf(stderr, "\r  Track %s/%d …            ",
                         ln.c_str() + 9, n); std::fflush(stderr);
        } else if (ln.rfind("RIPERR ", 0) == 0) {
            std::fprintf(stderr, "\n%s\n", ln.c_str());
        } else if (ln.rfind("FATAL ", 0) == 0) {
            fatal = true; fatalmsg = ln.substr(6);
        }
    };
    cdr::RipResult rr = cdr::rip_session(cfg.device, dir.string(),
        cfg.read_speed, true, 90, onEv, [] { return g_stop.load(); });
    std::fprintf(stderr, "\n");
    if (rr == cdr::RipResult::Stalled) {
        std::fprintf(stderr, "Laufwerk hängt (kein Fortschritt > 90 s) — "
            "bitte Laufwerk zurücksetzen/neu anstecken. Abgebrochen.\n");
        fs::remove_all(dir, ec); return 4;
    }
    if (rr == cdr::RipResult::Aborted) { fs::remove_all(dir, ec); return 1; }
    if (rr == cdr::RipResult::Fatal || fatal) {
        std::fprintf(stderr, "Rip fehlgeschlagen: %s\n",
            fatalmsg.empty() ? "unbekannt" : fatalmsg.c_str());
        fs::remove_all(dir, ec); return 1;
    }
    if (n == 0) n = (int)tracks.size();
    if (tracks.empty() || offs.empty()) {
        std::fprintf(stderr, "Keine TOC vom Worker erhalten.\n");
        fs::remove_all(dir, ec); return 1;
    }
    cdr::ArIds ids = cdr::ar_ids_from_toc(offs, leadout);
    auto db = cdr::ar_db_crcs(ids, cfg.mb_useragent);
    bool any = false;
    for (auto& v : db) if (!v.empty()) any = true;
    if (!any) {
        std::fprintf(stderr,
            "Diese Disc ist NICHT in AccurateRip — bitte eine gängige "
            "Mainstream-CD zum Kalibrieren nehmen.\n");
        fs::remove_all(dir, ec);
        return 2;
    }

    // Repräsentanten-Track (längster, nicht erster/letzter, mit DB-Einträgen)
    int rep = -1, repsec = -1;
    for (const auto& at : tracks) {
        if (at.index == 1 || at.index == n) continue;
        if (at.index - 1 < (int)db.size() && !db[at.index - 1].empty() &&
            at.n_sectors > repsec) { rep = at.index; repsec = at.n_sectors; }
    }
    if (rep < 0) rep = (n >= 1 ? 1 : -1);
    if (rep < 0) { fs::remove_all(dir, ec); return 2; }

    std::fprintf(stderr, "Sweep Offset über Track %d …\n", rep);
    auto buf = load_wav(dir / (std::to_string(rep) + ".wav"));
    std::vector<int> cands;
    for (int off = -1500; off <= 1500; ++off)
        if (in(db[rep - 1], v1_buf(buf, rep == 1, rep == n, off)))
            cands.push_back(off);
    if (cands.empty()) {
        std::fprintf(stderr,
            "Kein passender Offset in [-1500,+1500] gefunden. Rip evtl. "
            "fehlerhaft oder Disc-Pressung unbekannt.\n");
        fs::remove_all(dir, ec);
        return 3;
    }

    int best = cands[0], bestm = -1;
    for (int off : cands) {
        int m = 0;
        for (const auto& at : tracks) {
            if (at.index - 1 >= (int)db.size() || db[at.index - 1].empty()) continue;
            uint32_t c1 = 0, c2 = 0;
            cdr::ar_crc_file(dir / (std::to_string(at.index) + ".wav"),
                             at.index == 1, at.index == n, off, c1, c2);
            if (in(db[at.index - 1], c1) || in(db[at.index - 1], c2)) ++m;
        }
        if (m > bestm) { bestm = m; best = off; }
    }
    fs::remove_all(dir, ec);

    std::fprintf(stderr,
        "Bester Offset: %d  (%d/%d Tracks gegen AccurateRip bestätigt)\n",
        best, bestm, n);
    if (bestm <= 0) { std::fprintf(stderr, "Nicht gespeichert.\n"); return 3; }
    if (cdr::save_drive_offset(did, best))
        std::fprintf(stderr, "Gespeichert: \"%s\" → %d in %s\n",
                     did.c_str(), best, cdr::drive_offsets_path().c_str());
    else
        std::fprintf(stderr, "FEHLER beim Speichern.\n");
    // T5: kalibrierter Offset ist AccurateRip-bewiesen → idealer Punkt zum
    // Teilen (Opt-in, nur bei gesetzter URL).
    if (cfg.registry_submit && !cfg.registry_url.empty()) {
        if (cdr::registry_submit_offset(cfg.registry_url, did, best,
                                        cfg.mb_useragent))
            std::fprintf(stderr, "An Offset-Registry gemeldet.\n");
    }
    return 0;
}

// ── Rip-Worker-Subprozess (T8) ─────────────────────────────────────────────────
// Läuft als eigener Prozess `cdripper --rip-worker DEV WORKDIR SPEED FAST`.
// stdout = Protokoll (zeilenweise, geflusht); WAVs nach WORKDIR/<idx>.wav.
// Hängt dieser Prozess im D-State, killt der Elternprozess ihn — die App
// selbst bleibt responsiv (genau das ist der Zweck).
int run_rip_worker(const std::string& device, const std::string& workdir,
                   int speed, bool fast, const std::string& plan_csv) {
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    auto line = [](const std::string& s) {
        std::fputs(s.c_str(), stdout); std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    // Scan-adaptiver Plan: "idx:code" je Track, code f=schnell c=careful
    // r=recovery(+zuletzt). Fehlt ein Track → 'd' = Basis (speed/fast argv).
    std::map<int, char> prof;
    if (plan_csv != "-" && !plan_csv.empty()) {
        std::string tok;
        for (char ch : plan_csv + ",") {
            if (ch == ',') {
                auto c = tok.find(':');
                if (c != std::string::npos)
                    prof[std::atoi(tok.substr(0, c).c_str())] =
                        tok.substr(c + 1).empty() ? 'd' : tok[c + 1];
                tok.clear();
            } else tok += ch;
        }
    }
    auto code_of = [&](int idx) {
        auto it = prof.find(idx); return it == prof.end() ? 'd' : it->second;
    };
    std::unique_ptr<cdr::Ripper> rip;
    try {
        rip = std::make_unique<cdr::Ripper>(device, speed, fast);
    } catch (const std::exception& e) {
        line(std::string("FATAL ") + e.what());
        return 1;
    }
    const auto& tr = rip->tracks();
    line("TOC " + std::to_string(tr.size()));
    for (const auto& at : tr)
        line("LEN " + std::to_string(at.index) + " " +
             std::to_string(at.cd_track) + " " +
             std::to_string(at.n_sectors));
    {
        std::vector<int> offs; int leadout = 0;
        rip->ar_toc(offs, leadout);
        std::string ar = "AR";
        for (int o : offs) ar += " " + std::to_string(o);
        ar += " " + std::to_string(leadout);
        line(ar);
    }
    // Rückgabe: 0 ok, 2 = sauber abgebrochen. Vor jedem Track Speed/Modus
    // gemäß Scan-Plan: f=schnell(max,fast) c=careful(max,full) r=recovery
    // (4×,full) d=Basis (unverändert wie konstruiert).
    auto rip_one = [&](const cdr::AudioTrack& at) -> int {
        if (g_stop) return 2;
        switch (code_of(at.index)) {
            case 'f': rip->set_speed(0); rip->set_fast(true);  break;
            case 'c': rip->set_speed(0); rip->set_fast(false); break;
            case 'r': rip->set_speed(4); rip->set_fast(false); break;
            default: break;                       // 'd' → Basis beibehalten
        }
        line("RIPSTART " + std::to_string(at.index));
        try {
            long bad = rip->rip(at,
                fs::path(workdir) / (std::to_string(at.index) + ".wav"),
                [&](double f) {
                    line("RIP " + std::to_string(at.index) + " " +
                         std::to_string((int)(f * 1000)));
                },
                [] { return g_stop.load(); },
                [&](int lba, int sev) {
                    line("DEFECT " + std::to_string(lba) + " " +
                         std::to_string(sev));
                });
            line("RIPDONE " + std::to_string(at.index) + " " +
                 std::to_string(bad));
        } catch (const std::exception& e) {
            line("RIPERR " + std::to_string(at.index) + " " + e.what());
            if (std::string(e.what()) == "abgebrochen") return 2;
        }
        return 0;
    };
    // Reihenfolge = SCHNELLSTE zuerst (Dennis-Vorgabe): erst 'f' (sauber,
    // Schnell-Rip), dann 'd' (Basis), dann 'c' (volle paranoia, langsamer),
    // GANZ zuletzt 'r' (bekannte Hänger, 4×). 's' (schon am Ziel) komplett
    // aus. So sind die guten Tracks früh sicher im Kasten, bevor ein zäher
    // Track Zeit/Drive frisst.
    for (char want : { 'f', 'd', 'c', 'r' }) {
        for (const auto& at : tr) {
            char c = code_of(at.index);
            if (c == 's') continue;
            if (c == want && rip_one(at) == 2) return 2;
        }
    }
    line("ALLDONE");
    return 0;
}

// ── Probe-Worker-Subprozess (Preflight) ────────────────────────────────────────
// `cdripper --probe-worker DEV`. Schneller Roh-Quality-Scan; stdout-Protokoll
// `PROBE <track> <lba> <ok 0|1> <ms>`. Watchdog-geschützt im Eltern-Prozess.
int run_probe_worker(const std::string& device, int start_track,
                      int density) {
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    auto line = [](const std::string& s) {
        std::fputs(s.c_str(), stdout); std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    std::unique_ptr<cdr::Ripper> rip;
    try {
        rip = std::make_unique<cdr::Ripper>(device, 0, false);
    } catch (const std::exception& e) {
        line(std::string("FATAL ") + e.what());
        return 1;
    }
    line("PTOC " + std::to_string(rip->tracks().size()));
    {   // echtes Disc-Ende (Leadout) → korrekte Ring-Geometrie + Hang-Zone
        std::vector<int> offs; int leadout = 0;
        rip->ar_toc(offs, leadout);
        line("PEND " + std::to_string(leadout));
    }
    rip->probe(density < 1 ? 6 : density, 4,
        [&](int trk, int lba, bool ok, long ms) {
            line("PROBE " + std::to_string(trk) + " " +
                 std::to_string(lba) + " " + std::to_string(ok ? 1 : 0) +
                 " " + std::to_string(ms));
        },
        [] { return g_stop.load(); },
        start_track < 1 ? 1 : start_track,
        [&](int lba) { line("PCUR " + std::to_string(lba)); });
    line("PDONE");
    return 0;
}
