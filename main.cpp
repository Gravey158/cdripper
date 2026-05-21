// main.cpp — Argumente, Config, GUI- oder CLI-Modus wählen.
#include "engine.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int run_cli(const cdr::Config& cfg, bool once);   // cli.cpp
int run_calibrate(const cdr::Config& cfg);        // cli.cpp
int run_rip_worker(const std::string& dev, const std::string& work,
                   int speed, bool fast,
                   const std::string& defer_csv);  // cli.cpp (Subprozess)
int run_probe_worker(const std::string& dev, int start_track,
                     int density);              // cli.cpp

#ifdef HAVE_QT
#include <QApplication>
#include "gui.h"
#endif

#ifdef __APPLE__
// Sparkle-Auto-Update (mac/sparkle_bridge.mm, nur APPLE in CMakeLists
// gelinkt). cdripper-Build OHNE Sparkle.framework liefert Stubs (siehe
// CMakeLists). Aufruf erst nach QApplication-Konstruktion — Sparkle
// erwartet eine laufende NSApplication-Instanz, die Qt auf Mac aufsetzt.
extern "C" void cdripper_sparkle_init();
#endif

static void usage(const char* a0) {
    std::cerr <<
      "cdripper — Audio-CD → FLAC → Nextcloud (Navidrome)\n"
      "  " << a0 << " [Optionen]\n"
      "    --device PFAD   Laufwerk (mehrfach = parallel, nur --cli)\n"
      "    --all-drives    alle erkannten Laufwerke parallel (--cli)\n"
      "    --config PFAD   Config-Datei (Default ~/.config/cdripper/config.ini)\n"
      "    --once          Nur eine CD verarbeiten, dann beenden\n"
      "    --dry-run       Rippen+Encoden, aber NICHT hochladen\n"
      "    --calibrate     Drive-Offset gegen AccurateRip kalibrieren+speichern\n"
      "    --cli           Headless erzwingen (keine GUI)\n"
      "    --gui           GUI erzwingen\n"
      "    -h, --help      Diese Hilfe\n";
}

