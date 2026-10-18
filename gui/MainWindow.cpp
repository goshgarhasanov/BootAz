#include "MainWindow.h"
#include "Brand.h"
#include "Dialogs.h"
#include "Downloads.h"
#include "Engine.h"
#include "Theme.h"
#include "Widgets.h"

#include "I18n.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QSlider>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

using namespace bootaz;

static QString L(const char* s) { return QString::fromStdString(T(s)); }
static QString Q(const std::string& s) { return QString::fromStdString(s); }
static QString HB(uint64_t b) { return Q(humanBytes(b)); }

namespace {

QSettings settings() { return QSettings(QSettings::IniFormat, QSettings::UserScope, "BootAz", "BootAz"); }

QWidget* row(const QString& label, QWidget* field, QWidget* extra = nullptr)
{
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(10);
    auto* l = new QLabel(label);
    l->setMinimumWidth(150);
    h->addWidget(l);
    h->addWidget(field, 1);
    if (extra)
        h->addWidget(extra);
    return w;
}

QString etaText(double secs)
{
    if (secs <= 0 || secs > 86400)
        return {};
    const int s = int(secs);
    return s >= 3600 ? QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'))
                     : QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

} // namespace

// ======================================================================

MainWindow::MainWindow()
{
    setWindowTitle("BootAz");
    setWindowIcon(brand::appIcon());
    setAcceptDrops(true);
    resize(1180, 880);
    setMinimumSize(900, 620);

    engine_ = new Engine(this);
    logDlg_ = new LogDialog(this);
    connect(engine_, &Engine::progress, this, &MainWindow::onProgress);
    connect(engine_, &Engine::logLine, this, [this](const QString& dev, const QString& t) {
        log((dev.isEmpty() || devProgress_.size() <= 1 ? QString() : "[" + dev + "] ") + t);
    });
    connect(engine_, &Engine::failed, this, [this](const QString& dev, const QString& m) {
        lastError_ = (dev.isEmpty() || devProgress_.size() <= 1 ? QString() : dev + ": ") + m;
        log("✗ " + lastError_);
    });
    connect(engine_, &Engine::planned, this, [this](const QString&, const QJsonObject& p) {
        for (const auto& w : p.value("warnings").toArray())
            log("! " + w.toString());
    });
    connect(engine_, &Engine::report, this, [this](const QString&, const QJsonObject& o) { lastReport_ = o; });
    connect(engine_, &Engine::finished, this, &MainWindow::onFinished);

    auto* root = new QWidget;
    root->setObjectName("Root");
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildHeader());

    auto* body = new QWidget;
    auto* grid = new QGridLayout(body);
    grid->setContentsMargins(18, 16, 18, 8);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(14);
    grid->addWidget(buildDeviceCard(), 0, 0);
    grid->addWidget(buildImageCard(), 1, 0);
    grid->addWidget(buildBootCard(), 0, 1);
    grid->addWidget(buildFormatCard(), 1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    grid->setRowStretch(2, 1);
    // The cards scroll instead of being squeezed when the window is small.
    auto* scroll = new QScrollArea;
    scroll->setWidget(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }");
    v->addWidget(scroll, 1);
    v->addWidget(buildActionBar());
    setCentralWidget(root);

    // Restore the last session's choices.
    QSettings s = settings();
    showFixed_->setChecked(s.value("showFixed", false).toBool());
    verifyCheck_->setChecked(s.value("verify", true).toBool());
    ejectCheck_->setChecked(s.value("eject", false).toBool());
    autorunCheck_->setChecked(s.value("autorun", true).toBool());

    refreshDrives(false);
    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &MainWindow::pollDrives);
    pollTimer_->start(2500);
    updateControls();
    setStatus(L("Ready. Choose a drive and an image."));
    if (!qEnvironmentVariableIsEmpty("BOOTAZ_IMAGE"))
        setImage(qEnvironmentVariable("BOOTAZ_IMAGE"));
}

MainWindow::~MainWindow()
{
    QSettings s = settings();
    s.setValue("showFixed", showFixed_->isChecked());
    s.setValue("verify", verifyCheck_->isChecked());
    s.setValue("eject", ejectCheck_->isChecked());
    s.setValue("autorun", autorunCheck_->isChecked());
}

// ---------------------------------------------------------------- layout

QWidget* MainWindow::buildHeader()
{
    auto* h = new QWidget;
    h->setObjectName("Header");
    auto* l = new QHBoxLayout(h);
    l->setContentsMargins(20, 12, 16, 12);
    auto* logo = new QLabel;
    const qreal dpr = devicePixelRatioF();
    QPixmap pm = brand::wordmarkPixmap(int(46 * dpr), theme::current().dark);
    pm.setDevicePixelRatio(dpr);
    logo->setPixmap(pm);
    l->addWidget(logo);
    auto* sub = new QLabel(L("Bootable USB drives for Windows and Linux"));
    sub->setObjectName("Subtitle");
    l->addSpacing(12);
    l->addWidget(sub);
    l->addStretch();

    auto* dl = new QPushButton("⬇  " + L("Download image"));
    dl->setToolTip(L("Download Ubuntu, Debian, Fedora, Mint, Windows and more from their official sites"));
    connect(dl, &QPushButton::clicked, this, [this] {
        DownloadDialog d(this);
        if (d.exec() == QDialog::Accepted && !d.downloadedFile().isEmpty()) {
            bootCombo_->setCurrentIndex(0);
            setImage(d.downloadedFile());
        }
    });
    lockWhileRunning_ << dl;
    const bool az = i18n::language() == i18n::Lang::Az;
    auto* lang = new QPushButton(az ? "🌐  English" : "🌐  Azərbaycanca");
    connect(lang, &QPushButton::clicked, this, [this] {
        if (running_)
            return;
        const auto next = i18n::language() == i18n::Lang::Az ? i18n::Lang::En : i18n::Lang::Az;
        i18n::setLanguage(next);
        i18n::saveLanguage(next);
        emit rebuildRequested();
    });
    lockWhileRunning_ << lang;
    auto* themeBtn = new QPushButton(theme::current().dark ? "☀" : "☾");
    themeBtn->setToolTip(L("Light / dark theme"));
    connect(themeBtn, &QPushButton::clicked, this, [this] {
        if (running_)
            return;
        QSettings s = settings();
        s.setValue("dark", !theme::current().dark);
        emit rebuildRequested();
    });
    lockWhileRunning_ << themeBtn;
    auto* about = new QPushButton("ⓘ");
    about->setToolTip(L("About BootAz"));
    connect(about, &QPushButton::clicked, this, [this] { AboutDialog(this).exec(); });
    for (auto* b : {dl, lang, themeBtn, about}) {
        b->setCursor(Qt::PointingHandCursor);
        l->addWidget(b);
    }
    return h;
}

