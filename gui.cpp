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

#include <QElapsedTimer>
#include <QEvent>
#include <QGraphicsBlurEffect>
#include <QMouseEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QScrollArea>
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
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <algorithm>
#include <sstream>
#include <QFont>
#include <QProgressBar>
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

// ───────────────────────────── Controller ─────────────────────────────────────

Controller::Controller(QObject* p) : QObject(p) {}

Controller::~Controller() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
}

void Controller::start(const cdr::Config& cfg, bool once) {
    if (running_.load()) return;
    stop_ = false;
    running_ = true;

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
        "Bitte die richtige wählen:",
        labels, def < labels.size() ? def : 0, false, &ok);
    if (!ok) return def;
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

// KDE/Freedesktop-Notification (best effort; notify-send via Session-DBus,
// XDG_RUNTIME_DIR ist im Container gemountet).
static void notify(const QString& title, const QString& body) {
    QProcess::startDetached("notify-send",
        { "-a", "CD Ripper", "-i", "media-optical-audio", title, body });
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
    // Drei Darstellungsvarianten — zum Vergleichen im Scan-Dialog.
    // Default Ringe → Hauptfenster-Verhalten unverändert (nur schärfer).
    enum class Mode { Rings, Spiral, Bar };
    explicit DiscScanWidget(QWidget* p = nullptr) : QWidget(p) {
        setMinimumSize(110, 110);   // Hauptfenster: kompakt fix (s.u.);
        auto* pt = new QTimer(this);            // sanftes Pulsieren
        connect(pt, &QTimer::timeout, this, [this]{
            pulse_ += 0.09; update(); });
        pt->start(60);
    }                               // Scan-Dialog: flexibel größer
    void setMode(Mode m) { mode_ = m; update(); }
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
        switch (mode_) {
            case Mode::Spiral: paintSpiral(g); break;
            case Mode::Bar:    paintBar(g);    break;
            default:           paintRings(g);  break;
        }
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

    // ── Variante B: Spiral-Heatmap (LBA → Winkel, echtes Lesemuster) ──────
    void paintSpiral(QPainter& g) {
        const int W = width(), H = height();
        const QPointF c(W / 2.0, H / 2.0);
        const double R = std::min(W, H) / 2.0 - 8.0;
        const double ro = R * 0.99, ri = R * 0.16;
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setPen(Qt::NoPen);
        g.setBrush(QColor("#23272e"));
        g.drawEllipse(c, R, R);
        double turns = (ro - ri) / 4.0;               // ~4px Ganghöhe
        if (turns < 8) turns = 8; if (turns > 44) turns = 44;
        double pitch = (ro - ri) / turns;
        const int STEPS = 2200;
        double curFrac = -1.0;
        std::vector<int> b = (r_.lba_max > r_.lba_min)
            ? bucketize(STEPS, &curFrac) : std::vector<int>(STEPS, -1);
        const double TAU = 6.28318530717958647692;
        auto pt = [&](double t){
            double ang = -TAU / 4.0 + t * turns * TAU;
            double rr  = ri + t * (ro - ri);
            return QPointF(c.x() + rr * std::cos(ang),
                           c.y() + rr * std::sin(ang));
        };
        QPointF prev = pt(0.0);
        for (int i = 1; i < STEPS; ++i) {
            double t = (double)i / (STEPS - 1);
            QPointF cur = pt(t);
            int st = b[i];
            QColor cc = st == -1 ? QColor("#2b2f37") : stcol(st);
            g.setPen(QPen(cc, pitch + 0.7, Qt::SolidLine,
                          Qt::FlatCap, Qt::RoundJoin));
            g.drawLine(prev, cur);
            prev = cur;
        }
        if (curFrac >= 0.0) {                          // Cursor = Punkt
            QPointF p = pt(curFrac);
            g.setPen(Qt::NoPen);
            g.setBrush(QColor("#4fc3f7"));
            g.drawEllipse(p, 4.0, 4.0);
        }
        g.setPen(Qt::NoPen);
        g.setBrush(QColor("#1e2127"));
        g.drawEllipse(c, ri * 0.6, ri * 0.6);
        g.setBrush(Qt::NoBrush);
        g.setPen(QColor("#3a3f4b"));
        g.drawEllipse(c, R, R);
        double s = 0.5 + 0.5 * std::sin(pulse_);
        QColor glow(0x4f, 0xc3, 0xf7, (int)(28 + 46 * s));
        g.setPen(QPen(glow, 2.0 + 1.5 * s));
        g.drawEllipse(c, R - 1.5, R - 1.5);
    }

    // ── Variante C: lineare Qualitätsleiste (Anfang → Ende) ──────────────
    void paintBar(QPainter& g) {
        const int W = width(), H = height();
        int mx = 24;
        int bh = std::min(std::max((int)(H * 0.34), 30), 130);
        int x0 = mx, x1 = W - mx;
        int bw = std::max(8, x1 - x0);
        int y0 = (H - bh) / 2 - 12;
        QRectF bar(x0, y0, bw, bh);
        g.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath rp; rp.addRoundedRect(bar, 9, 9);
        g.fillPath(rp, QColor("#21262e"));
        g.save();
        g.setClipPath(rp);
        g.setRenderHint(QPainter::Antialiasing, false);
        double curFrac = -1.0;
        std::vector<int> b = (r_.lba_max > r_.lba_min)
            ? bucketize(bw, &curFrac) : std::vector<int>(bw, -1);
        for (int x = 0; x < bw; ++x) {
            if (b[x] == -1) continue;                  // Rohling = bg
            g.fillRect(QRectF(x0 + x, y0, 1.0, bh), stcol(b[x]));
        }
        if (ripFrac_ >= 0.0)
            g.fillRect(QRectF(x0, y0, bw * ripFrac_, bh),
                       QColor(0x29, 0x79, 0xff, 150));
        if (curFrac >= 0.0) {                          // Cursor = Strich
            g.setPen(QPen(QColor("#4fc3f7"), 2));
            double cx = x0 + curFrac * bw;
            g.drawLine(QPointF(cx, y0), QPointF(cx, y0 + bh));
        }
        g.restore();
        g.setRenderHint(QPainter::Antialiasing, true);
        g.setBrush(Qt::NoBrush);
        g.setPen(QColor("#343b47"));
        g.drawPath(rp);
        // Grobe Skala: 10 Teilstriche + Anfang/Ende-Beschriftung (echte
        // Track-LBAs liegen nicht im ProbeResult → bewusst keine Nummern).
        g.setPen(QColor("#4a525f"));
        for (int k = 1; k < 10; ++k) {
            double tx = x0 + bw * k / 10.0;
            g.drawLine(QPointF(tx, y0 + bh + 3),
                       QPointF(tx, y0 + bh + 8));
        }
        g.setPen(QColor("#9aa0aa"));
        QFont f = g.font(); f.setPointSize(8); g.setFont(f);
        g.drawText(QRectF(x0, y0 + bh + 9, bw, 16),
                   Qt::AlignLeft,  "Anfang");
        g.drawText(QRectF(x0, y0 + bh + 9, bw, 16),
                   Qt::AlignRight, "Ende");
        double s = 0.5 + 0.5 * std::sin(pulse_);
        QColor glow(0x4f, 0xc3, 0xf7, (int)(22 + 40 * s));
        g.setPen(QPen(glow, 1.5 + 1.0 * s));
        g.drawPath(rp);
    }

    int cur_ = -1;                               // Live-Scan-Cursor (LBA)
    double ripFrac_ = -1.0;                      // Rip-Fortschritt 0..1 (-1=aus)
    double pulse_ = 0.0;                          // Pulsier-Phase
    Mode   mode_ = Mode::Rings;                    // Default = Hauptfenster
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

// ───────────────────────────── MainWindow ─────────────────────────────────────

MainWindow::MainWindow(cdr::Config cfg, bool once,
                       std::string cfgPath, QWidget* parent)
    : QMainWindow(parent), cfg_(std::move(cfg)), once_(once),
      cfgPath_(std::move(cfgPath)) {
    setWindowTitle("CD-Ripper → Navidrome");
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

    auto* central = new QWidget;
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

    // Tray-Icon (Indikator + Schnellzugriff)
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(
            style()->standardIcon(QStyle::SP_DriveCDIcon), this);
        tray_->setToolTip("CD Ripper — bereit");
        auto* tm = new QMenu(this);
        tm->addAction("Fenster zeigen", this, [this] {
            showNormal(); raise(); activateWindow(); });
        tm->addAction("Start", this, &MainWindow::onStart);
        tm->addAction("Stop",  this, &MainWindow::onStop);
        tm->addSeparator();
        tm->addAction("Beenden", qApp, &QApplication::quit);
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
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideRight);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(true);
    table_->setFrameShape(QFrame::NoFrame);
    table_->setMinimumHeight(110);   // ~3 Zeilen Minimum, skaliert mit Fenster
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

void MainWindow::onAlbumReady(const QString& aa, const QString& at,
                              const QString& yr, const QStringList& ti,
                              const QStringList& ar) {
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
        auto* pb = new QProgressBar;
        pb->setRange(0, 100);
        pb->setValue(0);
        pb->setTextVisible(true);
        table_->setCellWidget(i, 4, pb);
    }
    fillingTable_ = false;
    bannerLbl_->setText(QString("%1 — %2 (%3), %4 Tracks")
        .arg(aa, at, yr.isEmpty() ? "—" : yr).arg(ti.size()));
}

