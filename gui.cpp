// gui.cpp — Qt-Controller + Hauptfenster.
#include "gui.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QColor>
#include <QComboBox>
#include <QMenu>
#include <QMenuBar>
#include <QToolButton>
#include <QDialogButtonBox>
#include <cmath>
#include <cstdlib>

#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsBlurEffect>
#include <QMouseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QCursor>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPainter>
#include <QPainterPath>
#include <QProgressDialog>
#include <QCloseEvent>
#include <QShowEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <algorithm>
#include <sstream>
#include <QFont>
#include <QProgressBar>
#include <QStyledItemDelegate>
#include <QGraphicsDropShadowEffect>
#include <QLinearGradient>
#include <QImage>
#include <QSplitter>
#include <QDialog>
#include <QListWidgetItem>
#include <QPushButton>
#include <QShortcut>
#include <QSpacerItem>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextCursor>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <windowsx.h>
#  undef IN   // windows.h-Annotation-Makros (leer) kollidieren mit CoverSpin-
#  undef OUT  // Konstanten IN/OUT in gui.cpp — wegräumen.
#endif

// ───────────────────────────── Controller ─────────────────────────────────────

// Prozessweiter Zähler laufender Rips über ALLE Controller/Fenster hinweg.
// Damit lässt sich der Settings-Dialog global sperren, solange irgendwo
// (Einzel- ODER Multi-Fenster) gerippt wird — vorher prüfte jedes Fenster
// nur seinen eigenen Controller, sodass man die Einstellungen übers jeweils
// andere Fenster doch öffnen konnte. (Deklaration vor ~Controller, damit der
// Leak-Ausgleich dort darauf zugreifen kann.)
static std::atomic<int> g_active_rips{0};
bool anyRipActive() { return g_active_rips.load() > 0; }

Controller::Controller(QObject* p) : QObject(p) {}

Controller::~Controller() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    // joinWorker() wird per QueuedConnection gepostet und läuft nach der
    // Zerstörung NICHT mehr → sein g_active_rips.fetch_sub bliebe aus und der
    // globale Zähler leakt (der Einstellungs-Dialog wäre dann bis zum
    // App-Neustart gesperrt — passiert z. B. beim Schließen des Multi-Fensters
    // während eines laufenden Rips). Genau einmal nachholen, falls joinWorker
    // noch nicht lief (running_ noch true).
    if (running_.exchange(false))
        g_active_rips.fetch_sub(1);
}

void Controller::start(const cdr::Config& cfg, bool once) {
    if (running_.load()) return;
    stop_ = false;
    running_ = true;
    g_active_rips.fetch_add(1);

    cdr::Callbacks cb;
    cb.onWaiting = [this](const std::string& m) {
        emit waiting(QString::fromStdString(m));
    };
    cb.onDiscIdent = [this](const cdr::DiscIdent& d) {
        emit discIdent(QString::fromStdString(d.id), d.toc_tracks);
    };
    cb.onAlbum = [this](const cdr::Album& a) {
        QStringList ti, ar;
        for (const auto& t : a.tracks) {
            ti << QString::fromStdString(t.title);
            ar << QString::fromStdString(t.artist);
        }
        emit albumReady(QString::fromStdString(a.artist),
                        QString::fromStdString(a.title),
                        QString::fromStdString(a.year()), ti, ar);
        emit coverReleaseId(QString::fromStdString(a.mb_release_id));
    };
    cb.onCover = [this](const fs::path& p) {
        emit coverReady(QString::fromStdString(p.string()));
    };
    cb.onTrack = [this](int i, cdr::TrackState s, double f, const std::string& m) {
        emit trackState(i, (int)s, f, QString::fromStdString(m));
    };
    cb.onProgress = [this](double e, double eta, int r, int u, int t) {
        emit progress(e, eta, r, u, t);
    };
    cb.onMetrics = [this](double r, double en, double up) {
        emit metrics(r, en, up);
    };
    cb.onChooseRelease = [this](const std::vector<std::string>& l,
                                int def) -> int {
        // Manuelle Metadaten gesetzt → nicht nachfragen, stumm Default nehmen.
        if (suppressChooser_.load()) return def;
        QStringList ql;
        for (const auto& s : l) ql << QString::fromStdString(s);
        int res = def;
        QMetaObject::invokeMethod(this, "chooseReleaseSlot",
            Qt::BlockingQueuedConnection,
            Q_RETURN_ARG(int, res), Q_ARG(QStringList, ql),
            Q_ARG(int, def));
        return res;
    };
    cb.onLog = [this](const std::string& l) {
        cdr::log_to_file(l);
        emit logLine(QString::fromStdString(l));
    };
    cb.onDiscDone = [this](bool ok, const std::string& m) {
        cdr::log_to_file((ok ? "[OK] " : "[FEHLER] ") + m);
        emit discDone(ok, QString::fromStdString(m));
    };
    cb.onFatal = [this](const std::string& m) {
        emit fatal(QString::fromStdString(m));
    };
    cb.onDiscScanInit = [this](int lo, int hi) {
        emit discScanInit(lo, hi);
    };
    cb.onDiscScanCell = [this](int lba, int st) {
        emit discScanCell(lba, st);
    };
    cb.onDiscScanCursor = [this](int lba) {
        emit discScanCursor(lba);
    };
    cb.onRipProgress = [this](double f) {
        emit ripProgress(f);
    };
    if (deferFn_) {
        auto fn = deferFn_;
        cb.ripDeferTracks = [fn](const std::string& id) { return fn(id); };
    }
    if (statusFn_) {
        auto fn = statusFn_;
        cb.scannedTrackStatus = [fn](const std::string& id) { return fn(id); };
    }

    pl_ = std::make_unique<cdr::Pipeline>(cfg, std::move(cb));
    worker_ = std::thread([this, once] {
        pl_->run(stop_, once);
        QMetaObject::invokeMethod(this, "joinWorker", Qt::QueuedConnection);
    });
}

void Controller::requestStop() { stop_ = true; }

void Controller::joinWorker() {
    if (worker_.joinable()) worker_.join();
    pl_.reset();
    running_ = false;
    g_active_rips.fetch_sub(1);
    emit finished();
}

void Controller::editTrackTitle(int idx, const QString& t) {
    if (pl_) pl_->set_track_title(idx, t.toStdString());
}
void Controller::editTrackArtist(int idx, const QString& a) {
    if (pl_) pl_->set_track_artist(idx, a.toStdString());
}
void Controller::editAlbum(const QString& ar, const QString& ti, const QString& y) {
    if (pl_) pl_->set_album(ar.toStdString(), ti.toStdString(), y.toStdString());
}
int Controller::chooseReleaseSlot(QStringList labels, int def) {
    if (labels.isEmpty()) return def;
    bool ok = false;
    QString sel = QInputDialog::getItem(
        nullptr, "MusicBrainz — Release wählen",
        "Diese Disc passt auf mehrere Releases (Edition/Land).\n"
        "Bitte die richtige wählen — oder Abbrechen, um den Rip zu stoppen\n"
        "(dann per „Metadaten suchen…\" das richtige Album wählen).",
        labels, def < labels.size() ? def : 0, false, &ok);
    // Abbrechen → Rip abbrechen (nicht stumm mit dem Default weiterrippen).
    if (!ok) { requestStop(); return def; }
    int i = labels.indexOf(sel);
    return i >= 0 ? i : def;
}
void Controller::setCover(const QString& path) {
    if (pl_) pl_->set_cover(path.toStdString());
}

// ───────────────────────────── Helfer ─────────────────────────────────────────

static QString mmss(double s) {
    if (s < 0) return "—";
    int t = (int)(s + 0.5);
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

// Wird beim Anlegen des MainWindow-Tray-Icons gesetzt → notify() kann darüber
// plattformübergreifend (Linux/mac/Windows) Benachrichtigungen zeigen.
static QSystemTrayIcon* g_notify_tray = nullptr;

// Desktop-Notification (best effort). Bevorzugt das System-Tray-Icon (portabel);
// auf Linux zusätzlich notify-send (KDE/Freedesktop) als Fallback, falls kein
// Tray verfügbar ist. Früher NUR notify-send → auf mac/Windows funktionslos.
static void notify(const QString& title, const QString& body) {
    if (g_notify_tray && QSystemTrayIcon::supportsMessages()) {
        g_notify_tray->showMessage(title, body,
                                   QSystemTrayIcon::Information, 5000);
        return;
    }
#ifdef __linux__
    QProcess::startDetached("notify-send",
        { "-a", "CD Ripper", "-i", "media-optical-audio", title, body });
#endif
}

// QMessageBox mit erzwungener Mindestbreite (Default ist oft zu schmal).
static void msgWide(QWidget* p, QMessageBox::Icon ic, const QString& title,
                    const QString& text, int minw = 520) {
    QMessageBox m(ic, title, text, QMessageBox::Ok, p);
    m.setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (auto* g = qobject_cast<QGridLayout*>(m.layout())) {
        auto* sp = new QSpacerItem(minw, 0, QSizePolicy::Minimum,
                                   QSizePolicy::Expanding);
        g->addItem(sp, g->rowCount(), 0, 1, g->columnCount());
    }
    m.exec();
}

static QColor state_color(int s) {
    switch ((cdr::TrackState)s) {
        case cdr::TrackState::Pending:   return QColor(0x88, 0x88, 0x88);
        case cdr::TrackState::Ripping:   return QColor(0x29, 0x80, 0xb9);
        case cdr::TrackState::Ripped:    return QColor(0x16, 0xa0, 0x85);
        case cdr::TrackState::Encoding:  return QColor(0x8e, 0x44, 0xad);
        case cdr::TrackState::Uploading: return QColor(0xe6, 0x7e, 0x22);
        case cdr::TrackState::Done:      return QColor(0x27, 0xae, 0x60);
        case cdr::TrackState::Failed:    return QColor(0xc0, 0x39, 0x2b);
    }
    return Qt::black;
}

// ── Disc-Scan: polare Ring-Visualisierung ──────────────────────────────────────
// LBA ↔ physischer Radius (CD wird spiralförmig von innen nach außen
// gelesen) → zerkratzte Ringe erscheinen als rote Bänder, genau dort wo
// der Schaden physisch sitzt. Wird sowohl statisch (Scan/Archiv-Dialog)
// als auch live (Hauptfenster während des Rips) genutzt.
class DiscScanWidget : public QWidget {
public:
    // Darstellung: scharfe Pixel-Ringe (Spiral/Bar-Vergleichsvarianten
    // 2026-05-19 nach Dennis' Test entfernt — Ringe sind das Finale).
    explicit DiscScanWidget(QWidget* p = nullptr) : QWidget(p) {
        setMinimumSize(110, 110);   // Hauptfenster: kompakt fix (s.u.);
        auto* pt = new QTimer(this);            // sanftes Pulsieren
        connect(pt, &QTimer::timeout, this, [this]{
            pulse_ += 0.09; update(); });
        pt->start(60);
    }                               // Scan-Dialog: flexibel größer
    void setResult(const cdr::ProbeResult& r) {
        r_ = r; cur_ = -1; ripFrac_ = -1.0; update();
    }
    void beginScan(int lo, int hi) {            // Live: leeren + Bereich
        r_ = cdr::ProbeResult{};
        r_.lba_min = lo; r_.lba_max = hi; r_.completed = true;
        cur_ = -1; ripFrac_ = -1.0;
        update();
    }
    // Rip-Gesamtfortschritt 0..1: eigenfarbiger Indikator, der die Scan-
    // Vorfärbung von innen nach außen überschreibt („bis hierhin gerippt").
    void setRipProgress(double f) {
        ripFrac_ = f < 0 ? 0.0 : (f > 1 ? 1.0 : f);
        update();
    }
    void addCell(int lba, int status) {         // Live: eine Position
        if (r_.lba_max <= r_.lba_min) { r_.lba_min = lba; r_.lba_max = lba + 1; }
        r_.map.push_back({ lba, status });
        cur_ = lba;                              // Scan-Cursor = hier gerade
        update();
    }
    void setCursor(int lba) {                   // Echtzeit: liest GERADE hier
        if (r_.lba_max <= r_.lba_min) { r_.lba_min = lba; r_.lba_max = lba + 1; }
        else if (lba > r_.lba_max) r_.lba_max = lba;
        cur_ = lba;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter g(this);
        // Hintergrund = Karten-Oberfläche (#262b33, identisch zum QGroupBox),
        // damit das Widget im „DISC"-Kasten kein dunkleres Quadrat bildet.
        g.fillRect(rect(), QColor("#262b33"));
        paintRings(g);
    }
private:
    static QColor stcol(int s) {                  // -2 in Arbeit · 0/1/2
        return s == 2 ? QColor("#c0392b")
             : s == 1 ? QColor("#e0a83e")
             : s == 0 ? QColor("#27ae60")
             : s == -2 ? QColor("#3a4250")
                       : QColor("#2b2f37");        // -1 = ungescannt (Rohling)
    }
    // Stichproben in N gleich breite Eimer entlang der LBA-Achse legen
    // (schlechtester Status gewinnt je Eimer → keine Sub-Pixel-Schlieren).
    // Eimer ohne Probe, aber vor dem Cursor → -2 (in Arbeit), sonst -1.
    std::vector<int> bucketize(int N, double* curFrac) const {
        std::vector<int> b(N, -1);
        double span = (double)(r_.lba_max - r_.lba_min);
        if (span <= 0) return b;
        auto fr = [&](int lba){ double n=(lba-r_.lba_min)/span;
                                return n<0?0.0:(n>1?1.0:n); };
        for (auto& s : r_.map) {
            int i = (int)std::lround(fr(s.lba) * (N - 1));
            if (i < 0) i = 0; if (i >= N) i = N - 1;
            if (s.status > b[i]) b[i] = s.status;
        }
        double cf = -1.0;
        if (cur_ >= 0) {
            cf = fr(cur_);
            int ci = (int)std::lround(cf * (N - 1));
            for (int i = 0; i <= ci && i < N; ++i)
                if (b[i] == -1) b[i] = -2;
        }
        if (curFrac) *curFrac = cf;
        return b;
    }

    // ── Variante A: scharfe Pixel-Ringe ───────────────────────────────────
    void paintRings(QPainter& g) {
        const int W = width(), H = height();
        const QPointF c(W / 2.0, H / 2.0);
        const double R = std::min(W, H) / 2.0 - 8.0;
        const double ro = R * 0.99, ri = R * 0.22;   // breiterer Nutzradius
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setPen(Qt::NoPen);
        g.setBrush(QColor("#2b2f37"));
        g.drawEllipse(c, R, R);
        int lo = (int)std::ceil(ri), hi = (int)std::floor(ro);
        int N = std::max(8, hi - lo + 1);
        double curFrac = -1.0;
        bool have = !(r_.map.empty() && cur_ < 0) &&
                    r_.lba_max > r_.lba_min;
        std::vector<int> b = have ? bucketize(N, &curFrac)
                                  : std::vector<int>();
        // Pixel-genaue, satt gefüllte konzentrische Ringe (AA aus → knackig);
        // gleichfarbige Läufe zu einem dicken Pen-Kreis zusammengefasst.
        g.setRenderHint(QPainter::Antialiasing, false);
        for (int i = 0; i < (int)b.size(); ) {
            int st = b[i]; int j = i;
            while (j < (int)b.size() && b[j] == st) ++j;
            if (st != -1) {
                int w = j - i;
                if (st == 2 && w < 2) w = 2;          // einzelner Defekt bleibt
                double rr = lo + (i + (j - 1)) / 2.0;
                g.setBrush(Qt::NoBrush);
                g.setPen(QPen(stcol(st), w));
                g.drawEllipse(c, rr, rr);
            }
            i = j;
        }
        // Rip-Fortschritt (nur Hauptfenster setzt ripFrac_): blaues Overlay
        // von innen bis zur Position, Defekte darüber rot nachgezeichnet.
        if (ripFrac_ >= 0.0) {
            double pr = ri + ripFrac_ * (ro - ri);
            QPainterPath o;  o.addEllipse(c, pr, pr);
            QPainterPath in; in.addEllipse(c, ri, ri);
            g.setPen(Qt::NoPen);
            g.fillPath(o.subtracted(in), QColor(0x29, 0x79, 0xff));
            for (int i = 0; i < (int)b.size(); ++i)
                if (b[i] == 2) { double rr = lo + i;
                    g.setBrush(Qt::NoBrush);
                    g.setPen(QPen(QColor("#c0392b"), 2));
                    g.drawEllipse(c, rr, rr); }
            g.setBrush(Qt::NoBrush);
            g.setPen(QPen(QColor("#7ab8ff"), 2));
            g.drawEllipse(c, pr, pr);
        }
        // Live-Cursor: scharfer (kein Dash → wäre fusselig) heller Ring.
        if (curFrac >= 0.0) {
            double cr = ri + curFrac * (ro - ri);
            g.setBrush(Qt::NoBrush);
            g.setPen(QPen(QColor("#4fc3f7"), 2));
            g.drawEllipse(c, cr, cr);
        }
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setPen(Qt::NoPen);
        g.setBrush(QColor("#1e2127"));
        g.drawEllipse(c, ri * 0.55, ri * 0.55);       // Spindelloch
        g.setBrush(Qt::NoBrush);
        g.setPen(QColor("#3a3f4b"));
        g.drawEllipse(c, R, R);
        double s = 0.5 + 0.5 * std::sin(pulse_);
        QColor glow(0x4f, 0xc3, 0xf7, (int)(28 + 46 * s));
        g.setPen(QPen(glow, 2.0 + 1.5 * s));
        g.drawEllipse(c, R - 1.5, R - 1.5);
    }

    int cur_ = -1;                               // Live-Scan-Cursor (LBA)
    double ripFrac_ = -1.0;                      // Rip-Fortschritt 0..1 (-1=aus)
    double pulse_ = 0.0;                          // Pulsier-Phase
    cdr::ProbeResult r_;
};

// Easter-Egg: Klick aufs Cover → es morpht zur CD-Scheibe, rotiert ~5 s
// und morpht zurück zum Quadrat. Reines Overlay über dem Cover-Label,
// nichts am Backend. Selbst-zerstörend nach der Animation.
class CoverSpin : public QWidget {
    static constexpr double IN = 600, SPIN = 5000, OUT = 600;
    static constexpr int    TOTAL = 600 + 5000 + 600 + 60;
    static constexpr double PI = 3.14159265358979323846;
public:
    CoverSpin(QWidget* parent, const QPixmap& cover, const QRect& geom)
        : QWidget(parent), pm_(cover) {
        setGeometry(geom);
        setAttribute(Qt::WA_NoSystemBackground);
        blur_ = new QGraphicsBlurEffect(this);
        blur_->setBlurRadius(0.0);
        setGraphicsEffect(blur_);                       // nur beim Morph aktiv
        clock_.start();
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this]{
            double tt = (double)clock_.elapsed();
            if (tt >= TOTAL) { if (onFinished) onFinished();
                               deleteLater(); return; }
            // Blur fährt NUR während der Übergänge hoch (Morph rein/raus),
            // im Spin gestochen scharf — verstärkt den Verwandlungseffekt.
            double tr = 0.0;
            if (tt < IN)                 tr = std::sin(PI * (tt / IN));
            else if (tt >= IN + SPIN)    tr = std::sin(PI *
                                              ((tt - IN - SPIN) / OUT));
            blur_->setBlurRadius(16.0 * tr);
            update();
        });
        t->start(16);
    }
    std::function<void()> onFinished;
protected:
    void paintEvent(QPaintEvent*) override {
        double t = (double)clock_.elapsed();
        double morph;                                   // 0=Quadrat 1=Kreis
        if (t < IN)            morph = t / IN;
        else if (t < IN+SPIN)  morph = 1.0;
        else                   morph = 1.0 - (t-IN-SPIN) / OUT;
        if (morph < 0) morph = 0; if (morph > 1) morph = 1;
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        const double W = width(), H = height();
        const double side = std::min(W, H);
        const QPointF c(W/2.0, H/2.0);
        // Ecken mit App-bg füllen → das ungemalte Quadrat „verschwindet"
        // optisch (sonst dunkler Block, wirkt wie gedrehtes Foto).
        g.fillRect(rect(), QColor("#1e2127"));
        // Stärkeres Morphing: smoothstep^1.5 → die Rundung schiebt sich
        // betonter rein; Quadrat → Kreis (r=0 → side/2).
        double e = morph * morph * (3.0 - 2.0 * morph);
        e = e * e * (3.0 - 2.0 * e);                     // doppelt = knackiger
        double r = e * (side/2.0);
        QPainterPath clip;
        clip.addRoundedRect(QRectF((W-side)/2.0, (H-side)/2.0, side, side),
                            r, r);
        g.setClipPath(clip);
        // Rotation sanft hochfahren, einen Hauch langsamer als zuvor.
        double ang = (t > IN ? (t-IN) : 0.0) * 0.40 * (0.15 + 0.85 * e);
        g.save();
        g.translate(c);
        g.rotate(ang);
        QRectF dst(-side/2.0, -side/2.0, side, side);
        g.drawPixmap(dst, pm_, QRectF(pm_.rect()));
        g.restore();
        // CD-Anmutung. Mittelloch = SAUBERES Loch in App-bg-Farbe (voll
        // deckend) statt halbtransparentem Grau — liest sich als echtes
        // Spindelloch, durch das man „durchsieht".
        if (morph > 0.05) {
            g.setClipping(false);
            g.setPen(Qt::NoPen);
            g.setBrush(QColor(60, 64, 72, (int)(120*morph)));
            g.drawEllipse(c, side*0.17, side*0.17);     // Klemmring (dunkel)
            g.setBrush(QColor("#1e2127"));               // echtes Loch
            g.drawEllipse(c, side*0.085, side*0.085);
            g.setBrush(Qt::NoBrush);
            g.setPen(QPen(QColor(255,255,255,(int)(55*morph)), 1.5));
            g.drawEllipse(c, side*0.085, side*0.085);    // Lochkante hell
            g.drawEllipse(c, side*0.17, side*0.17);
            g.drawEllipse(c, side*0.49, side*0.49);
            // wandernder Glanzkeil
            QPainterPath sheen;
            double sa = ang * 1.7;
            sheen.moveTo(c);
            sheen.arcTo(QRectF(c.x()-side*0.49, c.y()-side*0.49,
                               side*0.98, side*0.98), sa, 28);
            sheen.closeSubpath();
            g.setPen(Qt::NoPen);
            g.setBrush(QColor(255,255,255,(int)(26*morph)));
            g.drawPath(sheen);
        }
    }
private:
    QPixmap              pm_;
    QElapsedTimer        clock_;
    QGraphicsBlurEffect* blur_ = nullptr;
};

// ── Multi-Laufwerk-Modus (T7-GUI) ──────────────────────────────────────────────
// ───────────────────────── Randlose Titelleiste (Win/Linux) ───────────────────
// macOS behält die native Titelleiste; nur Win/Linux werden randlos mit eigener
// Leiste (Titel links, Min/Max/Close rechts). Von MainWindow UND MultiWindow
// genutzt → vor beiden definiert.

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
// Randlos-Resize unter Linux (X11 UND Wayland): hier gibt es kein natives
// Hit-Testing wie WM_NCHITTEST — stattdessen übergibt startSystemResize()
// die gepackte Kante an den Fenstermanager (KWin/Mutter ziehen dann selbst,
// inkl. Live-Cursor). Greifzonen wie im Win-Pfad: Kanten 8 px, Ecken 14 px.
// Beide randlosen Fenster rufen das aus event(); die Titelleiste prüft es
// in ihrem mousePressEvent zuerst (oberste 8 px = Resize, nicht Drag).
static Qt::Edges framelessEdgesAt(const QWidget* w, const QPoint& pt) {
    const QRect r = w->rect();
    const int b = 8, c = 14;
    const bool L = pt.x() < b, R = pt.x() >= r.width()  - b;
    const bool T = pt.y() < b, B = pt.y() >= r.height() - b;
    const bool Lc = pt.x() < c, Rc = pt.x() >= r.width()  - c;
    const bool Tc = pt.y() < c, Bc = pt.y() >= r.height() - c;
    Qt::Edges e;
    // Ecken zuerst (größere Zone) — wie winFramelessEvent().
    if ((Tc || Bc) && (Lc || Rc) && ((Tc && Lc) || (Tc && Rc) ||
                                     (Bc && Lc) || (Bc && Rc))) {
        if (Lc) e |= Qt::LeftEdge;
        if (Rc) e |= Qt::RightEdge;
        if (Tc) e |= Qt::TopEdge;
        if (Bc) e |= Qt::BottomEdge;
        return e;
    }
    if (L) e |= Qt::LeftEdge;
    if (R) e |= Qt::RightEdge;
    if (T) e |= Qt::TopEdge;
    if (B) e |= Qt::BottomEdge;
    return e;
}

static Qt::CursorShape framelessCursorFor(Qt::Edges e) {
    const bool L = e & Qt::LeftEdge, R = e & Qt::RightEdge;
    const bool T = e & Qt::TopEdge,  B = e & Qt::BottomEdge;
    if ((T && L) || (B && R)) return Qt::SizeFDiagCursor;
    if ((T && R) || (B && L)) return Qt::SizeBDiagCursor;
    if (L || R)               return Qt::SizeHorCursor;
    if (T || B)               return Qt::SizeVerCursor;
    return Qt::ArrowCursor;
}

// Aus event() der randlosen Fenster aufrufen (braucht Qt::WA_Hover am Fenster).
// true = Event verbraucht (Resize gestartet).
static bool framelessLinuxResizeEvent(QWidget* w, QEvent* ev) {
    if (w->isMaximized() || w->isFullScreen()) return false;
    switch (ev->type()) {
    case QEvent::HoverMove:
    case QEvent::HoverEnter: {
        // QCursor::pos() statt Event-API → identisch unter Qt5 und Qt6.
        const Qt::Edges e =
            framelessEdgesAt(w, w->mapFromGlobal(QCursor::pos()));
        const Qt::CursorShape cur = framelessCursorFor(e);
        if (cur == Qt::ArrowCursor) w->unsetCursor(); else w->setCursor(cur);
        return false;
    }
    case QEvent::HoverLeave:
        w->unsetCursor();
        return false;
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(ev);
        if (me->button() != Qt::LeftButton) return false;
        const Qt::Edges e = framelessEdgesAt(w, me->pos());
        if (!e) return false;
        if (auto* wh = w->windowHandle()) {
            wh->startSystemResize(e);
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}
#endif  // !Q_OS_WIN && !Q_OS_MACOS

#ifndef Q_OS_MACOS
class FramelessTitleBar : public QWidget {
public:
    explicit FramelessTitleBar(QWidget* win, const QString& titleText)
        : QWidget(win), win_(win) {
        setObjectName("framelessTitleBar");
        setFixedHeight(34);
        setAttribute(Qt::WA_Hover, true);   // Cursor-Feedback obere Resize-Zone
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(12, 0, 0, 0);
        h->setSpacing(0);
        auto* title = new QLabel(titleText);
        title->setObjectName("framelessTitle");
        title->setAttribute(Qt::WA_TransparentForMouseEvents);   // Klick → Drag
        h->addWidget(title);
        h->addStretch(1);
        auto mkBtn = [&](const QString& glyph, const QString& obj) {
            auto* b = new QPushButton(glyph);
            b->setObjectName(obj);
            b->setFixedSize(46, 34);
            b->setFocusPolicy(Qt::NoFocus);
            b->setCursor(Qt::ArrowCursor);
            h->addWidget(b);
            return b;
        };
        QPushButton* bMin = mkBtn(QString::fromUtf8("─"), "winBtnMin");
        QPushButton* bMax = mkBtn(QString::fromUtf8("□"), "winBtnMax");
        QPushButton* bCls = mkBtn(QString::fromUtf8("✕"), "winBtnClose");
        connect(bMin, &QPushButton::clicked, win_, &QWidget::showMinimized);
        connect(bMax, &QPushButton::clicked, this, [this]{ toggleMax(); });
        connect(bCls, &QPushButton::clicked, win_, &QWidget::close);
    }
    void toggleMax() {
        if (win_->isMaximized()) win_->showNormal(); else win_->showMaximized();
    }
protected:
#ifndef Q_OS_WIN
    // Linux: Ziehen via startSystemMove, Doppelklick = Max-Toggle.
    // (Auf Windows erledigt das WM_NCHITTEST/HTCAPTION nativ — inkl. Aero-Snap.)
    // Die Leiste liegt am oberen Fensterrand → obere Greifzone (und die
    // Ecken links/rechts) gehören dem Resize, erst darunter beginnt Drag.
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        auto* wh = win_->windowHandle();
        if (!wh) return;
        if (!win_->isMaximized()) {
            // mapTo statt globalPosition(): identische API in Qt5 und Qt6.
            const Qt::Edges edges =
                framelessEdgesAt(win_, mapTo(win_, e->pos()));
            if (edges) { wh->startSystemResize(edges); return; }
        }
        if (childAt(e->pos()) == nullptr) wh->startSystemMove();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) toggleMax();
    }
    // Cursor-Feedback für die Resize-Zone innerhalb der Leiste (das Fenster
    // selbst bekommt Hover-Events nur außerhalb seiner Kinder).
    bool event(QEvent* ev) override {
        if (ev->type() == QEvent::HoverMove || ev->type() == QEvent::HoverEnter) {
            const Qt::CursorShape cur = win_->isMaximized()
                ? Qt::ArrowCursor
                : framelessCursorFor(framelessEdgesAt(
                      win_, win_->mapFromGlobal(QCursor::pos())));
            if (cur == Qt::ArrowCursor) unsetCursor(); else setCursor(cur);
        } else if (ev->type() == QEvent::HoverLeave) {
            unsetCursor();
        }
        return QWidget::event(ev);
    }
#endif
private:
    QWidget* win_;
};
#endif  // !Q_OS_MACOS