QWidget* MainWindow::buildDeviceCard()
{
    auto* card = new Card("🔌", L("Drive"));
    auto* b = card->body();
    driveCombo_ = new QComboBox;
    driveCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* refresh = new QPushButton("⟳");
    refresh->setToolTip(L("Refresh the list"));
    connect(refresh, &QPushButton::clicked, this, [this] { refreshDrives(); });
    auto* top = new QHBoxLayout;
    top->addWidget(driveCombo_, 1);
    top->addWidget(refresh);
    b->addLayout(top);
    driveInfo_ = new QLabel;
    driveInfo_->setObjectName("Muted");
    driveInfo_->setWordWrap(true);
    b->addWidget(driveInfo_);
    multiCheck_ = new QCheckBox(L("Write to several drives at once"));
    b->addWidget(multiCheck_);
    multiList_ = new QListWidget;
    multiList_->setMaximumHeight(110);
    multiList_->setVisible(false);
    b->addWidget(multiList_);
    showFixed_ = new QCheckBox(L("Also list USB hard disks and internal drives"));
    showFixed_->setToolTip(L("Drives that hold the running system are never shown as writable"));
    b->addWidget(showFixed_);
    connect(multiCheck_, &QCheckBox::toggled, this, [this](bool on) {
        multiList_->setVisible(on);
        driveCombo_->setEnabled(!on);
    });
    connect(showFixed_, &QCheckBox::toggled, this, [this] { refreshDrives(); });
    connect(driveCombo_, &QComboBox::currentIndexChanged, this, [this] { updateDriveInfo(); updateControls(); });
    lockWhileRunning_ << driveCombo_ << refresh << multiCheck_ << multiList_ << showFixed_;
    b->addStretch();
    return card;
}

QWidget* MainWindow::buildImageCard()
{
    auto* card = new Card("💿", L("Boot selection"));
    auto* b = card->body();
    bootCombo_ = new QComboBox;
    bootCombo_->addItem(L("Disk or ISO image"));
    bootCombo_->addItem(L("Non bootable (format only)"));
    b->addWidget(bootCombo_);
    imagePath_ = new QLineEdit;
    imagePath_->setReadOnly(true);
    imagePath_->setPlaceholderText(L("Choose an ISO / IMG file or drag it here"));
    selectBtn_ = new QPushButton(L("Choose…"));
    selectBtn_->setProperty("variant", "primary");
    selectBtn_->setStyleSheet("padding: 7px 16px; font-size: 10.5pt;");
    connect(selectBtn_, &QPushButton::clicked, this, &MainWindow::chooseImage);
    auto* pr = new QHBoxLayout;
    pr->addWidget(imagePath_, 1);
    pr->addWidget(selectBtn_);
    b->addLayout(pr);
    chipsBox_ = new QWidget;
    chips_ = new FlowLayout(chipsBox_, 6);
    b->addWidget(chipsBox_);
    imageNote_ = new QLabel;
    imageNote_->setObjectName("Muted");
    imageNote_->setWordWrap(true);
    b->addWidget(imageNote_);
    auto* tools = new QHBoxLayout;
    hashBtn_ = new QPushButton("#  " + L("Checksum"));
    hashBtn_->setToolTip(L("MD5, SHA-1, SHA-256 and SHA-512 of the image, compared with the official checksum"));
    connect(hashBtn_, &QPushButton::clicked, this, [this] {
        if (haveImage_)
            HashDialog(Q(image_.path), this).exec();
    });
    vmBtn_ = new QPushButton("▶  " + L("Test in a virtual machine"));
    vmBtn_->setToolTip(L("Boots the image in QEMU so you can see it work before writing"));
    connect(vmBtn_, &QPushButton::clicked, this, &MainWindow::testInVm);
    tools->addWidget(hashBtn_);
    tools->addWidget(vmBtn_);
    tools->addStretch();
    b->addLayout(tools);
    connect(bootCombo_, &QComboBox::currentIndexChanged, this, [this] { updateControls(); });
    lockWhileRunning_ << bootCombo_ << selectBtn_ << hashBtn_ << vmBtn_;
    b->addStretch();
    return card;
}

