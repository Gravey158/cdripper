// gui.h — Qt-Schicht: Controller (Pipeline↔Qt-Signals) + MainWindow.
#pragma once

#include "pipeline.h"

#include <atomic>
#include <memory>
#include <thread>

#include <QDialog>
#include <QMainWindow>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

class QTableWidget;
class QLabel;
class QPushButton;
class QLineEdit;
class QCheckBox;
class QTimer;
class QPlainTextEdit;
class QComboBox;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;
class QCloseEvent;
class MultiWindow;                 // T7-GUI: Multi-Laufwerk-Fenster

// Wrappt cdr::Pipeline: marshalt alle Callbacks (die aus Worker-Threads kommen)
// in Qt-Signals mit reinen Qt-Typen → cross-thread queued, kein qRegisterMetaType.
class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;

    void start(const cdr::Config& cfg, bool once);
    void requestStop();
    bool running() const { return running_.load(); }
    // Scan-geführter Rip: liefert für eine Disc-ID die zuletzt zu
    // rippenden (Hänger-)Tracks. Von MainWindow gesetzt; nur ein
    // frischer Session-Scan derselben Disc liefert hier etwas.
    std::function<std::vector<int>(const std::string&)> deferFn_;
    // Voller Pro-Track-Status des letzten Session-Scans (für „Preflight
    // überspringen, vorherigen Scan nehmen").
    std::function<std::vector<int>(const std::string&)> statusFn_;

public slots:
    void editTrackTitle(int idx, const QString& t);
    void editTrackArtist(int idx, const QString& a);
    void editAlbum(const QString& artist, const QString& title, const QString& year);
    void setCover(const QString& path);
    // Aus Worker-Thread via BlockingQueuedConnection aufgerufen.
    Q_INVOKABLE int chooseReleaseSlot(QStringList labels, int def);

signals:
    void waiting(const QString& msg);
    void discIdent(const QString& id, int tocTracks);
    void albumReady(const QString& albumArtist, const QString& albumTitle,
                    const QString& year, const QStringList& titles,
                    const QStringList& artists);
    void coverReady(const QString& path);
    void coverReleaseId(const QString& mbid);
    void trackState(int idx, int state, double frac, const QString& msg);
    void progress(double elapsed, double eta, int ripped, int uploaded, int total);
    void metrics(double ripMBps, double encMBps, double upMBps);
    void logLine(const QString& line);
    void discDone(bool ok, const QString& msg);
    void fatal(const QString& msg);
    void finished();
    void discScanInit(int lbaMin, int lbaMax);
    void discScanCell(int lba, int status);
    void discScanCursor(int lba);
    void ripProgress(double frac);

private:
    Q_INVOKABLE void joinWorker();   // queued aus Worker-Thread

    std::unique_ptr<cdr::Pipeline> pl_;
    std::thread          worker_;
    std::atomic<bool>    stop_{false};
    std::atomic<bool>    running_{false};
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(cdr::Config cfg, bool once,
                        std::string cfgPath, QWidget* parent = nullptr);
    ~MainWindow() override;        // Threads sauber stoppen/joinen

private slots:
    void onStart();
    void onStop();
    void onStartStopToggle();
    void onPickCoverMB();
    void onEject();
    void onLoadTray();
    void onShowLogs();
    void onShowHistory();
    void onShowArchive();
    void onScanDisc();
    void onSearchMeta();         // manuelle MusicBrainz-Namenssuche
    void onIdentifyAcoustID();   // Erkennung am Klang (Chromaprint/AcoustID)
    void onAbout();
    void onOpenSettings();
    void onPickCover();
    void onCellChanged(int row, int col);
    void onWaiting(const QString&);
    void onAlbumReady(const QString&, const QString&, const QString&,
                      const QStringList&, const QStringList&);
    void onCoverReady(const QString&);
    void onTrackState(int idx, int state, double frac, const QString& msg);
    void onProgress(double elapsed, double eta, int ripped, int uploaded, int total);
    void onMetrics(double ripMBps, double encMBps, double upMBps);
    void onLog(const QString&);
    void onDiscDone(bool ok, const QString&);
    void onFatal(const QString&);
    void onFinished();
    void tick();

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;  // Cover-Easter-Egg
    void closeEvent(QCloseEvent* e) override;              // Größe merken