#ifdef Q_OS_WIN
// Frameless-ABER-resizebar unter Windows: Qt::FramelessWindowHint entfernt
// WS_THICKFRAME → Windows ignoriert dann die WM_NCHITTEST-Resize-Codes (Resize
// komplett tot, egal wie groß die Greifzone). Fix: WS_THICKFRAME (+ Snap/Min/
// Max) zurück an den HWND, und per WM_NCCALCSIZE den nativen Rahmen wegrechnen,
// damit es optisch randlos bleibt. Beide randlosen Fenster rufen das.
static void applyWinFrameless(QWidget* w) {
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    LONG_PTR s = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    ::SetWindowLongPtr(hwnd, GWL_STYLE,
        s | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static bool winFramelessEvent(QWidget* w, QWidget* titleBar,
                              void* message, qintptr* result) {
    MSG* m = static_cast<MSG*>(message);
    if (m->message == WM_NCCALCSIZE && m->wParam == TRUE) {
        // Client-Area = ganzes Fenster (nativen Rahmen wegrechnen). Maximiert:
        // Rahmenränder zurückgeben, sonst hängt das Fenster ~8px über jeden
        // Monitorrand (Inhalt am Rand + Taskleiste verdeckt).
        if (w->isMaximized()) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(m->lParam);
            int fx = ::GetSystemMetrics(SM_CXSIZEFRAME) +
                     ::GetSystemMetrics(SM_CXPADDEDBORDER);
            int fy = ::GetSystemMetrics(SM_CYSIZEFRAME) +
                     ::GetSystemMetrics(SM_CXPADDEDBORDER);
            p->rgrc[0].left += fx; p->rgrc[0].right  -= fx;
            p->rgrc[0].top  += fy; p->rgrc[0].bottom -= fy;
        }
        *result = 0;
        return true;
    }
    if (m->message != WM_NCHITTEST) return false;
    const qreal dpr = w->devicePixelRatioF();
    const QPoint pt = w->mapFromGlobal(
        QPoint(int(GET_X_LPARAM(m->lParam) / dpr),
               int(GET_Y_LPARAM(m->lParam) / dpr)));
    const QRect r = w->rect();
    const int b = 8, c = 14;     // Kanten 8px, Ecken 14px Greifzone
    const bool L = pt.x() < b, R = pt.x() >= r.width() - b;
    const bool T = pt.y() < b, B = pt.y() >= r.height() - b;
    const bool Lc = pt.x() < c, Rc = pt.x() >= r.width() - c;
    const bool Tc = pt.y() < c, Bc = pt.y() >= r.height() - c;
    if (!w->isMaximized()) {
        if (Tc && Lc) { *result = HTTOPLEFT;     return true; }
        if (Tc && Rc) { *result = HTTOPRIGHT;    return true; }
        if (Bc && Lc) { *result = HTBOTTOMLEFT;  return true; }
        if (Bc && Rc) { *result = HTBOTTOMRIGHT; return true; }
        if (L) { *result = HTLEFT;   return true; }
        if (R) { *result = HTRIGHT;  return true; }
        if (T) { *result = HTTOP;    return true; }
        if (B) { *result = HTBOTTOM; return true; }
    }
    if (titleBar && pt.y() >= 0 && pt.y() < titleBar->height() &&
        pt.x() >= 0 && pt.x() < r.width()) {
        if (!qobject_cast<QPushButton*>(w->childAt(pt))) {
            *result = HTCAPTION; return true;
        }
    }
    return false;
}
#endif  // Q_OS_WIN

// Farbcodierung der Laufwerke im Multi-Fenster: feste, gedeckte Palette,
// fürs Dunkel-Theme (#262b33) validiert (Kontrast ≥3:1, CVD-Abstand ok).
// Zuordnung strikt nach Panel-Index — nie umsortieren, die Reihenfolge
// ist Teil der Farbenblind-Sicherheit. Identität hängt nie an Farbe allein
// (Laufwerks-Tag steht immer daneben).
static QColor driveAccent(int idx) {
    static const QColor pal[] = {
        QColor(0x39, 0x87, 0xe5),   // Blau
        QColor(0x19, 0x9e, 0x70),   // Grün
        QColor(0xc9, 0x85, 0x00),   // Amber
        QColor(0x90, 0x85, 0xe9),   // Violett
        QColor(0xe6, 0x67, 0x67),   // Rot
        QColor(0xd5, 0x51, 0x81),   // Magenta
    };
    return pal[idx % 6];
}

// Ambilight-Glow aus den Randfarben des Covers — wie die Hinterleuchtung
// eines Fernsehers: oben blauer Himmel → blauer Schein oben, unten rote
// Jacke → roter Schein unten. Vorher lag hier ein einfarbiger
// QGraphicsDropShadowEffect in der „dominanten" Cover-Farbe, der das Bild
// zwangsläufig auf einen Ton reduzierte.
//
// Trick: das Cover auf 12x12 runterrechnen und weich auf Cover+Rand
// hochziehen. Beim Hochskalieren setzen genau die Randpixel ihre Farbe nach
// außen fort — der Glow bildet den Bildrand also 1:1 nach, ohne dass man
// einzelne Kantenpixel abklappern muss. Nach außen wird das Alpha über vier
// Verläufe weich auf 0 gezogen (an den Ecken multiplizieren sie sich zum
// runden Abfall), am Ende dämpft ein globaler Alpha-Faktor das Ganze.
// `alpha` steuert die Gesamtdeckkraft — darüber „atmet" der Glow (mehrere
// vorgerenderte Stufen statt eines pro Frame neu berechneten Blurs).
static QPixmap coverAmbilight(const QPixmap& src, const QSize& inner,
                              int pad, int alpha) {
    if (src.isNull() || pad <= 0 || inner.isEmpty()) return {};
    const int W = inner.width() + pad * 2, H = inner.height() + pad * 2;
    // Farbquelle: 10x10-Miniatur. Auf diesen 100 Pixeln ist ein kräftiger
    // Sättigungs-Schub praktisch gratis und macht aus blassen Bildrändern
    // einen sichtbaren Schein — hochskaliert interpoliert Qt ohnehin alles.
    QImage mini = src.scaled(10, 10, Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation).toImage()
                     .convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < mini.height(); ++y)
        for (int x = 0; x < mini.width(); ++x) {
            QColor c = mini.pixelColor(x, y);
            int hh, ss, vv; c.getHsv(&hh, &ss, &vv);
            if (hh < 0) hh = 0;
            c.setHsv(hh, std::min(255, (int)(ss * 1.45)),
                         std::min(255, std::max(vv, 110) + 25));
            mini.setPixelColor(x, y, c);
        }
    QImage up = mini.scaled(W, H, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_ARGB32);
    // Hüllkurve: Abstand zum Cover-Rechteck, quadratisch ausgeblendet. Vier
    // lineare Kanten-Verläufe (die frühere Fassung) ergaben eine erkennbar
    // rechteckige Aura mit harten Ecken; so strahlt es rundum gleichmäßig aus
    // und läuft nach außen sauber gegen null.
    const int ix0 = pad, iy0 = pad;
    const int ix1 = pad + inner.width(), iy1 = pad + inner.height();
    QImage out(W, H, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const double A = std::clamp(alpha, 0, 255) / 255.0;
    for (int y = 0; y < H; ++y) {
        const auto* srcLine = reinterpret_cast<const QRgb*>(up.constScanLine(y));
        auto* dstLine = reinterpret_cast<QRgb*>(out.scanLine(y));
        const double dy = y < iy0 ? (iy0 - y) : (y > iy1 ? y - iy1 : 0);
        for (int x = 0; x < W; ++x) {
            const double dx = x < ix0 ? (ix0 - x) : (x > ix1 ? x - ix1 : 0);
            const double d  = std::sqrt(dx * dx + dy * dy) / pad;   // 0…>1
            if (d >= 1.0) { dstLine[x] = 0; continue; }
            // Zwei überlagerte Hüllkurven: ein weit tragender, weicher Schein
            // (flacher Exponent) plus ein enger Lichtsaum direkt am Bildrand
            // (steiler Exponent). Eine einzelne quadratische Kurve sah
            // entweder nach schmalem Rand oder nach Schmiere aus.
            const double e = 1.0 - d;
            const double f = 0.55 * std::pow(e, 1.5) + 0.45 * std::pow(e, 4.0);
            const int a = (int)std::lround(255.0 * A * f);
            if (a <= 0) { dstLine[x] = 0; continue; }
            const QRgb s = srcLine[x];
            dstLine[x] = qRgba(qRed(s) * a / 255, qGreen(s) * a / 255,
                               qBlue(s) * a / 255, a);
        }
    }
    return QPixmap::fromImage(out);
}

// „Bling"-Cover: das Album leicht schräg (Pseudo-3D, wie aufgeklappt) mit
// einer ausgefadeten Spiegelung darunter rendern. Gibt ein transparentes
// Pixmap der Gesamtgröße (Cover + Neigung + Spiegelung + Glow-Rand) zurück.
// glowPad>0 legt den Ambilight-Glow (s. coverAmbilight) unter das Cover.
static QPixmap makeCoverArt(const QPixmap& src, int side,
                            int glowPad = 0, int glowAlpha = 150) {
    if (src.isNull()) return {};
    QPixmap cover = src.scaled(side, side, Qt::KeepAspectRatio,
                               Qt::SmoothTransformation);
    const int w = cover.width(), h = cover.height();
    const int reflH = h / 3;            // Spiegelungshöhe
    const int pad   = 10;               // Rand für Neigung/Schatten
    const int gp    = std::max(0, glowPad);
    QPixmap out(w + pad * 2 + gp * 2, h + reflH + pad + gp * 2);
    out.fill(Qt::transparent);
    QPainter pt(&out);
    pt.setRenderHint(QPainter::SmoothPixmapTransform, true);
    pt.setRenderHint(QPainter::Antialiasing, true);
    // Pseudo-3D: ganz dezenter vertikaler Shear → das Cover „steht" leicht
    // schräg, als wäre es aufgeklappt. Bewusst subtil, damit es lesbar bleibt.
    QTransform tf;
    tf.translate(pad + gp, gp);
    tf.shear(0.0, -0.05);
    tf.translate(0, h * 0.05);
    pt.setTransform(tf);
    // Ambilight UNTER dem Cover und mit derselben Neigung — sonst sitzt der
    // Schein nicht deckungsgleich zum gescherten Bild (links breiter Saum,
    // rechts kaum einer).
    if (gp > 0) {
        QPixmap amb = coverAmbilight(cover, QSize(w, h), gp, glowAlpha);
        if (!amb.isNull()) pt.drawPixmap(-gp, -gp, amb);
    }
    pt.drawPixmap(0, 0, cover);
    // Spiegelung: vertikal gespiegeltes Cover mit Alpha-Verlauf (oben ~35 %
    // → unten transparent).
    QImage mir = cover.toImage().mirrored(false, true);
    QPixmap refl = QPixmap::fromImage(mir).copy(0, 0, w, reflH);
    QPixmap reflFaded(w, reflH);
    reflFaded.fill(Qt::transparent);
    {
        QPainter rp(&reflFaded);
        rp.drawPixmap(0, 0, refl);
        rp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient g(0, 0, 0, reflH);
        g.setColorAt(0.0, QColor(0, 0, 0, 90));
        g.setColorAt(1.0, QColor(0, 0, 0, 0));
        rp.fillRect(0, 0, w, reflH, g);
    }
    pt.drawPixmap(0, h + 3, reflFaded);
    pt.end();
    return out;
}

// Dominante, „lebendige" Farbe eines Covers — für Glow/Akzent, der zum Bild
// passt. Bei fast farblosen (s/w-)Covern → Fallback auf die Laufwerksfarbe.
//
// Wichtig ist die FLÄCHE, nicht der knalligste Einzelpixel: Die frühere
// Fassung nahm schlicht das Pixel mit dem besten Sättigung×Helligkeit-Wert.
// Damit gewann auf „Images — Jean Michel Jarre" (halbes Bild hellblauer
// Himmel) die kleine rote Jacke, und praktisch jedes Cover mit irgendeinem
// roten Detail bekam einen roten Glow.
//
// Jetzt: Farbtöne in 24 Sektoren à 15° einsortieren, jeder Pixel mit seiner
// Sättigung gewichtet (große blasse Flächen schlagen kleine knallige, aber
// ein grauer Hintergrund bleibt chancenlos). Gewertet wird über ein Fenster
// aus drei benachbarten Sektoren, damit ein Farbton, der genau auf einer
// Sektorgrenze liegt, sich nicht selbst halbiert — Rot um 0°/360° wird dabei
// zyklisch korrekt gemittelt (Vektorsumme statt arithmetischem Mittel).
static QColor coverAccentColor(const QPixmap& src, const QColor& fallback) {
    if (src.isNull()) return fallback;
    QImage img = src.scaled(48, 48, Qt::IgnoreAspectRatio,
                            Qt::SmoothTransformation).toImage();
    constexpr int kBuckets = 24;
    double wsum[kBuckets] = {0}, xs[kBuckets] = {0}, ys[kBuckets] = {0};
    double ss[kBuckets] = {0}, vs[kBuckets] = {0};
    double colored = 0, total = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            QColor c = img.pixelColor(x, y);
            int h, s, v; c.getHsv(&h, &s, &v);
            total += 1.0;
            // Zu dunkel, ausgebrannt oder praktisch grau → trägt keine
            // Farbinformation (zählt aber beim S/W-Test unten mit).
            if (h < 0 || s < 40 || v < 40 || v > 245) continue;
            colored += 1.0;
            const double w  = s / 255.0;              // Flächenanteil × Sättigung
            const double rad = h * M_PI / 180.0;
            const int b = (h / (360 / kBuckets)) % kBuckets;
            wsum[b] += w;
            xs[b]   += w * std::cos(rad);
            ys[b]   += w * std::sin(rad);
            ss[b]   += w * s;
            vs[b]   += w * v;
        }
    // Weniger als ~12 % farbige Fläche → S/W-Cover, Laufwerksfarbe behalten.
    if (total <= 0 || colored / total < 0.12) return fallback;
    int bestB = -1; double bestW = 0;
    for (int b = 0; b < kBuckets; ++b) {
        const int l = (b + kBuckets - 1) % kBuckets, r = (b + 1) % kBuckets;
        const double win = wsum[l] + wsum[b] + wsum[r];
        if (win > bestW) { bestW = win; bestB = b; }
    }
    if (bestB < 0 || bestW <= 0) return fallback;
    const int l = (bestB + kBuckets - 1) % kBuckets, r = (bestB + 1) % kBuckets;
    const double X = xs[l] + xs[bestB] + xs[r];
    const double Y = ys[l] + ys[bestB] + ys[r];
    const double W = wsum[l] + wsum[bestB] + wsum[r];
    double hue = std::atan2(Y, X) * 180.0 / M_PI;
    if (hue < 0) hue += 360.0;
    const int s = (int)((ss[l] + ss[bestB] + ss[r]) / W);
    const int v = (int)((vs[l] + vs[bestB] + vs[r]) / W);
    QColor best;
    best.setHsv(((int)std::lround(hue)) % 360,
                std::min(255, s + 45),           // fürs Leuchten aufsatten
                std::min(255, std::max(v, 150) + 30));
    return best;
}

// Cover-Rendering als PNG herausschreiben (s. gui.h). Nutzt exakt denselben
// Weg wie die Disc-Karte, damit das Bild dem entspricht, was man später in
// der App sieht. Ohne pad/alpha gelten die Werte, die auch die Karte nutzt.
bool render_cover_preview(const QString& inPath, const QString& outPath,
                          int side, int pad, int alpha) {
    QPixmap src(inPath);
    if (src.isNull()) return false;
    if (pad   < 0) pad   = 38;
    if (alpha < 0) alpha = 115;
    QPixmap art = makeCoverArt(src, side, pad, alpha);
    if (art.isNull()) return false;
    // Auf den Karten-Hintergrund legen — der Glow wird gegen #262b33
    // geblendet, alles andere wäre irreführend.
    QPixmap out(art.width() + 40, art.height() + 40);
    out.fill(QColor(0x26, 0x2b, 0x33));
    QPainter p(&out);
    p.drawPixmap(20, 20, art);
    p.end();
    return out.save(outPath, "PNG");
}

// Additiv & eigenständig: Single-Drive-MainWindow bleibt unberührt. Eine
// Spalte je Laufwerk (Cover+Album oben, Live-Disc darunter), eine gemeinsame
// Rip-Tabelle unten. Je Spalte ein eigener Controller (= eigene Pipeline +
// Worker, wie der CLI-Parallelpfad seit 1.4.0). Kein Q_OBJECT nötig: nur
// Functor-Connects mit Kontext-Objekt → Qt trennt bei Zerstörung selbst.
class DrivePanel : public QWidget {
public:
    DrivePanel(const cdr::Config& base, const std::string& dev,
               const QColor& accent, QWidget* parent = nullptr)
        : QWidget(parent), cfg_(base), dev_(dev), accent_(accent) {
        cfg_.device = dev;            // Kind-Pipeline = Single-Device
        cfg_.devices.clear();
        tag_ = QString::fromStdString(dev);
        int sl = tag_.lastIndexOf('/');
        if (sl >= 0) tag_ = tag_.mid(sl + 1);
        std::string did;
        try { did = cdr::drive_id(dev); } catch (...) {}
        // Karten-Hintergrund (#262b33) → Live-Disc (malt selbst #262b33)
        // blendet sich nahtlos ein statt heller als die Box zu wirken.
        setObjectName("drivePanel");
        setAttribute(Qt::WA_StyledBackground, true);
        // Feste Kartenbreite. Vorher richtete sich die Breite nach dem
        // breitesten Kind — sobald in der Statuspille „lege nächste CD ein"
        // stand, wuchs die Karte und schob die Nachbarkarten zur Seite.
        // Maß gibt das Cover-Feld vor (Cover + Neigung + Ambilight) plus die
        // Layout-Ränder; alle variablen Texte bekommen unten feste Größen.
        setFixedWidth(kCardW);
        // Akzentfarbe des Laufwerks als oberer Kartenrand (Basis-QSS aus
        // main.cpp wird hier pro Instanz um den Border ergänzt).
        setStyleSheet(QString(
            "QWidget#drivePanel { background:#262b33; border-radius:12px;"
            " border:2px solid %1; }").arg(accent_.name()));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 10, 10, 10);
        // Kopf: Farbpunkt + Laufwerkspfad + Laufwerks-ID (Hersteller/Modell).
        auto* hd = new QLabel(
            "<span style='color:" + accent_.name() + ";'>●</span> <b>" +
            QString::fromStdString(dev) + "</b>" +
            (did.empty() ? QString() :
             "<br><span style='color:#9aa0aa;font-size:8pt;'>" +
             QString::fromStdString(did).toHtmlEscaped() + "</span>"));
        hd->setAlignment(Qt::AlignHCenter);
        hd->setTextFormat(Qt::RichText);
        // Kopfzeile als abgerundete, leicht durchscheinende Box — hebt
        // Laufwerk und Modell vom Karten-Hintergrund ab.
        hd->setObjectName("panelHead");
        hd->setStyleSheet(
            "QLabel#panelHead { background:rgba(18,21,27,0.55);"
            " border:1px solid rgba(255,255,255,0.08); border-radius:9px;"
            " padding:5px 8px; }");
        v->addWidget(hd);
        cover_ = new QLabel;
        // Muss das komplette Cover-Pixmap fassen, sonst schneidet QLabel den
        // Glow an der Kante hart ab (genau das passierte bis 1.9.20):
        //   Breite = Cover + Neigung 2*10 + Ambilight 2*kGlowPad
        //   Höhe   = Cover + Spiegelung (Cover/3) + 10 + Ambilight 2*kGlowPad
        cover_->setFixedSize(kCoverSide + 20 + 2 * kGlowPad,
                             kCoverSide + kCoverSide / 3 + 10 + 2 * kGlowPad);
        cover_->setAlignment(Qt::AlignCenter);
        cover_->setText("kein\nCover");
        // Cover anklickbar: Menü zum Nachlegen von Hand. Für manche Ausgaben
        // hat das Cover Art Archive schlicht kein Bild („Free the Spirit —
        // Pan from Paradise, Vol. 2"); im Multi-Fenster gab es bisher gar
        // keinen Weg, dann eins zu setzen — nur das Einzel-Fenster hatte
        // Cover-Knöpfe.
        cover_->setCursor(Qt::PointingHandCursor);
        cover_->setToolTip(QString::fromUtf8(
            "Klicken: Cover von Hand setzen oder erneut suchen."));
        cover_->installEventFilter(this);
        // Farbiger, atmender Glow rund ums Cover in der Laufwerksfarbe
        // (blurRadius wird vom Anim-Timer sanft pulsiert → „lebt").
        // Kein QGraphicsDropShadowEffect mehr am Cover: der Glow steckt jetzt
        // als Ambilight im Cover-Pixmap selbst (s. buildCoverFrames) — das
        // bildet die Randfarben nach statt alles einfarbig zu überstrahlen und
        // kostet nebenbei keine Blur-Berechnung pro Repaint.
        v->addWidget(cover_, 0, Qt::AlignHCenter);
        alb_ = new QLabel("—");
        alb_->setWordWrap(true);
        alb_->setAlignment(Qt::AlignHCenter);
        // Feste Größe: Ein zweizeiliger Albumtitel darf die Karte nicht höher
        // machen als ein einzeiliger (s. kCardW-Kommentar am Konstruktoranfang).
        // Platz für zwei Titelzeilen plus die Interpreten-Zeile — mit den
        // vorherigen 46 px wurde bei „Viva la Vida or Death and All His
        // Friends" die Zeile „Coldplay" unten abgeschnitten.
        alb_->setFixedSize(kCardW - 30, 68);
        // Dezenter Text-Glow am Album-Titel.
        { auto* ag = new QGraphicsDropShadowEffect(this);
          ag->setBlurRadius(10); ag->setColor(QColor(0,0,0,180));
          ag->setOffset(0, 1); alb_->setGraphicsEffect(ag); }
        v->addWidget(alb_, 0, Qt::AlignHCenter);
        disc_ = new DiscScanWidget;
        disc_->setFixedSize(160, 160);
        v->addWidget(disc_, 0, Qt::AlignHCenter);
        cap_ = new QLabel;
        cap_->setAlignment(Qt::AlignCenter);
        // Ebenfalls fest: Sonst zieht ein langer Statustext („lege nächste CD
        // ein …") die ganze Karte breiter als die Nachbarkarte.
        cap_->setWordWrap(true);
        cap_->setFixedSize(kCardW - 24, 42);
        // Farbiger Glow an der Status-Pille (Farbe/Stärke setzt setState).
        capGlow_ = new QGraphicsDropShadowEffect(this);
        capGlow_->setBlurRadius(18);
        capGlow_->setOffset(0, 0);
        cap_->setGraphicsEffect(capGlow_);
        v->addWidget(cap_);
        // Start pro Laufwerk: rippt genau die eingelegte CD (once=true).
        // Nur im Manuell-Modus sichtbar — mit Turbo/Dauerlauf startet ohnehin
        // „Alle starten" und jedes Laufwerk läuft danach von selbst weiter,
        // da wäre ein Einzelstart nur verwirrend. Während eines laufenden
        // Rips ist der Knopf gesperrt (Storno bleibt der Ausweg).
        startBtn_ = new QPushButton(QString::fromUtf8("▶ Rippen"));
        startBtn_->setProperty("primary", true);
        startBtn_->setToolTip(QString::fromUtf8(
            "Nur die in diesem Laufwerk liegende CD rippen."));
        startBtn_->setVisible(false);       // Turbo ist Default → versteckt
        connect(startBtn_, &QPushButton::clicked, this, [this] {
            if (ctl_->running()) return;
            plog(QString::fromUtf8("▶ Rip gestartet — nur die eingelegte CD."));
            start(true);
        });
        v->addWidget(startBtn_);
        // Storno pro Laufwerk: laufenden Rip abbrechen + CD auswerfen (z. B.
        // bei falsch erkanntem Album/Cover). Nur aktiv während ein Rip läuft.
        cancelBtn_ = new QPushButton(QString::fromUtf8("⏏ Abbrechen & Auswerfen"));
        cancelBtn_->setToolTip("Diesen Rip stoppen und die CD auswerfen.");
        cancelBtn_->setEnabled(false);
        connect(cancelBtn_, &QPushButton::clicked, this,
                [this] { stopAndEject(); });
        v->addWidget(cancelBtn_);
        setState(PanelState::Empty);
        v->addStretch(1);
        ctl_ = new Controller(this);   // Kind → dtor stoppt/joint Worker
        // Cancel-Button nur während eines laufenden Rips aktiv.
        connect(ctl_, &Controller::finished, this,
                [this] {
                    cancelBtn_->setEnabled(false);
                    startBtn_->setEnabled(true);    // nächste CD kann starten
                    if (pendingEject_) {           // Storno → jetzt auswerfen
                        pendingEject_ = false;
                        std::string d = dev_;
                        std::thread([d]{ cdr::eject_device(d); }).detach();
                        plog("Rip abgebrochen — CD wird ausgeworfen.");
                    }
                });
        connect(ctl_, &Controller::albumReady, this,
            [this](const QString& aa, const QString& at, const QString&,
                   const QStringList& ti, const QStringList&) {
                setAlbumText(at, aa);
                // Track-Titel in die Sammeltabelle: im Turbo-Dauerlauf läuft
                // die Vorschau (onTracks) nicht, sonst blieben die Titel leer
                // und würden von den AccurateRip-Meldungen ersetzt.
                if (onTracks && !ti.isEmpty()) onTracks(ti); });
        connect(ctl_, &Controller::coverReady, this,
            [this](const QString& p) { setCover(p); });
        // Neue Disc im (Dauerlauf-)Rip: Anzeige des Laufwerks für die neue CD
        // frisch machen — Cover/Album/Trackliste des vorigen Albums weg, und
        // die Sammeltabelle für dieses Laufwerk zurücksetzen (onNewDisc).
        connect(ctl_, &Controller::discIdent, this,
            [this](const QString& id, int) {
                if (id != ripId_) {
                    const bool sameAsPreview =
                        id.toStdString() == lastId_;
                    ripId_ = id;
                    // Anzeige nur leeren, wenn wirklich eine ANDERE Disc
                    // kommt. Startet der Rip auf der Disc, die die Vorschau
                    // schon geladen hat, blieben Cover und Album sonst für
                    // die ganze Preflight-Phase auf „lädt …" stehen — bei
                    // einem langsamen Laufwerk sind das mehrere Minuten, in
                    // denen die Karte aussieht, als täte sich nichts.
                    if (!sameAsPreview) {
                        clearCoverFrames();   // sonst malt der Atem-Timer das
                        cover_->setPixmap(QPixmap());   // alte Cover wieder
                        cover_->setText("lädt …");
                        alb_->setText("—");
                        if (onNewDisc) onNewDisc();
                    }
                }
                setState(PanelState::Detected); });
        connect(ctl_, &Controller::discScanInit, this,
            [this](int lo, int hi) { disc_->beginScan(lo, hi);
                // Nur die Preflight-Scan-Phase (vor dem Rip) als „Scan"
                // markieren. Beim Rip wird derselbe Init für die Live-Karte
                // gefeuert — dann NICHT auf Scan zurückschalten.
                if (state_ == PanelState::Detected ||
                    state_ == PanelState::Empty)
                    setState(PanelState::Scanning); });
        connect(ctl_, &Controller::discScanCell, this,
            // Nur zeichnen — der Zustand darf hier NICHT geändert werden,
            // sonst überschreiben die Live-Rip-Zellen den „Rip läuft"-Status
            // („scan läuft" blieb sonst während des ganzen Rips hängen).
            [this](int l, int s) { disc_->addCell(l, s); });
        connect(ctl_, &Controller::discScanCursor, this,
            [this](int l) { disc_->setCursor(l); });
        connect(ctl_, &Controller::ripProgress, this,
            [this](double f) { disc_->setRipProgress(f);
                setState(PanelState::Ripping); });
        connect(ctl_, &Controller::trackState, this,
            [this](int, int, double, const QString&) {
                setState(PanelState::Ripping); });
        connect(ctl_, &Controller::waiting, this,
            [this](const QString& m) {
                // „Werfe CD aus" / „warte auf nächste CD" → entnehmbar.
                setState(PanelState::Ejected, m); });
        connect(ctl_, &Controller::discDone, this,
            [this](bool ok, const QString&) {
                setState(ok ? PanelState::Done : PanelState::Error); });
        // Auto-Cover: solange NICHT gerippt wird, Disc beobachten und
        // Cover/Album automatisch erkennen (lean: MB + Cover, sonst
        // Platzhalter). Aktive Probe + frische Drive je Poll (D-State-
        // Gotcha), genau eine Preview-Thread pro Panel.
        auto* pt = new QTimer(this);
        connect(pt, &QTimer::timeout, this, [this] { previewTick(); });
        pt->start(3000);
        // Puls-Animation: lässt Cover- und Status-Glow sanft atmen. Bei
        // aktiven Zuständen (Scan/Rip) kräftiger, sonst ruhig.
        auto* anim = new QTimer(this);
        connect(anim, &QTimer::timeout, this, [this] {
            animPhase_ = (animPhase_ + 1) % 100000;
            const bool busy = state_ == PanelState::Ripping ||
                              state_ == PanelState::Scanning;
            const double s = std::sin(animPhase_ * 0.09);      // -1..1
            // Cover-Glow atmet über die vorgerenderten Ambilight-Stufen:
            // im Leerlauf dezent (untere Hälfte), bei Scan/Rip kräftiger.
            if (!coverFrames_.empty()) {
                const double u = s * 0.5 + 0.5;                // 0..1
                const int lo = busy ? kGlowSteps / 2 : 0;
                const int hi = busy ? kGlowSteps - 1 : kGlowSteps / 2;
                int idx = lo + (int)std::lround(u * (hi - lo));
                idx = std::clamp(idx, 0, (int)coverFrames_.size() - 1);
                if (idx != coverFrame_) {
                    coverFrame_ = idx;
                    cover_->setPixmap(coverFrames_[idx]);
                }
            }
            if (capGlow_)
                capGlow_->setBlurRadius(14 + 8 * (s * 0.5 + 0.5));
        });
        anim->start(55);
    }
    ~DrivePanel() override {
        prevStop_ = true;
        if (scanStop_) scanStop_->store(true);
        if (prevThr_.joinable())  prevThr_.join();
        if (scanThr_.joinable())  scanThr_.join();
        if (coverThr_.joinable()) coverThr_.join();
    }
    // once=false → Dauerlauf (Turbo): nach jeder CD auswerfen, auf die
    // nächste warten, weiter. once=true → nur die eingelegte CD.
    void start(bool once) {
        if (!ctl_->running()) { ctl_->start(cfg_, once);
                                cancelBtn_->setEnabled(true);
                                startBtn_->setEnabled(false); }
    }
    // Manuell-Modus (Turbo aus): Einzelstart-Knopf pro Laufwerk einblenden.
    void setManualMode(bool on) {
        startBtn_->setVisible(on);
        startBtn_->setEnabled(!ctl_->running());
    }
    void stop()  { ctl_->requestStop(); }
    bool running() const { return ctl_->running(); }
    // Greift dieses Panel gerade aufs Laufwerk zu? (Rip, Qualitäts-Scan oder
    // Cover-Vorschau) — wer das Gerät enumerieren will, wartet solange.
    bool busy() const {
        return ctl_->running() || scanBusy_.load() || prevBusy_.load();
    }
    // Storno pro Laufwerk: laufenden Rip abbrechen, danach (im finished-
    // Handler) die CD auswerfen. Ohne laufenden Rip: nur auswerfen.
    void stopAndEject() {
        if (ctl_->running()) {
            pendingEject_ = true;
            plog("Abbruch angefordert — stoppe Rip, werfe dann aus …");
            ctl_->requestStop();
            cancelBtn_->setEnabled(false);
        } else {
            std::string d = dev_;
            std::thread([d]{ cdr::eject_device(d); }).detach();
            plog("CD wird ausgeworfen.");
        }
    }
    // Neue Einstellungen aus dem Settings-Dialog übernehmen (Format, WebDAV,
    // Preflight …). Der Gerätepfad dieses Panels bleibt fix — er gehört zur
    // Spalte, nicht zur globalen Config. Greift beim nächsten Rip-Start.
    void applyBaseConfig(const cdr::Config& base) {
        std::string keep = dev_;
        cfg_ = base;
        cfg_.device = keep;
        cfg_.devices.clear();
    }
    // Standalone Disc-Qualitäts-Scan für dieses Panel (füllt den Ring live).
    // Eigener Thread + Stop-Flag, unabhängig vom Rip/Preview; pausiert die
    // Auto-Cover-Vorschau via scanBusy_ (sonst Drive-Poll-Kollision).
    void scan() {
        if (ctl_->running() || scanBusy_.load()) return;
        std::string id;
        try { id = cdr::probe_disc_id(dev_); } catch (...) { id.clear(); }
        if (id.empty()) { setState(PanelState::Empty, "keine Disc zum Scannen");
                          if (onLog) onLog("keine Disc zum Scannen"); return; }
        scanBusy_ = true;
        setState(PanelState::Scanning);
        if (onLog) onLog("Disc-Qualitäts-Scan gestartet …");
        std::string dev = dev_;
        int dens = cfg_.scan_density;
        if (scanStop_) scanStop_->store(true);
        scanStop_ = std::make_shared<std::atomic<bool>>(false);
        auto stopF = scanStop_;
        if (scanThr_.joinable()) scanThr_.join();
        scanThr_ = std::thread([this, dev, dens, stopF] {
            auto errCells = std::make_shared<std::atomic<int>>(0);
            int leadout = 0;
            try {                                   // Disc-Geometrie VOR dem Scan
                cdr::DiscIdent di = cdr::read_disc_ident(dev);
                std::istringstream ts(di.toc); int a, b;
                if (ts >> a >> b >> leadout && leadout < 0) leadout = 0;
            } catch (...) {}
            QMetaObject::invokeMethod(this, [this, leadout] {
                if (leadout > 0) disc_->beginScan(0, leadout);
            }, Qt::QueuedConnection);
            try {
                cdr::disc_probe(dev,
                    [stopF] { return stopF->load(); },
                    [this, errCells](int lba, int st, long) {  // Live-Zelle
                        if (st == 2) errCells->fetch_add(1);
                        QMetaObject::invokeMethod(this, [this, lba, st] {
                            disc_->addCell(lba, st);
                        }, Qt::QueuedConnection);
                    },
                    dens,
                    [this](int lba) {                          // Live-Cursor
                        QMetaObject::invokeMethod(this, [this, lba] {
                            disc_->setCursor(lba);
                        }, Qt::QueuedConnection);
                    });
            } catch (...) {}
            const int errs = errCells->load();
            plog(errs == 0
                ? QString::fromUtf8("Disc-Scan fertig — fehlerfrei ✓")
                : QString::fromUtf8("Disc-Scan fertig — %1 Zelle(n) mit "
                                    "Lesefehlern").arg(errs));
            QMetaObject::invokeMethod(this, [this] {
                setState(PanelState::Detected, "Scan fertig — Disc bereit");
                scanBusy_ = false;
            }, Qt::QueuedConnection);
        });
    }
    Controller* controller() const { return ctl_; }
    QString     tag() const { return tag_; }
    std::string device() const { return dev_; }
    QColor      accent() const { return accent_; }
    // Von MultiWindow gesetzt: Trackliste aus der Preview → Sammeltabelle.
    std::function<void(const QStringList&)> onTracks;
    // Von MultiWindow gesetzt: Panel-Ereignisse (Scan/Preview) → Sammel-Log.
    // Bisher landeten dort nur Rip-Events; Scans liefen komplett stumm.
    std::function<void(const QString&)> onLog;
    // Von MultiWindow gesetzt: neue Disc erkannt → Tabellenzeilen des
    // Laufwerks zurücksetzen (Dauerlauf-Refresh).
    std::function<void()> onNewDisc;

    // Lebenszyklus einer Disc im Panel — steuert Statuspille + Rahmenglow.
    enum class PanelState { Empty, Detected, Scanning, Ripping, Done,
                            Error, Ejected };
