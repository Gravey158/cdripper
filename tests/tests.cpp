// tests/tests.cpp — Unit-Tests für die deterministische, schwer-per-Ohr-
// prüfbare Logik (AccurateRip-IDs, latin1→utf8, sanitize, compilation-
// Heuristik). Kein Test-Framework (Projekt-Ethos: minimal) — winziges
// CHECK-Makro, Exit != 0 bei Fehler.
#include "../engine.h"

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>   // getpid (isolierter tmp-Config-Dir im Archiv-Test)

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, msg) do {                                            \
    if (cond) { ++g_ok; }                                                \
    else { ++g_fail; std::printf("FAIL %s:%d  %s\n",                      \
           __FILE__, __LINE__, msg); } } while (0)

template <class A, class B>
static void check_eq(const A& got, const B& exp, const char* what,
                     const char* file, int ln) {
    if (got == exp) { ++g_ok; }
    else { ++g_fail; std::printf("FAIL %s:%d  %s\n", file, ln, what); }
}
#define CHECK_EQ(got, exp, what) check_eq((got), (exp), what, __FILE__, __LINE__)

int main() {
    using namespace cdr;

    // ── AccurateRip Disc-IDs (Golden: Bravo Hits Best of 95, rohe LBA;
    //    live gegen accuraterip.com verifiziert → HTTP 200). Fängt den
    //    +150-Bug sofort.
    {
        std::vector<int> off = { 0,18373,38245,62720,84305,100073,120840,
            138653,152710,168830,185400,201385,219010,236485,248618,
            265573,283243,304473,322255,338790 };
        int leadout = 352858;
        ArIds id = ar_ids_from_toc(off, leadout);
        CHECK_EQ(id.ntracks, 20, "AR ntracks");
        CHECK_EQ((unsigned)id.id1, 3842839u, "AR id1 (raw LBA, kein +150)");
        CHECK_EQ((unsigned)id.id2, 55638805u, "AR id2");
        CHECK_EQ((unsigned)id.cddb, 538075156u, "AR cddb");
        std::string u = id.url();
        CHECK(u.find("accuraterip.com/accuraterip/") != std::string::npos,
              "AR url host/path");
        CHECK(u.find("dBAR-020-") != std::string::npos, "AR url dBAR-020");
        CHECK(u.size() > 60 && u.substr(u.size() - 4) == ".bin",
              "AR url endet .bin");
    }

    // ── latin1_to_utf8 / is_valid_utf8 (CD-TEXT-Pfad)
    {
        CHECK(!is_valid_utf8(std::string("\xE4")),
              "lone 0xE4 ist kein UTF-8");
        CHECK(is_valid_utf8(std::string("\xC3\xA4")),
              "C3 A4 ist gültiges UTF-8 (ä)");
        CHECK(is_valid_utf8("Stayin\xE2\x80\x99 Alive"),
              "MB-Apostroph ist gültiges UTF-8");
        CHECK_EQ(latin1_to_utf8(std::string("\xE4")),
                 std::string("\xC3\xA4"), "Latin-1 ä → UTF-8");
        CHECK_EQ(latin1_to_utf8("abc"), std::string("abc"),
                 "ASCII unverändert");
        // bereits gültiges UTF-8 darf NICHT doppelt kodiert werden
        CHECK_EQ(latin1_to_utf8("Stayin\xE2\x80\x99"),
                 std::string("Stayin\xE2\x80\x99"),
                 "gültiges UTF-8 bleibt (kein Doppel-Encode)");
    }

    // ── sanitize (Dateinamen/Ordner)
    {
        CHECK_EQ(sanitize("AC/DC"), std::string("AC-DC"), "slash → -");
        CHECK_EQ(sanitize("a:b*c?\"d"), std::string("a_b_c__d"),
                 "verbotene Zeichen → _");
        CHECK_EQ(sanitize("name."), std::string("name"),
                 "Trailing-Dot entfernt");
        CHECK_EQ(sanitize("   "), std::string("Unknown"),
                 "leer → Unknown");
        CHECK(sanitize(std::string(300, 'x')).size() <= 120,
              "Länge gekappt ≤120");
    }

    // ── looks_compilation
    {
        Album a; a.artist = "Various Artists";
        CHECK(looks_compilation(a), "Various Artists → Compilation");
        Album b; b.artist = "Foo";
        b.tracks = { {1,"t","Foo",""}, {2,"u","Foo",""} };
        CHECK(!looks_compilation(b), "ein Künstler → keine Compilation");
        Album c; c.artist = "Foo";
        c.tracks = { {1,"t","Alice",""}, {2,"u","Bob",""} };
        CHECK(looks_compilation(c), "≥2 Track-Artists → Compilation");
    }

    // ── Album::year / folder_segments
    {
        Album a; a.date = "1994-08-12";
        CHECK_EQ(a.year(), std::string("1994"), "year aus YYYY-MM-DD");
        Album e; CHECK_EQ(e.year(), std::string(""), "year leer");
        Album f; f.artist = "AC/DC"; f.title = "X:Y"; f.date = "1980";
        auto seg = f.folder_segments();
        CHECK_EQ(seg.size(), (size_t)2, "folder: 2 Segmente");
        CHECK_EQ(seg[0], std::string("AC-DC"), "folder artist sanitisiert");
        CHECK_EQ(seg[1], std::string("X_Y (1980)"),
                 "folder album+jahr sanitisiert");
    }

    // ── probe_classify (aus disc_probe extrahiert; Laufwerk-frei testbar).
    //    Relativ-Median-Klassifikation: Median = Basislinie, nur Ausreißer
    //    >max(3×med, med+300ms) + echte Lesefehler (ok==0) zählen.
    {
        using R = RipResult;
        // Saubere Disc: 4 Tracks, gleichmäßig schnell → Clean, alles ok.
        {
            std::vector<ProbeRaw> raw = {
                {1,100,1,200},{1,200,1,210},{2,300,1,205},{2,400,1,195},
                {3,500,1,200},{3,600,1,208},{4,700,1,202},{4,800,1,198} };
            ProbeResult r = probe_classify(raw, 4, 1000, {}, 0, R::Ok);
            CHECK(r.quality == DiscQuality::Clean, "clean → Clean");
            CHECK_EQ(r.problems, 0, "clean → 0 problems");
            CHECK(r.completed, "clean → completed");
            CHECK_EQ((int)r.track_status.size(), 5, "track_status 0..4");
            CHECK(r.track_status[1] == 0 && r.track_status[4] == 0,
                  "clean → alle Tracks rippbar (0)");
        }
        // Regression „alles zerkratzt": langsames Laufwerk, ALLE Proben
        // hoch aber gleichmäßig → KEINE Ausreißer → NICHT Bad/Marginal.
        {
            std::vector<ProbeRaw> raw;
            for (int t = 1; t <= 4; ++t)
                for (int k = 0; k < 3; ++k)
                    raw.push_back({ t, t*1000+k, 1, 2500 + (k*7) });
            ProbeResult r = probe_classify(raw, 4, 5000, {}, 0, R::Ok);
            CHECK(r.quality == DiscQuality::Clean,
                  "uniform-langsam → Clean (kein 'alles zerkratzt')");
            CHECK_EQ(r.problems, 0, "uniform-langsam → 0 problems");
        }
        // Regression „Spin-up": Laufwerk-Anlauf am Disc-Anfang = glatt
        // fallende Lesezeiten (2600→500 ms) dann Plateau. Lokaler Nachbar-
        // Median darf das NICHT als „langsam" werten (sonst saubere CD
        // fälschlich grenzwertig). Erwartung: 0 problems, Clean.
        {
            std::vector<ProbeRaw> raw;
            long ramp[] = { 2600,2450,2300,2150,2000,1700,1400,1100,
                            900,750,650,600,560,540,520,510,505,500 };
            int lba = 1000;
            for (long ms : ramp)
                raw.push_back({ 1, lba += 1000, 1, ms });
            for (int k = 0; k < 12; ++k)               // Plateau ~500 ms
                raw.push_back({ 2, lba += 1000, 1, 500 + (k % 3) });
            ProbeResult r = probe_classify(raw, 2, 40000, {}, 0, R::Ok);
            CHECK_EQ(r.problems, 0,
                     "Spin-up-Rampe → 0 problems (kein Flag)");
            CHECK(r.quality == DiscQuality::Clean,
                  "Spin-up-Rampe → Clean (nicht grenzwertig)");
        }
        // Lokaler Spike in sonst langsamer Zone (Kratzer auf zäher Disc):
        // einzelne Probe weit über ihren Nachbarn → weiterhin geflaggt.
        {
            std::vector<ProbeRaw> raw;
            for (int k = 0; k < 20; ++k)
                raw.push_back({ k < 10 ? 1 : 2, 1000 + k*1000, 1, 2000 });
            raw[10] = { 2, 11000, 1, 9000 };           // Spike mitten drin
            ProbeResult r = probe_classify(raw, 2, 25000, {}, 0, R::Ok);
            CHECK_EQ(r.problems, 1, "lokaler Spike → 1 problem");
            CHECK_EQ(r.track_status[2], 1,
                     "Spike-Track langsam trotz globaler Zähigkeit");
            CHECK_EQ(r.track_status[1], 0, "ruhiger Track unauffällig");
        }
        // Ein deutlicher Zeit-Ausreißer (>3×Median) → genau dieser Track 1,
        // Disc Marginal (score>0, kein Fehler, <10%).
        {
            std::vector<ProbeRaw> raw;
            for (int t = 1; t <= 4; ++t)
                for (int k = 0; k < 4; ++k)
                    raw.push_back({ t, t*1000+k, 1, 200 });
            raw.push_back({ 1, 1500, 1, 9000 });   // Ausreißer in Track 1
            ProbeResult r = probe_classify(raw, 4, 5000, {}, 0, R::Ok);
            CHECK(r.quality == DiscQuality::Marginal,
                  "ein Ausreißer → Marginal");
            CHECK_EQ(r.problems, 1, "ein Ausreißer → 1 problem");
            CHECK_EQ(r.track_status[1], 1, "Track 1 langsam (1)");
            CHECK_EQ(r.track_status[2], 0, "Track 2 unauffällig (0)");
        }
        // Echter Lesefehler (ok==0) → Bad, betroffener Track defekt (2).
        {
            std::vector<ProbeRaw> raw = {
                {1,100,1,200},{1,200,1,210},{2,300,0,50},{2,400,1,205} };
            ProbeResult r = probe_classify(raw, 2, 600, {}, 0, R::Ok);
            CHECK(r.quality == DiscQuality::Bad, "Lesefehler → Bad");
            CHECK_EQ(r.track_status[2], 2, "Track 2 mit Fehler → defekt (2)");
            CHECK_EQ(r.track_status[1], 0, "Track 1 weiterhin rippbar (0)");
        }
        // Hänger-Tracks (Skip-ahead): Track 3 hing, Scan gab auf (Stalled).
        // → Track 3 defekt, KEIN Phantom-Track jenseits ntracks, Track 4
        //   ungescannt (3), Detail nennt Hänger + gestoppt.
        {
            std::vector<ProbeRaw> raw = {
                {1,100,1,200},{1,200,1,205},{2,300,1,210},{2,400,1,198} };
            ProbeResult r = probe_classify(raw, 4, 1000, {3}, 2, R::Stalled);
            CHECK(r.quality == DiscQuality::Bad, "Hänger → Bad");
            CHECK_EQ((int)r.track_status.size(), 5,
                     "track_status genau 0..4 (kein Phantom-Track)");
            CHECK_EQ(r.track_status[3], 2, "Hänger-Track 3 → defekt (2)");
            CHECK_EQ(r.track_status[4], 3,
                     "Track 4 hinter Hänger, Scan gab auf → ungescannt (3)");
            CHECK(!r.completed, "Stalled → nicht completed");
            CHECK(r.detail.find("Hänger umgangen") != std::string::npos,
                  "Detail nennt Hänger");
            CHECK(r.detail.find("Sprüngen gestoppt") != std::string::npos,
                  "Detail nennt Scan-Abbruch");
        }
        // Hänger-Track-Nr darf ntr NICHT über ntracks heben (Phantom-19-
        // Regression): hung={19} bei ntracks=4 → size bleibt 5.
        {
            std::vector<ProbeRaw> raw = { {1,100,1,200},{2,300,1,205} };
            ProbeResult r = probe_classify(raw, 4, 1000, {19}, 1, R::Stalled);
            CHECK_EQ((int)r.track_status.size(), 5,
                     "hung jenseits TOC erzeugt keinen Phantom-Track");
        }
        // Abbruch durch Nutzer (Aborted): Teil-Karte, completed=false,
        // Detail nennt Abbruch — aber nicht 'gave_up' (kein ungescannt).
        {
            std::vector<ProbeRaw> raw = { {1,100,1,200},{1,200,1,205} };
            ProbeResult r = probe_classify(raw, 2, 600, {}, 0, R::Aborted);
            CHECK(!r.completed, "Aborted → nicht completed");
            CHECK(r.detail.find("abgebrochen") != std::string::npos,
                  "Detail nennt Abbruch");
            CHECK_EQ(r.track_status[2], 0,
                     "Aborted (nicht gave_up) → Track 2 nicht ungescannt");
        }
        // Leerer Scan (gar keine Proben) → 0 samples, Clean, leere Karte.
        {
            ProbeResult r = probe_classify({}, 0, 0, {}, 0, R::Ok);
            CHECK_EQ(r.samples, 0, "keine Proben → 0 samples");
            CHECK(r.map.empty(), "keine Proben → leere Karte");
        }
    }

    // ── b64 (RFC 4648 Testvektoren)
    {
        CHECK_EQ(b64(""), std::string(""), "b64('') = ''");
        CHECK_EQ(b64("f"), std::string("Zg=="), "b64('f')");
        CHECK_EQ(b64("fo"), std::string("Zm8="), "b64('fo')");
        CHECK_EQ(b64("foo"), std::string("Zm9v"), "b64('foo')");
        CHECK_EQ(b64("foob"), std::string("Zm9vYg=="), "b64('foob')");
        CHECK_EQ(b64("fooba"), std::string("Zm9vYmE="), "b64('fooba')");
        CHECK_EQ(b64("foobar"), std::string("Zm9vYmFy"), "b64('foobar')");
        // Binär (hohe Bytes) → +/ Alphabet, korrektes Padding.
        CHECK_EQ(b64(std::string("\xFF\xFF\xFF", 3)), std::string("////"),
                 "b64(0xFFFFFF) = '////'");
    }

    // ── scan_svg (reiner Text, Qt-frei)
    {
        ProbeResult empty;
        std::string s0 = scan_svg(empty);
        CHECK(s0.find("<svg") != std::string::npos, "scan_svg → SVG");
        CHECK(s0.find("kein Scan") != std::string::npos,
              "leere Karte → 'kein Scan'");
        ProbeResult r;
        r.lba_min = 0; r.lba_max = 1000;
        r.map = { {0,0},{500,0},{900,0} };
        std::string s1 = scan_svg(r);
        CHECK(s1.find("kein Scan") == std::string::npos,
              "mit Karte → kein 'kein Scan'");
        CHECK(s1.find("#27ae60") != std::string::npos,
              "saubere Sektoren → grüne Ringe");
    }

    // ── audio_ext (Format-Dispatch)
    {
        Config c;
        c.audio_format = "flac"; CHECK_EQ(audio_ext(c), std::string("flac"),
                                          "audio_ext flac");
        c.audio_format = "opus"; CHECK_EQ(audio_ext(c), std::string("opus"),
                                          "audio_ext opus");
        c.audio_format = "mp3";  CHECK_EQ(audio_ext(c), std::string("mp3"),
                                          "audio_ext mp3");
    }

    // ── Archiv-Roundtrip (append → load; neueste zuletzt). Isolierter
    //    tmp-Config-Dir via set_config_path → keine echte Config berührt.
    {
        fs::path dir = fs::temp_directory_path() /
            ("cdripper_test_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        set_config_path((dir / "config.ini").string());

        CHECK(load_archive().empty(), "frischer Dir → leeres Archiv");

        ArchiveEntry a;
        a.ts = 1000; a.disc_id = "DISC-A"; a.artist = "Alice";
        a.title = "First"; a.year = "1999"; a.tracks = 10;
        a.kind = "rip"; a.format = "flac"; a.ar_ok = 8; a.ar_total = 10;
        a.damaged_tracks = 1; a.quality = 1; a.scan_completed = true;
        a.outcome = "ok"; a.lba_min = 0; a.lba_max = 5000;
        a.map = { {0,0},{2500,1},{5000,2} };
        a.track_status = { -1, 0, 1, 2, 0 };   // [0] leer, dann 4 Tracks
        CHECK(append_archive(a), "append #1 ok");

        ArchiveEntry b;
        b.ts = 2000; b.disc_id = "DISC-B"; b.artist = "Bob";
        b.title = "Second"; b.kind = "scan"; b.quality = 2;
        b.scan_completed = false; b.outcome = "scan";
        CHECK(append_archive(b), "append #2 ok");

        auto v = load_archive();
        CHECK_EQ((int)v.size(), 2, "Archiv hat 2 Einträge");
        if (v.size() == 2) {
            CHECK_EQ(v[0].disc_id, std::string("DISC-A"),
                     "Reihenfolge: ältester zuerst");
            CHECK_EQ(v[1].disc_id, std::string("DISC-B"),
                     "Reihenfolge: neuester zuletzt");
            CHECK_EQ(v[0].artist, std::string("Alice"), "RT artist");
            CHECK_EQ(v[0].ar_ok, 8, "RT ar_ok");
            CHECK_EQ(v[0].ar_total, 10, "RT ar_total");
            CHECK_EQ(v[0].damaged_tracks, 1, "RT damaged_tracks");
            CHECK_EQ(v[0].quality, 1, "RT quality");
            CHECK(v[0].scan_completed, "RT scan_completed true");
            CHECK_EQ((int)v[0].map.size(), 3, "RT map size");
            CHECK_EQ(v[0].map[2].status, 2, "RT map sample status");
            CHECK_EQ((int)v[0].track_status.size(), 5, "RT tstatus size");
            CHECK_EQ(v[0].track_status[3], 2, "RT tstatus[3] defekt");
            CHECK(v[1].track_status.empty(), "RT tstatus leer wenn ungesetzt");
            CHECK(!v[1].scan_completed, "RT scan_completed false");
            CHECK_EQ(v[1].kind, std::string("scan"), "RT kind scan");
        }
        fs::remove_all(dir, ec);
    }

    // ── classify_damage (Schadensform aus Defekt-LBA-Muster) ───────────
    {
        DamageReport e = classify_damage({}, 0, 300000, 15);
        CHECK_EQ(e.kind, DamageReport::None, "dmg leer → None");
        CHECK_EQ(e.bad_sectors, 0, "dmg leer → 0 Sektoren");

        // Ein langer zusammenhängender Block → umlaufender Wisch/Schmutz.
        std::vector<ProbeSample> scuff;
        for (int l = 50000; l <= 50300; ++l) scuff.push_back({ l, 1 });
        DamageReport s = classify_damage(scuff, 0, 300000, 15);
        CHECK_EQ(s.kind, DamageReport::Scuff, "dmg Block → Scuff");
        CHECK_EQ(s.clusters, 1, "dmg Scuff 1 Cluster");
        CHECK_EQ(s.bad_sectors, 301, "dmg Scuff 301 Sektoren");
        CHECK(!s.advice.empty(), "dmg Scuff hat Empfehlung");

        // Viele winzige Cluster, regelmäßig ~1/Umdrehung → radialer Kratzer.
        std::vector<ProbeSample> scr;
        for (int i = 0; i < 60; ++i) scr.push_back({ 30000 + i * 20, 2 });
        DamageReport sc = classify_damage(scr, 0, 300000, 15);
        CHECK_EQ(sc.kind, DamageReport::Scratch, "dmg periodisch → Scratch");
        CHECK_EQ(sc.clusters, 60, "dmg Scratch 60 Cluster");

        // Dichter, fast nur Total-Verlust, lokal → tiefe Gouge.
        std::vector<ProbeSample> gg;
        for (int l = 80000; l <= 80120; ++l) gg.push_back({ l, 2 });
        DamageReport gd = classify_damage(gg, 0, 300000, 15);
        CHECK_EQ(gd.kind, DamageReport::Gouge, "dmg dichter Block → Gouge");

        // Wenige verstreute Einzelfehler → kein klares Muster (Mixed).
        std::vector<ProbeSample> few = { {1000,1},{9000,2},{50000,1} };
        DamageReport fw = classify_damage(few, 0, 300000, 15);
        CHECK_EQ(fw.kind, DamageReport::Mixed, "dmg verstreut → Mixed");
        CHECK_EQ(fw.bad_sectors, 3, "dmg verstreut 3 Sektoren");

        // Hänger-Diagnose: leere Karte + Tracks hängten Laufwerk → DriveHang
        // (NICHT "sauberer Rip"). Inner-Circle-Fall.
        DamageReport h0 = classify_damage({}, 0, 300000, 18, 4);
        CHECK_EQ(h0.kind, DamageReport::DriveHang, "dmg leer+hung → DriveHang");
        // Dünne verstreute Punkte + Hänger → DriveHang statt "kein Muster"
        // (genau der reale Inner-Circle-Befund: 4 Sektoren, 4 Hänger).
        std::vector<ProbeSample> ic = { {120000,2},{160000,2},
                                        {200000,2},{240000,2} };
        DamageReport hr = classify_damage(ic, 0, 300000, 18, 4);
        CHECK_EQ(hr.kind, DamageReport::DriveHang,
                 "dmg dünn+hung → DriveHang (kein 'kein Muster')");
        // Hänger ohne Defekte UND ohne hung → weiterhin None (sauber).
        DamageReport hn = classify_damage({}, 0, 300000, 18, 0);
        CHECK_EQ(hn.kind, DamageReport::None, "dmg leer+0 hung → None");
        // Echtes Muster bleibt erhalten, auch wenn Tracks hingen
        // (Override greift nur beim schwachen Mixed).
        DamageReport hs = classify_damage(scuff, 0, 300000, 15, 2);
        CHECK_EQ(hs.kind, DamageReport::Scuff,
                 "dmg Scuff+hung → bleibt Scuff (Override nur bei Mixed)");
    }

    std::printf("\n%d OK, %d FAIL\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