int main(int argc, char** argv) {
    // Subprozess-Modus: ganz früh abfangen, vor Config/curl/GUI.
    if (argc >= 6 && std::string(argv[1]) == "--rip-worker") {
        cdr::curl_global_setup();   // (Ripper braucht kein curl, aber harmlos)
        int rc = run_rip_worker(argv[2], argv[3],
                                std::atoi(argv[4]),
                                std::string(argv[5]) == "1",
                                argc >= 7 ? argv[6] : "-");
        cdr::curl_global_teardown();
        return rc;
    }
    if (argc >= 3 && std::string(argv[1]) == "--probe-worker") {
        return run_probe_worker(argv[2], argc >= 4 ? std::atoi(argv[3]) : 1,
                                argc >= 5 ? std::atoi(argv[4]) : 6);
    }

    std::string cfg_path;          // leer = aus aktivem Profil ableiten
    std::vector<std::string> dev_overrides;     // T7: --device mehrfach
    bool all_drives = false;                     // T7: --all-drives
    bool once = false, dry = false, force_cli = false, force_gui = false;
    bool calibrate = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--device" && i + 1 < argc)
            dev_overrides.push_back(argv[++i]);  // mehrfach = Multi-Laufwerk
        else if (a == "--all-drives") all_drives = true;
        else if (a == "--config" && i + 1 < argc) cfg_path     = argv[++i];
        else if (a == "--once")    once      = true;
        else if (a == "--dry-run") dry       = true;
        else if (a == "--calibrate") calibrate = true;
        else if (a == "--cli")     force_cli = true;
        else if (a == "--gui")     force_gui = true;
        else if (a == "--version") {
            std::cout << "cdripper " << cdr::VERSION << "\n"; return 0; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { std::cerr << "Unbekannte Option: " << a << "\n";
               usage(argv[0]); return 2; }
    }

    if (cfg_path.empty())
        cfg_path = cdr::profile_path(cdr::active_profile());
    cdr::set_config_path(cfg_path);     // drive_offsets.ini neben die Config
    cdr::Config cfg = cdr::load_config(cfg_path);
    if (all_drives) {                            // T7: alle erkannten
        auto all = cdr::list_optical_devices();
        if (!all.empty()) { cfg.devices = all; cfg.device = all.front(); }
    } else if (dev_overrides.size() == 1) {
        cfg.device = dev_overrides[0];           // Single (Legacy)
        cfg.devices.clear();
    } else if (dev_overrides.size() > 1) {       // Multi
        cfg.devices = dev_overrides;
        cfg.device  = dev_overrides.front();
    }
    if (dry) cfg.dry_run = true;

    cdr::curl_global_setup();
    int rc = 0;

    if (calibrate) {
        rc = run_calibrate(cfg);
        cdr::curl_global_teardown();
        return rc;
    }

#ifdef HAVE_QT
    // Mac und Windows haben kein DISPLAY/WAYLAND_DISPLAY — auf APPLE/_WIN32
    // ist der Window-Server immer da. Auf Linux ist die Env-Variable die
    // zuverlässige Disambiguierung GUI-Session vs. SSH/Headless.
#if defined(__APPLE__) || defined(_WIN32)
    bool have_display = true;
#else
    bool have_display = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
#endif
    if (!force_cli && (force_gui || have_display)) {
        QApplication app(argc, argv);
        app.setApplicationName("CD Ripper");
        app.setApplicationVersion(cdr::VERSION);
#ifdef __APPLE__
        cdripper_sparkle_init();   // Auto-Update-Check (24h, siehe Info.plist)
#endif
        // ── Design-System „Modern Dark, kuratiert" ───────────────────────
        // Tokens: bg #1e2127 · surface #23272e · card #262a31 · elevated
        // #2b313b · border #343b47 · hover #2f3640 · text #e8eaed · muted
        // #9aa0aa · accent #2979ff (= Rip-Fortschritt) · ok/warn/err.
        app.setStyleSheet(R"(
            * { font-size: 10pt; }
            QMainWindow, QDialog { background:#1e2127; color:#e8eaed; }
            QWidget { color:#e8eaed; }
            QLabel { color:#e8eaed; background:transparent; }
            QLabel[muted="true"] { color:#9aa0aa; }

            QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QComboBox,
            QAbstractSpinBox {
                background:#262a31; color:#e8eaed;
                border:1px solid #343b47; border-radius:7px;
                padding:4px 8px; min-height:16px;
                selection-background-color:#2979ff;
                selection-color:#ffffff; }
            QLineEdit:hover, QSpinBox:hover, QComboBox:hover,
            QPlainTextEdit:hover { border-color:#3f4756; }
            QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
            QSpinBox:focus, QComboBox:focus, QAbstractSpinBox:focus {
                border:1px solid #2979ff; background:#222a36; }
            QComboBox::drop-down { border:0; width:22px; }
            QComboBox QAbstractItemView {
                background:#262a31; color:#e8eaed;
                border:1px solid #343b47; border-radius:8px;
                padding:4px; outline:0;
                selection-background-color:#2979ff;
                selection-color:#ffffff; }

            QGroupBox {
                background:#262b33; border:0;
                border-radius:12px; margin-top:13px;
                padding:9px 12px 9px 12px; }
            QGroupBox::title {
                subcontrol-origin:margin; subcontrol-position:top left;
                left:4px; padding:0 2px; color:#7c8593;
                font-size:8pt; font-weight:800; letter-spacing:2px; }

            QToolButton {
                background:#2b313b; color:#e8eaed;
                border:1px solid #3a414e; border-radius:8px;
                padding:7px 14px; font-weight:600; }
            QToolButton:hover  { background:#333b48; border-color:#475264; }
            QToolButton:pressed{ background:#1f63d6; border-color:#1f63d6; }
            QToolButton::menu-indicator { image:none; width:0; height:0; }

            QPushButton {
                background:#2b313b; color:#e8eaed;
                border:1px solid #3a414e; border-radius:7px;
                padding:5px 14px; min-height:16px; font-weight:600; }
            QPushButton:hover   { background:#333b48; border-color:#475264; }
            QPushButton:pressed { background:#1f63d6; border-color:#1f63d6; }
            QPushButton:disabled{ color:#5b6675; background:#23272e;
                border-color:#2b313b; }
            QPushButton:default,
            QPushButton[primary="true"] {
                background:#2979ff; border-color:#2979ff; color:#ffffff; }
            QPushButton:default:hover,
            QPushButton[primary="true"]:hover { background:#4a90ff; }
            QPushButton:default:pressed,
            QPushButton[primary="true"]:pressed { background:#1f63d6; }

            QTableWidget, QTableView {
                background:#262a31; alternate-background-color:#23272e;
                color:#e8eaed; border:1px solid #343b47; border-radius:10px;
                gridline-color:#2c323d; outline:0;
                selection-background-color:#2979ff;
                selection-color:#ffffff; }
            QTableWidget::item { padding:5px 8px; }
            QHeaderView::section {
                background:#2b313b; color:#aab0bb; border:0;
                border-right:1px solid #23272e; padding:7px 8px;
                font-weight:600; }
            QHeaderView::section:first { border-top-left-radius:10px; }
            QHeaderView::section:last  { border-top-right-radius:10px;
                border-right:0; }
            QTableCornerButton::section { background:#2b313b; border:0; }

            QListWidget {
                background:#23272e; color:#e8eaed; border:1px solid #343b47;
                border-radius:10px; outline:0; padding:4px; }
            QListWidget::item { padding:9px 12px; border-radius:7px; }
            QListWidget::item:hover    { background:#2b313b; }
            QListWidget::item:selected { background:#2979ff; color:#fff; }

            QMenuBar { background:#1e2127; color:#e8eaed; padding:2px; }
            QMenuBar::item { padding:6px 12px; border-radius:7px; }
            QMenuBar::item:selected { background:#2b313b; }
            QMenu { background:#262a31; color:#e8eaed;
                border:1px solid #343b47; border-radius:9px; padding:5px; }
            QMenu::item { padding:7px 22px 7px 14px; border-radius:6px; }
            QMenu::item:selected { background:#2979ff; color:#fff; }
            QMenu::separator { height:1px; background:#343b47; margin:5px 8px; }

            QProgressBar {
                background:#21262e; border:0; border-radius:7px;
                text-align:center; color:#cfd3da; min-height:16px; }
            QProgressBar::chunk {
                background:#2979ff; border-radius:7px; }

            QScrollArea { background:#1e2127; border:0; }
            QScrollArea > QWidget > QWidget { background:#1e2127; }
            QAbstractScrollArea { background:#1e2127; }
            QWidget#multiWin { background:#1e2127; }
            QWidget#drivePanel { background:#262b33; border-radius:12px; }
            QStatusBar { background:#1a1d22; color:#9aa0aa; }
            QStatusBar::item { border:0; }
            QToolTip {
                background:#2b313b; color:#e8eaed;
                border:1px solid #475264; border-radius:7px; padding:5px 8px; }

            QScrollBar:vertical { background:transparent; width:11px;
                margin:2px; }
            QScrollBar::handle:vertical { background:#3a414e;
                border-radius:5px; min-height:28px; }
            QScrollBar::handle:vertical:hover { background:#4a5364; }
            QScrollBar:horizontal { background:transparent; height:11px;
                margin:2px; }
            QScrollBar::handle:horizontal { background:#3a414e;
                border-radius:5px; min-width:28px; }
            QScrollBar::handle:horizontal:hover { background:#4a5364; }
            QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
            QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }

            QCheckBox { spacing:8px; background:transparent; }
            QCheckBox::indicator, QGroupBox::indicator, QMenu::indicator {
                width:16px; height:16px; border-radius:5px;
                border:1px solid #5b6675; background:#262a31; }
            QCheckBox::indicator:hover { border-color:#7a8699; }
            QCheckBox::indicator:checked, QGroupBox::indicator:checked,
            QMenu::indicator:checked {
                background:#2979ff; border:1px solid #2979ff; }
            QCheckBox::indicator:checked:hover {
                background:#4a90ff; border-color:#4a90ff; }
            QCheckBox::indicator:disabled {
                border-color:#343b47; background:#23262d; }

            QTabWidget::pane { border:1px solid #343b47; border-radius:10px;
                top:-1px; }
            QTabBar::tab { background:transparent; color:#9aa0aa;
                padding:8px 16px; border-radius:7px; margin:2px; }
            QTabBar::tab:selected { background:#2b313b; color:#e8eaed; }
        )");
        MainWindow w(cfg, once, cfg_path);
        w.show();
        rc = app.exec();
        cdr::curl_global_teardown();
        return rc;
    }
    if (force_gui)
        std::cerr << "GUI angefordert, aber kein DISPLAY — Fallback auf CLI.\n";
#else
    if (force_gui)
        std::cerr << "Ohne Qt gebaut — nur CLI verfügbar.\n";
#endif

    if (cfg.webdav_pass.empty() && !cfg.dry_run) {
        std::cerr << "FEHLER: kein WebDAV-Passwort. Setze webdav_pass in "
                  << cfg_path << " (chmod 600) oder Env CDRIPPER_WEBDAV_PASS, "
                     "oder nutze --dry-run.\n";
        cdr::curl_global_teardown();
        return 1;
    }
    rc = run_cli(cfg, once);
    cdr::curl_global_teardown();
    return rc;
}