private:
    // Farbige Statuspille + farbiger Rahmenglow je Zustand. Die
    // Laufwerks-Akzentfarbe (border-top) bleibt als Identität erhalten; der
    // Zustand kommt als getönte Pille (cap_) und farbiger Panel-Rahmen dazu.
    void setState(PanelState s, const QString& textOverride = QString()) {
        state_ = s;
        struct Vis { const char* col; const char* text; };
        Vis vis;
        switch (s) {
            case PanelState::Empty:   vis = {"#9aa0aa", "bereit — Disc einlegen"}; break;
            case PanelState::Detected:vis = {"#3987e5", "Disc erkannt"}; break;
            case PanelState::Scanning:vis = {"#c98500", "Scan läuft …"}; break;
            case PanelState::Ripping: vis = {"#199e70", "Rip läuft …"}; break;
            case PanelState::Done:    vis = {"#22c07a", "fertig ✓ — CD entnehmbar"}; break;
            case PanelState::Error:   vis = {"#e66767", "Fehler"}; break;
            case PanelState::Ejected: vis = {"#17b0a0", "ausgeworfen — CD entnehmbar"}; break;
        }
        const QString col = vis.col;
        stateColor_ = QColor(col);
        if (capGlow_) capGlow_->setColor(stateColor_);   // Pillen-Glow = Status
        cap_->setText(textOverride.isEmpty() ? QString::fromUtf8(vis.text)
                                             : textOverride);
        // Pille: getönter Hintergrund + farbiger Rand + kräftige Schrift.
        cap_->setStyleSheet(QString(
            "QLabel { color:%1; background:rgba(%2,%2,%2,0); "
            "border:1px solid %1; border-radius:9px; padding:3px 10px; "
            "font-weight:600; }").arg(col).arg(0));
        // Panel-Rahmen einheitlich in der Zustandsfarbe. Bis 1.9.20 lag hier
        // zusätzlich ein oberer Rand in der Laufwerksfarbe — beim Rippen war
        // die Karte dann oben blau und an den übrigen drei Seiten grün, was
        // schlicht nach Darstellungsfehler aussah. Die Laufwerks-Identität
        // trägt ohnehin der farbige Punkt vor dem Gerätenamen (und der
        // Swatch in der Sammeltabelle).
        setStyleSheet(QString(
            "QWidget#drivePanel { background:#262b33; border-radius:12px;"
            " border:2px solid %1; }").arg(col));
    }
    // Threadsicher ins Sammel-Log melden (Queued auf den GUI-Thread).
    void plog(const QString& m) {
        QMetaObject::invokeMethod(this, [this, m] {
            if (onLog) onLog(m);
        }, Qt::QueuedConnection);
    }
    // Klick aufs Cover-Feld → kleines Menü (s. Konstruktor).
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == cover_ && e->type() == QEvent::MouseButtonRelease) {
            showCoverMenu();
            return true;
        }
        return QWidget::eventFilter(o, e);
    }
    void showCoverMenu() {
        QMenu m(this);
        m.addAction(QString::fromUtf8("Cover aus Datei wählen …"), this,
                    [this] { pickCoverFile(); });
        m.addAction(QString::fromUtf8("Cover erneut suchen"), this,
                    [this] { refetchCover(); });
        m.exec(QCursor::pos());
    }
    // Cover von der Platte übernehmen: sofort anzeigen und — falls gerade
    // gerippt wird — an die Pipeline geben, damit es in die Dateien wandert.
    void pickCoverFile() {
        const QString f = QFileDialog::getOpenFileName(
            this, QString::fromUtf8("Cover-Bild für %1 wählen").arg(tag_),
            QString(), "Bilder (*.jpg *.jpeg *.png *.webp);;Alle Dateien (*)");
        if (f.isEmpty()) return;
        QPixmap pm(f);
        if (pm.isNull()) { plog("Bild konnte nicht gelesen werden."); return; }
        buildCoverFrames(pm);
        coverAccent_ = coverAccentColor(pm, accent_);
        ctl_->setCover(f);
        plog("Cover von Hand gesetzt: " + QFileInfo(f).fileName());
    }
    // Nochmal im Cover Art Archive nachsehen (inkl. der weiteren Ausgaben
    // desselben Albums) — z. B. wenn beim ersten Versuch das Netz klemmte.
    void refetchCover() {
        if (lastAlbum_.title.empty() && lastAlbum_.artist.empty()) {
            plog("Noch keine Metadaten — erst Disc erkennen lassen.");
            return;
        }
        if (coverBusy_.exchange(true)) return;
        plog("Suche Cover erneut …");
        cdr::Album al = lastAlbum_;
        std::string ua = cfg_.mb_useragent, tmp = cfg_.tmpdir, dev = dev_;
        if (coverThr_.joinable()) coverThr_.join();
        coverThr_ = std::thread([this, al, ua, tmp, dev] {
            QString found;
            try {
                std::string devtag = dev;
                auto slash = devtag.find_last_of('/');
                if (slash != std::string::npos) devtag.erase(0, slash + 1);
                fs::path d = fs::path(tmp) / ("mpreview-" + devtag);
                std::error_code ec; fs::create_directories(d, ec);
                fs::path out;
                if (cdr::fetch_cover_for_album(al, ua, d, out))
                    found = QString::fromStdString(out.string());
            } catch (...) {}
            QMetaObject::invokeMethod(this, [this, found] {
                if (found.isEmpty()) plog("Weiterhin kein Cover gefunden.");
                else { setCover(found); ctl_->setCover(found);
                       plog("Cover gefunden."); }
                coverBusy_ = false;
            }, Qt::QueuedConnection);
        });
    }
    // Album/Interpret ins Label — Titel auf zwei Zeilen gekürzt, Interpret
    // auf eine. Ohne das Kürzen schiebt ein sehr langer Titel die
    // Interpreten-Zeile aus dem (fest hohen) Feld heraus.
    void setAlbumText(const QString& title, const QString& artist) {
        const int w = std::max(40, alb_->width() - 6);
        const QFontMetrics fm(alb_->font());
        const QString t = fm.elidedText(title,  Qt::ElideRight, w * 2 - 12);
        const QString a = fm.elidedText(artist, Qt::ElideRight, w);
        alb_->setText("<b>" + t.toHtmlEscaped() + "</b><br>" +
                      a.toHtmlEscaped());
    }
    void setCover(const QString& p) {
        QPixmap pm(p);
        if (!pm.isNull()) {
            buildCoverFrames(pm);        // schräg + Spiegelung + Ambilight
            // Dominante Cover-Farbe weiterhin für Zeilen-Tint in der
            // Sammeltabelle und den Glow der Status-Pille.
            coverAccent_ = coverAccentColor(pm, accent_);
        }
    }
    // Ambilight-Stufen für das „Atmen" vorrendern: der Atem-Timer schaltet
    // danach nur noch zwischen fertigen Pixmaps um, statt (wie früher über
    // QGraphicsDropShadowEffect) pro Frame einen Blur zu berechnen.
    void buildCoverFrames(const QPixmap& pm) {
        coverFrames_.clear();
        coverFrame_ = -1;
        for (int i = 0; i < kGlowSteps; ++i) {
            // Deutlich zurückhaltender als in 1.9.20 (dort bis 215 ≈ 84 %
            // Deckkraft): So dicht wirkte der Ambilight nicht wie Licht,
            // sondern wie eine unscharfe Kopie des Covers, die um das Bild
            // herum ausblutet.
            // 16 statt 6 Stufen: Bei sechs sprang der Atem sichtbar im
            // halben Sekundentakt, weil zwischen zwei Deckkraft-Werten
            // mehrere hundert Millisekunden lagen. Jetzt wechselt das Bild
            // etwa alle 100 ms und wirkt als gleichmäßiges Pulsieren.
            const int a = 80 + (70 * i) / (kGlowSteps - 1);       // 80…150
            coverFrames_.push_back(makeCoverArt(pm, kCoverSide, kGlowPad, a));
        }
        if (!coverFrames_.empty()) {
            coverFrame_ = 1;
            cover_->setPixmap(coverFrames_[1]);
        }
    }
    void clearCoverFrames() { coverFrames_.clear(); coverFrame_ = -1; }
    // Alle 3 s: liegt eine (neue) Disc im Laufwerk? probe_disc_id() spricht
    // dafür das Laufwerk an — das lief bis 1.9.20 direkt im GUI-Thread und
    // ließ die Oberfläche im 3-Sekunden-Takt kurz stehen (dasselbe Muster wie
    // beim früheren Hotplug-Timer). Jetzt: Probe im Worker, Auswertung per
    // Queued-Connection zurück im GUI-Thread (onProbeResult).
    void previewTick() {
        if (ctl_->running() || prevBusy_.load() || scanBusy_.load()) return;
        prevBusy_ = true;                       // deckt Probe UND Vorschau ab
        if (prevThr_.joinable()) prevThr_.join();
        const std::string dev = dev_;
        prevThr_ = std::thread([this, dev] {
            std::string id;
            try { id = cdr::probe_disc_id(dev); } catch (...) { id.clear(); }
            QMetaObject::invokeMethod(this, [this, id] { onProbeResult(id); },
                                      Qt::QueuedConnection);
        });
    }
    // GUI-Thread: Ergebnis der Disc-Probe auswerten.
    void onProbeResult(const std::string& id) {
        if (id.empty()) {
            if (!lastId_.empty()) {                 // Disc raus → zurücksetzen
                lastId_.clear();
                cover_->setPixmap(QPixmap()); cover_->setText("kein\nCover");
                alb_->setText("—"); setState(PanelState::Empty);
                clearCoverFrames();                 // Ambilight-Glow zurück
                // Auch die Zeilen dieses Laufwerks aus der Sammeltabelle
                // nehmen — sie gehören zur ausgeworfenen Disc.
                if (onNewDisc) onNewDisc();
            }
            prevBusy_ = false;
            return;
        }
        if (id == lastId_) { prevBusy_ = false; return; }
        lastId_ = id;
        // Disc-Wechsel: die Zeilen der VORIGEN Disc aus der Sammeltabelle
        // werfen, bevor die neue Trackliste kommt. Bisher meldete nur der
        // Rip-Pfad (Controller::discIdent) einen Wechsel — beim Wechseln ohne
        // laufenden Rip (Turbo aus, Disc von Hand tauschen) blieb die alte
        // Liste stehen und die neuen Titel mischten sich darunter.
        if (onNewDisc) onNewDisc();
        setState(PanelState::Detected, "Disc erkannt — lade Cover …");
        if (onLog) onLog("Disc erkannt — lese Metadaten …");
        std::string dev = dev_, ua = cfg_.mb_useragent, tmp = cfg_.tmpdir;
        // prevBusy_ steht schon (previewTick); der Probe-Thread ist mit dem
        // invokeMethod hierher fertig und wird nur noch eingesammelt.
        if (prevThr_.joinable()) prevThr_.join();
        prevThr_ = std::thread([this, dev, ua, tmp] {
            QString at, aa, cov, src;
            QStringList tl;                          // Trackliste (Titel)
            cdr::Album meta;                         // für „Cover erneut suchen"
            try {
                cdr::DiscIdent di = cdr::read_disc_ident(dev);
                cdr::Album al; bool have = false;     // volle Kette wie Hauptfenster
                try {
                    auto c = cdr::mb_release_candidates(di.id, ua, di.toc);
                    if (!c.empty()) { al = c[0]; have = true;
                                      src = "MusicBrainz"; }
                } catch (...) {}
                if (!have) {                           // CDDB-Fallback
                    try { auto cd = cdr::cddb_lookup(di.toc, ua);
                          if (cd) { al = *cd; have = true; src = "CDDB"; }
                    } catch (...) {}
                }
                if (!have) {                           // CD-TEXT-Fallback
                    auto c = cdr::cdtext_lookup(dev, di.toc_tracks);
                    if (c) { al = *c; have = true; src = "CD-TEXT"; }
                }
                if (!have) { al = cdr::placeholder_album(di.toc_tracks);
                             src = "Platzhalter — nichts gefunden"; }
                // Cover-Fallback: echter Treffer (CD-TEXT/CDDB) ohne MB-Release-ID
                // → per Titelsuche eine Release finden, nur fürs Cover.
                if (have && al.mb_release_id.empty() &&
                    !al.artist.empty() && !al.title.empty()) {
                    try { auto hits = cdr::mb_search_releases(al.artist, al.title, ua);
                          if (!hits.empty()) al.mb_release_id = hits[0].mbid;
                    } catch (...) {}
                }
                meta = al;                          // Stand für Nachschlag
                at = QString::fromStdString(al.title);
                aa = QString::fromStdString(al.artist);
                for (const auto& trk : al.tracks)
                    tl << QString::fromStdString(trk.title);
                plog(QString::fromUtf8("Metadaten [%1]: %2 — %3 (%4 Tracks)")
                         .arg(src, aa, at).arg(tl.size()));
                try {
                    // Pro Laufwerk eigenes Verzeichnis: fetch_cover schreibt
                    // fix <dir>/cover.jpg — bei gemeinsamem Verzeichnis über-
                    // schreiben sich parallele Panel-Scans gegenseitig die
                    // Datei (vertauschte/korrupte Cover beim Multi-Scan).
                    std::string devtag = dev;
                    auto slash = devtag.find_last_of('/');
                    if (slash != std::string::npos) devtag.erase(0, slash + 1);
                    fs::path d = fs::path(tmp) / ("mpreview-" + devtag);
                    std::error_code ec; fs::create_directories(d, ec);
                    fs::path out;
                    if (cdr::fetch_cover_for_album(al, ua, d, out))
                        cov = QString::fromStdString(out.string());
                } catch (...) {}
                plog(cov.isEmpty() ? "kein Cover gefunden"
                                   : "Cover geladen");
            } catch (...) {
                plog("Disc-Vorschau fehlgeschlagen (Lesefehler?)");
            }
            QMetaObject::invokeMethod(this, [this, at, aa, cov, tl, meta] {
                lastAlbum_ = meta;          // Basis für „Cover erneut suchen"
                if (!at.isEmpty() || !aa.isEmpty())
                    setAlbumText(at, aa);
                if (!cov.isEmpty()) setCover(cov);
                // Trackliste sofort an die gemeinsame Tabelle melden
                // (erscheint pro Laufwerk noch vor dem Rip).
                if (onTracks && !tl.isEmpty()) onTracks(tl);
                setState(PanelState::Detected, "bereit — Disc erkannt");
                prevBusy_ = false;
            }, Qt::QueuedConnection);
        });
    }
    cdr::Config       cfg_;
    std::string       dev_;
    QColor            accent_;       // Laufwerks-Farbcode (driveAccent)
    QColor            coverAccent_;  // aus dem Cover gezogene Glow-Farbe
    PanelState        state_ = PanelState::Empty;
    QColor            stateColor_{0x9a, 0xa0, 0xaa};  // aktuelle Zustandsfarbe
    int               animPhase_ = 0;                 // Puls-Animation
    // Vorgerenderte Cover-Pixmaps mit Ambilight in aufsteigender Deckkraft
    // (s. buildCoverFrames) + aktuell gezeigte Stufe.
    static constexpr int kGlowSteps = 16;   // feine Atem-Stufen (s. unten)
    static constexpr int kCoverSide = 145;  // Cover-Kantenlänge in der Karte
    static constexpr int kGlowPad   = 38;   // Ausstrahlung ums Cover herum
    // Kartenbreite = Cover-Feld + Layout-Ränder (2*10). Das Cover-Feld ist
    // kCoverSide + 2*10 Neigung + 2*kGlowPad Ambilight breit.
    static constexpr int kCardW = kCoverSide + 20 + 2 * kGlowPad + 20;
    std::vector<QPixmap> coverFrames_;
    int               coverFrame_ = -1;
    QGraphicsDropShadowEffect* capGlow_ = nullptr;
    QString           ripId_;        // Disc-ID des laufenden Rips (Wechsel-Erkennung)
    QString           tag_;
    QLabel*           cover_;
    QLabel*           alb_;
    QLabel*           cap_;
    QPushButton*      startBtn_  = nullptr;         // Einzelstart (Turbo aus)
    QPushButton*      cancelBtn_ = nullptr;         // Storno pro Laufwerk
    std::atomic<bool> pendingEject_{false};         // nach Abbruch auswerfen
    DiscScanWidget*   disc_;
    Controller*       ctl_;
    std::string       lastId_;
    std::atomic<bool> prevBusy_{false};
    std::atomic<bool> prevStop_{false};
    std::thread       prevThr_;
    // Letzte erkannte Metadaten — Basis für „Cover erneut suchen".
    cdr::Album        lastAlbum_;
    std::atomic<bool> coverBusy_{false};
    std::thread       coverThr_;
    std::atomic<bool> scanBusy_{false};
    std::thread       scanThr_;
    std::shared_ptr<std::atomic<bool>> scanStop_;
};

// Fortschritts-Delegate für die %-Spalte der Multi-Rip-Tabelle: zeichnet
// einen abgerundeten Balken (Füllung = Fortschritt) mit diagonaler
// Schraffur, die – vom Animations-Timer über phase getrieben – nach rechts
// fließt. Die aktiv rippende Zeile leuchtet grün und trägt ein Sparkle.
// Werte kommen aus der Zelle: UserRole = Fortschritt 0..1, UserRole+1 =
// „rippt gerade" (bool).
class RipProgressDelegate : public QStyledItemDelegate {
public:
    int phase = 0;                       // Animations-Offset (Timer erhöht ihn)
    explicit RipProgressDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}
    void paint(QPainter* p, const QStyleOptionViewItem& opt,
               const QModelIndex& idx) const override {
        const double frac = std::clamp(idx.data(Qt::UserRole).toDouble(),
                                       0.0, 1.0);
        const bool active = idx.data(Qt::UserRole + 1).toBool();
        QRect r = opt.rect.adjusted(5, 5, -5, -5);
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const int h = r.height();
        // Track-Rille mit dezentem Tiefen-Verlauf (oben dunkler → unten
        // minimal heller, wirkt eingelassen).
        {
            QLinearGradient bg(r.topLeft(), r.bottomLeft());
            bg.setColorAt(0.0, QColor(0x15, 0x18, 0x1e));
            bg.setColorAt(1.0, QColor(0x22, 0x27, 0x30));
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(r, h / 2, h / 2);
        }
        if (frac > 0.001) {
            QRect fill(r.left(), r.top(),
                       std::max(h, int(r.width() * frac)), r.height());
            const QColor base = active ? QColor(0x22, 0xc0, 0x7a)   // grün
                                       : QColor(0x29, 0x79, 0xff);  // blau
            p->save();
            QPainterPath clip; clip.addRoundedRect(r, h / 2, h / 2);
            p->setClipPath(clip);
            // Füllung als vertikaler Verlauf (glänzt oben, satt unten).
            QLinearGradient g(fill.topLeft(), fill.bottomLeft());
            g.setColorAt(0.0, base.lighter(145));
            g.setColorAt(0.5, base);
            g.setColorAt(1.0, base.darker(120));
            p->setBrush(g); p->setPen(Qt::NoPen);
            p->drawRect(fill);
            // Fließende Diagonal-Schraffur im Füllbereich.
            p->setClipRect(fill, Qt::IntersectClip);
            p->setPen(QPen(QColor(255, 255, 255, active ? 60 : 40), 5));
            const int step = 16;
            for (int x = r.left() - h - step * 2 + (phase % step);
                 x < fill.right() + h; x += step)
                p->drawLine(x, r.bottom(), x + h, r.top());
            // Glanzlicht oben (schmaler heller Streifen).
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(255, 255, 255, 45));
            p->drawRoundedRect(QRect(fill.left() + 2, fill.top() + 2,
                                     fill.width() - 4, h / 3), h / 4, h / 4);
            // Leuchtendes Fortschritts-Ende: pulsierender Glow am Füllrand
            // (nur solange nicht voll).
            if (frac < 0.999) {
                const int gx = fill.right();
                const int puls = 80 + (int)(60 * std::abs(std::sin(phase * 0.06)));
                QRadialGradient rg(QPointF(gx, r.center().y()), h * 1.4);
                QColor glow = base.lighter(160); glow.setAlpha(puls);
                rg.setColorAt(0.0, glow);
                glow.setAlpha(0); rg.setColorAt(1.0, glow);
                p->setBrush(rg);
                p->drawRect(QRect(gx - h, r.top(), h * 2, h));
            }
            p->restore();
        }
        // Prozent-Text mit dunkler Outline (Halo) → auf jedem Balken lesbar,
        // löst die frühere Kollision Text↔Balkenfarbe.
        QString txt = QString::number(int(frac * 100)) + "%";
        if (active) txt = "✨ " + txt;
        QFont f = opt.font; f.setBold(true); p->setFont(f);
        p->setPen(QColor(0, 0, 0, 190));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy)
                    p->drawText(opt.rect.translated(dx, dy),
                                Qt::AlignCenter, txt);
        p->setPen(QColor(0xff, 0xff, 0xff));
        p->drawText(opt.rect, Qt::AlignCenter, txt);
        p->restore();
    }
};

// Animierter Hintergrund fürs Multi-Fenster: dunkle Basis, langsam wandernde
// weiche Farb-Glows und aufsteigende funkelnde Glitzer-Partikel. Liegt als
// unterster Layer hinter dem (transparent gehaltenen) Inhalt; die Panels/
// Tabelle/Log decken ihre Bereiche ab, in den Lücken schimmert der Effekt.
// Bewusst dezent gehalten — Bling ohne die Lesbarkeit zu stören.
class BackgroundFx : public QWidget {
public:
    explicit BackgroundFx(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        lower();
        for (int i = 0; i < 70; ++i) {           // deterministisch verteilt
            Star s;
            s.x = ((i * 137 + 17) % 1000) / 1000.0;
            s.y = ((i * 91 + 43) % 1000) / 1000.0;
            s.speed = 0.15 + (i % 7) * 0.06;
            s.size = 1.0 + (i % 5) * 0.5;
            s.phase = (i % 13) * 0.5;
            stars_.push_back(s);
        }
        auto* t = new QTimer(this);
        connect(t, &QTimer::timeout, this, [this]{
            // Verdecktes/minimiertes Fenster: gar nicht erst neu zeichnen.
            if (!isVisible()) return;
            tick_++;
            if (pulsing_ && ++pulseTick_ >= kPulseDur) pulsing_ = false;
            update();
        });
        t->start(kTickMs);
    }
    // Status-Puls (PS-UI-Vibe): der Hintergrund faded von seiner Farbe in die
    // Statusfarbe (grün=Erfolg, rot=Fehler) und wieder zurück — ein Herzschlag,
    // radial aus der Mitte. Retriggert einfach neu.
    void pulse(const QColor& statusColor) {
        pulseColor_ = statusColor;
        pulseTick_  = 0;
        pulsing_    = true;
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const double w = width(), h = height();
        // Aufbau in drei Schichten, jede so billig wie möglich:
        //   1. Basis-Verlauf — ändert sich nie, liegt fertig im Cache (volle
        //      Auflösung, mit Dither gegen Streifenbildung).
        //   2. Drei Farb-Glows — je EIN vorgerendertes Sprite, das pro Frame
        //      nur an eine neue Position geblittet wird. Früher wurden hier
        //      drei Radial-Verläufe über die ganze Fläche neu berechnet; das
        //      war so teuer, dass ich sie in einen Cache gelegt hatte, der nur
        //      alle vier Ticks neu entstand — wodurch die Glows sichtbar im
        //      240-ms-Takt sprangen („3 fps"). Als Sprite ist das Blitten
        //      billig genug für jeden einzelnen Frame.
        //   3. Glitzer live darüber.
        if (base_.isNull() || cacheSize_ != size()) rebuildCaches();
        p.drawPixmap(0, 0, base_);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        for (int b = 0; b < 3; ++b) {
            if (blob_[b].isNull()) continue;
            const double phb = tick_ * kBlobSpeed + b * 2.1;
            const double bx = w * (0.5 + 0.42 * std::sin(phb));
            const double by = h * (0.5 + 0.42 * std::cos(phb * 0.73));
            const double r  = blobR_;
            p.drawPixmap(QRectF(bx - r, by - r, r * 2, r * 2), blob_[b],
                         QRectF(blob_[b].rect()));
        }
        // Feines Rauschen ÜBER die fertige Farbfläche. Der Dither in Basis und
        // Wolken allein reicht nicht: Beim Überblenden der Wolken wird erneut
        // auf 8 Bit gerundet, und genau dabei entstehen die Streifen neu. Die
        // Textur ist statisch (flimmert also nicht) und wird gekachelt — ein
        // Blit pro Bild.
        if (!noise_.isNull()) p.drawTiledPixmap(rect(), noise_);
        p.setRenderHint(QPainter::Antialiasing, true);
        // Status-Puls: radialer Fade in die Statusfarbe und wieder zurück.
        // Hüllkurve sin(pi·t): 0→1→0 über kPulseDur Ticks (weicher Herzschlag).
        if (pulsing_) {
            const double t   = (double)pulseTick_ / kPulseDur;   // 0..1
            const double env = std::sin(3.14159265 * t);         // 0→1→0
            QRadialGradient pg(QPointF(w * 0.5, h * 0.5),
                               std::max(w, h) * 0.78);
            QColor c = pulseColor_;
            c.setAlpha((int)(150 * env)); pg.setColorAt(0.0, c);
            c.setAlpha((int)(45  * env)); pg.setColorAt(1.0, c);
            p.fillRect(rect(), pg);
        }
        // Aufsteigende, funkelnde Glitzer.
        p.setPen(Qt::NoPen);
        for (const auto& s : stars_) {
            double y = s.y - std::fmod(tick_ * s.speed * kStarSpeed, 1.0);
            if (y < 0) y += 1.0;
            const double px = s.x * w, py = y * h;
            const double tw = 0.5 + 0.5 * std::sin(tick_ * kTwinkle + s.phase);
            const int a = int(35 + 150 * tw);
            QColor core(210, 226, 255, a);
            QRadialGradient g(QPointF(px, py), s.size * 4);
            QColor gc = core; gc.setAlpha(a / 3); g.setColorAt(0.0, gc);
            gc.setAlpha(0);                       g.setColorAt(1.0, gc);
            p.setBrush(g);
            p.drawEllipse(QPointF(px, py), s.size * 4, s.size * 4);
            p.setBrush(core);
            p.drawEllipse(QPointF(px, py), s.size * 0.7, s.size * 0.7);
        }
    }
private:
    // Deterministisches Dither-Rauschen: ±1 pro Kanal, aus den Koordinaten
    // gehasht. Ohne das zeigt ein so flacher Verlauf über die volle
    // Fensterhöhe deutlich sichtbare Streifen — 8 Bit pro Kanal reichen für
    // knapp 30 Helligkeitsstufen über 700 Pixel schlicht nicht, jede Stufe
    // wird als Kante wahrgenommen. Das Rauschen bricht die Kanten auf, ohne
    // dass man es als Rauschen erkennt.
    // Liefert eine deterministische Pseudo-Zufallszahl in [0,1) aus den
    // Koordinaten. Damit wird beim Runden auf 8 Bit gewürfelt statt
    // gerundet: floor(exakt + zufall). Das ist echtes Dithering — der
    // vorherige Ansatz („runden, dann ±1 addieren") verschiebt die Kanten
    // nur, statt sie aufzulösen, weshalb die Streifen sichtbar blieben.
    // Nötig ist das, weil der Verlauf über die volle Fensterhöhe nur rund
    // sechs Helligkeitsstufen durchläuft: ohne Dither liegt alle ~180 Pixel
    // eine harte Kante.
    static inline double ditherNoise(int x, int y) {
        unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663);
        h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
        return (h & 0xffffu) / 65536.0;                   // [0,1)
    }
    static inline int ditherRound(double v, int x, int y) {
        return std::clamp((int)std::floor(v + ditherNoise(x, y)), 0, 255);
    }
    // Basis-Verlauf (statisch) und die drei Glow-Sprites neu erzeugen. Läuft
    // nur beim ersten Zeichnen und nach Größenänderungen, deshalb dürfen es
    // hier Pixel-Schleifen sein.
    void rebuildCaches() {
        cacheSize_ = size();
        const int W = std::max(1, width()), H = std::max(1, height());
        // ── Rausch-Kachel: je Pixel etwas Weiß ODER Schwarz mit sehr kleinem
        // Alpha, im Mittel neutral. ±5/255 genügt, um die Kanten eines
        // Verlaufs aufzubrechen, der über die ganze Höhe nur rund sechs
        // Helligkeitsstufen durchläuft — sichtbar ist das Rauschen nicht.
        if (noise_.isNull()) {
            constexpr int N = 128;
            QImage n(N, N, QImage::Format_ARGB32_Premultiplied);
            n.fill(Qt::transparent);
            for (int y = 0; y < N; ++y) {
                auto* line = reinterpret_cast<QRgb*>(n.scanLine(y));
                for (int x = 0; x < N; ++x) {
                    const double r = ditherNoise(x * 31 + 7, y * 17 + 3);
                    const int a = (int)std::lround(std::abs(r - 0.5) * 10.0);
                    if (a <= 0) { line[x] = 0; continue; }
                    line[x] = (r < 0.5) ? qRgba(0, 0, 0, a)          // dunkler
                                        : qRgba(a, a, a, a);         // heller
                }
            }
            noise_ = QPixmap::fromImage(n);
        }
        // ── Basis: vertikaler Verlauf mit Dither, volle Auflösung ──────────
        QImage img(W, H, QImage::Format_RGB32);
        const QColor top(0x1a, 0x1e, 0x27), bot(0x14, 0x17, 0x1e);
        for (int y = 0; y < H; ++y) {
            const double t = H > 1 ? (double)y / (H - 1) : 0.0;
            const double r = top.red()   + (bot.red()   - top.red())   * t;
            const double g = top.green() + (bot.green() - top.green()) * t;
            const double b = top.blue()  + (bot.blue()  - top.blue())  * t;
            auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < W; ++x)
                line[x] = qRgb(ditherRound(r, x, y),
                               ditherRound(g, x + 977, y),
                               ditherRound(b, x, y + 613));
        }
        base_ = QPixmap::fromImage(img);
        // ── Glow-Sprites: je ein weicher Radial-Verlauf mit Alpha-Dither ───
        // Quadratisch abfallende Hüllkurve (statt linear) — das wirkt wie ein
        // echtes Leuchten und hat keine sichtbare Außenkante.
        blobR_ = std::max(W, H) * 0.42;
        // Sprite möglichst in Zielgröße: Wird es beim Zeichnen stark
        // hochskaliert, glättet Qt das eingebackene Dither wieder weg und die
        // Streifen kommen zurück. 1024 deckt auch große Fenster ab.
        const int S = std::clamp((int)std::lround(blobR_), 64, 1024);
        static const QColor tint[3] = { QColor(0x39,0x87,0xe5),
                                        QColor(0x90,0x85,0xe9),
                                        QColor(0x19,0x9e,0x70) };
        for (int b = 0; b < 3; ++b) {
            QImage sp(S * 2, S * 2, QImage::Format_ARGB32_Premultiplied);
            sp.fill(Qt::transparent);
            for (int y = 0; y < S * 2; ++y) {
                auto* line = reinterpret_cast<QRgb*>(sp.scanLine(y));
                for (int x = 0; x < S * 2; ++x) {
                    const double dx = (x - S) / (double)S, dy = (y - S) / (double)S;
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d >= 1.0) { line[x] = 0; continue; }
                    const double f = (1.0 - d) * (1.0 - d);   // quadratisch aus
                    const int a = ditherRound(34.0 * f, x, y);
                    if (a <= 0) { line[x] = 0; continue; }
                    // Premultiplied: Farbanteile mit Alpha skalieren.
                    line[x] = qRgba(tint[b].red()   * a / 255,
                                    tint[b].green() * a / 255,
                                    tint[b].blue()  * a / 255, a);
                }
            }
            blob_[b] = QPixmap::fromImage(sp);
        }
    }
    struct Star { double x, y, speed, size, phase; };
    std::vector<Star> stars_;
    int tick_ = 0;
    // Zeichen-Takt und die davon abgeleiteten Bewegungs-Konstanten. 40 ms
    // (25 fps) statt der früheren 60 ms — durch die Sprite-Technik ist ein
    // Frame billig genug dafür, und die Bewegung wirkt spürbar flüssiger.
    // Die Faktoren sind gegenüber 60 ms mit 2/3 skaliert, damit Glows,
    // Glitzer und Funkeln exakt gleich schnell laufen wie vorher.
    static constexpr int    kTickMs    = 25;         // 40 Bilder/s
    static constexpr double kBlobSpeed = 0.0025;     // war 0.006 bei 60 ms
    static constexpr double kStarSpeed = 0.000667;   // war 0.0016
    static constexpr double kTwinkle   = 0.0208;     // war 0.05
    // Caches (s. rebuildCaches): Basis-Verlauf + Glow-Sprites.
    QPixmap base_;
    QPixmap noise_;          // statische Rausch-Kachel gegen Banding
    QPixmap blob_[3];
    double  blobR_ = 0;
    QSize   cacheSize_;
    // Status-Puls-Zustand
    static constexpr int kPulseDur = 72;   // ~1,8 s bei 25 ms/Tick
    bool   pulsing_   = false;
    int    pulseTick_ = 0;
    QColor pulseColor_{0x35, 0xc7, 0x59};
};

