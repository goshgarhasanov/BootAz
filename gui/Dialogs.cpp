#include "Dialogs.h"
#include "Brand.h"
#include "Theme.h"
#include "Widgets.h"

#include "Hash.h"
#include "I18n.h"
#include "Jobs.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <atomic>

static QString L(const char* s) { return QString::fromStdString(T(s)); }

// ---------------------------------------------------------------- Windows options

WindowsOptionsDialog::WindowsOptionsDialog(const bootaz::WindowsOptions& cur, bool windows11, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(L("Windows setup options"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    v->setSpacing(8);
    auto* t = new QLabel(L("Customise the Windows installation"));
    t->setObjectName("CardTitle");
    v->addWidget(t);
    v->addWidget(hint(L("These answers are written to the drive (autounattend.xml) and used automatically by Windows setup.")));

    bypass_ = new QCheckBox(L("Remove the requirement for 4 GB+ RAM, Secure Boot and TPM 2.0"));
    bypass_->setChecked(cur.bypassHardwareChecks || (windows11 && !cur.any()));
    noOnline_ = new QCheckBox(L("Remove the requirement for an online Microsoft account"));
    noOnline_->setChecked(cur.noOnlineAccount || !cur.any());
    local_ = new QCheckBox(L("Create a local account with the name:"));
    localName_ = new QLineEdit(cur.localAccount.empty() ? qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME")).left(20)
                                                        : QString::fromStdString(cur.localAccount));
    local_->setChecked(!cur.localAccount.empty());
    privacy_ = new QCheckBox(L("Skip the privacy questions (answer \"no\" to data collection)"));
    privacy_->setChecked(cur.skipPrivacyQuestions || !cur.any());
    bitlocker_ = new QCheckBox(L("Turn off automatic BitLocker drive encryption"));
    bitlocker_->setChecked(cur.disableBitLocker);
    locale_ = new QCheckBox(L("Use this computer's regional format and keyboard:"));
    localeName_ = new QLineEdit(cur.locale.empty() ? QString::fromStdString(bootaz::systemWindowsLocale()) : QString::fromStdString(cur.locale));
    locale_->setChecked(!cur.locale.empty());
    computer_ = new QCheckBox(L("Computer name:"));
    computerName_ = new QLineEdit(QString::fromStdString(cur.computerName));
    computerName_->setPlaceholderText("BAKI-PC");
    computer_->setChecked(!cur.computerName.empty());

    auto* g = new QGridLayout;
    g->setVerticalSpacing(8);
    int r = 0;
    g->addWidget(bypass_, r++, 0, 1, 2);
    g->addWidget(noOnline_, r++, 0, 1, 2);
    g->addWidget(local_, r, 0);
    g->addWidget(localName_, r++, 1);
    g->addWidget(privacy_, r++, 0, 1, 2);
    g->addWidget(bitlocker_, r++, 0, 1, 2);
    g->addWidget(locale_, r, 0);
    g->addWidget(localeName_, r++, 1);
    g->addWidget(computer_, r, 0);
    g->addWidget(computerName_, r++, 1);
    v->addLayout(g);
    v->addWidget(hint(L("The local account starts with an empty password; Windows asks for a new one at the first sign-in. On Windows 11 24H2 and later keep the computer offline during setup when you skip the Microsoft account.")));

    auto* br = new QHBoxLayout;
    br->addStretch();
    auto* cancel = new QPushButton(L("Cancel"));
    auto* ok = new QPushButton(L("OK"));
    ok->setProperty("variant", "primary");
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(ok, &QPushButton::clicked, this, [this] {
        if (local_->isChecked() && !bootaz::validAccountName(localName_->text().toStdString())) {
            localName_->setFocus();
            localName_->setStyleSheet("border-color: #ef3340;");
            return;
        }
        if (computer_->isChecked() && !bootaz::validComputerName(computerName_->text().toStdString())) {
            computerName_->setFocus();
            computerName_->setStyleSheet("border-color: #ef3340;");
            return;
        }
        accept();
    });
    br->addWidget(cancel);
    br->addWidget(ok);
    v->addLayout(br);
}

bootaz::WindowsOptions WindowsOptionsDialog::options() const
{
    bootaz::WindowsOptions o;
    o.bypassHardwareChecks = bypass_->isChecked();
    o.noOnlineAccount = noOnline_->isChecked();
    if (local_->isChecked())
        o.localAccount = localName_->text().trimmed().toStdString();
    o.skipPrivacyQuestions = privacy_->isChecked();
    o.disableBitLocker = bitlocker_->isChecked();
    if (locale_->isChecked())
        o.locale = localeName_->text().trimmed().toStdString();
    if (computer_->isChecked())
        o.computerName = computerName_->text().trimmed().toStdString();
    return o;
}

// ---------------------------------------------------------------- hashes

HashDialog::HashDialog(const QString& path, QWidget* parent) : QDialog(parent), path_(path)
{
    setWindowTitle(L("Checksums"));
    resize(720, 360);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    auto* t = new QLabel(QFileInfo(path).fileName());
    t->setObjectName("CardTitle");
    v->addWidget(t);
    bar_ = new FlashBar;
    v->addWidget(bar_);
    auto* g = new QGridLayout;
    const char* names[4] = {"MD5", "SHA-1", "SHA-256", "SHA-512"};
    for (int i = 0; i < 4; ++i) {
        g->addWidget(new QLabel(names[i]), i, 0);
        fields_[i] = new QLineEdit;
        fields_[i]->setReadOnly(true);
        fields_[i]->setFont(QFont("monospace"));
        g->addWidget(fields_[i], i, 1);
        auto* copy = new QPushButton(L("Copy"));
        copy->setProperty("variant", "flat");
        connect(copy, &QPushButton::clicked, this, [this, i] { QApplication::clipboard()->setText(fields_[i]->text()); });
        g->addWidget(copy, i, 2);
    }
    v->addLayout(g);
    auto* er = new QHBoxLayout;
    er->addWidget(new QLabel(L("Expected checksum:")));
    expected_ = new QLineEdit;
    expected_->setPlaceholderText(L("paste the checksum from the download page"));
    er->addWidget(expected_, 1);
    v->addLayout(er);
    verdict_ = new QLabel;
    verdict_->setWordWrap(true);
    v->addWidget(verdict_);
    connect(expected_, &QLineEdit::textChanged, this, &HashDialog::compare);
    auto* close = new QPushButton(L("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* br = new QHBoxLayout;
    br->addStretch();
    br->addWidget(close);
    v->addLayout(br);

    bootaz::HashAlgo pubAlgo = bootaz::HashAlgo::Sha256;
    published_ = QString::fromStdString(bootaz::findPublishedHash(path.toStdString(), pubAlgo));
    if (!published_.isEmpty())
        expected_->setText(published_);

    // Hash in the background; the progress comes back through an atomic.
    auto* progress = new std::atomic<double>(0);
    auto* poll = new QTimer(this);
    connect(poll, &QTimer::timeout, this, [this, progress] {
        bar_->setProgress(*progress);
        bar_->setText(QString::number(int(*progress * 100)) + "%");
    });
    poll->start(150);
    bar_->setProgress(0);
    auto* w = new QFutureWatcher<std::vector<std::pair<bootaz::HashAlgo, std::string>>>(this);
    connect(w, &QFutureWatcherBase::finished, this, [this, w, poll, progress] {
        poll->stop();
        delete progress;
        const auto res = w->result();
        w->deleteLater();
        for (size_t i = 0; i < res.size() && i < 4; ++i)
            fields_[i]->setText(QString::fromStdString(res[i].second));
        bar_->setState(res.empty() ? 2 : 1);
        bar_->setProgress(1);
        bar_->setText(res.empty() ? L("Failed") : L("Done"));
        compare();
    });
    const QString p = path;
    w->setFuture(QtConcurrent::run([p, progress] {
        bootaz::JobContext ctx;
        ctx.onProgress = [progress](const bootaz::Progress& pr) {
            if (pr.total)
                *progress = double(pr.done) / double(pr.total);
        };
        try {
            return bootaz::hashFile(p.toStdString(), {bootaz::HashAlgo::Md5, bootaz::HashAlgo::Sha1, bootaz::HashAlgo::Sha256, bootaz::HashAlgo::Sha512}, ctx);
        } catch (...) {
            return std::vector<std::pair<bootaz::HashAlgo, std::string>>{};
        }
    }));
}

void HashDialog::compare()
{
    const QString want = expected_->text().trimmed().toLower();
    if (want.isEmpty() || fields_[2]->text().isEmpty()) {
        verdict_->clear();
        return;
    }
    for (auto* f : fields_)
        if (f->text() == want) {
            verdict_->setObjectName("Good");
            verdict_->setText("✓ " + (want == published_ ? L("Matches the official checksum published with the image") : L("The checksum matches")));
            verdict_->setStyleSheet(QString("color:%1; font-weight:600;").arg(theme::current().good.name()));
            return;
        }
    verdict_->setStyleSheet(QString("color:%1; font-weight:600;").arg(theme::current().bad.name()));
    verdict_->setText("✗ " + L("The checksum does NOT match: the file is damaged or not the original"));
}

// ---------------------------------------------------------------- log

LogDialog::LogDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(L("Log"));
    resize(760, 460);
    auto* v = new QVBoxLayout(this);
    text_ = new QPlainTextEdit;
    text_->setReadOnly(true);
    text_->setFont(QFont("monospace", 9));
    v->addWidget(text_);
    auto* br = new QHBoxLayout;
    auto* copy = new QPushButton(L("Copy"));
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(text_->toPlainText()); });
    auto* save = new QPushButton(L("Save…"));
    connect(save, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, L("Save…"), "bootaz-log.txt", "*.txt");
        if (f.isEmpty())
            return;
        QFile out(f);
        if (out.open(QIODevice::WriteOnly | QIODevice::Text))
            out.write(text_->toPlainText().toUtf8());
    });
    auto* clr = new QPushButton(L("Clear"));
    connect(clr, &QPushButton::clicked, this, &LogDialog::clear);
    auto* close = new QPushButton(L("Close"));
    connect(close, &QPushButton::clicked, this, &QDialog::hide);
    br->addWidget(copy);
    br->addWidget(save);
    br->addWidget(clr);
    br->addStretch();
    br->addWidget(close);
    v->addLayout(br);
}