QWidget* MainWindow::buildBootCard()
{
    auto* card = new Card("🚀", L("Boot options"));
    auto* b = card->body();
    modeCombo_ = new QComboBox;
    modeCombo_->addItem(L("Automatic (recommended)"));
    modeCombo_->addItem(L("ISO mode: copy the files (drive stays usable)"));
    modeCombo_->addItem(L("DD mode: write the image byte for byte"));
    b->addWidget(row(L("Write mode"), modeCombo_));
    schemeCombo_ = new QComboBox;
    schemeCombo_->addItem("GPT");
    schemeCombo_->addItem("MBR");
    b->addWidget(row(L("Partition scheme"), schemeCombo_));
    targetCombo_ = new QComboBox;
    b->addWidget(row(L("Target system"), targetCombo_));
    targetHint_ = hint(QString());
    b->addWidget(targetHint_);

    winLayoutCombo_ = new QComboBox;
    winLayoutCombo_->addItem(L("NTFS + UEFI:NTFS (like Rufus)"));
    winLayoutCombo_->addItem(L("FAT32 boot + NTFS data (Microsoft's method)"));
    winLayoutCombo_->addItem(L("FAT32, split install.wim (needs wimlib)"));
    winLayoutRow_ = row(L("Large install.wim"), winLayoutCombo_);
    b->addWidget(winLayoutRow_);

    persistSlider_ = new QSlider(Qt::Horizontal);
    persistSlider_->setRange(0, 64);
    persistLabel_ = new QLabel;
    persistLabel_->setMinimumWidth(80);
    persistRow_ = row(L("Persistent storage"), persistSlider_, persistLabel_);
    b->addWidget(persistRow_);
    connect(persistSlider_, &QSlider::valueChanged, this, [this](int v) {
        persistLabel_->setText(v == 0 ? L("Off") : QString::number(v) + " GB");
    });
    persistLabel_->setText(L("Off"));

    winBtn_ = new QPushButton("⚙  " + L("Windows setup options…"));
    winSummary_ = new QLabel;
    winSummary_->setObjectName("Muted");
    winSummary_->setWordWrap(true);
    winRow_ = new QWidget;
    auto* wr = new QVBoxLayout(winRow_);
    wr->setContentsMargins(0, 0, 0, 0);
    wr->addWidget(winBtn_);
    wr->addWidget(winSummary_);
    b->addWidget(winRow_);
    connect(winBtn_, &QPushButton::clicked, this, [this] {
        WindowsOptionsDialog d(winOpts_, image_.isWindows11, this);
        if (d.exec() == QDialog::Accepted) {
            winOpts_ = d.options();
            winOptsSet_ = true;
            updateControls();
        }
    });
    for (QComboBox* c : {modeCombo_, schemeCombo_, targetCombo_, winLayoutCombo_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { updateControls(); });
    lockWhileRunning_ << modeCombo_ << schemeCombo_ << targetCombo_ << winLayoutCombo_ << persistSlider_ << winBtn_;
    b->addStretch();
    return card;
}

QWidget* MainWindow::buildFormatCard()
{
    auto* card = new Card("🗂", L("Format options"));
    auto* b = card->body();
    labelEdit_ = new QLineEdit;
    labelEdit_->setPlaceholderText(L("automatic: named after the system"));
    b->addWidget(row(L("Volume label"), labelEdit_));
    fsCombo_ = new QComboBox;
    fsCombo_->addItem(L("Automatic"), -1);
    for (FsType f : availableFileSystems())
        fsCombo_->addItem(fsName(f), int(f));
    b->addWidget(row(L("File system"), fsCombo_));
    clusterCombo_ = new QComboBox;
    clusterCombo_->addItem(L("Default"), 0);
    for (int k : {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536})
        clusterCombo_->addItem(k >= 1024 ? QString("%1 KB").arg(k / 1024) : QString("%1 B").arg(k), k);
    b->addWidget(row(L("Cluster size"), clusterCombo_));
    badCombo_ = new QComboBox;
    badCombo_->addItem(L("No bad block test"), 0);
    badCombo_->addItem(L("Bad block test: 1 pass"), 1);
    badCombo_->addItem(L("Bad block test: 2 passes"), 2);
    badCombo_->addItem(L("Bad block test: 4 passes"), 4);
    b->addWidget(row(L("Check the drive"), badCombo_));
    quickCheck_ = new QCheckBox(L("Quick format"));
    quickCheck_->setChecked(true);
    verifyCheck_ = new QCheckBox(L("Verify after writing (read everything back)"));
    verifyCheck_->setChecked(true);
    ejectCheck_ = new QCheckBox(L("Eject the drive when done"));
    autorunCheck_ = new QCheckBox(L("Show the label and BootAz icon in Windows Explorer"));
    autorunCheck_->setChecked(true);
    auto* g = new QGridLayout;
    g->addWidget(quickCheck_, 0, 0);
    g->addWidget(ejectCheck_, 0, 1);
    g->addWidget(verifyCheck_, 1, 0, 1, 2);
    g->addWidget(autorunCheck_, 2, 0, 1, 2);
    b->addLayout(g);
    connect(fsCombo_, &QComboBox::currentIndexChanged, this, [this] { updateControls(); });
    lockWhileRunning_ << labelEdit_ << fsCombo_ << clusterCombo_ << badCombo_ << quickCheck_ << verifyCheck_ << ejectCheck_ << autorunCheck_;
    b->addStretch();
    return card;
}

QWidget* MainWindow::buildActionBar()
{
    auto* w = new QWidget;
    w->setObjectName("Header");
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(18, 12, 18, 10);
    v->setSpacing(8);
    bar_ = new FlashBar;
    bar_->setText(L("Ready"));
    v->addWidget(bar_);
    auto* h = new QHBoxLayout;
    status_ = new QLabel;
    status_->setObjectName("Status");
    status_->setWordWrap(true);
    h->addWidget(status_, 1);
    toolsBtn_ = new QPushButton("🧰  " + L("Tools"));
    auto* menu = new QMenu(toolsBtn_);
    menu->addAction("🔍  " + L("Quick fake-drive check (keeps your files)"), this, [this] { runTool("fakecheck"); });
    menu->addAction("🧪  " + L("Full drive test (bad blocks, erases the drive)"), this, [this] { runTool("badblocks"); });
    menu->addAction("♻  " + L("Restore the drive to normal (one partition)"), this, [this] { runTool("restore"); });
    menu->addAction("💾  " + L("Save the drive as an image file…"), this, [this] { runTool("save"); });
    toolsBtn_->setMenu(menu);
    logBtn_ = new QPushButton("📜  " + L("Log"));
    connect(logBtn_, &QPushButton::clicked, this, [this] {
        logDlg_->show();
        logDlg_->raise();
    });
    cancelBtn_ = new QPushButton(L("Cancel"));
    cancelBtn_->setEnabled(false);
    connect(cancelBtn_, &QPushButton::clicked, this, [this] {
        engine_->cancel();
        setStatus(L("Cancelling… the drive is left in an unfinished state"), 3);
    });
    startBtn_ = new QPushButton(L("START"));
    startBtn_->setProperty("variant", "primary");
    startBtn_->setCursor(Qt::PointingHandCursor);
    connect(startBtn_, &QPushButton::clicked, this, &MainWindow::start);
    lockWhileRunning_ << toolsBtn_;
    h->addWidget(toolsBtn_);
    h->addWidget(logBtn_);
    h->addWidget(cancelBtn_);
    h->addWidget(startBtn_);
    v->addLayout(h);
    auto* foot = new QLabel(QString("Developed By <a href=\"https://goshgarhasanov.github.io\" style=\"color:%1; text-decoration:none; font-weight:600;\">"
                                    "Goshgar Hasanzadeh</a>  ·  BootAz %2  ·  %3")
                                .arg(theme::current().accent.name(), BOOTAZ_VERSION, L("Made in Azerbaijan")));
    foot->setObjectName("Footer");
    foot->setOpenExternalLinks(true);
    foot->setAlignment(Qt::AlignRight);
    v->addWidget(foot);
    return w;
}

// ---------------------------------------------------------------- drives

void MainWindow::refreshDrives(bool keepSelection)
{
    const QString keep = keepSelection && driveCombo_->currentIndex() >= 0 ? driveCombo_->currentData().toString() : QString();
    QStringList checked;
    for (int i = 0; i < multiList_->count(); ++i)
        if (multiList_->item(i)->checkState() == Qt::Checked)
            checked << multiList_->item(i)->data(Qt::UserRole).toString();
    ListOptions lo;
    lo.includeFixed = showFixed_->isChecked();
    lo.includeVirtual = showFixed_->isChecked() || !qEnvironmentVariableIsEmpty("BOOTAZ_SHOW_VIRTUAL");
    try {
        drives_ = listDrives(lo);
    } catch (...) {
        drives_.clear();
    }
    // Drives with the running system are listed (so the user sees them) but never selectable.
    QString sig;
    driveCombo_->blockSignals(true);
    driveCombo_->clear();
    multiList_->clear();
    int sel = -1;
    for (size_t i = 0; i < drives_.size(); ++i) {
        const DriveInfo& d = drives_[i];
        sig += Q(d.id) + ";";
        QString name = Q(d.displayName());
        if (d.system)
            name = "⛔ " + name + "  — " + L("system drive");
        else if (!d.usb && !d.removable)
            name = "⚠ " + name;
        driveCombo_->addItem(d.usb ? "🔌  " + name : "💽  " + name, Q(d.path));
        if (d.system) {
            if (auto* m = qobject_cast<QStandardItemModel*>(driveCombo_->model()))
                m->item(int(i))->setEnabled(false);
        } else if (sel < 0 || Q(d.path) == keep) {
            sel = int(i);
        }
        auto* it = new QListWidgetItem(name);
        it->setData(Qt::UserRole, Q(d.path));
        it->setFlags(d.system ? Qt::NoItemFlags : (Qt::ItemIsEnabled | Qt::ItemIsUserCheckable));
        it->setCheckState(checked.contains(Q(d.path)) ? Qt::Checked : Qt::Unchecked);
        multiList_->addItem(it);
    }
    if (drives_.empty())
        driveCombo_->addItem(L("No USB drive found — plug one in"), QString());
    driveCombo_->setCurrentIndex(sel < 0 ? 0 : sel);
    driveCombo_->blockSignals(false);
    driveSignature_ = sig;
    updateDriveInfo();
    updateControls();
}

void MainWindow::pollDrives()
{
    if (running_ || analysing_)
        return;
    ListOptions lo;
    lo.includeFixed = showFixed_->isChecked();
    lo.includeVirtual = showFixed_->isChecked() || !qEnvironmentVariableIsEmpty("BOOTAZ_SHOW_VIRTUAL");
    QString sig;
    try {
        for (const DriveInfo& d : listDrives(lo))
            sig += Q(d.id) + ";";
    } catch (...) {
        return;
    }
    if (sig != driveSignature_) {
        refreshDrives();
        log(L("The list of drives changed"));
    }
}

std::vector<DriveInfo> MainWindow::selectedDrives() const
{
    std::vector<DriveInfo> out;
    if (multiCheck_->isChecked()) {
        for (int i = 0; i < multiList_->count(); ++i)
            if (multiList_->item(i)->checkState() == Qt::Checked)
                for (const DriveInfo& d : drives_)
                    if (Q(d.path) == multiList_->item(i)->data(Qt::UserRole).toString())
                        out.push_back(d);
        return out;
    }
    const QString p = driveCombo_->currentData().toString();
    for (const DriveInfo& d : drives_)
        if (Q(d.path) == p && !d.system)
            out.push_back(d);
    return out;
}

void MainWindow::updateDriveInfo()
{
    const auto sel = selectedDrives();
    if (sel.empty() || multiCheck_->isChecked()) {
        driveInfo_->setText(multiCheck_->isChecked() ? L("Tick the drives to write") : QString());
        return;
    }
    const DriveInfo& d = sel.front();
    QString t = QString("%1 · %2 · %3").arg(Q(d.bus), HB(d.size), Q(d.path));
    if (!d.serial.empty())
        t += " · " + L("serial") + " " + Q(d.serial);
    QStringList parts;
    for (const VolumeInfo& vv : d.volumes)
        parts << QString("%1 %2 %3").arg(vv.label.empty() ? "—" : Q(vv.label), Q(vv.fsType), HB(vv.size));
    if (!parts.isEmpty())
        t += "\n" + L("Partitions:") + " " + parts.join("  ·  ");
    driveInfo_->setText(t);
}

// ---------------------------------------------------------------- image

void MainWindow::chooseImage()
{
    QSettings s = settings();
    const QString dir = s.value("lastDir", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
    const QString f = QFileDialog::getOpenFileName(this, L("Choose an image"), dir,
                                                   L("Disk images") + " (*.iso *.img *.bin *.raw *.dd *.vhd *.gz *.xz *.zst *.bz2);;" + L("All files") + " (*)");
    if (f.isEmpty())
        return;
    s.setValue("lastDir", QFileInfo(f).absolutePath());
    bootCombo_->setCurrentIndex(0);
    setImage(f);
}

void MainWindow::setImage(const QString& path)
{
    imagePath_->setText(QDir::toNativeSeparators(path));
    haveImage_ = false;
    analysing_ = true;
    imageNote_->setText(L("Reading the image…"));
    bar_->setProgress(-1);
    bar_->setText(L("Reading the image…"));
    updateControls();
    auto* w = new QFutureWatcher<QPair<ImageInfo, QString>>(this);
    connect(w, &QFutureWatcherBase::finished, this, [this, w] {
        const auto [info, err] = w->result();
        w->deleteLater();
        analysing_ = false;
        bar_->setState(0);
        bar_->setProgress(0);
        bar_->setText(L("Ready"));
        if (!err.isEmpty()) {
            imageNote_->clear();
            setStatus(err, 2);
            image_ = ImageInfo{};
            showImageInfo();
            updateControls();
            return;
        }
        image_ = info;
        haveImage_ = true;
        winOptsSet_ = false;
        // Sensible defaults for this image (the user can still change them).
        modeCombo_->setCurrentIndex(0);
        persistSlider_->setValue(0);
        if (image_.uefiBootable()) {
            schemeCombo_->setCurrentIndex(0);   // GPT, UEFI
        } else {
            schemeCombo_->setCurrentIndex(1);   // MBR for BIOS-only images
        }
        labelEdit_->clear();
        showImageInfo();
        updateControls();
        setStatus(L("Image ready. Check the options and press START."));
        log(L("Image") + ": " + Q(image_.path));
    });
    const std::string p = path.toStdString();
    w->setFuture(QtConcurrent::run([p] {
        try {
            return qMakePair(analyzeImage(p), QString());
        } catch (const Error& e) {
            return qMakePair(ImageInfo{}, L(e.what()));
        } catch (const std::exception& e) {
            return qMakePair(ImageInfo{}, QString::fromUtf8(e.what()));
        }
    }));
}

void MainWindow::showImageInfo()
{
    while (QLayoutItem* it = chips_->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
    if (!haveImage_) {
        imageNote_->clear();
        return;
    }
    const theme::Palette& p = theme::current();
    const ImageInfo& i = image_;
    if (!i.friendlyName.empty())
        chips_->addWidget(chip(Q(i.friendlyName), i.isWindows ? p.accent : p.accent2));
    const char* kinds[] = {N_("Unknown"), N_("ISO image"), N_("Disk image"), N_("VHD disk image"), N_("Compressed disk image")};
    chips_->addWidget(chip(L(kinds[int(i.kind)]), p.muted));
    chips_->addWidget(chip(HB(i.dataSize ? i.dataSize : i.fileSize), p.muted));
    if (i.uefiBootable())
        chips_->addWidget(chip("UEFI", p.good));
    if (i.biosBootable())
        chips_->addWidget(chip("BIOS", p.good));
    if (i.efiSigned)
        chips_->addWidget(chip(i.efiOldMicrosoftCa ? "Secure Boot ⚠" : "Secure Boot ✓", i.efiOldMicrosoftCa ? p.warn : p.good));
    if (!i.over4GB.empty())
        chips_->addWidget(chip("> 4 GB", p.warn));
    QStringList notes;
    if (!i.label.empty())
        notes << L("Label") + ": " + Q(i.label);
    if (i.fileCount)
        notes << QString::number(i.fileCount) + " " + L("files");
    if (i.isohybrid)
        notes << L("hybrid image (can be written as is)");
    if (i.efiOldMicrosoftCa)
        notes << L("signed with Microsoft's old 2011 certificate");
    imageNote_->setText(notes.join("  ·  "));
}

void MainWindow::updateControls()
{
    const bool bootable = bootCombo_->currentIndex() == 0;
    imagePath_->setEnabled(bootable);
    selectBtn_->setEnabled(bootable && !running_);
    hashBtn_->setEnabled(bootable && haveImage_ && !running_);
    vmBtn_->setEnabled(bootable && haveImage_ && !running_);
    chipsBox_->setVisible(bootable);
    imageNote_->setVisible(bootable);

    // Write mode: DD disables everything about partitions and file systems.
    bool dd = false;
    if (bootable && haveImage_) {
        if (modeCombo_->currentIndex() == 0)
            dd = image_.ddRecommended();
        else
            dd = modeCombo_->currentIndex() == 2;
        if (!image_.canIsoMode())
            dd = true;
    }
    modeCombo_->setEnabled(bootable && haveImage_ && image_.canIsoMode() && !running_);
    for (QWidget* w : {static_cast<QWidget*>(schemeCombo_), static_cast<QWidget*>(targetCombo_), static_cast<QWidget*>(fsCombo_),
                       static_cast<QWidget*>(clusterCombo_), static_cast<QWidget*>(labelEdit_), static_cast<QWidget*>(quickCheck_),
                       static_cast<QWidget*>(autorunCheck_)})
        w->setEnabled(!dd && !running_);

    // Target system depends on the scheme (GPT boots with UEFI only).
    const bool gpt = schemeCombo_->currentIndex() == 0;
    const int prevTarget = targetCombo_->currentData().toInt();
    targetCombo_->blockSignals(true);
    targetCombo_->clear();
    if (gpt) {
        targetCombo_->addItem(L("UEFI (non CSM)"), int(TargetSystem::Uefi));
    } else {
        targetCombo_->addItem(L("BIOS and UEFI"), int(TargetSystem::BiosUefi));
        targetCombo_->addItem(L("BIOS (or UEFI-CSM)"), int(TargetSystem::Bios));
        targetCombo_->addItem(L("UEFI (non CSM)"), int(TargetSystem::Uefi));
        const int idx = targetCombo_->findData(prevTarget);
        if (idx >= 0)
            targetCombo_->setCurrentIndex(idx);
    }
    targetCombo_->blockSignals(false);
    targetHint_->setText(dd ? L("DD mode writes the image exactly as it was built; it boots the way the image was made.")
                            : gpt ? L("For computers from about 2012 on. If the computer does not see the drive, turn off CSM/Legacy in its firmware setup.")
                                  : L("MBR works on old (BIOS) and new (UEFI) computers. Choose GPT for newer PCs that have CSM turned off."));

    const bool win = bootable && haveImage_ && image_.isWindows && !dd;
    winRow_->setVisible(win);
    winLayoutRow_->setVisible(win && image_.windowsInstallSize >= 4ULL * GiB);
    if (win) {
        QStringList s;
        if (winOpts_.bypassHardwareChecks) s << L("no TPM/Secure Boot/RAM check");
        if (winOpts_.noOnlineAccount) s << L("no Microsoft account");
        if (!winOpts_.localAccount.empty()) s << L("local account") + " \"" + Q(winOpts_.localAccount) + "\"";
        if (winOpts_.skipPrivacyQuestions) s << L("no privacy questions");
        if (winOpts_.disableBitLocker) s << L("no BitLocker");
        if (!winOpts_.locale.empty()) s << Q(winOpts_.locale);
        if (!winOpts_.computerName.empty()) s << Q(winOpts_.computerName);
        winSummary_->setText(winOptsSet_ ? (s.isEmpty() ? L("Standard Windows setup") : s.join(" · "))
                                         : L("You will be asked before writing"));
    }
    const bool persist = bootable && haveImage_ && !image_.persistenceKind.empty() && !dd;
    persistRow_->setVisible(persist);
    if (persist && !selectedDrives().empty()) {
        const uint64_t room = selectedDrives().front().size > image_.filesTotal ? selectedDrives().front().size - image_.filesTotal : 0;
        persistSlider_->setMaximum(int(std::max<uint64_t>(1, room / GiB > 1 ? room / GiB - 1 : 0)));
    }

    const bool ready = !running_ && !analysing_ && !selectedDrives().empty() && (!bootable || haveImage_);
    startBtn_->setEnabled(ready);
}

// ---------------------------------------------------------------- start

WriteOptions MainWindow::collectOptions(const DriveInfo& d) const
{
    WriteOptions o;
    const bool bootable = bootCombo_->currentIndex() == 0;
    if (bootable)
        o.image = image_.path;
    o.mode = modeCombo_->currentIndex() == 1 ? WriteMode::Iso : modeCombo_->currentIndex() == 2 ? WriteMode::Dd : WriteMode::Auto;
    o.scheme = schemeCombo_->currentIndex() == 0 ? Scheme::Gpt : Scheme::Mbr;
    o.target = TargetSystem(targetCombo_->currentData().toInt());
    const int fs = fsCombo_->currentData().toInt();
    o.fsAuto = fs < 0;
    if (!o.fsAuto)
        o.fs = FsType(fs);
    o.label = labelEdit_->text().trimmed().toStdString();
    if (!bootable && o.label.empty())
        o.label = "USB";
    o.clusterSize = uint32_t(clusterCombo_->currentData().toInt());
    o.quickFormat = quickCheck_->isChecked();
    o.badBlockPasses = badCombo_->currentData().toInt();
    o.verify = verifyCheck_->isChecked();
    o.eject = ejectCheck_->isChecked();
    o.autorun = autorunCheck_->isChecked();
    o.persistenceBytes = persistRow_->isVisible() ? uint64_t(persistSlider_->value()) * GiB : 0;
    if (winLayoutRow_->isVisible())
        o.winLayout = winLayoutCombo_->currentIndex() == 1 ? WinLayout::Dual : winLayoutCombo_->currentIndex() == 2 ? WinLayout::Split : WinLayout::Ntfs;
    if (o.winLayout == WinLayout::Split)
        o.splitWim = true;
    o.windows = winOpts_;
    o.expectedId = d.id;
    return o;
}

void MainWindow::start()
{
    const auto drives = selectedDrives();
    if (drives.empty())
        return;
    const bool bootable = bootCombo_->currentIndex() == 0;
    // Windows images: ask the customisation questions first, as Rufus does.
    if (bootable && image_.isWindows && !winOptsSet_ && modeCombo_->currentIndex() != 2) {
        WindowsOptionsDialog d(winOpts_, image_.isWindows11, this);
        if (d.exec() != QDialog::Accepted)
            return;
        winOpts_ = d.options();
        winOptsSet_ = true;
        updateControls();
    }

    // Plan every drive here first so problems show up before any password prompt.
    QStringList names, warnings;
    QString summary;
    Plan first;
    for (const DriveInfo& d : drives) {
        const Plan plan = planWrite(d, collectOptions(d));
        if (!plan.errors.empty()) {
            QMessageBox::warning(this, "BootAz", Q(d.displayName()) + "\n\n" + Q(plan.errors.front()));
            return;
        }
        if (names.isEmpty())
            first = plan;
        names << Q(d.displayName());
        for (const auto& w : plan.warnings)
            if (!warnings.contains(Q(w)))
                warnings << Q(w);
    }
    if (bootable)
        summary = first.dd ? L("DD mode: the image is written byte for byte.")
                           : QString("%1 · %2 · %3 · %4 · \"%5\"").arg(L("ISO mode"), Q(schemeName(first.options.scheme)),
                                                                       Q(targetName(first.options.target)), Q(fsName(first.options.fs)),
                                                                       Q(first.options.label));
    else
        summary = QString("%1 · %2 · \"%3\"").arg(Q(schemeName(first.options.scheme)), Q(fsName(first.options.fs)), Q(first.options.label));
    if (!confirmErase(this, names, summary, warnings))
        return;

    QStringList args;
    args << (bootable ? "write" : "format");
    for (const DriveInfo& d : drives)
        args << "--device" << Q(d.path);
    const WriteOptions o = first.options;
    if (bootable)
        args << "--image" << Q(o.image) << "--mode" << (first.dd ? "dd" : "iso");
    args << "--scheme" << (o.scheme == Scheme::Gpt ? "gpt" : "mbr") << "--target" << Q(bootaz::lower(targetName(o.target)));
    if (!collectOptions(drives.front()).fsAuto)
        args << "--fs" << Q(bootaz::lower(fsName(o.fs)));
    if (!labelEdit_->text().trimmed().isEmpty() || !bootable)
        args << "--label" << Q(o.label);
    if (o.clusterSize)
        args << "--cluster" << QString::number(o.clusterSize);
    if (!o.quickFormat) args << "--full-format";
    if (o.badBlockPasses) args << "--badblocks" << QString::number(o.badBlockPasses);
    if (!o.verify) args << "--no-verify";
    if (o.eject) args << "--eject";
    if (!o.autorun) args << "--no-autorun";
    if (o.persistenceBytes) args << "--persistence" << QString::number(o.persistenceBytes / GiB) + "G";
    const WriteOptions raw = collectOptions(drives.front());
    if (raw.winLayout != WinLayout::Auto)
        args << "--win-layout" << (raw.winLayout == WinLayout::Dual ? "dual" : raw.winLayout == WinLayout::Split ? "split" : "ntfs");
    if (bootable && image_.isWindows) {
        const WindowsOptions& w = winOpts_;
        if (w.bypassHardwareChecks) args << "--win-bypass";
        if (w.noOnlineAccount) args << "--win-no-online";
        if (!w.localAccount.empty()) args << "--win-local-user" << Q(w.localAccount);
        if (w.skipPrivacyQuestions) args << "--win-no-privacy";
        if (w.disableBitLocker) args << "--win-no-bitlocker";
        if (!w.locale.empty()) args << "--win-locale" << Q(w.locale);
        if (!w.computerName.empty()) args << "--win-computer-name" << Q(w.computerName);
    }
    for (const DriveInfo& d : drives)   // one per --device, same order
        args << "--expect-id" << Q(d.id);
    bool fixed = false;
    for (const DriveInfo& d : drives)
        fixed = fixed || (!d.usb && !d.removable);
    if (fixed)
        args << "--allow-fixed";
    args << "--yes";

    devProgress_.clear();
    for (const DriveInfo& d : drives)
        devProgress_[Q(d.path)] = 0;
    logDlg_->clear();
    log("BootAz " BOOTAZ_VERSION " — " + names.join(", "));
    setRunning(true, bootable ? L("Writing") : L("Formatting"));
    if (!engine_->start(args))
        setRunning(false);
}

void MainWindow::runTool(const QString& what)
{
    const auto drives = selectedDrives();
    if (drives.empty() || drives.size() > 1) {
        QMessageBox::information(this, "BootAz", L("Choose one drive first"));
        return;
    }
    const DriveInfo d = drives.front();
    QStringList args;
    if (what == "fakecheck") {
        args << "fakecheck" << "--device" << Q(d.path);
    } else if (what == "badblocks") {
        if (!confirmErase(this, {Q(d.displayName())}, L("The whole drive is written and read back: this takes a long time."), {}))
            return;
        args << "badblocks" << "--device" << Q(d.path) << "--passes" << "1" << "--yes";
    } else if (what == "restore") {
        if (!confirmErase(this, {Q(d.displayName())},
                          L("The drive gets one partition over its whole size, like when it was new (FAT32 up to 32 GB, exFAT above)."), {}))
            return;
        args << "restore" << "--device" << Q(d.path) << "--yes";
    } else if (what == "save") {
        const QString out = QFileDialog::getSaveFileName(this, L("Save the drive as an image file…"),
                                                         QDir::home().filePath("usb-backup.img.zst"),
                                                         "zstd (*.img.zst);;gzip (*.img.gz);;xz (*.img.xz);;raw (*.img);;VHD (*.vhd)");
        if (out.isEmpty())
            return;
        args << "save" << "--device" << Q(d.path) << "--out" << out << "--used-only";
    }
    if (!d.usb && !d.removable)
        args << "--allow-fixed";
    if (what != "save")
        args << "--expect-id" << Q(d.id);
    devProgress_.clear();
    devProgress_[Q(d.path)] = 0;
    lastReport_ = {};
    setRunning(true, what);
    if (!engine_->start(args))
        setRunning(false);
}

void MainWindow::testInVm()
{
    if (!haveImage_)
        return;
#ifdef _WIN32
    QString qemu = QStandardPaths::findExecutable("qemu-system-x86_64");
    if (qemu.isEmpty() && QFileInfo::exists("C:/Program Files/qemu/qemu-system-x86_64.exe"))
        qemu = "C:/Program Files/qemu/qemu-system-x86_64.exe";
#else
    const QString qemu = QStandardPaths::findExecutable("qemu-system-x86_64");
#endif
    if (qemu.isEmpty()) {
        QMessageBox::information(this, "BootAz", L("QEMU is not installed. Install it to try images in a virtual machine:") +
#ifdef _WIN32
                                                     "\n\nhttps://www.qemu.org/download/#windows");
#else
                                                     "\n\nsudo apt install qemu-system-x86 ovmf");
#endif
        return;
    }
    if (image_.compression != Compression::None) {
        QMessageBox::information(this, "BootAz", L("Compressed images cannot be started directly; write them first."));
        return;
    }
    QStringList args = {"-m", "4096", "-smp", "2", "-machine", "q35", "-name", "BootAz test: " + Q(image_.fileName)};
#ifndef _WIN32
    if (QFileInfo(QStringLiteral("/dev/kvm")).isWritable())
        args << "-enable-kvm" << "-cpu" << "host";
#endif
    // UEFI firmware when the image boots that way and OVMF is installed.
    QString ovmf;
    for (const QString c : {"/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_CODE.fd",
                             "/usr/share/qemu/edk2-x86_64-code.fd", "C:/Program Files/qemu/share/edk2-x86_64-code.fd"})
        if (QFileInfo::exists(c)) {
            ovmf = c;
            break;
        }
    if (image_.uefiBootable() && !ovmf.isEmpty())
        args << "-drive" << "if=pflash,format=raw,readonly=on,file=" + ovmf;
    QString file = Q(image_.path);
    file.replace(",", ",,");   // QEMU option syntax
    if (image_.kind == ImageKind::Iso && !image_.isohybrid)
        args << "-drive" << "file=" + file + ",media=cdrom,readonly=on";
    else
        args << "-drive" << "file=" + file + ",format=raw,if=none,id=usb,snapshot=on" << "-device" << "qemu-xhci"
             << "-device" << "usb-storage,drive=usb,bootindex=0";
    if (QProcess::startDetached(qemu, args))
        setStatus(L("The image is starting in a virtual machine. Nothing is written to the drive."));
    else
        setStatus(L("QEMU could not be started"), 2);
}

// ---------------------------------------------------------------- job progress

void MainWindow::setRunning(bool running, const QString& title)
{
    running_ = running;
    jobTitle_ = title;
    for (QWidget* w : lockWhileRunning_)
        w->setEnabled(!running);
    cancelBtn_->setEnabled(running);
    if (running) {
        lastError_.clear();
        jobClock_.start();
        bar_->setState(0);
        bar_->setProgress(-1);
        bar_->setText(L("Waiting for administrator permission…"));
        setStatus(L("Working… do not unplug the drive."));
    }
    updateControls();
}

void MainWindow::onProgress(const QString& dev, const QString& stage, qint64 done, qint64 total, double speed)
{
    const QString key = devProgress_.contains(dev) ? dev : (devProgress_.isEmpty() ? QString() : devProgress_.firstKey());
    devProgress_[key] = total > 0 ? double(done) / double(total) : -1;
    double sum = 0;
    bool unknown = false;
    for (double f : devProgress_) {
        if (f < 0)
            unknown = true;
        sum += std::max(0.0, f);
    }
    const double f = devProgress_.isEmpty() ? 0 : sum / devProgress_.size();
    bar_->setProgress(unknown && devProgress_.size() == 1 ? -1 : f);
    QString text = stage;
    if (total > 0) {
        text += QString("  ·  %1%").arg(int(f * 100));
        if (devProgress_.size() == 1) {
            if (speed > 0) {
                text += "  ·  " + HB(uint64_t(speed)) + "/s";
                const QString eta = etaText(double(total - done) / speed);
                if (!eta.isEmpty())
                    text += "  ·  " + L("left") + " " + eta;
            }
        }
    }
    bar_->setText(text);
}

void MainWindow::onFinished(bool ok, bool cancelled)
{
    const QString job = jobTitle_;
    setRunning(false);
    const qint64 secs = jobClock_.elapsed() / 1000;
    const QString took = QString("  (%1)").arg(etaText(double(std::max<qint64>(1, secs))));
    if (cancelled) {
        bar_->setState(2);
        bar_->setText(L("Cancelled"));
        setStatus(L("Cancelled. Write the drive again before using it."), 3);
    } else if (ok) {
        bar_->setState(1);
        bar_->setProgress(1);
        QString msg;
        const QString type = lastReport_.value("type").toString();
        if (job == "fakecheck" && type == "fakecheck")
            msg = lastReport_.value("fake").toBool()
                      ? L("FAKE DRIVE") : L("The drive really has the capacity it reports. Your files were kept.");
        else if (job == "badblocks")
            msg = L("No bad blocks found");
        else if (job == "save")
            msg = L("The image file was saved.");
        else
            msg = L("Ready. The drive was written successfully.");
        bar_->setText(L("Done"));
        setStatus("✓ " + msg + took, 1);
        QApplication::alert(this);
        QApplication::beep();
    } else {
        bar_->setState(2);
        bar_->setText(L("Failed"));
        QString msg = lastError_.isEmpty() ? L("The job failed. See the log for details.") : lastError_;
        if (job == "fakecheck" && lastReport_.value("fake").toBool())
            msg = format(T("FAKE DRIVE: it reports %1 but only about %2 can really be stored"),
                         {humanBytes(uint64_t(lastReport_.value("size").toDouble())), humanBytes(uint64_t(lastReport_.value("real").toDouble()))})
                      .c_str();
        setStatus("✗ " + msg, 2);
        QApplication::alert(this);
        QMessageBox::warning(this, "BootAz", msg);
    }
    refreshDrives();
}

void MainWindow::log(const QString& line) { logDlg_->append(line); }

void MainWindow::setStatus(const QString& text, int kind)
{
    const theme::Palette& p = theme::current();
    const QColor c = kind == 1 ? p.good : kind == 2 ? p.bad : kind == 3 ? p.warn : p.text;
    status_->setStyleSheet(QString("color:%1;").arg(c.name()));
    status_->setText(text);
}

// ---------------------------------------------------------------- window events

void MainWindow::dragEnterEvent(QDragEnterEvent* e)
{
    if (!running_ && e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e)
{
    const auto urls = e->mimeData()->urls();
    if (urls.isEmpty() || !urls.front().isLocalFile())
        return;
    bootCombo_->setCurrentIndex(0);
    setImage(urls.front().toLocalFile());
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (running_) {
        const auto r = QMessageBox::question(this, "BootAz", L("A job is running. Cancel it and quit? The drive will be left unfinished."));
        if (r != QMessageBox::Yes) {
            e->ignore();
            return;
        }
        engine_->cancel();
    }
    e->accept();
}