class MultiWindow : public QWidget {
public:
    explicit MultiWindow(const cdr::Config& base, QWidget* parent = nullptr)
        : QWidget(parent, Qt::Window), base_(base) {
        setObjectName("multiWin");                 // Dunkler Fenster-BG (QSS)
        setAttribute(Qt::WA_StyledBackground, true);
        setWindowTitle("CD-Ripper — Multi-Laufwerk");
        resize(1120, 780);
#ifndef Q_OS_MACOS
        setWindowFlag(Qt::FramelessWindowHint);    // randlos wie MainWindow
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
        setAttribute(Qt::WA_Hover, true);          // Kanten-Resize (s. event())
#endif
        // Äußeres Layout randlos; eigene Titelleiste oben, Inhalt im Container
        // (behält die normalen Ränder/Spacing des bisherigen Aufbaus).
        auto* outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
#ifndef Q_OS_MACOS
        titleBar_ = new FramelessTitleBar(this, "CD-Ripper — Multi-Laufwerk");
        outer->addWidget(titleBar_);
#endif
        auto* content = new QWidget;
        outer->addWidget(content, 1);
        // Animierter Glitzer-/Glow-Hintergrund als unterster Layer. content
        // wird transparent gehalten, damit der Effekt durchscheint; die
        // Panels/Tabelle/Log haben eigene Hintergründe und decken ihre
        // Bereiche ab.
        content->setObjectName("multiContent");
        // content malt KEINEN eigenen Hintergrund (kein Stylesheet → keine
        // Kaskade auf Panels/Tabelle) → der fx_-Layer dahinter scheint durch.
        // Die Panels/Tabelle behalten dadurch ihre eigenen opaken Hintergründe.
        content->setAttribute(Qt::WA_NoSystemBackground, true);
        fx_ = new BackgroundFx(content);
        fx_->setGeometry(content->rect());
        content->installEventFilter(this);         // Resize → fx_ nachziehen
        auto* root = new QVBoxLayout(content);     // restlicher Aufbau unverändert
        auto* bar  = new QHBoxLayout;
        hdr_ = new QLabel;
        bar->addWidget(hdr_);
        bar->addStretch(1);
        // Turbo = Dauerlauf: alle Laufwerke rippen nach dem ersten Start
        // kontinuierlich weiter (CD raus, nächste rein, Scan, Rip …), bis
        // „Alle stoppen". Aus = jedes Laufwerk rippt nur die eingelegte CD.
        // Für den Multi-Betrieb ist Dauerlauf der Normalfall → default an.
        turbo_ = new QCheckBox(QString::fromUtf8("🔥 Turbo (Dauerlauf)"));
        turbo_->setChecked(true);
        turbo_->setToolTip(QString::fromUtf8(
            "An: nach jeder CD automatisch auswerfen und die nächste "
            "rippen, bis gestoppt.\nAus: jedes Laufwerk rippt nur die "
            "aktuell eingelegte CD — dann hat jede Disc-Karte einen "
            "eigenen ▶-Knopf für den Einzelstart."));
        // Turbo aus → Einzelstart-Knopf auf jeder Disc-Karte einblenden.
        connect(turbo_, &QCheckBox::toggled, this, [this](bool on) {
            for (auto* p : panels_) p->setManualMode(!on);
            log_->appendPlainText(on
                ? QString::fromUtf8("🔥 Turbo an — „Alle starten\" rippt im "
                                    "Dauerlauf weiter.")
                : QString::fromUtf8("▶ Turbo aus — jede Disc-Karte hat jetzt "
                                    "einen eigenen Start-Knopf."));
        });
        auto* setB   = new QPushButton(QString::fromUtf8("⚙  Einstellungen…"));
        auto* scanB  = new QPushButton("⊙  Alle scannen");
        auto* startB = new QPushButton("▶  Alle starten");
        startB->setProperty("primary", true);
        auto* stopB  = new QPushButton("■  Alle stoppen");
        bar->addWidget(turbo_);
        bar->addWidget(setB);
        bar->addWidget(scanB);
        bar->addWidget(startB);
        bar->addWidget(stopB);
        root->addLayout(bar);
        auto* colsW = new QWidget;
        colsW_ = colsW;
        cols_ = new QHBoxLayout(colsW);
        cols_->setSpacing(10);
        cols_->addStretch(1);                      // links: zentriert die
        cols_->addStretch(1);                      // rechts:  Spaltengruppe
        // Karten-Zone durchscheinend: Scrollbereich, sein Viewport und der
        // Spalten-Container malen keinen eigenen Hintergrund, damit der
        // Glitzer-Layer auch neben und zwischen den Disc-Karten sichtbar ist
        // (vorher lag hier eine opake Fläche und der Effekt kam nur in der
        // halbtransparenten Tabelle durch). Die Karten selbst behalten ihren
        // deckenden Hintergrund — der ID-Selektor #drivePanel ist
        // spezifischer als die transparenten Regeln hier.
        colsW->setAttribute(Qt::WA_NoSystemBackground, true);
        colsW->setAutoFillBackground(false);
        colsW->installEventFilter(this);      // Höhe nachziehen (s. eventFilter)
        sc_ = new QScrollArea;
        auto* sc = sc_;
        sc->setWidget(colsW);
        sc->setWidgetResizable(true);
        sc->setFrameShape(QFrame::NoFrame);
        sc->viewport()->setAutoFillBackground(false);
        sc->viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
        sc->setStyleSheet("QScrollArea { background:transparent; border:0; }"
                          "QScrollArea > QWidget > QWidget"
                          " { background:transparent; }");
        tbl_ = new QTableWidget(0, 5);
        tbl_->setHorizontalHeaderLabels(
            { "Laufwerk", "#", "Titel", "Status", "%" });
        tbl_->verticalHeader()->setVisible(false);
        tbl_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tbl_->horizontalHeader()->setSectionResizeMode(
            2, QHeaderView::Stretch);
        // Schönere, luftigere Tabelle: halbtransparent (Glitzer schimmert
        // durch), größere Schrift, mehr Zeilenhöhe, kräftiger Header.
        tbl_->setShowGrid(false);
        tbl_->verticalHeader()->setDefaultSectionSize(30);
        { QFont tf = tbl_->font(); tf.setPointSizeF(tf.pointSizeF() + 0.5);
          tbl_->setFont(tf); }
        tbl_->setStyleSheet(
            "QTableWidget { background:rgba(24,27,34,0.62); border:0;"
            " gridline-color:transparent; selection-background-color:transparent;"
            " color:#eef1f6; }"
            "QTableWidget::item { padding:5px 8px; }"
            "QHeaderView::section { background:rgba(30,35,45,0.85);"
            " color:#aeb6c4; border:0; border-bottom:2px solid #3a4150;"
            " padding:7px 8px; font-weight:700; letter-spacing:0.4px; }"
            "QHeaderView::section:first { border-top-left-radius:10px; }"
            "QHeaderView::section:last  { border-top-right-radius:10px; }");
        // Animierter Fortschrittsbalken in der %-Spalte (wie im Hauptfenster,
        // plus fließende Schraffur). Ein Timer treibt die Diagonal-Animation.
        progressDelegate_ = new RipProgressDelegate(this);
        tbl_->setItemDelegateForColumn(4, progressDelegate_);
        tbl_->setColumnWidth(4, 150);
        auto* anim = new QTimer(this);
        connect(anim, &QTimer::timeout, this, [this] {
            progressDelegate_->phase += 2;
            // Nur die %-Spalte neu zeichnen (günstig).
            if (tbl_->rowCount() > 0)
                tbl_->viewport()->update();
        });
        anim->start(45);
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setMaximumBlockCount(4000);
        log_->setMinimumHeight(70);
        auto* lower = new QWidget;
        auto* lowerV = new QVBoxLayout(lower);
        lowerV->setContentsMargins(0, 0, 0, 0);
        lowerV->setSpacing(6);
        lowerV->addWidget(tbl_, 3);
        lowerV->addWidget(log_, 1);
        // Die Karten-Zone bekommt eine feste Höhe (aus dem Karteninhalt
        // abgeleitet, s. syncCardsHeight) und wächst beim Vergrößern des
        // Fensters nicht mit — der zusätzliche Platz gehört Tabelle und Log,
        // die ihn auch brauchen. Damit entfällt der frühere ziehbare Trenner:
        // bei fixierter Oberkante hätte sein Griff nichts mehr zu verschieben.
        root->addWidget(sc, 0);
        root->addWidget(lower, 1);
        syncCardsHeight();
        connect(scanB, &QPushButton::clicked, this,
            [this] { for (auto* p : panels_) p->scan(); });
        connect(startB, &QPushButton::clicked, this,
            [this] {
                const bool once = !turbo_->isChecked();
                log_->appendPlainText(once
                    ? QString::fromUtf8("[start] Einzeldurchlauf — je eine CD "
                        "pro Laufwerk.")
                    : QString::fromUtf8("🔥 [start] Turbo — Dauerlauf, bis "
                        "Alle stoppen gedrückt wird."));
                for (auto* p : panels_) p->start(once);
            });
        connect(stopB, &QPushButton::clicked, this,
            [this] { for (auto* p : panels_) p->stop(); });
        connect(setB, &QPushButton::clicked, this,
            [this] { openSettings(); });
        // Initiale Laufwerke + Hotplug: neu erkannte Laufwerke on-the-fly als
        // Spalte addieren (Gruppe bleibt zentriert).
        //
        // list_optical_devices() öffnet und probt JEDEN Laufwerksknoten
        // (cdio_get_devices) und kanonisiert die Pfade. Solange die Laufwerke
        // idle sind, kostet das nichts — während sie rippen, hängt so ein
        // open()/ioctl() aber gut und gern eine halbe bis ganze Sekunde im
        // Kernel. Früher lief das alle 3 s direkt im GUI-Thread: die
        // Oberfläche stand dabei jedes Mal sichtbar still (auf latitude01
        // gemessen: 39 % der Zeit im D-State, Blöcke bis 1,5 s).
        // Jetzt: Enumeration im Worker-Thread, Ergebnis per Queued-Connection
        // zurück, Intervall 10 s, und während laufender Rips gar nicht erst
        // gesucht — neue Laufwerke steckt man nicht mitten im Rip an.
        std::vector<std::string> devs = cdr::list_optical_devices();
        if (devs.empty()) devs = base.device_list();
        for (const auto& d : devs) addPanel(d);
        auto* hot = new QTimer(this);
        connect(hot, &QTimer::timeout, this, [this] {
            if (hotBusy_.load()) return;              // Vorgänger läuft noch
            if (anyRipActive()) return;               // irgendwo wird gerippt
            for (auto* p : panels_)                   // Scan/Vorschau aktiv
                if (p->busy()) return;
            hotBusy_ = true;
            if (hotThr_.joinable()) hotThr_.join();   // beendeter Vorgänger
            hotThr_ = std::thread([this] {
                std::vector<std::string> found;
                try { found = cdr::list_optical_devices(); } catch (...) {}
                QMetaObject::invokeMethod(this, [this, found] {
                    for (const auto& d : found) {
                        bool known = false;
                        for (auto* p : panels_)
                            if (p->device() == d) { known = true; break; }
                        if (!known) {
                            addPanel(d);
                            log_->appendPlainText("[hotplug] Laufwerk erkannt: " +
                                QString::fromStdString(d));
                        }
                    }
                    hotBusy_ = false;
                }, Qt::QueuedConnection);
            });
        });
        hot->start(10000);
    }
    ~MultiWindow() override {
        // Enumerations-Thread sauber einsammeln, bevor das Fenster (und damit
        // das invokeMethod-Ziel) verschwindet.
        if (hotThr_.joinable()) hotThr_.join();
    }
protected:
    // Hält den animierten Hintergrund auf Größe des content-Widgets und
    // ganz nach hinten (hinter den Inhalt).
    bool eventFilter(QObject* o, QEvent* e) override {
        // Karten-Zone nachziehen, sobald sich der Inhalt ändert. Die Höhe
        // steht beim Anlegen einer Karte noch nicht fest: Cover und Trackliste
        // treffen erst Sekunden später ein und machen die Karte höher — mit
        // einer einmal berechneten Fixhöhe wurde sie dann unten abgeschnitten.
        if (o == colsW_ && (e->type() == QEvent::LayoutRequest ||
                            e->type() == QEvent::Resize))
            syncCardsHeight();
        if (fx_ && e->type() == QEvent::Resize)
            if (auto* w = qobject_cast<QWidget*>(o)) {
                if (w != colsW_) { fx_->setGeometry(w->rect()); fx_->lower(); }
            }
        return QWidget::eventFilter(o, e);
    }
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    // Linux: Kanten-Resize fürs randlose Fenster (Windows: WM_NCHITTEST,
    // macOS: native Titelleiste — beide brauchen das hier nicht).
    bool event(QEvent* ev) override {
        if (framelessLinuxResizeEvent(this, ev)) return true;
        return QWidget::event(ev);
    }
#endif
private:
    // Log-Zeile mit Laufwerks-Farbpunkt + Tag (Farbe nur als Marker neben
    // dem Text — Text selbst bleibt in normaler Schriftfarbe lesbar).
    void appendLog(const DrivePanel* p, const QString& msg) {
        log_->appendHtml(QString("<span style='color:%1;'>●</span> "
                                 "<b>[%2]</b> %3")
            .arg(p->accent().name(), p->tag(), msg.toHtmlEscaped()));
    }
    // Feste Höhe der Karten-Zone: so hoch wie die Disc-Karten selbst, plus
    // Platz für die waagerechte Scrollleiste, sobald mehr Karten als
    // Fensterbreite da sind. Nach jedem addPanel neu bestimmt, damit auch ein
    // per Hotplug nachgemeldetes Laufwerk vollständig zu sehen ist.
    void syncCardsHeight() {
        if (!sc_ || !colsW_) return;
        // sizeHint deckt den aktuellen Inhalt ab; minimumSizeHint fängt
        // Karten ab, die (z. B. durch ein gerade geladenes Cover) mehr Platz
        // verlangen als der Hint meldet.
        int h = std::max({ 140, colsW_->sizeHint().height(),
                                colsW_->minimumSizeHint().height() });
        if (auto* hb = sc_->horizontalScrollBar())
            if (hb->isVisible()) h += hb->sizeHint().height();
        h += 6;
        if (sc_->height() == h && sc_->minimumHeight() == h) return;  // nichts zu tun
        sc_->setFixedHeight(h);
    }
    void addPanel(const std::string& dev) {
        auto* colsW = cols_->parentWidget();
        int idx = (int)panels_.size();
        auto* p = new DrivePanel(base_, dev, driveAccent(idx), colsW);
        p->setManualMode(!turbo_->isChecked());   // gilt auch für Hotplug-LW
        panels_.push_back(p);
        cols_->insertWidget(cols_->count() - 1, p);  // vor rechtem Stretch
        connect(p->controller(), &Controller::trackState, this,
            [this, idx](int t, int st, double f, const QString& m) {
                onTrack(idx, t, st, f, m); });
        connect(p->controller(), &Controller::logLine, this,
            [this, p](const QString& l) { appendLog(p, l); });
        connect(p->controller(), &Controller::discDone, this,
            [this, p](bool ok, const QString& m) {
                appendLog(p, (ok ? "[OK] " : "[FEHLER] ") + m);
                // Status-Puls im Hintergrund: grün=Erfolg, rot=Fehler.
                if (fx_) fx_->pulse(ok ? QColor(0x35, 0xc7, 0x59)
                                       : QColor(0xff, 0x45, 0x45)); });
        p->onTracks = [this, idx](const QStringList& titles) {
            fillPreviewTracks(idx, titles); };
        p->onLog = [this, p](const QString& m) { appendLog(p, m); };
        p->onNewDisc = [this, idx] { resetDriveRows(idx); };
        hdr_->setText(QString::fromUtf8(
            "<b>%1 Laufwerk(e)</b> — pro Laufwerk eine "
            "<i>unterschiedliche</i> Disc einlegen, dann 'Alle starten'.")
            .arg((int)panels_.size()));
        syncCardsHeight();          // Karten-Zone an die neue Karte anpassen
    }
    int ensureRow(int drive, int t) {
        QString key = QString::number(drive) + "-" + QString::number(t);
        auto it = rows_.find(key);
        if (it != rows_.end()) return it.value();
        int row = tbl_->rowCount();
        tbl_->insertRow(row);
        rows_.insert(key, row);
        const QColor acc = panels_[drive]->accent();
        auto* tagIt = new QTableWidgetItem(panels_[drive]->tag());
        // Farbquadrat neben dem Tag (DecorationRole malt einen Swatch) —
        // Text bleibt normal lesbar, Identität hängt nicht an Farbe allein.
        tagIt->setData(Qt::DecorationRole, acc);
        // Laufwerks-Index als UserRole am Zeilen-Ankeritem (Spalte 0)
        // hinterlegen → beim Disc-Wechsel lassen sich die Zeilen genau eines
        // Laufwerks gezielt entfernen und rows_ sauber neu indizieren.
        tagIt->setData(Qt::UserRole, drive);
        tbl_->setItem(row, 0, tagIt);
        tbl_->setItem(row, 1, new QTableWidgetItem(QString::number(t)));
        for (int c = 2; c < 5; ++c)
            tbl_->setItem(row, c, new QTableWidgetItem(""));
        // Titel-Platzhalter, falls (noch) keine Metadaten da sind — nie leer,
        // nie mit Statusmeldung verunreinigt. Der echte Titel überschreibt.
        tbl_->item(row, 2)->setText(QString("Track %1").arg(t));
        tbl_->item(row, 2)->setForeground(QColor(0x8a, 0x90, 0x9a));
        // Dezenter Zeilen-Tint in der Laufwerksfarbe (~8 % Alpha) — genug
        // zum Zuordnen, ohne die dunkle Tabelle bunt zu machen.
        QColor tint = acc; tint.setAlpha(20);
        for (int c = 0; c < 5; ++c)
            tbl_->item(row, c)->setBackground(tint);
        return row;
    }
    // Beim Disc-Wechsel eines Laufwerks (Dauerlauf): dessen Zeilen entfernen
    // und rows_ komplett neu aus der verbliebenen Tabelle aufbauen — so
    // bleiben die Zeilen der anderen (weiter rippenden) Laufwerke korrekt
    // indiziert.
    void resetDriveRows(int drive) {
        std::vector<int> del;
        for (int r = 0; r < tbl_->rowCount(); ++r) {
            auto* it0 = tbl_->item(r, 0);
            if (it0 && it0->data(Qt::UserRole).toInt() == drive)
                del.push_back(r);
        }
        for (auto it = del.rbegin(); it != del.rend(); ++it)
            tbl_->removeRow(*it);
        rows_.clear();
        for (int r = 0; r < tbl_->rowCount(); ++r) {
            int d = tbl_->item(r, 0)->data(Qt::UserRole).toInt();
            int t = tbl_->item(r, 1)->text().toInt();
            rows_.insert(QString::number(d) + "-" + QString::number(t), r);
        }
    }
    // Trackliste aus der Preview sofort einfüllen (vor dem Rip): Titel +
    // Status „erkannt". Der Rip aktualisiert später dieselben Zeilen
    // (gleiche drive-track-Keys) — Titel bleibt stehen.
    void fillPreviewTracks(int drive, const QStringList& titles) {
        for (int i = 0; i < titles.size(); ++i) {
            int row = ensureRow(drive, i + 1);
            const QString t = titles[i].trimmed();
            if (t.isEmpty()) continue;             // Platzhalter „Track N" lassen
            tbl_->item(row, 2)->setText(t);
            tbl_->item(row, 2)->setForeground(QColor(0xe8, 0xea, 0xed));  // echt
            if (tbl_->item(row, 3)->text().isEmpty())
                tbl_->item(row, 3)->setText("erkannt");
        }
    }
    void onTrack(int drive, int t, int st, double f, const QString& m) {
        int row = ensureRow(drive, t);
        tbl_->item(row, 3)->setText(QString::fromUtf8(
            cdr::state_label((cdr::TrackState)st)));
        // %-Spalte: Fortschritt + „aktiv"-Flag an den Progress-Delegate.
        // aktiv = Ripping/Ripped/Encoding/Uploading (1..4).
        const bool active = (st >= 1 && st <= 4);
        auto* pcell = tbl_->item(row, 4);
        pcell->setData(Qt::UserRole, f);
        pcell->setData(Qt::UserRole + 1, active);
        // Sparkle-Zeilenhighlight: die aktiv bearbeitete Zeile hebt sich
        // heller ab, fertige/wartende fallen auf den Laufwerks-Tint zurück.
        const QColor acc = panels_[drive]->accent();
        QColor rowbg = acc;
        rowbg.setAlpha(active ? 70 : 20);
        for (int c = 0; c < 5; ++c)
            if (auto* it = tbl_->item(row, c)) it->setBackground(rowbg);
        // Zusatz-Meldung (z. B. AccurateRip-Ergebnis) gehört NICHT in die
        // Titel-Spalte — die zeigt nur Track-Titel. Info steht im Log; hier
        // als Tooltip auf der Statuszelle, damit die Tabelle sauber bleibt.
        if (!m.isEmpty()) tbl_->item(row, 3)->setToolTip(m);
    }
    // Settings-Dialog aus dem Multi-Fenster: gemeinsame Einstellungen für
    // alle Laufwerke ändern (Format, WebDAV, Preflight, Kalibrieren). Der
    // pro-Laufwerk-Gerätepfad bleibt unberührt. Speichert ins aktive Profil.
    void openSettings() {
        if (anyRipActive()) {
            QMessageBox::information(this, "Einstellungen",
                "Während irgendwo ein Lauf aktiv ist nicht änderbar — erst "
                "alle Laufwerke stoppen (auch im Einzel-Fenster).");
            return;
        }
        std::string prof = cdr::active_profile();
        std::string path = cdr::profile_path(prof);
        SettingsDialog dlg(base_, QString::fromStdString(path), this);
        if (dlg.exec() != QDialog::Accepted) return;
        base_ = dlg.config();
        for (auto* p : panels_) p->applyBaseConfig(base_);
        std::string sprof = dlg.selectedProfile().toStdString();
        std::string spath = cdr::profile_path(sprof);
        if (cdr::save_config(base_, spath)) {
            cdr::set_active_profile(sprof);
            log_->appendPlainText(QString::fromUtf8(
                "⚙ Einstellungen gespeichert — greifen ab dem nächsten "
                "Rip-Start."));
        } else {
            log_->appendPlainText("⚠ Einstellungen konnten nicht gespeichert "
                                  "werden.");
        }
    }
    cdr::Config              base_;
    QHBoxLayout*             cols_ = nullptr;
    QLabel*                  hdr_  = nullptr;
    QCheckBox*               turbo_ = nullptr;      // Dauerlauf-Schalter
    QScrollArea*             sc_    = nullptr;      // Karten-Zone (fixe Höhe)
    QWidget*                 colsW_ = nullptr;      // Spalten-Container darin
    // Hotplug-Enumeration im Hintergrund (s. Konstruktor): ein Thread zur
    // Zeit, im Destruktor eingesammelt.
    std::thread              hotThr_;
    std::atomic<bool>        hotBusy_{false};
    BackgroundFx*            fx_ = nullptr;         // animierter Hintergrund
    RipProgressDelegate*     progressDelegate_ = nullptr;
    std::vector<DrivePanel*> panels_;
    QMap<QString, int>       rows_;
    QTableWidget*            tbl_;
    QPlainTextEdit*          log_;
    QWidget*                 titleBar_ = nullptr;   // randlose Leiste (Win/Linux)
#ifdef Q_OS_WIN
    bool                     winFrameApplied_ = false;
protected:
    bool nativeEvent(const QByteArray& et, void* message,
                     qintptr* result) override {
        if (et == "windows_generic_MSG" &&
            winFramelessEvent(this, titleBar_, message, result))
            return true;
        return false;
    }
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        if (!winFrameApplied_) { winFrameApplied_ = true; applyWinFrameless(this); }
    }
#endif
};

// (FramelessTitleBar + Win-Frameless-Helper sind weiter oben definiert — vor
//  DrivePanel/MultiWindow, damit beide Fenster sie nutzen können.)

#ifdef Q_OS_WIN
// Native Fenster-Hit-Tests: Ränder → Resize, Titelleisten-Bereich → Caption
// (Windows macht Drag + Aero-Snap + Doppelklick-Maximieren selbst).
bool MainWindow::nativeEvent(const QByteArray& et, void* message, qintptr* result) {
    if (et == "windows_generic_MSG" &&
        winFramelessEvent(this, titleBar_, message, result))
        return true;
    return false;
}
void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    // Erst nach Qt's Fenster-Setup WS_THICKFRAME setzen (sonst überschreibt Qt
    // es) → randlos UND resizebar. Genau einmal.
    if (!winFrameApplied_) { winFrameApplied_ = true; applyWinFrameless(this); }
}
#endif

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
// Linux-Gegenstück zu nativeEvent(): Kanten-Resize fürs randlose Hauptfenster.
bool MainWindow::event(QEvent* ev) {
    if (framelessLinuxResizeEvent(this, ev)) return true;
    return QMainWindow::event(ev);
}
#endif

// ─────────────────────── Metadaten-Such-Popup ─────────────────────────────
// Freitext-/Keyword-Suche in MusicBrainz, um bei falsch erkannten Discs das
// richtige Album selbst zu finden (z. B. „Phil Fuldner" statt „Bryan Adams").
// Treffer wählen → volle Metadaten (Album-Artist/-Titel/Jahr + Track-Titel)
// werden übernommen. Kein Q_OBJECT nötig (nur Functor-Connects).
class MetaSearchDialog : public QDialog {
public:
    MetaSearchDialog(const cdr::Config& cfg, const QString& seedArtist,
                     const QString& seedTitle, int discTracks = 0,
                     QWidget* parent = nullptr)
        : QDialog(parent), cfg_(cfg), discTracks_(discTracks) {
        setWindowTitle(QString::fromUtf8("Metadaten suchen — MusicBrainz"));
        resize(640, 520);
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(QString::fromUtf8(
            "Künstler, Album oder Titel als Stichworte eingeben — die "
            "passende Veröffentlichung wählen und übernehmen:")));
        auto* h = new QHBoxLayout;
        search_ = new QLineEdit;
        search_->setPlaceholderText(
            QString::fromUtf8("z. B. Phil Fuldner Everything I Do …"));
        QString seed = (seedArtist + " " + seedTitle).trimmed();
        search_->setText(seed);
        // Quelle: MusicBrainz (Standard) oder Discogs (bessere Abdeckung bei
        // Promos/Maxis/Singles; braucht Token in den Einstellungen).
        source_ = new QComboBox;
        source_->addItem("MusicBrainz");
        source_->addItem("Discogs");
        source_->addItem(QString::fromUtf8("MusicBrainz + Discogs"));
        source_->setToolTip(QString::fromUtf8(
            "MusicBrainz + Discogs: MB als Basis (Cover/Titel), fehlende "
            "Metadaten (Genres, Label, Barcode …) aus dem verlinkten "
            "Discogs-Release ergänzt."));
        auto* go = new QPushButton(QString::fromUtf8("🔍 Suchen"));
        go->setProperty("primary", true);
        h->addWidget(source_); h->addWidget(search_, 1); h->addWidget(go);
        v->addLayout(h);
        connect(source_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int){
                    // Track-Filter gilt nur für MB-basierte Quellen (0 und 2);
                    // Discogs hat in der Suche keine Titelzahl → Häkchen weg.
                    if (onlyMatch_)
                        onlyMatch_->setVisible(source_->currentIndex() != 1);
                    if (!search_->text().trimmed().isEmpty()) doSearch();
                });
        // Track-Zahl-Filter: die eingelegte Disc hat discTracks_ Titel. Bei
        // kurzen Maxis/Singles verschwindet die passende Veröffentlichung sonst
        // hinter den populären Alben — dieses Häkchen filtert MB hart auf die
        // Disc-Länge. Ohne Häkchen werden passende Längen nur nach oben sortiert.
        if (discTracks_ > 0) {
            onlyMatch_ = new QCheckBox(QString::fromUtf8(
                "Nur Veröffentlichungen mit %1 Titeln (passend zur Disc)")
                .arg(discTracks_));
            connect(onlyMatch_, &QCheckBox::toggled, this,
                    [this]{ doSearch(); });
            v->addWidget(onlyMatch_);
        }
        list_ = new QListWidget;
        v->addWidget(list_, 1);
        hint_ = new QLabel;
        hint_->setStyleSheet("color:#9aa0aa;");
        v->addWidget(hint_);
        auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                        QDialogButtonBox::Cancel);
        bb->button(QDialogButtonBox::Ok)->setText(
            QString::fromUtf8("Übernehmen"));
        v->addWidget(bb);
        connect(go, &QPushButton::clicked, this, [this]{ doSearch(); });
        connect(search_, &QLineEdit::returnPressed, this, [this]{ doSearch(); });
        connect(list_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem*){ accept(); });
        connect(bb, &QDialogButtonBox::accepted, this, [this]{ accept(); });
        connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
        if (!seed.isEmpty()) doSearch();
    }
    std::optional<cdr::Album> chosen() const { return chosen_; }