private:
    void setControlsRunning(bool r);
    void appendLog(const QString& line);         // farbig ins Log-Fenster
    void logChain(const QString& line);          // Log-Fenster + cdripper.log
    void populateDrives();                       // Laufwerks-Dropdown füllen
    void discWatch();                            // Vorschau beim Einlegen
    void resetDiscState();                       // Auswurf → letzten Stand leeren
    bool hadDisc_ = false;                        // Disc-präsent-Flanke (Auswurf)
    std::thread previewThr_;
    std::atomic<bool> previewBusy_{false};
    std::atomic<bool> scanBusy_{false};          // Standalone-Scan aktiv
    std::string lastDiscId_;
    int discPoll_ = 0;
    std::string scanDiscId_;                     // Disc des letzten Scans
    std::vector<int> scanTrackStatus_;           // voller Pro-Track-Status

    cdr::Config cfg_;
    bool        once_;
    std::string cfgPath_;
    Controller* ctl_;
    std::thread metaThr_;            // manuelle Suche / AcoustID (off-thread)
    std::atomic<bool> metaBusy_{false};
    std::thread scanThr_;
    std::shared_ptr<std::atomic<bool>> scanStop_;  // dtor signalisiert Scan-Stop
    class DiscScanWidget* discScan_ = nullptr;   // Live-Scan im Hauptfenster
    QLabel* discScanCap_ = nullptr;              // Status unter der Disc
    QWidget* coverSpin_ = nullptr;               // Cover-Easter-Egg (Overlay)
    MultiWindow* multiWin_ = nullptr;            // T7-GUI (Single-Instanz)

    QComboBox*  device_;                         // erkannte Laufwerke
    QCheckBox*  dryRun_;
    QCheckBox*  onceBox_;
    QPushButton* startBtn_;
    QPushButton* stopBtn_;
    QPushButton* settingsBtn_;
    QPushButton* ejectBtn_;
    QPushButton* loadBtn_;
    QSystemTrayIcon* tray_ = nullptr;
    QList<QStringList> history_;   // {Zeit, Album, Status, AccurateRip}
    QLabel*     cover_;
    QPushButton* coverBtn_;
    QPushButton* coverMbBtn_;
    QString     curReleaseId_;
    QLineEdit*  albArtist_;
    QLineEdit*  albTitle_;
    QLineEdit*  albYear_;
    QLabel*     bannerLbl_;
    QTableWidget* table_;
    QPlainTextEdit* logView_;

    QLabel* sbElapsed_;
    QLabel* sbEta_;
    QLabel* sbRip_;
    QLabel* sbUp_;
    QLabel* sbSpeed_;
    QTimer* timer_;

    double  lastElapsed_ = 0;
    double  lastEta_     = -1;
    bool    busy_        = false;
    bool    fillingTable_ = false;
};

// Settings-Dialog mit Sidebar-Kategorien (QListWidget + QStackedWidget).
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const cdr::Config& c, QString cfgPath,
                            QWidget* parent = nullptr);
    cdr::Config config() const;          // editiertes Ergebnis
    QString selectedProfile() const;     // "" = Standard

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;

private slots:
    void onCalibrate();
    void refreshDriveInfo();
    void onDeleteDrive();
    void onProfileChanged(int);
    void onNewProfile();

private:
    void populateDriveTable();
    void applyConfig(const cdr::Config& c);   // alle Felder setzen
    // Registriert einen Erklärungstext, der beim Hover/Fokus unten im
    // Hilfe-Kasten erscheint (installiert zugleich den Event-Filter).
    void setHelp(QWidget* w, const QString& text);
    QComboBox* profile_;
    cdr::Config base_;
    QString     cfgPath_;
    QLabel*     help_ = nullptr;              // Erklärungs-Kasten unten
    QMap<QObject*, QString> helpText_;
    QLineEdit *device_, *tmpdir_, *ua_, *musicRoot_, *acoustidKey_;
    QComboBox *readSpeed_;
    QComboBox *scanDensity_;
    QSpinBox  *recoveryBudget_;   // Recovery-Zeitbudget pro Track (Min)
    QComboBox *preset_;          // Audio-Format/Preset (Format+Qualität)
    QSpinBox  *retries_;
    QCheckBox *replaygain_, *jukebox_;
    QCheckBox *autoEject_, *chime_, *fastRip_, *lyrics_, *overwrite_;
    QCheckBox *preflight_;
    QCheckBox *accuraterip_;
    QSpinBox  *readOffset_;
    QLabel    *driveLbl_;
    QPushButton* calibrateBtn_;
    QTableWidget* driveTbl_;
    QPushButton* delDriveBtn_;
    QComboBox *backend_;
    QStackedWidget* backendPages_;
    QLineEdit *ncUrl_, *ncUser_, *ncPass_;
    QLineEdit *localBase_;
    QLineEdit *sshHost_, *sshUser_, *sshBase_;
    QSpinBox  *sshPort_;
    QLineEdit *smbUrl_, *smbUser_, *smbPass_;
    QLineEdit *regUrl_;
    QCheckBox *regSubmit_, *regStats_, *regCondition_;
};