void LogDialog::append(const QString& line)
{
    text_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + line);
}

void LogDialog::clear() { text_->clear(); }

// ---------------------------------------------------------------- about

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(L("About BootAz"));
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(28, 24, 28, 22);
    v->setSpacing(10);
    auto* logo = new QLabel;
    const qreal dpr = devicePixelRatioF();
    QPixmap pm = brand::wordmarkPixmap(int(64 * dpr), theme::current().dark);
    pm.setDevicePixelRatio(dpr);
    logo->setPixmap(pm);
    logo->setAlignment(Qt::AlignCenter);
    v->addWidget(logo);
    auto* ver = new QLabel(L("Version") + " " + BOOTAZ_VERSION);
    ver->setAlignment(Qt::AlignCenter);
    ver->setObjectName("Muted");
    v->addWidget(ver);
    auto* desc = new QLabel(L("Bootable USB drives from Windows and Linux images. Made in Azerbaijan, for Windows and Linux."));
    desc->setWordWrap(true);
    desc->setAlignment(Qt::AlignCenter);
    v->addWidget(desc);
    auto* dev = new QLabel(QString("<b>Developed By Goshgar Hasanzadeh</b><br><a href=\"https://goshgarhasanov.github.io\" style=\"color:%1\">goshgarhasanov.github.io</a>")
                               .arg(theme::current().accent.name()));
    dev->setAlignment(Qt::AlignCenter);
    dev->setOpenExternalLinks(true);
    v->addWidget(dev);
    auto* lic = new QLabel(L("Licence: GPL-3.0. Uses boot code from ms-sys and the UEFI:NTFS drivers from the Rufus project (Pete Batard)."));
    lic->setWordWrap(true);
    lic->setAlignment(Qt::AlignCenter);
    lic->setObjectName("Hint");
    v->addWidget(lic);
    auto* ok = new QPushButton(L("Close"));
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    auto* br = new QHBoxLayout;
    br->addStretch();
    br->addWidget(ok);
    br->addStretch();
    v->addLayout(br);
}