private:
    void doSearch() {
        const QString q = search_->text().trimmed();
        if (q.isEmpty()) return;
        const int idx = source_->currentIndex();
        onDiscogs_ = (idx == 1);
        combined_  = (idx == 2);
        if ((onDiscogs_ || combined_) && cfg_.discogs_token.empty()) {
            list_->clear();
            hint_->setText(QString::fromUtf8(
                "Discogs-Token fehlt — in Einstellungen → Discogs-Token "
                "hinterlegen (discogs.com/settings/developers)."));
            if (onDiscogs_) return;      // reines Discogs geht ohne Token nicht
            // Kombiniert läuft ohne Token als reines MB weiter.
        }
        list_->clear();
        hint_->setText(QString::fromUtf8("Suche läuft …"));
        QApplication::setOverrideCursor(Qt::WaitCursor);
        std::vector<cdr::ReleaseHit> hits;
        if (onDiscogs_) {
            try { hits = cdr::discogs_search(q.toStdString(),
                      cfg_.discogs_token, cfg_.mb_useragent); } catch (...) {}
        } else {
            // Freie Stichwort-Query, nach Disc-Track-Zahl sortiert; mit Häkchen
            // hart auf diese Länge gefiltert (findet kurze Maxis/Singles).
            const bool strict = onlyMatch_ && onlyMatch_->isChecked();
            try { hits = cdr::mb_search_free(q.toStdString(), discTracks_,
                      strict, cfg_.mb_useragent); } catch (...) {}
        }
        QApplication::restoreOverrideCursor();
        for (const auto& hh : hits) {
            QString label = QString::fromStdString(hh.artist) +
                QString::fromUtf8("  —  ") + QString::fromStdString(hh.title);
            if (!hh.date.empty())
                label += " (" + QString::fromStdString(hh.date) + ")";
            if (!hh.country.empty())
                label += " [" + QString::fromStdString(hh.country) + "]";
            // Discogs liefert keine Track-Zahl, dafür Format + Label/Katalognr./
            // Barcode → damit lässt sich die exakte Pressung eindeutig zuordnen.
            if (!hh.format.empty())
                label += QString::fromUtf8("  ·  ") + QString::fromStdString(hh.format);
            else if (hh.tracks > 0)
                label += QString::fromUtf8("  ·  %1 Tracks").arg(hh.tracks);
            QString cn;
            if (!hh.label.empty())  cn += QString::fromStdString(hh.label);
            if (!hh.catno.empty())  cn += (cn.isEmpty() ? "" : " ") +
                                          QString::fromStdString(hh.catno);
            if (!cn.isEmpty())
                label += QString::fromUtf8("  ·  ") + cn;
            if (!hh.barcode.empty())
                label += QString::fromUtf8("  ·  ⬛ ") +
                         QString::fromStdString(hh.barcode);
            auto* it = new QListWidgetItem(label);
            // Ganzer Datensatz als Tooltip (mehrzeilig, gut lesbar).
            {
                QString tip = QString::fromStdString(hh.artist) + " — " +
                              QString::fromStdString(hh.title);
                if (!hh.date.empty())    tip += "\nJahr: " + QString::fromStdString(hh.date);
                if (!hh.country.empty()) tip += "\nLand: " + QString::fromStdString(hh.country);
                if (!hh.format.empty())  tip += "\nFormat: " + QString::fromStdString(hh.format);
                if (!hh.label.empty())   tip += "\nLabel: " + QString::fromStdString(hh.label);
                if (!hh.catno.empty())   tip += "\nKatalognr.: " + QString::fromStdString(hh.catno);
                if (!hh.barcode.empty()) tip += "\nBarcode: " + QString::fromStdString(hh.barcode);
                if (hh.tracks > 0)       tip += "\nTitel: " + QString::number(hh.tracks);
                it->setToolTip(tip);
            }
            it->setData(Qt::UserRole, QString::fromStdString(hh.mbid));
            list_->addItem(it);
        }
        hint_->setText(hits.empty()
            ? QString::fromUtf8("Keine Treffer — Stichworte anpassen oder Quelle wechseln.")
            : QString::fromUtf8("%1 Treffer — den richtigen wählen.")
                  .arg((int)hits.size()));
    }
    void accept() override {
        auto* it = list_->currentItem();
        if (it) {
            const QString id = it->data(Qt::UserRole).toString();
            if (!id.isEmpty()) {
                QApplication::setOverrideCursor(Qt::WaitCursor);
                try {
                    if (onDiscogs_)
                        chosen_ = cdr::discogs_release_by_id(id.toStdString(),
                                      cfg_.discogs_token, cfg_.mb_useragent);
                    else if (combined_)   // MB-Anker + Discogs-Lücken
                        chosen_ = cdr::mb_release_enriched(id.toStdString(), 0,
                                      cfg_.discogs_token, cfg_.mb_useragent);
                    else
                        chosen_ = cdr::mb_release_by_id(id.toStdString(), 0,
                                      cfg_.mb_useragent);
                } catch (...) {}
                QApplication::restoreOverrideCursor();
            }
        }
        QDialog::accept();
    }
    cdr::Config cfg_;
    int         discTracks_ = 0;          // Track-Zahl der eingelegten Disc
    QComboBox* source_ = nullptr;         // MusicBrainz | Discogs | kombiniert
    bool       onDiscogs_ = false;        // reine Discogs-Trefferliste aktiv
    bool       combined_  = false;        // MB-Suche, beim Übernehmen anreichern
    QLineEdit* search_;
    QCheckBox* onlyMatch_ = nullptr;      // hart auf Disc-Länge filtern (nur MB)
    QListWidget* list_;
    QLabel* hint_;
    std::optional<cdr::Album> chosen_;
};

// ───────────────────────────── MainWindow ─────────────────────────────────────

MainWindow::MainWindow(cdr::Config cfg, bool once,
                       std::string cfgPath, QWidget* parent)
    : QMainWindow(parent), cfg_(std::move(cfg)), once_(once),
      cfgPath_(std::move(cfgPath)) {
    setWindowTitle("CD-Ripper → Navidrome");
#ifndef Q_OS_MACOS
    setWindowFlag(Qt::FramelessWindowHint);   // randlos → eigene Titelleiste (s.u.)
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
    setAttribute(Qt::WA_Hover, true);         // Kanten-Resize (s. event())
#endif
    setMinimumSize(820, 460);          // darf klein werden — Inhalt scrollt
    // Fenstergröße über Programmstarts hinweg merken — neben der config.ini
    // (persistenter /cfg-Mount; das Container-Home ist flüchtig). Erststart:
    // großzügige Größe, die alle Karten ohne Scrollen zeigt, aber NIE größer
    // als der Bildschirm (kleine Panels wie 1920×720 → passend gekappt,
    // Rest scrollt). Danach exakt der zuletzt geschlossene Stand.
    {
        QSettings ui(QString::fromStdString(cdr::config_dir()) +
                     "/gui-state.ini", QSettings::IniFormat);
        QByteArray geo = ui.value("geometry").toByteArray();
        if (!geo.isEmpty()) {
            restoreGeometry(geo);
        } else {
            QSize want(1180, 880);     // zeigt alle Karten ohne Scrollen
            if (auto* scr = QGuiApplication::primaryScreen()) {
                QRect av = scr->availableGeometry();
                want.setWidth (std::min(want.width(),  av.width()  - 40));
                want.setHeight(std::min(want.height(), av.height() - 40));
                resize(want);
                move(av.center() - QPoint(want.width()/2, want.height()/2));
            } else {
                resize(want);
            }
        }
    }

    ctl_ = new Controller(this);
    // Scan-geführter Rip: nur ein frischer Session-Scan DERSELBEN Disc
    // liefert Hänger-Tracks (sonst leer → Rip exakt wie ohne Scan).
    ctl_->deferFn_ = [this](const std::string& id) -> std::vector<int> {
        std::vector<int> d;
        if (!scanDiscId_.empty() && id == scanDiscId_)
            for (int t = 1; t < (int)scanTrackStatus_.size(); ++t)
                if (scanTrackStatus_[t] == 2) d.push_back(t);
        return d;
    };
    ctl_->statusFn_ = [this](const std::string& id) -> std::vector<int> {
        if (!scanDiscId_.empty() && id == scanDiscId_) return scanTrackStatus_;
        return {};
    };

    // KEINE klassische Menüleiste (altbacken) — alle Aktionen unter
    // einem ☰-Button in der schlanken App-Leiste.
    auto* mDatei = new QMenu("Datei", this);
    QAction* aSettings = mDatei->addAction("Einstellungen…",
        QKeySequence("Ctrl+,"), this, &MainWindow::onOpenSettings);
    mDatei->addSeparator();
    QAction* aQuit = mDatei->addAction("Beenden", QKeySequence("Ctrl+Q"),
        this, &QWidget::close);
    auto* mAktion = new QMenu("Aktion", this);
    mAktion->addAction("Start", this, &MainWindow::onStart);
    mAktion->addAction("Stop",  this, &MainWindow::onStop);
    mAktion->addAction("Disc-Qualität scannen…", this,
                       &MainWindow::onScanDisc);
    mAktion->addSeparator();
    mAktion->addAction("Titel manuell suchen…", this,
                       &MainWindow::onSearchMeta);
    mAktion->addAction("Titel per Klang erkennen (AcoustID)…", this,
                       &MainWindow::onIdentifyAcoustID);
    auto* mAnsicht = new QMenu("Ansicht", this);
    mAnsicht->addAction("Logs anzeigen…", this, &MainWindow::onShowLogs);
    mAnsicht->addAction("Sitzungs-Verlauf…", this, &MainWindow::onShowHistory);
    mAnsicht->addAction("Archiv / Zustand…", this, &MainWindow::onShowArchive);
    auto* mHilfe = new QMenu("Hilfe", this);
    mHilfe->addAction("Über CD Ripper…", this, &MainWindow::onAbout);
    addAction(aSettings); addAction(aQuit);    // Shortcuts global aktiv
    menuBar()->hide();
#ifndef Q_OS_MACOS
    // Randlose, app-gestylte Titelleiste statt der nativen (Win/Linux).
    // Die (versteckte) Menüleiste wird dadurch ersetzt; Aktionen laufen eh
    // über den ☰-Button + globale Shortcuts.
    titleBar_ = new FramelessTitleBar(this, "CD-Ripper → Navidrome");
    setMenuWidget(titleBar_);
#endif

    auto* central = new QWidget;
    // Animierter Glitzer-/Glow-Hintergrund (wie im Multi-Fenster). central
    // malt keinen eigenen Hintergrund → der fx-Layer dahinter scheint durch,
    // die Widgets darüber behalten ihre opaken Hintergründe.
    central->setAttribute(Qt::WA_NoSystemBackground, true);
    mainFx_ = new BackgroundFx(central);
    central->installEventFilter(this);          // Resize → fx nachziehen
    auto* root    = new QVBoxLayout(central);
    root->setContentsMargins(12, 8, 12, 10);
    root->setSpacing(8);

    // ── Schlanke App-Leiste (Brand links, ☰-Menü rechts) ──────────────
    {
        auto* appBar = new QWidget;
        auto* abL = new QHBoxLayout(appBar);
        abL->setContentsMargins(2, 0, 2, 2);
        auto* brand = new QLabel(QString::fromUtf8(
            "<span style='color:#2979ff;font-size:16pt;'>●</span>"
            "&nbsp;&nbsp;<span style='font-size:15pt;font-weight:600;"
            "letter-spacing:1px;color:#e8eaed;'>CD&nbsp;RIPPER</span>"));
        abL->addWidget(brand);
        abL->addStretch(1);
        auto* multiBtn = new QToolButton;
        multiBtn->setText(QString::fromUtf8("⧉  Multi-Laufwerk"));
        multiBtn->setCursor(Qt::PointingHandCursor);
        multiBtn->setToolTip("Parallel aus mehreren Laufwerken rippen "
            "(je Laufwerk eine eigene Disc)");
        connect(multiBtn, &QToolButton::clicked, this,
                [this] { showMultiWindow(); });
        abL->addWidget(multiBtn);
        auto* menuBtn = new QToolButton;
        menuBtn->setText(QString::fromUtf8("☰  Menü"));
        menuBtn->setPopupMode(QToolButton::InstantPopup);
        menuBtn->setCursor(Qt::PointingHandCursor);
        auto* big = new QMenu(this);
        big->addMenu(mDatei);  big->addMenu(mAktion);
        big->addMenu(mAnsicht); big->addMenu(mHilfe);
        menuBtn->setMenu(big);
        abL->addWidget(menuBtn);
        root->addWidget(appBar);
    }

    // Kopf: Cover + editierbare Album-Felder
    auto* head = new QHBoxLayout;
    cover_ = new QLabel;
    cover_->setFixedSize(200, 200);
    cover_->setFrameShape(QFrame::StyledPanel);
    cover_->setAlignment(Qt::AlignCenter);
    cover_->setText("kein\nCover");
    cover_->setCursor(Qt::PointingHandCursor);
    cover_->setToolTip("Tipp: draufklicken 😉");
    cover_->installEventFilter(this);            // Cover-Easter-Egg
    // Atmender Ambilight-Glow aus den Cover-Randfarben (wie im Multi-Fenster):
    // die Stufen sind vorgerendert, der Timer schaltet nur das Pixmap um.
    {
        auto* gt = new QTimer(this);
        gt->setInterval(55);
        connect(gt, &QTimer::timeout, this, [this]{
            mainAnimPhase_ = (mainAnimPhase_ + 1) % 360;
            if (coverFrames_.empty()) return;
            const double s = 0.5 + 0.5 *
                std::sin(mainAnimPhase_ * 3.14159265 / 180.0);
            int idx = (int)std::lround(s * (coverFrames_.size() - 1));
            idx = std::clamp(idx, 0, (int)coverFrames_.size() - 1);
            if (idx != coverFrame_) {
                coverFrame_ = idx;
                cover_->setPixmap(coverFrames_[idx]);
            }
        });
        gt->start();
    }
    coverBtn_ = new QPushButton("Cover: Datei…");
    coverMbBtn_ = new QPushButton("Cover: MusicBrainz…");
    auto* covBox = new QVBoxLayout;
    covBox->addWidget(cover_);
    covBox->addWidget(coverBtn_);
    covBox->addWidget(coverMbBtn_);
    auto* covW = new QWidget;
    covW->setLayout(covBox);
    head->addWidget(covW);
    connect(coverBtn_, &QPushButton::clicked, this, &MainWindow::onPickCover);
    connect(coverMbBtn_, &QPushButton::clicked,
            this, &MainWindow::onPickCoverMB);
    connect(ctl_, &Controller::coverReleaseId, this,
            [this](const QString& id) { curReleaseId_ = id; });

    auto* form = new QGridLayout;
    albArtist_ = new QLineEdit;
    albTitle_  = new QLineEdit;
    albYear_   = new QLineEdit;
    albYear_->setMaximumWidth(80);
    form->addWidget(new QLabel("Album-Artist:"), 0, 0);
    form->addWidget(albArtist_,                   0, 1);
    form->addWidget(new QLabel("Album:"),         1, 0);
    form->addWidget(albTitle_,                    1, 1);
    form->addWidget(new QLabel("Jahr:"),          2, 0);
    form->addWidget(albYear_,                     2, 1, Qt::AlignLeft);
    // Metadaten-Suche: falsch erkanntes Album per Stichwortsuche in
    // MusicBrainz korrigieren (Album-Artist/-Titel/Jahr + Track-Titel).
    auto* metaBtn = new QPushButton(QString::fromUtf8("🔍 Metadaten suchen…"));
    metaBtn->setToolTip(QString::fromUtf8(
        "Falscher Künstler/Album erkannt? Per Stichwortsuche das richtige "
        "in MusicBrainz finden und übernehmen."));
    form->addWidget(metaBtn, 3, 1, Qt::AlignLeft);
    connect(metaBtn, &QPushButton::clicked, this, [this] {
        MetaSearchDialog dlg(cfg_, albArtist_->text(), albTitle_->text(),
                             table_->rowCount(), this);
        if (dlg.exec() != QDialog::Accepted) return;
        auto alb = dlg.chosen();
        if (!alb) return;
        const QString aa = QString::fromStdString(alb->artist);
        albArtist_->setText(aa);
        albTitle_->setText(QString::fromStdString(alb->title));
        albYear_->setText(QString::fromStdString(alb->year()));
        ctl_->editAlbum(albArtist_->text(), albTitle_->text(),
                        albYear_->text());
        // 1) Künstler-Spalte ALLER Disc-Zeilen auf den Album-Artist setzen —
        //    sonst bleibt bei Track-Zahl-Mismatch der alte (falsche) Interpret
        //    stehen (Bug: „Black Veil Brides" blieb in Spalte 2).
        for (int r = 0; r < table_->rowCount(); ++r) {
            ctl_->editTrackArtist(r + 1, aa);
            if (table_->item(r, 2)) table_->item(r, 2)->setText(aa);
        }
        // 2) Titel + ggf. Per-Track-Artist aus dem gewählten Release übernehmen
        //    (nur so viele Zeilen, wie beide haben).
        const int nRows = table_->rowCount();
        const int nRel  = (int)alb->tracks.size();
        for (int i = 0; i < nRel && i < nRows; ++i) {
            QString t  = QString::fromStdString(alb->tracks[i].title);
            QString ta = QString::fromStdString(alb->tracks[i].artist);
            if (ta.isEmpty()) ta = aa;
            ctl_->editTrackTitle(i + 1, t);
            ctl_->editTrackArtist(i + 1, ta);
            if (table_->item(i, 1)) table_->item(i, 1)->setText(t);
            if (table_->item(i, 2)) table_->item(i, 2)->setText(ta);
        }
        // 3) Cover aktualisieren (best effort): MB/kombiniert über Cover Art
        //    Archive, Discogs über die Bild-URL.
        QString coverTmp = QDir::tempPath() + "/cdripper-meta-cover.jpg";
        bool gotCover = false;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        if (!alb->mb_release_id.empty()) {
            curReleaseId_ = QString::fromStdString(alb->mb_release_id);
            auto urls = cdr::caa_image_urls(alb->mb_release_id, cfg_.mb_useragent);
            if (!urls.empty())
                gotCover = cdr::fetch_url(urls[0], cfg_.mb_useragent,
                                          coverTmp.toStdString());
        }
        if (!gotCover && !alb->cover_url.empty())
            gotCover = cdr::fetch_url(alb->cover_url, cfg_.mb_useragent,
                                      coverTmp.toStdString());
        QApplication::restoreOverrideCursor();
        // Als „sticky" Override merken, damit die Korrektur den Rip-Start
        // überlebt (onAlbumReady wendet sie für dieselbe Disc erneut an).
        manualAlbum_     = *alb;
        manualDiscId_    = lastDiscId_;
        manualCoverPath_ = gotCover ? coverTmp : QString();
        if (gotCover) { ctl_->setCover(coverTmp); onCoverReady(coverTmp); }
        // 4) Track-Zahl-Mismatch offen ansagen (häufig bei Maxi/Promo vs. Disc).
        QString note = QString::fromUtf8(
            "Metadaten übernommen — greifen ab dem Rip.");
        if (nRel != nRows)
            note = QString::fromUtf8(
                "Übernommen, ABER die gewählte Veröffentlichung hat %1 Titel, "
                "die Disc %2 — bitte Titel/Künstler der übrigen Zeilen prüfen "
                "(evtl. eine passendere Version wählen).").arg(nRel).arg(nRows);
        bannerLbl_->setText(note);
    });
    auto* formW = new QWidget;
    formW->setLayout(form);
    head->addWidget(formW, 1);
    // Live-Disc-Scan: zeigt während des Rips den Zustand pro Position.
    // Caption UNTER der Disc (kein Text auf der Grafik) und Spalte oben
    // ausgerichtet → Disc-Oberkante bündig mit dem Albumcover.
    discScan_ = new DiscScanWidget;
    discScan_->setFixedSize(200, 200);   // exakt wie das Cover-Bild
    discScan_->setToolTip("Live-Disc-Scan: Ringe = Track-Position, "
        "grün ok · gelb langsam · rot Lesefehler");
    discScanCap_ = new QLabel("Disc-Scan: bereit");
    discScanCap_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    discScanCap_->setStyleSheet("color:#9aa0aa;");
    auto* scanCol = new QVBoxLayout;
    scanCol->setContentsMargins(0, 0, 0, 0);
    scanCol->addWidget(discScan_);
    scanCol->addWidget(discScanCap_);
    scanCol->addStretch(1);
    auto* scanW = new QWidget; scanW->setLayout(scanCol);
    head->addWidget(scanW, 0, Qt::AlignTop);
    connect(ctl_, &Controller::discScanInit, this,
            [this](int lo, int hi){ discScan_->beginScan(lo, hi);
                discScanCap_->setText("Disc-Scan: läuft …"); });
    connect(ctl_, &Controller::discScanCell, this,
            [this](int lba, int st){ discScan_->addCell(lba, st);
                if (st == 2) discScanCap_->setText(
                    "Disc-Scan: Lesefehler erkannt"); });
    connect(ctl_, &Controller::discScanCursor, this,
            [this](int lba){ discScan_->setCursor(lba); });
    connect(ctl_, &Controller::ripProgress, this,
            [this](double f){ discScan_->setRipProgress(f); });
    auto* discCard = new QGroupBox("DISC");
    head->setContentsMargins(2, 2, 2, 2);
    discCard->setLayout(head);
    root->addWidget(discCard);

    auto albEdit = [this] {
        ctl_->editAlbum(albArtist_->text(), albTitle_->text(), albYear_->text());
    };
    connect(albArtist_, &QLineEdit::editingFinished, this, albEdit);
    connect(albTitle_,  &QLineEdit::editingFinished, this, albEdit);
    connect(albYear_,   &QLineEdit::editingFinished, this, albEdit);

    // Steuerleiste
    auto* ctrl = new QHBoxLayout;
    ctrl->addWidget(new QLabel("Laufwerk:"));
    device_ = new QComboBox;
    device_->setMinimumWidth(260);
    device_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    device_->setToolTip("Im System erkannte optische Laufwerke "
                        "(● = CD eingelegt, ○ = leer)");
    ctrl->addWidget(device_);
    auto* refreshDrv = new QToolButton;
    refreshDrv->setText("⟳");
    refreshDrv->setToolTip("Laufwerke neu einlesen");
    refreshDrv->setCursor(Qt::PointingHandCursor);
    connect(refreshDrv, &QToolButton::clicked, this,
            &MainWindow::populateDrives);
    ctrl->addWidget(refreshDrv);
    populateDrives();
    connect(device_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int){
                QString d = device_->currentData().toString();
                if (!d.isEmpty()) cfg_.device = d.toStdString();
            });
    dryRun_  = new QCheckBox("Dry-Run (kein Upload)");
    dryRun_->setChecked(cfg_.dry_run);
    onceBox_ = new QCheckBox("Nur eine CD");
    // Standardmäßig AN: pro „Start" genau eine CD, danach Stopp — KEIN
    // Auto-Rip beim Einlegen weiterer CDs (eingelegte Discs werden nur
    // in der Vorschau gezeigt; Rip nur auf expliziten Start). AUS =
    // Dauerbetrieb: JEDE eingelegte CD wird automatisch gerippt.
    onceBox_->setChecked(true);
    onceBox_->setToolTip(QString::fromUtf8(
        "An (Standard): pro Start genau eine CD, dann Stopp — kein "
        "Auto-Rip beim Einlegen.\nAus: Dauerbetrieb — jede eingelegte "
        "CD wird automatisch gerippt."));
    ctrl->addWidget(dryRun_);
    ctrl->addWidget(onceBox_);
    ctrl->addStretch();
    settingsBtn_ = new QPushButton("Einstellungen…");
    ejectBtn_ = new QPushButton("⏏ Auswerfen");
    loadBtn_  = new QPushButton("⤵ Einziehen");
    startBtn_ = new QPushButton("Start");
    stopBtn_  = new QPushButton("Stop");
    stopBtn_->setEnabled(false);
    ctrl->addWidget(settingsBtn_);
    ctrl->addWidget(ejectBtn_);
    ctrl->addWidget(loadBtn_);
    ctrl->addWidget(startBtn_);
    ctrl->addWidget(stopBtn_);
    startBtn_->setProperty("primary", true);     // Akzent-Button
    auto* ctrlCard = new QGroupBox("STEUERUNG");
    ctrl->setContentsMargins(2, 2, 2, 2);
    ctrlCard->setLayout(ctrl);
    root->addWidget(ctrlCard);
    connect(settingsBtn_, &QPushButton::clicked,
            this, &MainWindow::onOpenSettings);
    connect(ejectBtn_, &QPushButton::clicked, this, &MainWindow::onEject);
    connect(loadBtn_,  &QPushButton::clicked, this, &MainWindow::onLoadTray);

    // Leertaste = Start/Stop
    auto* sc = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(sc, &QShortcut::activated, this,
            &MainWindow::onStartStopToggle);

    // Tray-Icon (Indikator + Schnellzugriff).
    //
    // Bewusst OHNE isSystemTrayAvailable()-Abfrage: Unter Wayland/Plasma
    // meldet Qt beim Start gern noch „kein Tray" (das Panel bzw. der
    // StatusNotifierWatcher meldet sich erst kurz danach am Bus an), und dann
    // gäbe es dauerhaft kein Icon. QSystemTrayIcon verkraftet es, wenn der
    // Watcher später kommt. Damit das in der Flatpak-Sandbox überhaupt
    // funktioniert, braucht das Manifest --talk-name=…StatusNotifierWatcher
    // und --own-name=org.kde.StatusNotifierItem-* — ohne die blieb das Icon
    // unsichtbar, egal was hier steht.
    {
        tray_ = new QSystemTrayIcon(
            style()->standardIcon(QStyle::SP_DriveCDIcon), this);
        g_notify_tray = tray_;            // notify() nutzt es plattformübergreifend
        tray_->setToolTip("CD Ripper — bereit");
        auto* tm = new QMenu(this);
        tm->addAction("Fenster zeigen", this, [this] {
            showNormal(); raise(); activateWindow(); });
        tm->addAction(QString::fromUtf8("Multi-Laufwerk-Fenster…"), this,
                      [this] { showMultiWindow(); });
        tm->addSeparator();
        tm->addAction("Start", this, &MainWindow::onStart);
        tm->addAction("Stop",  this, &MainWindow::onStop);
        tm->addSeparator();
        tm->addAction("Beenden", qApp, &QApplication::quit);
        // Notausgang: beendet den Prozess sofort, ohne auf laufende Threads
        // zu warten. Nützlich, wenn ein Rip oder ein Upload (curl-Timeout bis
        // zu 10 Minuten!) das reguläre Aufräumen aufhält.
        tm->addAction(QString::fromUtf8("Sofort beenden (erzwingen)"),
                      this, [] { std::_Exit(0); });
        tray_->setContextMenu(tm);
        connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) {
                if (r == QSystemTrayIcon::DoubleClick) {
                    showNormal(); raise(); activateWindow(); }
            });
        tray_->show();
    }

    bannerLbl_ = new QLabel("Bereit. Start drücken, dann CD einlegen.");
    bannerLbl_->setWordWrap(true);
    bannerLbl_->setStyleSheet(
        "QLabel{background:#222a36;border:1px solid #2f3a4d;"
        "border-radius:9px;padding:9px 13px;font-weight:600;"
        "color:#cfe0ff;}");                       // Status-Pille (Akzent)
    root->addWidget(bannerLbl_);

    // Track-Tabelle (Live-Vorschau)
    table_ = new QTableWidget(0, 5);
    table_->setHorizontalHeaderLabels(
        { "#", "Titel", "Künstler", "Status", "Fortschritt" });
    {
        auto* h = table_->horizontalHeader();
        h->setStretchLastSection(false);
        h->setSectionResizeMode(0, QHeaderView::Fixed);
        h->setSectionResizeMode(1, QHeaderView::Stretch);          // Titel
        h->setSectionResizeMode(2, QHeaderView::ResizeToContents); // Künstler
        h->setSectionResizeMode(3, QHeaderView::Interactive);      // Status
        h->setSectionResizeMode(4, QHeaderView::Fixed);
    }
    table_->setColumnWidth(0, 48);
    table_->setColumnWidth(3, 340);   // Status voll lesbar (AR-Hinweis etc.)
    table_->setColumnWidth(4, 190);
    // Fancy Fortschrittsbalken (wie im Multi-Fenster): custom-gemalt mit
    // Gradient, Glanzlicht, pulsierendem End-Glow, Konturschrift + Funkeln.
    // Ein Timer treibt die Diagonal-Animation. Werte liegen als Item-Daten
    // (UserRole = Fortschritt 0..1, UserRole+1 = aktiv) statt in einem
    // QProgressBar-Cell-Widget.
    progressDelegate_ = new RipProgressDelegate(this);
    table_->setItemDelegateForColumn(4, progressDelegate_);
    {
        auto* anim = new QTimer(this);
        connect(anim, &QTimer::timeout, this, [this] {
            progressDelegate_->phase += 2;
            if (table_->rowCount() > 0)
                table_->viewport()->update();   // nur %-Spalte neu zeichnen
        });
        anim->start(45);
    }
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideRight);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setFrameShape(QFrame::NoFrame);
    table_->setMinimumHeight(110);   // ~3 Zeilen Minimum, skaliert mit Fenster
    // Halbtransparente, „edle" Tabelle (der animierte Hintergrund scheint durch),
    // größere Schrift, kräftiger Header — wie im Multi-Fenster.
    table_->verticalHeader()->setDefaultSectionSize(30);
    { QFont tf = table_->font(); tf.setPointSizeF(tf.pointSizeF() + 0.5);
      table_->setFont(tf); }
    table_->setStyleSheet(
        "QTableWidget { background:rgba(24,27,34,0.62); border:0;"
        " gridline-color:transparent; selection-background-color:transparent;"
        " color:#eef1f6; }"
        "QTableWidget::item { padding:5px 8px; }"
        "QHeaderView::section { background:rgba(30,35,45,0.85);"
        " color:#aeb6c4; border:0; border-bottom:2px solid #3a4150;"
        " padding:7px 8px; font-weight:700; letter-spacing:0.4px; }"
        "QHeaderView::section:first { border-top-left-radius:10px; }"
        "QHeaderView::section:last  { border-top-right-radius:10px; }");
    {
        auto* tCard = new QGroupBox("TITEL");
        auto* tl = new QVBoxLayout(tCard);
        tl->setContentsMargins(2, 2, 2, 2);
        tl->addWidget(table_);
        root->addWidget(tCard, 1);
    }

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    logView_->setFrameShape(QFrame::NoFrame);
    logView_->setMaximumHeight(92);
    {
        auto* lCard = new QGroupBox("PROTOKOLL");
        auto* ll = new QVBoxLayout(lCard);
        ll->setContentsMargins(2, 2, 2, 2);
        ll->addWidget(logView_);
        root->addWidget(lCard);
    }

    // Alles in eine Scroll-Fläche → auf kurzen/kleinen Bildschirmen
    // scrollt der Inhalt statt abgeschnitten zu werden (skalierbar).
    auto* scroll = new QScrollArea;
    scroll->setWidget(central);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setCentralWidget(scroll);

    // Statusbar
    sbElapsed_ = new QLabel("Zeit 0:00");
    sbEta_     = new QLabel("Rest —");
    sbRip_     = new QLabel("Gerippt 0/0");
    sbUp_      = new QLabel("Hochgeladen 0/0");
    sbSpeed_   = new QLabel("R – · E – · U –");
    for (auto* l : { sbElapsed_, sbEta_, sbRip_, sbUp_, sbSpeed_ }) {
        l->setMinimumWidth(150);
        statusBar()->addWidget(l);
    }

    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(stopBtn_,  &QPushButton::clicked, this, &MainWindow::onStop);
    connect(table_, &QTableWidget::cellChanged, this, &MainWindow::onCellChanged);
    connect(ctl_, &Controller::waiting,    this, &MainWindow::onWaiting);
    connect(ctl_, &Controller::albumReady, this, &MainWindow::onAlbumReady);
    connect(ctl_, &Controller::coverReady, this, &MainWindow::onCoverReady);
    connect(ctl_, &Controller::trackState, this, &MainWindow::onTrackState);
    connect(ctl_, &Controller::progress,   this, &MainWindow::onProgress);
    connect(ctl_, &Controller::metrics,    this, &MainWindow::onMetrics);
    connect(ctl_, &Controller::logLine,    this, &MainWindow::onLog);
    connect(ctl_, &Controller::discDone,   this, &MainWindow::onDiscDone);
    connect(ctl_, &Controller::fatal,      this, &MainWindow::onFatal);
    connect(ctl_, &Controller::finished,   this, &MainWindow::onFinished);

    timer_ = new QTimer(this);
    timer_->setInterval(1000);
    connect(timer_, &QTimer::timeout, this, &MainWindow::tick);
    timer_->start();

    // Jukebox: nach dem Anzeigen automatisch starten (Default AUS via Config).
    if (cfg_.jukebox)
        QTimer::singleShot(400, this, &MainWindow::onStart);
}