void MainWindow::onCoverReady(const QString& path) {
    QPixmap p(path);
    if (!p.isNull())
        cover_->setPixmap(p.scaled(cover_->size(), Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
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
    if (auto* pb = qobject_cast<QProgressBar*>(table_->cellWidget(row, 4))) {
        cdr::TrackState s = (cdr::TrackState)state;
        if (s == cdr::TrackState::Done)        pb->setValue(100);
        else if (s == cdr::TrackState::Failed) { pb->setValue(0); }
        else if (s == cdr::TrackState::Ripping ||
                 s == cdr::TrackState::Uploading)
            pb->setValue((int)(frac * 100));
        else if (s == cdr::TrackState::Ripped)   pb->setValue(100);
        else if (s == cdr::TrackState::Encoding) pb->setValue(100);
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
            std::string cov;
            try {
                fs::path dir = fs::path(tmp) / "preview";
                std::error_code ec; fs::create_directories(dir, ec);
                fs::path out;
                if (cdr::fetch_cover(al.mb_release_id, ua, dir, out))
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
    if (cover_)       { cover_->setPixmap(QPixmap());
                        cover_->setText("kein\nCover"); }
    scanDiscId_.clear();
    scanTrackStatus_.clear();
    curReleaseId_.clear();
    lastDiscId_.clear();              // nächste Disc löst frische Vorschau aus
    bannerLbl_->setText("Tray leer — neue CD einlegen.");
}

// Cover-Easter-Egg: Klick aufs Cover-Bild → CD-Morph-Spin.
bool MainWindow::eventFilter(QObject* obj, QEvent* ev) {
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
    if (ctl_->running()) {
        QMessageBox::information(this, "Einstellungen",
            "Während ein Lauf aktiv ist nicht änderbar — erst stoppen.");
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
    // Darstellungs-Umschalter (zum Vergleichen der drei Varianten).
    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel("Darstellung:"));
    auto* modeBox = new QComboBox(dlg);
    modeBox->addItems({ "Ringe (scharf)", "Spirale (Heatmap)",
                        "Lineare Leiste" });
    modeRow->addWidget(modeBox);
    modeRow->addStretch(1);
    v->addLayout(modeRow);
    auto* legend = new QLabel(QString::fromUtf8(
        "<small>Außen = Disc-Rand, innen = Anfang. "
        "<span style='color:#27ae60'>■</span> ok &nbsp; "
        "<span style='color:#e0a83e'>■</span> langsam &nbsp; "
        "<span style='color:#c0392b'>■</span> Lesefehler</small>"));
    v->addWidget(legend);
    connect(modeBox, &QComboBox::currentIndexChanged,
            dlg, [sc, legend](int i) {
        sc->setMode(i == 1 ? DiscScanWidget::Mode::Spiral
                  : i == 2 ? DiscScanWidget::Mode::Bar
                           : DiscScanWidget::Mode::Rings);
        QString pos = i == 2 ? "Links = Anfang, rechts = Ende. "
                             : "Außen = Disc-Rand, innen = Anfang. ";
        legend->setText("<small>" + pos +
            "<span style='color:#27ae60'>■</span> ok &nbsp; "
            "<span style='color:#e0a83e'>■</span> langsam &nbsp; "
            "<span style='color:#c0392b'>■</span> Lesefehler</small>");
    });
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
        device_     = new QLineEdit(S(c.device));
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
        f->addRow("MusicBrainz UA:", ua_);
        f->addRow("AcoustID-Key:", acoustidKey_);
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
        driveLbl_ = new QLabel;
        driveLbl_->setWordWrap(true);
        calibrateBtn_ = new QPushButton("Dieses Laufwerk jetzt kalibrieren…");
        f->addRow("", accuraterip_);
        f->addRow("Manueller Offset:", readOffset_);
        f->addRow("Laufwerk:", driveLbl_);
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
    device_->setText(S(c.device));
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
    c.device       = device_->text().toStdString();
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
    std::string dev = device_->text().toStdString();
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
    std::string curId = cdr::drive_id(device_->text().toStdString());
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
    QStringList args{ "--calibrate", "--device", device_->text() };
    if (!cfgPath_.isEmpty()) { args << "--config" << cfgPath_; }
    proc->start("/usr/local/bin/cdripper", args);
    dlg->exec();
    dlg->deleteLater();
}