// ---------------------------------------------------------------- confirm

bool confirmErase(QWidget* parent, const QStringList& drives, const QString& summary, const QStringList& warnings)
{
    QDialog d(parent);
    d.setWindowTitle(L("Confirm"));
    auto* v = new QVBoxLayout(&d);
    v->setContentsMargins(22, 20, 22, 18);
    v->setSpacing(10);
    auto* t = new QLabel("⚠  " + L("ALL DATA ON THIS DRIVE WILL BE DESTROYED"));
    t->setStyleSheet(QString("color:%1; font-size:13pt; font-weight:800;").arg(theme::current().bad.name()));
    v->addWidget(t);
    for (const QString& dr : drives) {
        auto* l = new QLabel("•  " + dr);
        l->setStyleSheet("font-weight:600;");
        v->addWidget(l);
    }
    if (!summary.isEmpty())
        v->addWidget(hint(summary));
    for (const QString& w : warnings) {
        auto* l = new QLabel("!  " + w);
        l->setWordWrap(true);
        l->setStyleSheet(QString("color:%1;").arg(theme::current().warn.name()));
        v->addWidget(l);
    }
    auto* sure = new QCheckBox(L("I understand: everything on the drive will be erased"));
    v->addWidget(sure);
    auto* br = new QHBoxLayout;
    br->addStretch();
    auto* cancel = new QPushButton(L("Cancel"));
    auto* go = new QPushButton(L("Erase and write"));
    go->setProperty("variant", "danger");
    go->setEnabled(false);
    QObject::connect(sure, &QCheckBox::toggled, go, &QPushButton::setEnabled);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(go, &QPushButton::clicked, &d, &QDialog::accept);
    br->addWidget(cancel);
    br->addWidget(go);
    v->addLayout(br);
    cancel->setFocus();
    return d.exec() == QDialog::Accepted;
}