// Multi-Laufwerk-Fenster öffnen bzw. nach vorn holen (Toolbar-Knopf und
// Tray-Menü nutzen denselben Weg).
void MainWindow::showMultiWindow() {
    if (!multiWin_) {
        multiWin_ = new MultiWindow(cfg_, this);
        multiWin_->setAttribute(Qt::WA_DeleteOnClose);
        connect(multiWin_, &QObject::destroyed, this,
                [this] { multiWin_ = nullptr; });
    }
    multiWin_->show();
    multiWin_->raise();
    multiWin_->activateWindow();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // Aktuelle Fenstergröße/-position persistieren (siehe Ctor: gleiche
    // Datei neben der config.ini). Nächster Start nimmt exakt diesen Stand.
    QSettings ui(QString::fromStdString(cdr::config_dir()) +
                 "/gui-state.ini", QSettings::IniFormat);
    ui.setValue("geometry", saveGeometry());
    QMainWindow::closeEvent(e);
}

MainWindow::~MainWindow() {
    // Hintergrund-Threads sauber beenden, sonst std::terminate beim Quit
    // (scanThr_/previewThr_ noch joinable) bzw. invokeMethod auf tote
    // MainWindow. Scan ist über das Stop-Flag abbrechbar; Preview läuft
    // (kurze Netz-Ops) aus.
    if (scanStop_) scanStop_->store(true);
    if (scanThr_.joinable())    scanThr_.join();
    if (previewThr_.joinable()) previewThr_.join();
    if (metaThr_.joinable())    metaThr_.join();
}

void MainWindow::setControlsRunning(bool r) {
    startBtn_->setEnabled(!r);
    stopBtn_->setEnabled(r);
    device_->setEnabled(!r);
    dryRun_->setEnabled(!r);
    onceBox_->setEnabled(!r);
}

void MainWindow::onStart() {
    if (ctl_->running()) return;
    { QString d = device_->currentData().toString();
      if (!d.isEmpty()) cfg_.device = d.toStdString(); }
    cfg_.dry_run = dryRun_->isChecked();
    if (cfg_.webdav_pass.empty() && !cfg_.dry_run) {
        QMessageBox::warning(this, "Kein Passwort",
            "Kein WebDAV-Passwort gesetzt.\nSetze webdav_pass in der Config "
            "(chmod 600) oder Env CDRIPPER_WEBDAV_PASS — oder nutze Dry-Run.");
        return;
    }
    fillingTable_ = true;
    table_->setRowCount(0);
    fillingTable_ = false;
    logView_->clear();
    lastElapsed_ = 0; lastEta_ = -1; busy_ = true;
    bannerLbl_->setText("Läuft …");
    setControlsRunning(true);
    // Bei aktiver manueller Metadaten-Wahl für diese Disc den MB-Release-Dialog
    // unterdrücken (sonst fragt die Pipeline trotz Korrektur nach).
    ctl_->setSuppressChooser(manualAlbum_.has_value() &&
                             manualDiscId_ == lastDiscId_);
    ctl_->start(cfg_, onceBox_->isChecked());
}

void MainWindow::onStop() {
    bannerLbl_->setText("Stoppe (nach aktuellem Sektor/Track) …");
    stopBtn_->setEnabled(false);
    ctl_->requestStop();
}

void MainWindow::onStartStopToggle() {
    if (ctl_->running()) onStop(); else onStart();
}

void MainWindow::onEject() {
    if (ctl_->running()) {
        QMessageBox::information(this, "Auswerfen",
            "Während ein Lauf aktiv ist nicht möglich.");
        return;
    }
    if (!cdr::eject_device(cfg_.device)) {
        bannerLbl_->setText("Auswerfen fehlgeschlagen (" +
                            QString::fromStdString(cfg_.device) + ").");
        return;
    }
    lastDiscId_.clear();
    hadDisc_ = false;
    resetDiscState();                  // sofort leeren (nicht erst in 3 s)
}

void MainWindow::onLoadTray() {
    if (!cdr::load_tray(cfg_.device))
        bannerLbl_->setText("Einziehen fehlgeschlagen (" +
                            QString::fromStdString(cfg_.device) + ").");
}

void MainWindow::onShowHistory() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Sitzungs-Verlauf");
    dlg->resize(760, 420);
    auto* t = new QTableWidget(history_.size(), 4, dlg);
    t->setHorizontalHeaderLabels(
        { "Zeit", "Album", "Status", "AccurateRip" });
    t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->verticalHeader()->setVisible(false);
    for (int i = 0; i < history_.size(); ++i)
        for (int c = 0; c < 4; ++c)
            t->setItem(i, c,
                new QTableWidgetItem(history_[i].value(c)));
    auto* close = new QPushButton("Schließen");
    connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    auto* v = new QVBoxLayout(dlg);
    v->addWidget(t, 1);
    v->addWidget(close);
    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::onCellChanged(int row, int col) {
    if (fillingTable_) return;
    QString v = table_->item(row, col) ? table_->item(row, col)->text() : QString();
    if (col == 1) ctl_->editTrackTitle(row + 1, v);
    else if (col == 2) ctl_->editTrackArtist(row + 1, v);
}

void MainWindow::onWaiting(const QString& m) {
    bannerLbl_->setText("⏏  " + m);
    if (busy_) notify("Nächste CD einlegen", m);
}

void MainWindow::onAlbumReady(const QString& aa0, const QString& at0,
                              const QString& yr0, const QStringList& ti0,
                              const QStringList& ar0) {
    QString aa = aa0, at = at0, yr = yr0;
    QStringList ti = ti0, ar = ar0;
    // Manuelle Metadaten-Wahl hat Vorrang und überlebt den Rip-Start: die
    // frische Auto-Erkennung würde die Korrektur sonst überschreiben. Gilt nur
    // für dieselbe Disc; bei anderer Disc verwerfen.
    bool manual = manualAlbum_.has_value() && manualDiscId_ == lastDiscId_;
    if (!manual && manualAlbum_.has_value() && manualDiscId_ != lastDiscId_) {
        manualAlbum_.reset(); manualDiscId_.clear(); manualCoverPath_.clear();
    }
    if (manual) {
        const cdr::Album& m = *manualAlbum_;
        aa = QString::fromStdString(m.artist);
        at = QString::fromStdString(m.title);
        yr = QString::fromStdString(m.year());
        const int nRows = ti.size();          // Disc-Track-Zahl beibehalten
        QStringList mti, mar;
        for (int i = 0; i < nRows; ++i) {
            if (i < (int)m.tracks.size()) {
                mti << QString::fromStdString(m.tracks[i].title);
                QString a = QString::fromStdString(m.tracks[i].artist);
                mar << (a.isEmpty() ? aa : a);
            } else {                          // Disc hat mehr Tracks als Release
                mti << ti.value(i);           // Auto-Titel behalten
                mar << aa;                    // Künstler = Album-Artist
            }
        }
        ti = mti; ar = mar;
    }
    fillingTable_ = true;
    albArtist_->setText(aa);
    albTitle_->setText(at);
    albYear_->setText(yr);
    table_->setRowCount(ti.size());
    for (int i = 0; i < ti.size(); ++i) {
        auto* n = new QTableWidgetItem(QString::number(i + 1));
        n->setFlags(n->flags() & ~Qt::ItemIsEditable);
        n->setTextAlignment(Qt::AlignCenter);
        table_->setItem(i, 0, n);
        table_->setItem(i, 1, new QTableWidgetItem(ti[i]));
        table_->setItem(i, 2, new QTableWidgetItem(ar.value(i)));
        auto* st = new QTableWidgetItem("wartet");
        st->setFlags(st->flags() & ~Qt::ItemIsEditable);
        st->setForeground(state_color((int)cdr::TrackState::Pending));
        table_->setItem(i, 3, st);
        // %-Spalte: Daten-Item für den RipProgressDelegate (kein Cell-Widget).
        auto* pcell = new QTableWidgetItem;
        pcell->setFlags(pcell->flags() & ~Qt::ItemIsEditable);
        pcell->setData(Qt::UserRole, 0.0);        // Fortschritt 0..1
        pcell->setData(Qt::UserRole + 1, false);  // aktiv?
        table_->setItem(i, 4, pcell);
    }
    fillingTable_ = false;
    if (manual) {
        // An die (ggf. laufende) Pipeline pushen + Cover erneut setzen, damit
        // die Auto-Erkennung nichts zurücksetzt.
        ctl_->editAlbum(aa, at, yr);
        for (int i = 0; i < ti.size(); ++i) {
            ctl_->editTrackTitle(i + 1, ti[i]);
            ctl_->editTrackArtist(i + 1, ar[i]);
        }
        if (!manualCoverPath_.isEmpty()) {
            QPixmap p(manualCoverPath_);
            if (!p.isNull()) setCoverFrames(p);
            ctl_->setCover(manualCoverPath_);
        }
        bannerLbl_->setText(QString::fromUtf8(
            "Manuelle Metadaten aktiv — überschreiben die Auto-Erkennung."));
    } else {
        bannerLbl_->setText(QString("%1 — %2 (%3), %4 Tracks")
            .arg(aa, at, yr.isEmpty() ? "—" : yr).arg(ti.size()));
    }
}

// Cover ins Label setzen und die Ambilight-Stufen dafür vorrendern. Das Label
// hat im Layout eine feste Fläche — der Glow-Rand muss also INNERHALB dieser
// Fläche liegen, das Cover selbst wird entsprechend etwas kleiner skaliert.
void MainWindow::setCoverFrames(const QPixmap& src) {
    coverFrames_.clear();
    coverFrame_ = -1;
    if (src.isNull()) return;
    constexpr int pad = 16, steps = 6;
    const QSize box = cover_->size();
    QSize inner(std::max(16, box.width() - pad * 2),
                std::max(16, box.height() - pad * 2));
    QPixmap cov = src.scaled(inner, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    for (int i = 0; i < steps; ++i) {
        const int alpha = 95 + (120 * i) / (steps - 1);        // 95…215
        QPixmap out(cov.width() + pad * 2, cov.height() + pad * 2);
        out.fill(Qt::transparent);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPixmap amb = coverAmbilight(cov, cov.size(), pad, alpha);
        if (!amb.isNull()) p.drawPixmap(0, 0, amb);
        p.drawPixmap(pad, pad, cov);
        p.end();
        coverFrames_.push_back(out);
    }
    coverFrame_ = 1;
    cover_->setPixmap(coverFrames_[1]);
    // Dominante Farbe weiterhin für Tints/Status-Glow.
    coverAccent_ = coverAccentColor(cov, coverAccent_);
}

void MainWindow::onCoverReady(const QString& path) {
    // Manuell gewähltes Cover hat Vorrang — Auto-Erkennungs-Cover ignorieren.
    if (manualAlbum_.has_value() && manualDiscId_ == lastDiscId_ &&
        !manualCoverPath_.isEmpty() && path != manualCoverPath_)
        return;
    QPixmap p(path);
    if (!p.isNull()) setCoverFrames(p);
}

void MainWindow::onPickCover() {
    QString f = QFileDialog::getOpenFileName(
        this, "Cover-Bild wählen", QString(),
        "Bilder (*.jpg *.jpeg *.png *.webp);;Alle Dateien (*)");
    if (f.isEmpty()) return;
    ctl_->setCover(f);
    onCoverReady(f);
}

void MainWindow::onPickCoverMB() {
    if (curReleaseId_.isEmpty()) {
        QMessageBox::information(this, "Cover: MusicBrainz",
            "Noch keine MusicBrainz-Release bekannt (erst nach Disc-Lookup).");
        return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto urls = cdr::caa_image_urls(curReleaseId_.toStdString(),
                                    cfg_.mb_useragent);
    QApplication::restoreOverrideCursor();
    if (urls.empty()) {
        QMessageBox::information(this, "Cover: MusicBrainz",
            "Cover Art Archive hat für diese Release keine Bilder.");
        return;
    }
    QStringList items;
    for (int i = 0; i < (int)urls.size(); ++i)
        items << QString("Bild %1  —  %2").arg(i + 1)
                     .arg(QString::fromStdString(urls[i]));
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Cover wählen",
        QString("%1 Bild(er) gefunden:").arg(urls.size()),
        items, 0, false, &ok);
    if (!ok) return;
    int idx = items.indexOf(sel);
    if (idx < 0) return;
    QString tmp = QDir::tempPath() + "/cdripper-cover-pick.jpg";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    bool dl = cdr::fetch_url(urls[idx], cfg_.mb_useragent,
                             tmp.toStdString());
    QApplication::restoreOverrideCursor();
    if (!dl) {
        QMessageBox::warning(this, "Cover: MusicBrainz",
            "Download fehlgeschlagen.");
        return;
    }
    ctl_->setCover(tmp);
    onCoverReady(tmp);
}

void MainWindow::onTrackState(int idx, int state, double frac,
                              const QString& msg) {
    int row = idx - 1;
    if (row < 0 || row >= table_->rowCount()) return;
    // #-Spalte persistent markieren. WICHTIG: „AccurateRip kein DB-
    // Vergleich" ist KEIN Fehler — nur ein Info-Punkt (·, cyan), damit
    // man nicht jedes Mal alarmiert nachforscht. Grünes ✓ = AR bestätigt,
    // oranges ⚠ = echtes Problem (Lesefehler/Hänger/Failed).
    if (!msg.isEmpty() && state != (int)cdr::TrackState::Failed) {
        if (auto* nr = table_->item(row, 0)) {
            bool good = msg.contains(QString::fromUtf8("✓"));
            bool defect = msg.contains(QString::fromUtf8("⚠")) ||
                msg.contains("unlesbar", Qt::CaseInsensitive) ||
                msg.contains("Sektor",   Qt::CaseInsensitive) ||
                msg.contains("hing",     Qt::CaseInsensitive);
            QString mark; QColor mcol;
            if (good)        { mark = QString::fromUtf8(" ✓");
                               mcol = QColor("#27ae60"); }
            else if (defect) { mark = QString::fromUtf8(" ⚠");
                               mcol = QColor("#e0a83e"); }
            else             { mark = QString::fromUtf8(" ·");   // Info
                               mcol = QColor("#4fc3f7"); }
            nr->setText(QString::number(idx) + mark);
            nr->setToolTip(msg);
            nr->setForeground(mcol);
        }
    }
    if (auto* it = table_->item(row, 3)) {
        QString lbl = cdr::state_label((cdr::TrackState)state);
        if (!msg.isEmpty())
            lbl += (state == (int)cdr::TrackState::Failed ? ": " : " · ") + msg;
        it->setText(lbl);
        it->setToolTip(msg);
        it->setForeground(state_color(state));
    }
    if (auto* pcell = table_->item(row, 4)) {
        cdr::TrackState s = (cdr::TrackState)state;
        double f = frac;
        if (s == cdr::TrackState::Done || s == cdr::TrackState::Ripped ||
            s == cdr::TrackState::Encoding) f = 1.0;
        else if (s == cdr::TrackState::Failed) f = 0.0;
        const bool active = (state >= 1 && state <= 4);  // Ripping..Uploading
        pcell->setData(Qt::UserRole, f);
        pcell->setData(Qt::UserRole + 1, active);
        // Aktiv bearbeitete Zeile hebt sich in Cover-Farbe ab (wie im
        // Multi-Fenster); fertige/wartende fallen auf transparent zurück.
        QColor rowbg = coverAccent_;
        rowbg.setAlpha(active ? 64 : 0);
        for (int c = 0; c < 5; ++c)
            if (auto* it = table_->item(row, c)) it->setBackground(rowbg);
    }
}

void MainWindow::onProgress(double elapsed, double eta, int ripped,
                            int uploaded, int total) {
    lastElapsed_ = elapsed;
    lastEta_     = eta;
    sbElapsed_->setText("Zeit "  + mmss(elapsed));
    sbEta_->setText("Rest ~"     + mmss(eta));
    sbRip_->setText(QString("Gerippt %1/%2").arg(ripped).arg(total));
    sbUp_->setText(QString("Hochgeladen %1/%2").arg(uploaded).arg(total));
}

void MainWindow::onMetrics(double r, double e, double u) {
    auto f = [](double v) {
        return v > 0.01 ? QString::number(v, 'f', 1) + " MB/s"
                        : QString("–");
    };
    sbSpeed_->setText("R " + f(r) + " · E " + f(e) + " · U " + f(u));
}

// Farbige Log-Zeile: Schweregrad aus Schlüsselwörtern, nicht jeder
// Hinweis ist ein Fehler. Reihenfolge = Priorität (Fehler vor Warnung).
void MainWindow::appendLog(const QString& l) {
    if (!logView_) return;
    QString col = "#cfd3da";                       // neutral/Info-Default
    auto has = [&](const char* s) { return l.contains(QString::fromUtf8(s),
                                       Qt::CaseInsensitive); };
    if (has("FEHLER") || has("FATAL") || has("fehlgeschlagen") ||
        has("nicht lesbar") || has("unlesbar") || has("Exception"))
        col = "#e06c75";                           // rot = echtes Problem
    else if (l.contains(QString::fromUtf8("⚠")) || has("hing") ||
             has("Hänger") || has("übersprungen") || has("Stall") ||
             has("abgebrochen"))
        col = "#e0a83e";                           // gelb = Achtung/Recovery
    else if (has("[OK]") || l.contains(QString::fromUtf8("✓")) ||
             has("erkannt") || has("bestätigt") || has("übernommen") ||
             has("fertig") || has("hochgeladen"))
        col = "#27ae60";                           // grün = Erfolg
    else if (has("AccurateRip") || has("Disc-ID") || has("Vorschau") ||
             has("AcoustID") || has("Metadaten") || has("Suche"))
        col = "#4fc3f7";                           // cyan = Info/Erkennung
    logView_->appendHtml("<span style=\"color:" + col + "\">" +
                         l.toHtmlEscaped() + "</span>");
}

void MainWindow::onLog(const QString& l) { appendLog(l); }

// Eine Zeile in die Log-Kette: sichtbar im Log-Fenster UND persistent in
// ~/.local/share/cdripper/cdripper.log — damit JEDE Aktion (auch Vorschau
// & manuelle Erkennung, nicht nur Rips) nachvollziehbar/diagnostizierbar
// ist. Immer auf dem GUI-Thread aufrufen (logView_ ist ein Widget).
void MainWindow::logChain(const QString& line) {
    appendLog(line);
    cdr::log_to_file(line.toStdString());
}

void MainWindow::onDiscDone(bool ok, const QString& m) {
    bannerLbl_->setText((ok ? "✓ " : "✗ ") + m);
    appendLog((ok ? "[OK] " : "[FEHLER] ") + m);
    // Status-Puls im Hintergrund: grün bei Erfolg, rot bei Fehler.
    if (mainFx_)
        static_cast<BackgroundFx*>(mainFx_)->pulse(
            ok ? QColor(0x35, 0xc7, 0x59) : QColor(0xff, 0x45, 0x45));
    notify(ok ? "CD fertig ✓" : "CD mit Fehlern ✗", m);
    QString now = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString arInfo;
    if (m.contains("AccurateRip")) arInfo = "siehe Log";
    history_.append(QStringList{ now, m, ok ? "OK" : "FEHLER", arInfo });
    if (tray_) {
        tray_->setToolTip("CD Ripper — " + QString(ok ? "fertig" : "Fehler"));
        tray_->showMessage(ok ? "CD fertig" : "CD mit Fehlern", m,
                            QSystemTrayIcon::Information, 5000);
    }
    if (cfg_.chime) QApplication::beep();
}

void MainWindow::onFatal(const QString& m) {
    if (mainFx_)
        static_cast<BackgroundFx*>(mainFx_)->pulse(QColor(0xff, 0x45, 0x45));
    QMessageBox::critical(this, "Fataler Fehler", m);
}

void MainWindow::onFinished() {
    busy_ = false;
    setControlsRunning(false);
    if (!bannerLbl_->text().startsWith("✓") &&
        !bannerLbl_->text().startsWith("✗"))
        bannerLbl_->setText("Beendet.");
}

void MainWindow::tick() {
    if (busy_) {
        lastElapsed_ += 1;
        if (lastEta_ > 0) lastEta_ -= 1;
        sbElapsed_->setText("Zeit "  + mmss(lastElapsed_));
        sbEta_->setText("Rest ~"     + mmss(lastEta_));
    }
    discWatch();
}

// Laufwerks-Dropdown: im System erkannte optische Laufwerke + Hardware-
// Name + ob eine CD drin ist (● / ○). Passive Statusabfrage (kein Probe
// → kein Anlaufen). Fällt auf das konfigurierte Gerät zurück.
void MainWindow::populateDrives() {
    QString keep = device_->count()
        ? device_->currentData().toString()
        : QString::fromStdString(cfg_.device);
    QSignalBlocker blk(device_);
    device_->clear();
    auto devs = cdr::list_optical_devices();
    if (devs.empty()) devs.push_back(cfg_.device);   // Fallback
    for (const auto& d : devs) {
        QString label = QString::fromStdString(d);
        cdr::HwInfo hw = cdr::drive_hwinfo(d);
        if (hw.ok) {
            QString m = QString::fromStdString(
                (hw.vendor + " " + hw.model)).trimmed();
            if (!m.isEmpty()) label += "  ·  " + m;
        }
        bool disc = false;
        try { cdr::Drive dr(d); disc = dr.disc_ready(); } catch (...) {}
        label += disc ? QString::fromUtf8("   ● CD")
                       : QString::fromUtf8("   ○ leer");
        device_->addItem(label, QString::fromStdString(d));
        device_->setItemData(device_->count() - 1,
                             QString::fromStdString(d), Qt::ToolTipRole);
    }
    int idx = device_->findData(keep);
    device_->setCurrentIndex(idx >= 0 ? idx : 0);
    QString sel = device_->currentData().toString();
    if (!sel.isEmpty()) cfg_.device = sel.toStdString();
}

// Vorschau beim Einlegen: erkennt eine neu eingelegte Audio-CD (nur wenn
// kein Rip läuft), holt Metadaten + Cover + Trackliste off-thread und
// zeigt sie sofort. Danach entscheidet das Jukebox-Setting (an → Rip
// automatisch, aus → Vorschau stehen lassen, Rip erst auf „Start").
void MainWindow::discWatch() {
    if (ctl_->running() || previewBusy_.load() || scanBusy_.load() ||
        metaBusy_.load()) return;
    if (++discPoll_ % 3 != 0) return;            // ~alle 3 s
    // AKTIV proben (liest die TOC) statt nur den passiven Laufwerk-Status
    // abzufragen: eine schon beim Start liegende — oder ohne Kernel-Media-
    // Change-Event eingelegte — Disc meldet `disc_ready()` sonst nie, und
    // die Vorschau bliebe ewig auf „Tray leer". probe_disc_id weckt das
    // Laufwerk und ist die zuverlässige Erkennung (wie die Rip-Warteschleife).
    std::string id;
    try { id = cdr::probe_disc_id(cfg_.device); } catch (...) { id.clear(); }
    if (id.empty()) {                            // kein lesbarer Datenträger
        lastDiscId_.clear();
        if (hadDisc_) { hadDisc_ = false; resetDiscState(); } // einmal je Auswurf
        return;
    }
    hadDisc_ = true;
    if (id == lastDiscId_) return;               // Vorschau schon vorhanden
    // TOC wurde gerade gelesen → Laufwerk wach → has_audio() jetzt belastbar.
    bool audio = true;
    try { cdr::Drive d(cfg_.device); audio = d.has_audio(); }
    catch (...) { audio = true; }                // im Zweifel: wie Audio
    if (!audio) {                                // Daten-CD: (noch) kein Rip
        lastDiscId_ = id;                        // nicht endlos neu anlaufen
        bannerLbl_->setText("Daten-CD erkannt — Audio-Rip nicht anwendbar "
                            "(Disc-Image-Modus ist Zukunftsmusik).");
        return;
    }
    lastDiscId_ = id;
    previewBusy_ = true;
    bannerLbl_->setText("⏏  Disc erkannt — lade Cover & Trackliste …");
    std::string dev = cfg_.device, ua = cfg_.mb_useragent,
                tmp = cfg_.tmpdir;
    if (previewThr_.joinable()) previewThr_.join();
    previewThr_ = std::thread([this, dev, ua, tmp] {
        cdr::Album al; bool have = false;
        auto clog = [this](QString s) {
            QMetaObject::invokeMethod(this, [this, s] { logChain(s); },
                                      Qt::QueuedConnection);
        };
        try {
            cdr::DiscIdent di = cdr::read_disc_ident(dev);
            clog(QString("Vorschau: Disc-ID %1 (%2 Tracks laut TOC)")
                 .arg(QString::fromStdString(di.id)).arg(di.toc_tracks));
            if (di.reconstructed)
                clog("Vorschau: TOC rekonstruiert — evtl. Copy-Control "
                     "(Kopierschutz). Rip ggf. mit anderem Laufwerk.");
            std::string src;
            try {
                auto cands = cdr::mb_release_candidates(di.id, ua, di.toc,
                                                        &src);
                if (!cands.empty()) {
                    al = cands[0]; have = true;
                    clog(QString("Vorschau: Metadaten MusicBrainz [%1], "
                        "%2 Release(s) → %3 — %4")
                        .arg(QString::fromStdString(src.empty()?"?":src))
                        .arg(cands.size())
                        .arg(QString::fromStdString(al.artist))
                        .arg(QString::fromStdString(al.title)));
                }
            } catch (const std::exception& e) {
                clog(QString("Vorschau: MusicBrainz-Fehler — %1")
                     .arg(e.what()));
            } catch (...) { clog("Vorschau: MusicBrainz-Fehler (unbekannt)"); }
            if (!have) {
                try {
                    auto cd = cdr::cddb_lookup(di.toc, ua);
                    if (cd) { al = *cd; have = true;
                        clog(QString("Vorschau: Metadaten gnudb/CDDB → %1 — %2")
                            .arg(QString::fromStdString(al.artist))
                            .arg(QString::fromStdString(al.title))); }
                } catch (...) { clog("Vorschau: CDDB-Fehler"); }
            }
            if (!have) {
                auto c = cdr::cdtext_lookup(dev, di.toc_tracks);
                if (c) { al = *c; have = true;
                    clog("Vorschau: Metadaten aus CD-TEXT."); }
            }
            if (!have) {
                al = cdr::placeholder_album(di.toc_tracks);
                clog("Vorschau: KEIN Treffer (MB/CDDB/CD-TEXT) → Platzhalter. "
                     "Tipp: Aktion → 'Titel manuell suchen' / 'per Klang "
                     "erkennen'.");
            }
            // Cover-Fallback: Treffer aus CD-TEXT/CDDB hat keine MB-Release-ID
            // → per Titelsuche eine Release finden, nur fürs Cover.
            if (have && al.mb_release_id.empty() &&
                !al.artist.empty() && !al.title.empty()) {
                try {
                    auto hits = cdr::mb_search_releases(al.artist, al.title, ua);
                    if (!hits.empty()) {
                        al.mb_release_id = hits[0].mbid;
                        clog("Vorschau: Cover-Quelle via Titelsuche (MB).");
                    }
                } catch (...) {}
            }
            std::string cov;
            try {
                fs::path dir = fs::path(tmp) / "preview";
                std::error_code ec; fs::create_directories(dir, ec);
                fs::path out;
                if (cdr::fetch_cover_for_album(al, ua, dir, out))
                    cov = out.string();
            } catch (...) {}
            clog(cov.empty() ? "Vorschau: kein Cover gefunden."
                             : "Vorschau: Cover geladen.");
            QStringList ti, ar;
            for (auto& t : al.tracks) {
                ti << QString::fromStdString(t.title);
                ar << QString::fromStdString(t.artist);
            }
            QString aa = QString::fromStdString(al.artist),
                    at = QString::fromStdString(al.title),
                    yr = QString::fromStdString(al.year()),
                    cv = QString::fromStdString(cov);
            QMetaObject::invokeMethod(this, [this, aa, at, yr, ti, ar, cv] {
                onAlbumReady(aa, at, yr, ti, ar);
                if (!cv.isEmpty()) onCoverReady(cv);
                bannerLbl_->setText("Disc bereit: " + aa + " — " + at +
                    (cfg_.jukebox ? "  ·  Jukebox: starte Rip …"
                                  : "  ·  Start-Knopf drücken zum Rippen"));
                previewBusy_ = false;
                if (cfg_.jukebox && !ctl_->running()) onStart();
            }, Qt::QueuedConnection);
        } catch (...) {
            QMetaObject::invokeMethod(this, [this] {
                previewBusy_ = false;
                bannerLbl_->setText("Disc nicht lesbar — bitte prüfen.");
            }, Qt::QueuedConnection);
        }
    });
}

// Auswurf / neue Disc: den disc-spezifischen Stand der vorigen CD leeren
// (Scan-Ring, Scan-Caption, Log, scan-geführter Rip-Status) UND die
// sichtbare Disc-Info (Cover, Album-Felder, Trackliste) — sonst bleibt
// beim Auswerfen ohne neue Disc die alte CD im Fenster „stehen".
void MainWindow::resetDiscState() {
    if (discScan_)    discScan_->setResult(cdr::ProbeResult{});
    if (discScanCap_) discScanCap_->setText("Disc-Scan: bereit");
    if (logView_)     logView_->clear();
    if (table_)       { fillingTable_ = true;
                        table_->setRowCount(0);
                        fillingTable_ = false; }
    if (albArtist_)   albArtist_->clear();
    if (albTitle_)    albTitle_->clear();
    if (albYear_)     albYear_->clear();
    coverFrames_.clear(); coverFrame_ = -1;   // Ambilight-Stufen verwerfen
    if (cover_)       { cover_->setPixmap(QPixmap());
                        cover_->setText("kein\nCover"); }
    scanDiscId_.clear();
    scanTrackStatus_.clear();
    curReleaseId_.clear();
    manualAlbum_.reset();             // manuelle Metadaten gelten nur je Disc
    manualDiscId_.clear();
    manualCoverPath_.clear();
    lastDiscId_.clear();              // nächste Disc löst frische Vorschau aus
    bannerLbl_->setText("Tray leer — neue CD einlegen.");
}

// Cover-Easter-Egg: Klick aufs Cover-Bild → CD-Morph-Spin.
bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
    // Animierten Hintergrund auf die Größe des central-Widgets nachziehen.
    if (mainFx_ && obj == mainFx_->parentWidget() &&
        ev->type() == QEvent::Resize) {
        if (auto* w = qobject_cast<QWidget*>(obj)) {
            mainFx_->setGeometry(w->rect());
            mainFx_->lower();
        }
        return false;                             // nicht konsumieren
    }
    if (obj == cover_ && ev->type() == QEvent::MouseButtonPress &&
        !coverSpin_) {
        QPixmap pm = cover_->pixmap();
        if (!pm.isNull() && cover_->parentWidget()) {
            auto* s = new CoverSpin(cover_->parentWidget(), pm,
                                    cover_->geometry());
            coverSpin_ = s;
            s->onFinished = [this]{ coverSpin_ = nullptr; };
            s->show();
            s->raise();
        }
        return true;                              // Klick konsumiert
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::onOpenSettings() {
    if (anyRipActive()) {
        QMessageBox::information(this, "Einstellungen",
            "Während irgendwo ein Lauf aktiv ist nicht änderbar — erst "
            "alle Läufe (auch im Multi-Fenster) stoppen.");
        return;
    }
    // Ein laufender Scan liest live cfg_-Felder (registry_url/-condition,
    // mb_useragent) im Worker-Thread. `cfg_ = dlg.config()` unten würde diese
    // std::string-Felder gleichzeitig überschreiben → Data-Race/UB. Solange
    // ein Scan/Preview läuft, Einstellungen nicht öffnen.
    if (scanBusy_.load() || previewBusy_.load() || metaBusy_.load()) {
        QMessageBox::information(this, "Einstellungen",
            "Während eine Disc-/Metadaten-Suche läuft nicht änderbar — "
            "kurz warten, bis der Scan fertig ist.");
        return;
    }
    SettingsDialog dlg(cfg_, QString::fromStdString(cfgPath_), this);
    if (dlg.exec() != QDialog::Accepted) return;
    cfg_ = dlg.config();
    populateDrives();          // ggf. in den Einstellungen geändertes Gerät
    QString prof = dlg.selectedProfile();
    std::string path = cdr::profile_path(prof.toStdString());
    if (cdr::save_config(cfg_, path)) {
        cdr::set_active_profile(prof.toStdString());
        cfgPath_ = path;
        bannerLbl_->setText("Profil '" +
            (prof.isEmpty() ? QString("Standard") : prof) + "' gespeichert.");
        msgWide(this, QMessageBox::Information, "Gespeichert",
            "Profil '" + (prof.isEmpty() ? QString("Standard") : prof) +
            "' gespeichert in:\n\n" + QString::fromStdString(path) +
            "\n\nIst jetzt aktiv. Änderungen greifen ab der nächsten CD.");
    } else {
        msgWide(this, QMessageBox::Warning, "Speichern fehlgeschlagen",
            "Konnte die Konfiguration nicht schreiben:\n\n" +
            QString::fromStdString(path));
    }
}

void MainWindow::onShowLogs() {
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Logs — laufende Sitzung");
    dlg->resize(820, 520);
    auto* te = new QPlainTextEdit;
    te->setReadOnly(true);
    te->setPlainText(logView_->toPlainText());
    te->setLineWrapMode(QPlainTextEdit::NoWrap);
    auto* refresh = new QPushButton("Aktualisieren");
    auto* close   = new QPushButton("Schließen");
    connect(refresh, &QPushButton::clicked, dlg,
            [this, te] { te->setPlainText(logView_->toPlainText());
                         te->moveCursor(QTextCursor::End); });
    connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    auto* h = new QHBoxLayout;
    h->addStretch(); h->addWidget(refresh); h->addWidget(close);
    auto* v = new QVBoxLayout(dlg);
    v->addWidget(te, 1);
    v->addLayout(h);
    te->moveCursor(QTextCursor::End);
    dlg->exec();
    dlg->deleteLater();
}

void MainWindow::onAbout() {
    msgWide(this, QMessageBox::Information, "Über CD Ripper",
        QString(
        "<h3>CD Ripper → Navidrome</h3>"
        "<p>Version <b>%1</b></p>"
        "<p>Audio-CD → FLAC (MusicBrainz + Cover Art + ReplayGain) → "
        "Nextcloud/WebDAV/SSH/SMB/lokal, mit AccurateRip-Verifikation und "
        "parallelem Rip→Encode→Upload-Fließband.</p>"
        "<p>Komponenten: libcdio_paranoia · libdiscid · libcurl · "
        "nlohmann/json · flac · rsgain · Qt6</p>"
        "<p><small>Eigenbau für die athena-cluster Navidrome-Bibliothek.</small></p>"
        ).arg(cdr::VERSION), 560);
}

// (DiscScanWidget ist nach oben verschoben — vor MainWindow, damit das
//  Hauptfenster es einbetten kann; siehe oberhalb von MainWindow::MainWindow.)

void MainWindow::onScanDisc() {
    if (ctl_->running()) {
        msgWide(this, QMessageBox::Warning, "Scan nicht möglich",
            "Es läuft ein Rip — das Laufwerk ist belegt. Erst stoppen / "
            "warten, dann scannen.", 480);
        return;
    }
    if (scanThr_.joinable()) scanThr_.join();
    scanBusy_ = true;     // discWatch pausieren (sonst Drive-Poll-Kollision
                          // → GUI-Freeze, Live-Karte erst am Ende)
    { QString d = device_->currentData().toString();
      if (!d.isEmpty()) cfg_.device = d.toStdString(); }
    auto stopF = std::make_shared<std::atomic<bool>>(false);
    scanStop_ = stopF;    // dtor kann den Scan so abbrechen

    // Dialog SOFORT (kein Ladebalken): Ring baut sich live auf + Live-Log.
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Disc-Qualitäts-Scan (live)");
    dlg->resize(680, 760);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    auto* v = new QVBoxLayout(dlg);
    auto* head = new QLabel("Disc erkannt — starte Scan …");
    head->setWordWrap(true);
    v->addWidget(head);
    auto* sc = new DiscScanWidget(dlg);
    v->addWidget(sc, 1);
    v->addWidget(new QLabel(QString::fromUtf8(
        "<small>Außen = Disc-Rand, innen = Anfang. "
        "<span style='color:#27ae60'>■</span> ok &nbsp; "
        "<span style='color:#e0a83e'>■</span> langsam &nbsp; "
        "<span style='color:#c0392b'>■</span> Lesefehler</small>")));
    auto* trk = new QTableWidget(0, 4, dlg);
    trk->setHorizontalHeaderLabels(
        { "Track", "Titel", "Rip-Verdikt", "Empf. Lese-Speed" });
    auto* th = trk->horizontalHeader();
    th->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    th->setSectionResizeMode(1, QHeaderView::Stretch);
    th->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    th->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    trk->setEditTriggers(QAbstractItemView::NoEditTriggers);
    trk->verticalHeader()->setVisible(false);
    trk->setMaximumHeight(190);
    v->addWidget(trk);
    auto* log = new QPlainTextEdit(dlg);
    log->setReadOnly(true);
    log->setMaximumBlockCount(2000);
    log->setMinimumHeight(140);
    // Read-only QPlainTextEdit hat per Default das Copy-/Select-All-
    // Kontextmenü — explizit setzen, falls eine WM-/Wayland-Eigenheit
    // den Rechtsklick sonst als Fenster-Resize abfängt.
    log->setContextMenuPolicy(Qt::DefaultContextMenu);
    v->addWidget(log, 1);
    auto* btnRow = new QHBoxLayout;
    auto* copyBtn = new QPushButton("Log kopieren");
    connect(copyBtn, &QPushButton::clicked, dlg, [log] {
        QApplication::clipboard()->setText(log->toPlainText());
    });
    btnRow->addWidget(copyBtn);
    btnRow->addStretch(1);
    auto* close = new QPushButton("Abbrechen / Schließen");
    connect(close, &QPushButton::clicked, dlg, &QDialog::close);
    btnRow->addWidget(close);
    v->addLayout(btnRow);
    connect(dlg, &QObject::destroyed, this,
            [stopF]{ stopF->store(true); });
    dlg->show();

    QPointer<DiscScanWidget> pSc = sc;
    QPointer<QLabel> pHead = head;
    QPointer<QPlainTextEdit> pLog = log;
    QPointer<QTableWidget> pTrk = trk;
    std::string dev = cfg_.device;
    std::string ua  = cfg_.mb_useragent;
    int dens = cfg_.scan_density;
    scanThr_ = std::thread([this, dev, ua, dens, stopF, pSc, pHead,
                            pLog, pTrk] {
        cdr::ArchiveEntry ae;
        ae.kind = "scan"; ae.outcome = "scan";
        std::string relId;
        QStringList titles;
        int leadout = 0;       // echtes Disc-Ende (LBA) — Ring-Radius fixieren
        try {
            cdr::DiscIdent di = cdr::read_disc_ident(dev);
            ae.disc_id = di.id; ae.tracks = di.toc_tracks;
            // libdiscid-TOC = "first last leadout off1 off2 …": das 3. Token
            // ist die Leadout-LBA = volle Disc-Geometrie. VOR dem Scan setzen,
            // damit die Bänder nicht nachskalieren (Ziehharmonika) und der
            // Cursor die echte Laser-Radialposition zeigt statt sofort außen.
            { std::istringstream ts(di.toc); int a, b;
              if (ts >> a >> b >> leadout && leadout < 0) leadout = 0; }
            try {
                auto cands = cdr::mb_release_candidates(di.id, ua, di.toc);
                if (!cands.empty()) {
                    ae.artist = cands[0].artist;
                    ae.title  = cands[0].title;
                    ae.year   = cands[0].year();
                    relId     = cands[0].mb_release_id;
                    for (auto& t : cands[0].tracks)
                        titles << QString::fromStdString(t.title);
                }
            } catch (...) {}
        } catch (...) {}
        {
            QString t = QString::fromStdString(
                (ae.artist.empty() && ae.title.empty())
                ? std::string("(unbekannte Disc)")
                : ae.artist + " — " + ae.title);
            QMetaObject::invokeMethod(this, [pSc, pHead, pLog, t, leadout] {
                if (pSc && leadout > 0) pSc->beginScan(0, leadout);
                if (pHead) pHead->setText("Scanne: <b>" + t + "</b> …");
                if (pLog)  pLog->appendPlainText("Disc: " + t);
            }, Qt::QueuedConnection);
        }
        cdr::ProbeResult r = cdr::disc_probe(dev,
            [stopF]{ return stopF->load(); },
            [this, pSc, pLog](int lba, int st, long ms) {  // st = Live-Status
                QMetaObject::invokeMethod(this, [pSc, pLog, lba, st, ms] {
                    if (pSc)  pSc->addCell(lba, st);
                    if (pLog) pLog->appendPlainText(
                        QString("LBA %1  %2  %3 ms").arg(lba)
                            .arg(st == 2 ? "FEHLER" : st == 1 ? "langsam"
                                                              : "ok").arg(ms));
                }, Qt::QueuedConnection);
            },
            dens,
            [this, pSc, pLog](int lba) {          // Echtzeit-Cursor
                QMetaObject::invokeMethod(this, [pSc, pLog, lba] {
                    if (pSc)  pSc->setCursor(lba);
                    if (pLog) pLog->appendPlainText(
                        QString("→ scanne LBA %1 …").arg(lba));
                }, Qt::QueuedConnection);
            });
        scanBusy_ = false;     // Laufwerk frei → discWatch wieder erlauben
        ae.ts = (long)QDateTime::currentSecsSinceEpoch();
        ae.quality        = (int)r.quality;
        ae.scan_completed = r.completed;
        ae.lba_min = r.lba_min; ae.lba_max = r.lba_max;
        ae.map = r.map;
        ae.track_status = r.track_status;   // Rip kann den Scan-Plan
                                            // später aus dem Archiv ziehen
        cdr::append_archive(ae);
        if (cfg_.registry_condition && !cfg_.registry_url.empty())
            cdr::registry_submit_condition(cfg_.registry_url, ae.disc_id,
                ae.artist, ae.title, ae.year, relId, ae.quality,
                0, 0, 0, "scan", cfg_.mb_useragent);
        std::string did = ae.disc_id;
        QMetaObject::invokeMethod(this, [this, r, titles, did, pSc, pHead,
                                         pLog, pTrk] {
            // Scan-geführter Rip merken: Disc-ID + VOLLER Pro-Track-Status
            // für genau diese Disc, nur diese Sitzung. Damit überspringt
            // der nächste Rip dieser Disc seinen eigenen Preflight.
            if (!did.empty() && !r.track_status.empty()) {
                scanDiscId_ = did;
                scanTrackStatus_ = r.track_status;
            }
            const char* q = r.quality == cdr::DiscQuality::Clean ? "SAUBER"
                : r.quality == cdr::DiscQuality::Marginal ? "GRENZWERTIG"
                : "STARK ZERKRATZT";
            QString h = QString("Disc: <b>%1</b> — %2")
                .arg(q).arg(QString::fromStdString(r.detail));
            if (!r.completed)
                h += "<br><span style='color:#e0a83e'>Scan unvollständig "
                     "(Laufwerk hing / abgebrochen) — Teil-Karte zeigt, "
                     "wo es hängt.</span>";
            // Schadensform-Diagnose aus dem Stichproben-Muster (beim Scan
            // grob — die volle Auflösung liefert der echte Rip).
            cdr::DamageReport dmg = cdr::classify_damage(
                r.map, r.lba_min, r.lba_max,
                (int)r.track_status.size() - 1);
            if (dmg.kind != cdr::DamageReport::None) {
                h += "<br><span style='color:#9aa0aa'>Schadensbild (grob): " +
                     QString::fromStdString(dmg.headline) + "</span>";
                if (pLog) {
                    pLog->appendPlainText("Schadensbild: " +
                        QString::fromStdString(dmg.headline));
                    if (!dmg.advice.empty())
                        pLog->appendPlainText("Empfehlung: " +
                            QString::fromStdString(dmg.advice));
                }
            }
            if (pSc)   pSc->setResult(r);     // finale relative Einfärbung
            if (pHead) pHead->setText(h);
            if (pLog)  pLog->appendPlainText(
                "Fertig: " + QString::fromStdString(r.detail));
            // Pro-Track-Verdikt: aus Scan-Stichproben je Track.
            if (pTrk) {
                int nt = (int)r.track_status.size() - 1;
                if (nt < 0) nt = 0;
                pTrk->setRowCount(nt);
                auto vlabel = [](int s) {
                    return s == 0 ? QString("rippbar")
                         : s == 1 ? QString("langsam (paranoia)")
                         : s == 2 ? QString("möglicherweise defekt")
                         : s == 3 ? QString("ungescannt (hinter Hänger)")
                                  : QString("—"); };
                auto vcol = [](int s) {
                    return s == 0 ? QColor("#27ae60")
                         : s == 1 ? QColor("#e0a83e")
                         : s == 2 ? QColor("#c0392b")
                                  : QColor("#9aa0aa"); };
                auto spd = [](int s) {           // empfohlener Rip-Modus
                    return s == 0 ? QString("max · Schnell-Rip")
                         : s == 1 ? QString("max · volle paranoia")
                         : s == 2 ? QString("4× · paranoia · zuletzt")
                         : s == 3 ? QString("max · paranoia (unbekannt)")
                                  : QString("—"); };
                for (int t = 1; t <= nt; ++t) {
                    int st = r.track_status[t];
                    pTrk->setItem(t - 1, 0,
                        new QTableWidgetItem(QString::number(t)));
                    pTrk->setItem(t - 1, 1, new QTableWidgetItem(
                        t - 1 < titles.size() ? titles[t - 1] : QString()));
                    auto* vi = new QTableWidgetItem(vlabel(st));
                    vi->setForeground(vcol(st));
                    pTrk->setItem(t - 1, 2, vi);
                    auto* si = new QTableWidgetItem(spd(st));
                    si->setForeground(vcol(st));
                    pTrk->setItem(t - 1, 3, si);
                }
            }
            // Ergebnis auch auf die Disc im Hauptfenster spiegeln
            if (discScan_)    discScan_->setResult(r);
            if (discScanCap_) discScanCap_->setText(
                QString::fromUtf8("Letzter Scan: ") + q);
        }, Qt::QueuedConnection);
    });
}

// Manuelle MusicBrainz-Namenssuche → Release wählen → Felder + Cover.
void MainWindow::onSearchMeta() {
    if (ctl_->running()) {
        QMessageBox::information(this, "Suche",
            "Während ein Lauf aktiv ist nicht möglich."); return;
    }
    if (metaBusy_.load()) {
        QMessageBox::information(this, "Suche",
            "Eine Suche/Erkennung läuft bereits."); return;
    }
    bool ok = false;
    QString art = QInputDialog::getText(this, "Titel manuell suchen",
        "Interpret:", QLineEdit::Normal, albArtist_->text(), &ok);
    if (!ok) return;
    QString tit = QInputDialog::getText(this, "Titel manuell suchen",
        "Album:", QLineEdit::Normal, albTitle_->text(), &ok);
    if (!ok) return;
    if (art.trimmed().isEmpty() && tit.trimmed().isEmpty()) return;
    if (metaThr_.joinable()) metaThr_.join();
    metaBusy_ = true;
    bannerLbl_->setText("Suche bei MusicBrainz …");
    logChain("Manuelle Suche: MusicBrainz nach Interpret='" + art +
             "' Album='" + tit + "' …");
    std::string ua = cfg_.mb_useragent, dev = cfg_.device,
                tmp = cfg_.tmpdir;
    std::string sa = art.toStdString(), st = tit.toStdString();
    metaThr_ = std::thread([this, ua, dev, tmp, sa, st] {
        int wantTracks = 0;
        try { wantTracks = cdr::read_disc_ident(dev).toc_tracks; } catch (...) {}
        auto hits = cdr::mb_search_releases(sa, st, ua);
        if (hits.empty()) {
            QMetaObject::invokeMethod(this, [this] {
                metaBusy_ = false;
                bannerLbl_->setText("Keine MusicBrainz-Treffer.");
                logChain("Manuelle Suche: keine MusicBrainz-Treffer.");
            }, Qt::QueuedConnection);
            return;
        }
        QStringList labels;
        for (auto& h : hits)
            labels << QString::fromStdString(
                h.artist + " — " + h.title +
                (h.date.empty() ? "" : " (" + h.date + ")") +
                (h.country.empty() ? "" : " [" + h.country + "]") +
                (h.tracks ? "  " + std::to_string(h.tracks) + " Tracks" : ""));
        int pick = 0;
        QMetaObject::invokeMethod(ctl_, "chooseReleaseSlot",
            Qt::BlockingQueuedConnection, Q_RETURN_ARG(int, pick),
            Q_ARG(QStringList, labels), Q_ARG(int, 0));
        if (pick < 0 || pick >= (int)hits.size()) pick = 0;
        auto al = cdr::mb_release_by_id(hits[pick].mbid, wantTracks, ua);
        std::string cov;
        if (al) {
            try {
                fs::path dir = fs::path(tmp) / "preview";
                std::error_code ec; fs::create_directories(dir, ec);
                fs::path out;
                if (cdr::fetch_cover(al->mb_release_id, ua, dir, out))
                    cov = out.string();
            } catch (...) {}
        }
        QMetaObject::invokeMethod(this, [this, al, cov] {
            metaBusy_ = false;
            if (!al) { bannerLbl_->setText("Release-Details nicht ladbar.");
                       logChain("Manuelle Suche: Release-Details nicht "
                                "ladbar."); return; }
            QStringList ti, ar;
            for (auto& t : al->tracks) {
                ti << QString::fromStdString(t.title);
                ar << QString::fromStdString(t.artist);
            }
            onAlbumReady(QString::fromStdString(al->artist),
                         QString::fromStdString(al->title),
                         QString::fromStdString(al->year()), ti, ar);
            if (!cov.empty()) onCoverReady(QString::fromStdString(cov));
            bannerLbl_->setText("Metadaten übernommen: " +
                QString::fromStdString(al->artist + " — " + al->title));
            logChain("Manuelle Suche: übernommen — " +
                QString::fromStdString(al->artist + " — " + al->title) +
                " (" + QString::number(al->tracks.size()) + " Tracks)" +
                (cov.empty() ? ", kein Cover" : ", Cover ok"));
        }, Qt::QueuedConnection);
    });
}

// AcoustID/Chromaprint: Track 1 kurz lesen, am Klang erkennen.
void MainWindow::onIdentifyAcoustID() {
    if (ctl_->running()) {
        QMessageBox::information(this, "AcoustID",
            "Während ein Lauf aktiv ist nicht möglich."); return;
    }
    if (cfg_.acoustid_key.empty()) {
        msgWide(this, QMessageBox::Warning, "AcoustID",
            "Kein AcoustID-API-Key gesetzt. Kostenlos auf acoustid.org/"
            "new-application holen und in Einstellungen eintragen.", 480);
        return;
    }
    if (metaBusy_.load() || scanBusy_.load() || previewBusy_.load()) {
        QMessageBox::information(this, "AcoustID",
            "Laufwerk/Erkennung gerade belegt — kurz warten."); return;
    }
    if (metaThr_.joinable()) metaThr_.join();
    metaBusy_ = true; scanBusy_ = true;       // discWatch pausieren (Drive)
    bannerLbl_->setText("Lese Track 1 für die Klang-Erkennung …");
    logChain("AcoustID: lese Track 1 für den Fingerprint …");
    std::string dev = cfg_.device, ua = cfg_.mb_useragent,
                tmp = cfg_.tmpdir, key = cfg_.acoustid_key;
    metaThr_ = std::thread([this, dev, ua, tmp, key] {
        std::optional<cdr::Album> al;
        std::string note, cov;
        auto clog = [this](QString s) {
            QMetaObject::invokeMethod(this, [this, s] { logChain(s); },
                                      Qt::QueuedConnection);
        };
        try {
            int wantTracks = 0;
            try { wantTracks = cdr::read_disc_ident(dev).toc_tracks; }
            catch (...) {}
            cdr::Ripper rip(dev, 0, true);
            if (rip.tracks().empty())
                throw std::runtime_error("keine Audiotracks");
            fs::path dir = fs::path(tmp) / "acoustid";
            std::error_code ec; fs::create_directories(dir, ec);
            fs::path wav = dir / "t1.wav";
            rip.rip(rip.tracks()[0], wav, [](double) {}, [] { return false; });
            clog("AcoustID: Track 1 gelesen, berechne Chromaprint …");
            auto hit = cdr::acoustid_identify(wav, 0, key, ua);
            std::error_code e2; fs::remove(wav, e2);
            if (!hit)
                clog("AcoustID: kein Fingerprint-Treffer (oder Key/Netz).");
            if (hit) {
                clog(QString("AcoustID: Treffer (score %1) — Release-MBID %2")
                     .arg(hit->score, 0, 'f', 2)
                     .arg(hit->mb_release_id.empty()
                          ? QString("—") :
                          QString::fromStdString(hit->mb_release_id)));
                if (!hit->mb_release_id.empty())
                    al = cdr::mb_release_by_id(hit->mb_release_id,
                                               wantTracks, ua);
                if (!al && !hit->recording.empty()) {
                    cdr::Album a;
                    a.artist = hit->artist;
                    a.title  = hit->recording;
                    al = a;
                    note = " (nur Recording erkannt — Album ggf. anpassen)";
                }
                if (al && !al->mb_release_id.empty()) {
                    try {
                        fs::path cd = fs::path(tmp) / "preview";
                        fs::create_directories(cd, ec);
                        fs::path o;
                        if (cdr::fetch_cover(al->mb_release_id, ua, cd, o))
                            cov = o.string();
                    } catch (...) {}
                }
            }
        } catch (const std::exception& e) {
            note = std::string(" [") + e.what() + "]";
        }
        QMetaObject::invokeMethod(this, [this, al, cov, note] {
            metaBusy_ = false; scanBusy_ = false;
            if (!al) {
                bannerLbl_->setText("AcoustID: nicht erkannt" +
                                    QString::fromStdString(note));
                logChain("AcoustID: nicht erkannt" +
                         QString::fromStdString(note));
                return;
            }
            QStringList ti, ar;
            for (auto& t : al->tracks) {
                ti << QString::fromStdString(t.title);
                ar << QString::fromStdString(t.artist);
            }
            onAlbumReady(QString::fromStdString(al->artist),
                         QString::fromStdString(al->title),
                         QString::fromStdString(al->year()), ti, ar);
            if (!cov.empty()) onCoverReady(QString::fromStdString(cov));
            bannerLbl_->setText("AcoustID erkannt: " +
                QString::fromStdString(al->artist + " — " + al->title) +
                QString::fromStdString(note));
            logChain("AcoustID: erkannt — " +
                QString::fromStdString(al->artist + " — " + al->title) +
                " (" + QString::number(al->tracks.size()) + " Tracks)" +
                (cov.empty() ? ", kein Cover" : ", Cover ok") +
                QString::fromStdString(note));
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onShowArchive() {
    auto entries = cdr::load_archive();          // alt→neu
    std::reverse(entries.begin(), entries.end()); // neueste oben
    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Archiv / Zustand");
    dlg->resize(940, 520);
    auto* t = new QTableWidget((int)entries.size(), 8, dlg);
    t->setHorizontalHeaderLabels({ "Datum", "Art", "Interpret — Album",
        "Jahr", "Tracks", "Format", "AccurateRip", "Zustand" });
    t->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->verticalHeader()->setVisible(false);
    auto qname = [](int q, bool done) {
        if (!done) return QString("Scan unvollst.");
        return q == 0 ? QString("sauber")
             : q == 1 ? QString("grenzwertig") : QString("stark zerkratzt");
    };
    auto qcol = [](int q, bool done) {
        if (!done) return QColor("#e0a83e");
        return q == 0 ? QColor("#27ae60")
             : q == 1 ? QColor("#e0a83e") : QColor("#c0392b");
    };
    for (int i = 0; i < (int)entries.size(); ++i) {
        const auto& e = entries[i];
        QString dt = QDateTime::fromSecsSinceEpoch(e.ts)
                         .toString("yyyy-MM-dd HH:mm");
        QString al = (e.artist.empty() && e.title.empty())
            ? QString("(unbekannt)")
            : QString::fromStdString(e.artist + " — " + e.title);
        QString ar = e.ar_total > 0
            ? QString("%1/%2").arg(e.ar_ok).arg(e.ar_total) : QString("—");
        bool hasScan = !e.map.empty() || e.kind == "scan";
        QString zu = qname(e.quality, e.scan_completed && hasScan);
        if (!hasScan && e.kind == "rip") zu = "kein Scan";
        QString vals[8] = {
            dt, e.kind == "scan" ? "Scan" : "Rip", al,
            QString::fromStdString(e.year),
            e.tracks ? QString::number(e.tracks) : QString("—"),
            e.kind == "scan" ? QString("—") : QString::fromStdString(e.format),
            ar, zu };
        for (int c = 0; c < 8; ++c) {
            auto* it = new QTableWidgetItem(vals[c]);
            if (c == 7 && (hasScan || e.kind == "scan"))
                it->setForeground(qcol(e.quality,
                                       e.scan_completed && hasScan));
            t->setItem(i, c, it);
        }
    }
    connect(t, &QTableWidget::cellDoubleClicked, dlg,
        [this, entries](int row, int) {
            if (row < 0 || row >= (int)entries.size()) return;
            const auto& e = entries[row];
            cdr::ProbeResult pr;
            pr.quality = (cdr::DiscQuality)e.quality;
            pr.lba_min = e.lba_min; pr.lba_max = e.lba_max;
            pr.map = e.map; pr.completed = e.scan_completed;
            auto* d = new QDialog(this);
            d->setWindowTitle("Zustandsbeurteilung");
            d->resize(460, 600);
            auto* v = new QVBoxLayout(d);
            QString q = (e.map.empty() && e.kind == "rip")
                ? QString("ohne Preflight-Scan")
                : (e.quality == 0 ? "SAUBER"
                   : e.quality == 1 ? "GRENZWERTIG" : "STARK ZERKRATZT");
            QString head = QString(
                "<b>%1</b><br>%2 — %3<br>"
                "Disc-Zustand: <b>%4</b>%5<br>"
                "AccurateRip %6 · %7 Track(s) beschädigt · "
                "%8 · %9")
                .arg(e.artist.empty() && e.title.empty()
                     ? QString("(unbekannte Disc)")
                     : QString::fromStdString(e.artist + " — " + e.title))
                .arg(QDateTime::fromSecsSinceEpoch(e.ts)
                         .toString("yyyy-MM-dd HH:mm"))
                .arg(e.kind == "scan" ? "Scan" : "Rip")
                .arg(q)
                .arg(e.scan_completed ? QString()
                     : QString(" <span style='color:#e0a83e'>"
                               "(Scan unvollständig)</span>"))
                .arg(e.ar_total > 0
                     ? QString("%1/%2").arg(e.ar_ok).arg(e.ar_total) : "—")
                .arg(e.damaged_tracks)
                .arg(e.kind == "scan" ? QString("nur Scan")
                     : QString::fromStdString(e.format))
                .arg(QString::fromStdString(
                     e.outcome.empty() ? "—" : e.outcome));
            auto* lbl = new QLabel(head); lbl->setWordWrap(true);
            v->addWidget(lbl);
            auto* sc = new DiscScanWidget(d);
            sc->setResult(pr);
            v->addWidget(sc, 1);
            v->addWidget(new QLabel(QString::fromUtf8(
                "<small>Außen = Disc-Rand, innen = Anfang. "
                "<span style='color:#27ae60'>■</span> ok &nbsp; "
                "<span style='color:#e0a83e'>■</span> langsam &nbsp; "
                "<span style='color:#c0392b'>■</span> Lesefehler</small>")));
            d->setAttribute(Qt::WA_DeleteOnClose);
            d->show();
        });
    auto* close = new QPushButton("Schließen");
    connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    auto* lay = new QVBoxLayout(dlg);
    lay->addWidget(new QLabel(QString::fromUtf8(
        "Jeder Rip und jeder Scan wird protokolliert (Verlauf). "
        "Doppelklick auf eine Zeile → Zustandsbeurteilung mit Ring-Grafik.")));
    lay->addWidget(t, 1);
    lay->addWidget(close);
    dlg->exec();
    dlg->deleteLater();
}

// ───────────────────────────── SettingsDialog ─────────────────────────────────

static QString S(const std::string& s) { return QString::fromStdString(s); }

// Lese-Speed als Dropdown: Label → read_speed-Int (0 = Maximum/Default).
// Niedrigere Werte = bessere Fehlerkorrektur bei zerkratzten CDs.
static void fillSpeedCombo(QComboBox* cb) {
    static const struct { const char* lbl; int v; } kSpeeds[] = {
        { "Maximum (Standard)",        0  },
        { "48× — schnell",            48 },
        { "32×",                      32 },
        { "24×",                      24 },
        { "16×",                      16 },
        { "12×",                      12 },
        { "10×",                      10 },
        { "8× — bessere Recovery",     8 },
        { "4× — beste Recovery",       4 },
        { "2×",                        2 },
        { "1× — sehr langsam",         1 },
    };
    for (const auto& s : kSpeeds)
        cb->addItem(QString::fromUtf8(s.lbl), s.v);
}
static void selectSpeed(QComboBox* cb, int v) {
    for (int i = 0; i < cb->count(); ++i)
        if (cb->itemData(i).toInt() == v) { cb->setCurrentIndex(i); return; }
    // Hand-editierter Sonderwert aus der config.ini nicht verwerfen.
    cb->addItem(QString::number(v) + "× (eigen)", v);
    cb->setCurrentIndex(cb->count() - 1);
}

// Audio-Format/Preset: bündelt Encoder + Qualität für typische Use-Cases.
// userData = "format|quality" (z.B. "flac|8", "opus|128", "mp3|0").
struct AudioPreset { const char* lbl; const char* fmt; int q; };
static const AudioPreset kPresets[] = {
    { "FLAC — Archiv, verlustfrei (Standard)",            "flac", 8   },
    { "FLAC — verlustfrei, schnellerer Encode",          "flac", 3   },
    { "Opus 128 kbit/s — Streaming / Handy",             "opus", 128 },
    { "Opus 96 kbit/s — sehr platzsparend",              "opus", 96  },
    { "MP3 V0 — beste MP3-Qualität (~245k)",             "mp3",  0   },
    { "MP3 V2 — klein & überall, Auto/Altgeräte (~190k)", "mp3", 2   },
};
static void fillPresetCombo(QComboBox* cb) {
    for (const auto& p : kPresets)
        cb->addItem(QString::fromUtf8(p.lbl),
                    QString("%1|%2").arg(p.fmt).arg(p.q));
}
static void selectPreset(QComboBox* cb, const std::string& fmt, int q) {
    QString key = QString("%1|%2").arg(QString::fromStdString(fmt)).arg(q);
    for (int i = 0; i < cb->count(); ++i)
        if (cb->itemData(i).toString() == key) { cb->setCurrentIndex(i); return; }
    // Aus config.ini hand-editierte Kombination nicht verwerfen.
    cb->addItem(QString::fromStdString(fmt) + " q" + QString::number(q) +
                " (eigen)", key);
    cb->setCurrentIndex(cb->count() - 1);
}

SettingsDialog::SettingsDialog(const cdr::Config& c, QString cfgPath,
                               QWidget* parent)
    : QDialog(parent), base_(c), cfgPath_(std::move(cfgPath)) {
    setWindowTitle("Einstellungen");
    resize(900, 560);
    setMinimumSize(760, 480);

    auto* nav   = new QListWidget;
    auto* pages = new QStackedWidget;
    nav->setMinimumWidth(210);
    nav->setMaximumWidth(240);
    for (const char* n : { "Laufwerk & Rip", "Metadaten / Pfade",
                           "Upload", "AccurateRip", "Offset-Registry" })
        nav->addItem(n);

    // Seite 1 — Laufwerk & Rip
    {
        auto* w = new QWidget; auto* f = new QFormLayout(w);
        device_     = new QComboBox;
        device_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        device_->setToolTip("Laufwerk für die Kalibrierung wählen.");
        {   // Alle erkannten Laufwerke + Hersteller/Modell; das konfigurierte
            // vorwählen (auch wenn es gerade nicht angeschlossen ist).
            auto devs = cdr::list_optical_devices();
            bool have_cfg = false;
            for (const auto& d : devs) {
                QString label = QString::fromStdString(d);
                cdr::HwInfo hw = cdr::drive_hwinfo(d);
                if (hw.ok) {
                    QString m = QString::fromStdString(
                        (hw.vendor + " " + hw.model)).trimmed();
                    if (!m.isEmpty()) label += "  ·  " + m;
                }
                device_->addItem(label, QString::fromStdString(d));
                if (d == c.device) have_cfg = true;
            }
            if (!have_cfg && !c.device.empty())
                device_->addItem(S(c.device) + "  (nicht verbunden)",
                                 S(c.device));
            int ix = device_->findData(S(c.device));
            device_->setCurrentIndex(ix >= 0 ? ix : 0);
        }
        readSpeed_  = new QComboBox;
        fillSpeedCombo(readSpeed_);
        selectSpeed(readSpeed_, c.read_speed);
        preset_ = new QComboBox;
        fillPresetCombo(preset_);
        selectPreset(preset_, c.audio_format, c.audio_quality);
        replaygain_ = new QCheckBox("ReplayGain/R128-Tags schreiben (rsgain)");
        replaygain_->setChecked(c.replaygain);
        jukebox_    = new QCheckBox("Jukebox: Auto-Start bei Disc-Insert");
        jukebox_->setChecked(c.jukebox);
        fastRip_    = new QCheckBox("Schnell-Rip (FULL nur bei Lesefehlern)");
        fastRip_->setChecked(c.fast_rip);
        preflight_  = new QCheckBox(
            "Preflight: Disc vor dem Rip scannen, Speed/Modus automatisch");
        preflight_->setChecked(c.preflight);
        scanDensity_ = new QComboBox;
        scanDensity_->addItem("grob (3/Track, schnell)", 3);
        scanDensity_->addItem("normal (6/Track)", 6);
        scanDensity_->addItem("fein (12/Track, langsam)", 12);
        { int di = 1;
          if (c.scan_density <= 3) di = 0;
          else if (c.scan_density >= 12) di = 2;
          scanDensity_->setCurrentIndex(di); }
        recoveryBudget_ = new QSpinBox;
        recoveryBudget_->setRange(0, 60);
        recoveryBudget_->setSuffix(" Min");
        recoveryBudget_->setSpecialValueText("aus (unbegrenzt)");
        recoveryBudget_->setValue(c.recovery_budget_min);
        autoEject_  = new QCheckBox("CD nach Fertigstellung auswerfen");
        autoEject_->setChecked(c.auto_eject);
        chime_      = new QCheckBox("Ton bei Disc fertig");
        chime_->setChecked(c.chime);
        lyrics_     = new QCheckBox("Synced Lyrics (LRCLIB) → .lrc-Sidecar");
        lyrics_->setChecked(c.lyrics);
        overwrite_  = new QCheckBox(
            "Vorhandene Dateien überschreiben (sonst überspringen)");
        overwrite_->setChecked(c.overwrite_existing);
        f->addRow("Laufwerk:", device_);
        f->addRow("Lese-Speed:", readSpeed_);
        f->addRow("Audio-Format:", preset_);
        f->addRow("", replaygain_);
        f->addRow("", preflight_);
        f->addRow("Scan-Dichte:", scanDensity_);
        f->addRow("Recovery-Budget/Track:", recoveryBudget_);
        f->addRow("", fastRip_);
        f->addRow("", jukebox_);
        f->addRow("", autoEject_);
        f->addRow("", chime_);
        f->addRow("", lyrics_);
        f->addRow("", overwrite_);
        pages->addWidget(w);
    }
    // Seite 2 — Metadaten / Pfade
    {
        auto* w = new QWidget; auto* f = new QFormLayout(w);
        ua_        = new QLineEdit(S(c.mb_useragent));
        tmpdir_    = new QLineEdit(S(c.tmpdir));
        musicRoot_ = new QLineEdit(S(c.music_root));
        acoustidKey_ = new QLineEdit(S(c.acoustid_key));
        discogsToken_ = new QLineEdit(S(c.discogs_token));
        discogsToken_->setEchoMode(QLineEdit::PasswordEchoOnEdit);
        f->addRow("MusicBrainz UA:", ua_);
        f->addRow("AcoustID-Key:", acoustidKey_);
        f->addRow("Discogs-Token:", discogsToken_);
        f->addRow("Temp-Verzeichnis:", tmpdir_);
        f->addRow("Zielordner (music_root):", musicRoot_);
        pages->addWidget(w);
    }
    // Seite 3 — Upload (Backend-Auswahl + backend-spezifische Felder)
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* top = new QFormLayout;
        backend_  = new QComboBox;
        backend_->addItems({ "webdav", "local", "ssh", "smb" });
        int bi = 0;
        if (c.upload_backend == "local") bi = 1;
        else if (c.upload_backend == "ssh") bi = 2;
        else if (c.upload_backend == "smb") bi = 3;
        backend_->setCurrentIndex(bi);
        retries_ = new QSpinBox; retries_->setRange(1, 10);
        retries_->setValue(c.upload_retries < 1 ? 1 : c.upload_retries);
        top->addRow("Backend:", backend_);
        top->addRow("Upload-Retries:", retries_);
        v->addLayout(top);

        backendPages_ = new QStackedWidget;
        { auto* p = new QWidget; auto* g = new QFormLayout(p);
          ncUrl_  = new QLineEdit(S(c.nextcloud_url));
          ncUser_ = new QLineEdit(S(c.webdav_user));
          ncPass_ = new QLineEdit(S(c.webdav_pass));
          ncPass_->setEchoMode(QLineEdit::Password);
          g->addRow("Nextcloud-URL:", ncUrl_);
          g->addRow("User:", ncUser_);
          g->addRow("App-Passwort:", ncPass_);
          backendPages_->addWidget(p); }
        { auto* p = new QWidget; auto* g = new QFormLayout(p);
          localBase_ = new QLineEdit(S(c.local_base));
          g->addRow("Zielbasis (Pfad/Mount):", localBase_);
          backendPages_->addWidget(p); }
        { auto* p = new QWidget; auto* g = new QFormLayout(p);
          sshHost_ = new QLineEdit(S(c.ssh_host));
          sshUser_ = new QLineEdit(S(c.ssh_user));
          sshBase_ = new QLineEdit(S(c.ssh_base));
          sshPort_ = new QSpinBox; sshPort_->setRange(1, 65535);
          sshPort_->setValue(c.ssh_port);
          g->addRow("Host:", sshHost_);
          g->addRow("User:", sshUser_);
          g->addRow("Basispfad:", sshBase_);
          g->addRow("Port:", sshPort_);
          backendPages_->addWidget(p); }
        { auto* p = new QWidget; auto* g = new QFormLayout(p);
          smbUrl_  = new QLineEdit(S(c.smb_url));
          smbUser_ = new QLineEdit(S(c.smb_user));
          smbPass_ = new QLineEdit(S(c.smb_pass));
          smbPass_->setEchoMode(QLineEdit::Password);
          g->addRow("smb://host/share/Basis:", smbUrl_);
          g->addRow("User:", smbUser_);
          g->addRow("Passwort:", smbPass_);
          backendPages_->addWidget(p); }
        backendPages_->setCurrentIndex(bi);
        connect(backend_,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            backendPages_, &QStackedWidget::setCurrentIndex);
        v->addWidget(backendPages_);
        v->addStretch();
        pages->addWidget(w);
    }
    // Seite 4 — AccurateRip
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* f = new QFormLayout;
        accuraterip_ = new QCheckBox("AccurateRip-Prüfung aktiv");
        accuraterip_->setChecked(c.accuraterip);
        readOffset_  = new QSpinBox;
        readOffset_->setRange(-2000, 2000);
        readOffset_->setValue(c.read_offset);
        readOffset_->setToolTip("Manueller Fallback. Pro Laufwerk kalibrierte "
                                "Werte haben Vorrang (drive_offsets.ini).");
        // Eigene Laufwerks-Auswahl fürs Kalibrieren — vorher stand hier nur
        // ein Label mit dem auf Seite 1 gewählten Gerät, sodass man das
        // Kalibrier-Laufwerk hier gar nicht wechseln konnte.
        calibDev_ = new QComboBox;
        calibDev_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        {
            auto devs = cdr::list_optical_devices();
            for (const auto& d : devs) {
                QString label = QString::fromStdString(d);
                cdr::HwInfo hw = cdr::drive_hwinfo(d);
                if (hw.ok) {
                    QString m = QString::fromStdString(
                        (hw.vendor + " " + hw.model)).trimmed();
                    if (!m.isEmpty()) label += "  ·  " + m;
                }
                calibDev_->addItem(label, QString::fromStdString(d));
            }
            if (calibDev_->count() == 0 && !c.device.empty())
                calibDev_->addItem(S(c.device), S(c.device));
            int ix = calibDev_->findData(S(c.device));
            calibDev_->setCurrentIndex(ix >= 0 ? ix : 0);
        }
        driveLbl_ = new QLabel;
        driveLbl_->setWordWrap(true);
        calibrateBtn_ = new QPushButton("Gewähltes Laufwerk jetzt kalibrieren…");
        f->addRow("", accuraterip_);
        f->addRow("Manueller Offset:", readOffset_);
        f->addRow("Laufwerk:", calibDev_);
        f->addRow("", driveLbl_);
        // Laufwerkswechsel → Info + Tabellen-Markierung aktualisieren.
        connect(calibDev_, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { refreshDriveInfo(); populateDriveTable(); });
        v->addLayout(f);
        v->addWidget(calibrateBtn_);
        v->addWidget(new QLabel(
            "<small>Kalibrierung rippt einmal eine eingelegte <b>gängige "
            "Mainstream-CD</b> und ermittelt den Drive-Offset gegen die "
            "AccurateRip-DB. Ergebnis wird pro Laufwerk in "
            "drive_offsets.ini gespeichert (portabel).</small>"));

        v->addWidget(new QLabel("<b>Kalibrierte Laufwerke</b>"));
        driveTbl_ = new QTableWidget(0, 4);
        driveTbl_->setHorizontalHeaderLabels(
            { "Hersteller", "Modell", "Drive-ID (Schlüssel)", "Offset" });
        driveTbl_->horizontalHeader()->setSectionResizeMode(
            2, QHeaderView::Stretch);
        driveTbl_->setSelectionBehavior(QAbstractItemView::SelectRows);
        driveTbl_->setSelectionMode(QAbstractItemView::SingleSelection);
        driveTbl_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        driveTbl_->verticalHeader()->setVisible(false);
        driveTbl_->setMinimumHeight(140);
        v->addWidget(driveTbl_, 1);
        delDriveBtn_ = new QPushButton(
            "Markierten Eintrag löschen (Neu-Kalibrierung ermöglichen)");
        v->addWidget(delDriveBtn_);
        connect(delDriveBtn_, &QPushButton::clicked,
                this, &SettingsDialog::onDeleteDrive);

        connect(calibrateBtn_, &QPushButton::clicked,
                this, &SettingsDialog::onCalibrate);
        pages->addWidget(w);
        refreshDriveInfo();
        populateDriveTable();
    }

    // Seite 5 — Offset-Registry (Cluster-App; Privacy: Upload-Flags Opt-in)
    {
        auto* w = new QWidget; auto* v = new QVBoxLayout(w);
        auto* f = new QFormLayout;
        regUrl_ = new QLineEdit(S(c.registry_url));
        regUrl_->setPlaceholderText("https://or1-9c4k.x2-pandora.de  (leer = aus)");
        regSubmit_ = new QCheckBox(
            "Eigenen kalibrierten Offset teilen (nur AccurateRip-bestätigt)");
        regSubmit_->setChecked(c.registry_submit);
        regStats_  = new QCheckBox("Anonyme Rip-Statistik melden");
        regStats_->setChecked(c.registry_stats);
        regCondition_ = new QCheckBox(
            "CD-Zustand + Cover für den Zensus teilen (Album-ID wird geteilt!)");
        regCondition_->setChecked(c.registry_condition);
        f->addRow("Registry-URL:", regUrl_);
        f->addRow("", regSubmit_);
        f->addRow("", regStats_);
        f->addRow("", regCondition_);
        v->addLayout(f);
        v->addWidget(new QLabel(
            "<small>Bei gesetzter URL wird ein <b>fehlender</b> lokaler "
            "Laufwerks-Offset automatisch aus dem Registry-Konsens geholt "
            "(reiner Lookup). Hochladen passiert nur mit den Häkchen oben — "
            "beide standardmäßig aus. Übertragen werden ausschließlich "
            "Laufwerksmodell, Offset und anonyme Zähler, <b>keine</b> "
            "Album-/Track-Daten.</small>"));
        v->addStretch(1);
        pages->addWidget(w);
    }

    connect(nav, &QListWidget::currentRowChanged,
            pages, &QStackedWidget::setCurrentIndex);
    nav->setCurrentRow(0);

    // ── Hilfe-Kasten unten: erklärt die überfahrene/fokussierte Option ─
    help_ = new QLabel(QString::fromUtf8(
        "Bewege den Mauszeiger über eine Einstellung für eine Erklärung."));
    help_->setWordWrap(true);
    help_->setMinimumHeight(54);
    help_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    help_->setObjectName("settingsHelp");
    help_->setStyleSheet(
        "#settingsHelp{background:#1f2430;border:1px solid #3a3f4b;"
        "border-radius:6px;padding:8px 10px;color:#cfd3da;}");

    setHelp(device_,     QString::fromUtf8(
        "Pfad zum CD-Laufwerk (z. B. /dev/sr0). Bei mehreren Laufwerken "
        "hier das gewünschte angeben."));
    setHelp(readSpeed_,  QString::fromUtf8(
        "Lesegeschwindigkeit. „Maximum“ ist am schnellsten; ein niedriger "
        "Wert (4–8×) liest zerkratzte CDs zuverlässiger (mehr "
        "Fehlerkorrektur, dafür langsamer)."));
    setHelp(preset_,     QString::fromUtf8(
        "Audio-Format + Qualität als Preset. FLAC = verlustfrei fürs Archiv "
        "(Standard, Navidrome transkodiert beim Streamen selbst). Opus = "
        "kleiner bei sehr guter Qualität, ideal fürs Handy. MP3 = maximale "
        "Geräte-Kompatibilität (Auto, Altgeräte)."));
    setHelp(replaygain_, QString::fromUtf8(
        "Schreibt ReplayGain/R128-Lautstärke-Tags (via rsgain), damit alle "
        "Tracks gleich laut wiedergegeben werden."));
    setHelp(jukebox_,    QString::fromUtf8(
        "Startet den Rip automatisch, sobald eine Disc erkannt wird — "
        "kein Klick nötig."));
    setHelp(fastRip_,    QString::fromUtf8(
        "Erst ein schneller Lese-Durchgang; nur Tracks mit Lesefehlern "
        "werden langsam neu gelesen. ~3× schneller bei sauberen CDs."));
    setHelp(scanDensity_, QString::fromUtf8(
        "Stichproben pro Track beim Scan. Mehr = feinere Defekt-Karte, "
        "aber langsamer (besonders auf zähen Discs)."));
    setHelp(recoveryBudget_, QString::fromUtf8(
        "Zeitlimit pro Track beim Rip. Ein stark zerkratzter Track der "
        "trotz Fortschritt endlos grindet wird nach dieser Zeit "
        "übersprungen (als defekt gemerkt → nächster Rip dieser Disc "
        "lässt ihn gleich ganz zum Schluss). 0 = unbegrenzt."));
    setHelp(preflight_,  QString::fromUtf8(
        "Scannt die Disc vor dem Rip kurz (Stichproben) und wählt Speed + "
        "Modus automatisch: sauber → schnell; zerkratzt → langsam + volle "
        "Fehlerkorrektur + Warnung. Der Scan ist Watchdog-geschützt."));
    setHelp(autoEject_,  QString::fromUtf8(
        "Wirft die CD nach Fertigstellung automatisch aus."));
    setHelp(chime_,      QString::fromUtf8(
        "Spielt einen Ton, wenn die Disc fertig ist."));
    setHelp(lyrics_,     QString::fromUtf8(
        "Lädt synchronisierte Songtexte (LRCLIB) und legt eine .lrc-Datei "
        "neben den Track."));
    setHelp(overwrite_,  QString::fromUtf8(
        "Überschreibt am Ziel bereits vorhandene Dateien. Aus = vorhandene "
        "Tracks werden übersprungen (gut für Box-Sets / fortgesetzte Rips)."));
    setHelp(ua_,         QString::fromUtf8(
        "User-Agent für MusicBrainz-Anfragen — sollte eine Kontaktadresse "
        "enthalten (MusicBrainz-Richtlinie)."));
    setHelp(acoustidKey_, QString::fromUtf8(
        "AcoustID-API-Key (kostenlos: acoustid.org/new-application). "
        "Aktiviert 'Titel per Klang erkennen': identifiziert die Songs am "
        "akustischen Fingerprint, auch wenn die Disc in keiner TOC-DB steht."));
    setHelp(tmpdir_,     QString::fromUtf8(
        "Arbeitsverzeichnis für die temporären WAV/FLAC-Dateien während "
        "des Rippens."));
    setHelp(musicRoot_,  QString::fromUtf8(
        "Zielordner (relativ zur Upload-Basis), unter dem die Alben "
        "angelegt werden."));
    setHelp(backend_,    QString::fromUtf8(
        "Upload-Ziel: webdav (Nextcloud), local (Pfad/Mount), ssh (scp) "
        "oder smb."));
    setHelp(retries_,    QString::fromUtf8(
        "Wiederholversuche pro Datei bei einem Upload-/Netzfehler, bevor "
        "der Track als Fehler gilt."));
    setHelp(ncUrl_,      QString::fromUtf8(
        "Basis-URL des Nextcloud/WebDAV-Servers."));
    setHelp(ncUser_,     QString::fromUtf8(
        "WebDAV-Benutzername (Nextcloud-Anmeldename)."));
    setHelp(ncPass_,     QString::fromUtf8(
        "WebDAV-App-Passwort (NICHT das Login-Passwort)."));
    setHelp(localBase_,  QString::fromUtf8(
        "Basis-Zielpfad für das „local“-Backend (z. B. /mnt/music)."));
    setHelp(sshHost_,    QString::fromUtf8(
        "SSH-Host für das „ssh“-Backend (scp-Upload)."));
    setHelp(sshUser_,    QString::fromUtf8(
        "SSH-Benutzer (leer = aktueller Benutzer)."));
    setHelp(sshBase_,    QString::fromUtf8("Basispfad auf dem SSH-Host."));
    setHelp(sshPort_,    QString::fromUtf8("SSH-Port (Standard 22)."));
    setHelp(smbUrl_,     QString::fromUtf8(
        "SMB-Ziel als smb://host/share/Basis."));
    setHelp(smbUser_,    QString::fromUtf8("SMB-Benutzer (leer = Gast)."));
    setHelp(smbPass_,    QString::fromUtf8("SMB-Passwort."));
    setHelp(accuraterip_,QString::fromUtf8(
        "Vergleicht die Rips mit der AccurateRip-Datenbank, um bit-genaue "
        "Korrektheit zu bestätigen."));
    setHelp(readOffset_, QString::fromUtf8(
        "Manueller Laufwerks-Offset (Samples). Pro Laufwerk kalibrierte "
        "Werte haben Vorrang."));
    setHelp(calibrateBtn_, QString::fromUtf8(
        "Rippt einmal eine gängige Mainstream-CD und ermittelt den "
        "Laufwerks-Offset gegen AccurateRip."));
    setHelp(regUrl_,     QString::fromUtf8(
        "Adresse der Offset-Registry. Leer = aus. Ein fehlender lokaler "
        "Offset wird sonst aus dem Konsens geholt (reiner Lookup)."));
    setHelp(regSubmit_,  QString::fromUtf8(
        "Teilt den eigenen, per AccurateRip bestätigten Offset mit der "
        "Registry (Opt-in)."));
    setHelp(regStats_,   QString::fromUtf8(
        "Meldet anonyme Rip-Statistik (Modell, Version, Erfolg) — keine "
        "Album-/Track-Daten."));
    setHelp(regCondition_, QString::fromUtf8(
        "CD-Zustands-Zensus (opt-in): teilt Album-Identität (Disc-ID, "
        "Interpret/Album/Jahr), Zustand und ein kleines Cover-Thumbnail. "
        "Anders als die anderen Optionen ist die CD damit identifiziert "
        "(Zweck: aus mehreren Quellen den Zustand & noch existierende "
        "Pressungen dokumentieren). Keine Personendaten."));

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                    QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Profil-Leiste oben
    profile_ = new QComboBox;
    profile_->addItem("Standard");
    for (const auto& p : cdr::list_profiles())
        profile_->addItem(QString::fromStdString(p));
    QString act = QString::fromStdString(cdr::active_profile());
    profile_->setCurrentText(act.isEmpty() ? "Standard" : act);
    auto* newProfBtn = new QPushButton("Neues Profil…");
    auto* prow = new QHBoxLayout;
    prow->addWidget(new QLabel("Profil:"));
    prow->addWidget(profile_, 1);
    prow->addWidget(newProfBtn);
    connect(profile_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onProfileChanged);
    connect(newProfBtn, &QPushButton::clicked,
            this, &SettingsDialog::onNewProfile);

    auto* body = new QHBoxLayout;
    body->addWidget(nav);
    body->addWidget(pages, 1);
    auto* root = new QVBoxLayout(this);
    root->addLayout(prow);
    root->addLayout(body, 1);
    root->addWidget(help_);
    root->addWidget(bb);
}

void SettingsDialog::setHelp(QWidget* w, const QString& text) {
    if (!w) return;
    helpText_.insert(w, text);
    w->installEventFilter(this);
    w->setToolTip(text);
}

bool SettingsDialog::eventFilter(QObject* obj, QEvent* ev) {
    if (help_ && (ev->type() == QEvent::Enter ||
                  ev->type() == QEvent::FocusIn)) {
        auto it = helpText_.constFind(obj);
        if (it != helpText_.constEnd()) help_->setText(it.value());
    }
    return QDialog::eventFilter(obj, ev);
}

QString SettingsDialog::selectedProfile() const {
    QString p = profile_->currentText();
    return (p == "Standard") ? QString() : p;
}

void SettingsDialog::applyConfig(const cdr::Config& c) {
    { int ix = device_->findData(S(c.device));
      if (ix >= 0) device_->setCurrentIndex(ix); }
    selectSpeed(readSpeed_, c.read_speed);
    selectPreset(preset_, c.audio_format, c.audio_quality);
    replaygain_->setChecked(c.replaygain);
    fastRip_->setChecked(c.fast_rip);
    preflight_->setChecked(c.preflight);
    scanDensity_->setCurrentIndex(c.scan_density <= 3 ? 0
                                : c.scan_density >= 12 ? 2 : 1);
    recoveryBudget_->setValue(c.recovery_budget_min);
    jukebox_->setChecked(c.jukebox);
    autoEject_->setChecked(c.auto_eject);
    chime_->setChecked(c.chime);
    lyrics_->setChecked(c.lyrics);
    overwrite_->setChecked(c.overwrite_existing);
    ua_->setText(S(c.mb_useragent));
    acoustidKey_->setText(S(c.acoustid_key));
    discogsToken_->setText(S(c.discogs_token));
    tmpdir_->setText(S(c.tmpdir));
    musicRoot_->setText(S(c.music_root));
    int bi = 0;
    if (c.upload_backend == "local") bi = 1;
    else if (c.upload_backend == "ssh") bi = 2;
    else if (c.upload_backend == "smb") bi = 3;
    backend_->setCurrentIndex(bi);
    backendPages_->setCurrentIndex(bi);
    retries_->setValue(c.upload_retries < 1 ? 1 : c.upload_retries);
    ncUrl_->setText(S(c.nextcloud_url));
    ncUser_->setText(S(c.webdav_user));
    ncPass_->setText(S(c.webdav_pass));
    localBase_->setText(S(c.local_base));
    sshHost_->setText(S(c.ssh_host));
    sshUser_->setText(S(c.ssh_user));
    sshBase_->setText(S(c.ssh_base));
    sshPort_->setValue(c.ssh_port);
    smbUrl_->setText(S(c.smb_url));
    smbUser_->setText(S(c.smb_user));
    smbPass_->setText(S(c.smb_pass));
    accuraterip_->setChecked(c.accuraterip);
    readOffset_->setValue(c.read_offset);
    regUrl_->setText(S(c.registry_url));
    regSubmit_->setChecked(c.registry_submit);
    regStats_->setChecked(c.registry_stats);
    regCondition_->setChecked(c.registry_condition);
}

void SettingsDialog::onProfileChanged(int) {
    std::string path = cdr::profile_path(selectedProfile().toStdString());
    applyConfig(cdr::load_config(path));
}

void SettingsDialog::onNewProfile() {
    bool ok = false;
    QString name = QInputDialog::getText(this, "Neues Profil",
        "Profilname:", QLineEdit::Normal, "", &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty() || name == "Standard") return;
    if (profile_->findText(name) < 0) profile_->addItem(name);
    profile_->blockSignals(true);
    profile_->setCurrentText(name);     // aktuelle Felder bleiben → bei OK gespeichert
    profile_->blockSignals(false);
}

cdr::Config SettingsDialog::config() const {
    cdr::Config c = base_;
    c.device       = device_->currentData().toString().toStdString();
    c.read_speed   = readSpeed_->currentData().toInt();
    {
        const QStringList kv =
            preset_->currentData().toString().split('|');
        if (kv.size() == 2) {
            c.audio_format  = kv[0].toStdString();
            c.audio_quality = kv[1].toInt();
        }
    }
    c.replaygain   = replaygain_->isChecked();
    c.fast_rip     = fastRip_->isChecked();
    c.preflight    = preflight_->isChecked();
    c.scan_density = scanDensity_->currentData().toInt();
    c.recovery_budget_min = recoveryBudget_->value();
    c.auto_eject   = autoEject_->isChecked();
    c.chime        = chime_->isChecked();
    c.lyrics       = lyrics_->isChecked();
    c.overwrite_existing = overwrite_->isChecked();
    c.jukebox      = jukebox_->isChecked();
    c.mb_useragent = ua_->text().toStdString();
    c.acoustid_key = acoustidKey_->text().trimmed().toStdString();
    c.discogs_token = discogsToken_->text().trimmed().toStdString();
    c.tmpdir       = tmpdir_->text().toStdString();
    c.music_root   = musicRoot_->text().toStdString();
    c.upload_backend = backend_->currentText().toStdString();
    c.upload_retries = retries_->value();
    c.nextcloud_url = ncUrl_->text().toStdString();
    c.webdav_user   = ncUser_->text().toStdString();
    c.webdav_pass   = ncPass_->text().toStdString();
    c.local_base    = localBase_->text().toStdString();
    c.ssh_host      = sshHost_->text().toStdString();
    c.ssh_user      = sshUser_->text().toStdString();
    c.ssh_base      = sshBase_->text().toStdString();
    c.ssh_port      = sshPort_->value();
    c.smb_url       = smbUrl_->text().toStdString();
    c.smb_user      = smbUser_->text().toStdString();
    c.smb_pass      = smbPass_->text().toStdString();
    c.accuraterip   = accuraterip_->isChecked();
    c.read_offset   = readOffset_->value();
    c.registry_url    = regUrl_->text().trimmed().toStdString();
    c.registry_submit = regSubmit_->isChecked();
    c.registry_stats  = regStats_->isChecked();
    c.registry_condition = regCondition_->isChecked();
    return c;
}

void SettingsDialog::refreshDriveInfo() {
    std::string dev = calibDev_->currentData().toString().toStdString();
    cdr::HwInfo h = cdr::drive_hwinfo(dev);
    std::string did = cdr::drive_id(dev);
    QString info = "<b>" + S(did) + "</b><br>";
    if (h.ok)
        info += QString("Hersteller: %1 · Modell: %2 · Firmware: %3<br>")
                    .arg(S(h.vendor), S(h.model),
                         h.revision.empty() ? "—" : S(h.revision));
    if (auto o = cdr::lookup_drive_offset(did))
        info += QString("Status: <b>kalibriert</b>, Offset %1").arg(*o);
    else
        info += "Status: <i>nicht kalibriert</i>";
    driveLbl_->setText(info);
}

void SettingsDialog::populateDriveTable() {
    auto rows = cdr::list_drive_offsets();
    std::string curId = cdr::drive_id(
        calibDev_->currentData().toString().toStdString());
    driveTbl_->setRowCount((int)rows.size());
    for (int i = 0; i < (int)rows.size(); ++i) {
        const std::string& id = rows[i].id;
        auto sp = id.find(' ');
        QString vendor = QString::fromStdString(
            sp == std::string::npos ? id : id.substr(0, sp));
        QString model = QString::fromStdString(
            sp == std::string::npos ? std::string() : id.substr(sp + 1));
        auto* c0 = new QTableWidgetItem(vendor);
        auto* c1 = new QTableWidgetItem(model);
        auto* c2 = new QTableWidgetItem(S(id));
        auto* c3 = new QTableWidgetItem(QString::number(rows[i].offset));
        if (id == curId) {
            QFont fb = c0->font(); fb.setBold(true);
            for (auto* it : { c0, c1, c2, c3 }) it->setFont(fb);
            c2->setToolTip("aktuell angeschlossenes Laufwerk");
        }
        driveTbl_->setItem(i, 0, c0);
        driveTbl_->setItem(i, 1, c1);
        driveTbl_->setItem(i, 2, c2);
        driveTbl_->setItem(i, 3, c3);
    }
    if (rows.empty()) {
        driveTbl_->setRowCount(1);
        auto* it = new QTableWidgetItem("— noch keine Kalibrierung —");
        driveTbl_->setItem(0, 0, it);
        driveTbl_->setSpan(0, 0, 1, 4);
    }
}

void SettingsDialog::onDeleteDrive() {
    int r = driveTbl_->currentRow();
    auto* it = (r >= 0) ? driveTbl_->item(r, 2) : nullptr;
    if (!it || it->text().isEmpty()) {
        msgWide(this, QMessageBox::Information, "Löschen",
                "Bitte zuerst eine Zeile in der Tabelle auswählen.");
        return;
    }
    QString id = it->text();
    if (QMessageBox::question(this, "Eintrag löschen",
            "Kalibrierung für\n\n  " + id +
            "\n\nlöschen? Beim nächsten Rip gilt das Laufwerk als "
            "unkalibriert (manueller Offset als Fallback), bis du neu "
            "kalibrierst.") != QMessageBox::Yes)
        return;
    if (cdr::delete_drive_offset(id.toStdString())) {
        populateDriveTable();
        refreshDriveInfo();
    } else {
        msgWide(this, QMessageBox::Warning, "Löschen fehlgeschlagen",
                "Konnte den Eintrag nicht entfernen: " + id);
    }
}

void SettingsDialog::onCalibrate() {
    auto r = QMessageBox::question(this, "Kalibrieren",
        "Eine gängige Mainstream-CD einlegen (kein Sampler, nichts "
        "Obskures). Die Disc wird einmal komplett gerippt (~15 min) und "
        "der Drive-Offset gegen AccurateRip ermittelt.\n\nJetzt starten?");
    if (r != QMessageBox::Yes) return;

    auto* dlg = new QDialog(this);
    dlg->setWindowTitle("Kalibrierung läuft …");
    dlg->resize(680, 360);
    auto* lv = new QPlainTextEdit; lv->setReadOnly(true);
    auto* cl = new QPushButton("Schließen"); cl->setEnabled(false);
    auto* dv = new QVBoxLayout(dlg);
    dv->addWidget(lv); dv->addWidget(cl);
    connect(cl, &QPushButton::clicked, dlg, &QDialog::accept);

    auto* proc = new QProcess(dlg);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyRead, dlg, [proc, lv] {
        lv->appendPlainText(QString::fromUtf8(proc->readAll()).trimmed());
    });
    connect(proc,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        dlg, [this, lv, cl](int code, QProcess::ExitStatus) {
            lv->appendPlainText(code == 0
                ? "\n✓ Kalibrierung gespeichert."
                : "\n✗ Kalibrierung nicht erfolgreich (Code " +
                  QString::number(code) + ").");
            cl->setEnabled(true);
            refreshDriveInfo();
            populateDriveTable();
        });
    // Eigene Binary aufrufen (im Flatpak /app/bin/cdripper — der frühere
    // feste Pfad /usr/local/bin/cdripper existiert dort nicht, weshalb die
    // Kalibrierung bisher sofort scheiterte).
    QStringList args{ "--calibrate", "--device",
                      calibDev_->currentData().toString() };
    if (!cfgPath_.isEmpty()) { args << "--config" << cfgPath_; }
    proc->start(QCoreApplication::applicationFilePath(), args);
    dlg->exec();
    dlg->deleteLater();
}
