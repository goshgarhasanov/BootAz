#include "Downloads.h"
#include "Widgets.h"

#include "Hash.h"
#include "I18n.h"
#include "Jobs.h"

#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrent>

static QString L(const char* s) { return QString::fromStdString(T(s)); }

namespace {

// "hash *file" / "hash  file" lines -> (file, hash) pairs.
QList<QPair<QString, QString>> sumLines(const QByteArray& text)
{
    QList<QPair<QString, QString>> out;
    for (const QByteArray& raw : text.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const auto m = QRegularExpression(R"(^([0-9a-fA-F]{64})\s+\*?(.+)$)").match(line);
        if (m.hasMatch())
            out.append({m.captured(2).trimmed(), m.captured(1).toLower()});
    }
    return out;
}

// Highest version among file names matching a pattern ("ubuntu-26.04.1-desktop-amd64.iso").
QPair<QString, QString> newest(const QList<QPair<QString, QString>>& sums, const QRegularExpression& re)
{
    QPair<QString, QString> best;
    QVersionNumber bestV;
    for (const auto& [file, hash] : sums) {
        const auto m = re.match(file);
        if (!m.hasMatch())
            continue;
        const QVersionNumber v = QVersionNumber::fromString(m.captured(1));
        if (best.first.isEmpty() || v > bestV) {
            best = {file, hash};
            bestV = v;
        }
    }
    return best;
}

} // namespace

DownloadDialog::DownloadDialog(QWidget* parent) : QDialog(parent), net_(new QNetworkAccessManager(this))
{
    setWindowTitle(L("Download an image"));
    resize(640, 560);
    items_ = {
        {"ubuntu", "Ubuntu Desktop (LTS)", N_("The most popular Linux desktop, long-term support"), "🟠", false, ""},
        {"ubuntu-server", "Ubuntu Server (LTS)", N_("Ubuntu for servers"), "🟠", false, ""},
        {"mint", "Linux Mint Cinnamon", N_("Easy, familiar desktop for people coming from Windows"), "🟢", false, ""},
        {"debian-live", "Debian Live (GNOME)", N_("Stable Debian that runs from the USB"), "🔴", false, ""},
        {"debian", "Debian netinst", N_("Small installer, downloads the rest during setup"), "🔴", false, ""},
        {"fedora", "Fedora Workstation", N_("Latest GNOME desktop and new technology"), "🔵", false, ""},
        {"kali", "Kali Linux Live", N_("Security testing tools"), "⚫", false, ""},
        {"arch", "Arch Linux", N_("Rolling release for experienced users"), "🔷", false, ""},
        {"alpine", "Alpine Linux", N_("Tiny and fast, good for testing"), "🏔", false, ""},
        {"win11-eval", "Windows 11 Enterprise (90-day evaluation)", N_("Direct download from Microsoft; free for 90 days"), "🪟", false, ""},
        {"win11", "Windows 11", N_("Opens Microsoft's official download page"), "🪟", true, "https://www.microsoft.com/software-download/windows11"},
        {"win10", "Windows 10", N_("Opens Microsoft's official download page"), "🪟", true, "https://www.microsoft.com/software-download/windows10ISO"},
    };

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(10);
    auto* title = new QLabel(L("Download an operating system"));
    title->setObjectName("CardTitle");
    v->addWidget(title);
    v->addWidget(hint(L("Links are taken from each project's official site at the moment you download, and the file is checked with the published SHA-256 checksum.")));
    list_ = new QListWidget;
    for (const CatalogItem& it : items_) {
        auto* row = new QListWidgetItem(it.icon + "  " + it.name + "\n      " + L(it.note.toUtf8().constData()));
        list_->addItem(row);
    }
    list_->setCurrentRow(0);
    v->addWidget(list_, 1);

    auto* fr = new QHBoxLayout;
    fr->addWidget(new QLabel(L("Save to:")));
    folder_ = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    fr->addWidget(folder_, 1);
    auto* browse = new QPushButton(L("Choose…"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, L("Save to:"), folder_->text());
        if (!d.isEmpty())
            folder_->setText(d);
    });
    fr->addWidget(browse);
    v->addLayout(fr);

    bar_ = new FlashBar;
    v->addWidget(bar_);
    status_ = new QLabel;
    status_->setObjectName("Muted");
    status_->setWordWrap(true);
    v->addWidget(status_);

    auto* br = new QHBoxLayout;
    br->addStretch();
    cancel_ = new QPushButton(L("Close"));
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (active_) {
            aborted_ = true;
            active_->abort();
        } else {
            reject();
        }
    });
    go_ = new QPushButton(L("Download"));
    go_->setProperty("variant", "primary");
    connect(go_, &QPushButton::clicked, this, &DownloadDialog::startSelected);
    connect(list_, &QListWidget::itemDoubleClicked, this, &DownloadDialog::startSelected);
    br->addWidget(cancel_);
    br->addWidget(go_);
    v->addLayout(br);
}

void DownloadDialog::setBusy(bool busy)
{
    go_->setEnabled(!busy);
    list_->setEnabled(!busy);
    folder_->setEnabled(!busy);
    cancel_->setText(busy ? L("Cancel") : L("Close"));
}

void DownloadDialog::get(const QString& url, const std::function<void(const QByteArray&, const QString&)>& done)
{
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "BootAz/" BOOTAZ_VERSION);
    req.setTransferTimeout(30000);
    QNetworkReply* r = net_->get(req);
    connect(r, &QNetworkReply::finished, this, [r, done] {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError)
            done({}, r->errorString());
        else
            done(r->readAll(), {});
    });
}

void DownloadDialog::resolve(const CatalogItem& it, const std::function<void(const ResolvedImage&, const QString&)>& done)
{
    auto fromSums = [this, done](const QString& base, const QString& sumsFile, const QRegularExpression& re) {
        get(base + sumsFile, [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            const auto [file, hash] = newest(sumLines(d), re);
            if (file.isEmpty())
                return done({}, L("The image was not found in the project's checksum list"));
            ResolvedImage r{base + file, file, hash, re.match(file).captured(1)};
            done(r, {});
        });
    };
    if (it.id == "ubuntu" || it.id == "ubuntu-server") {
        get("https://changelogs.ubuntu.com/meta-release-lts", [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            QString dist;
            QString cur;
            for (const QByteArray& raw : d.split('\n')) {
                const QString l = QString::fromUtf8(raw).trimmed();
                if (l.startsWith("Dist:"))
                    cur = l.mid(5).trimmed();
                if (l == "Supported: 1")
                    dist = cur;
            }
            if (dist.isEmpty())
                return done({}, L("Could not find the latest release"));
            const QString kind = it.id == "ubuntu" ? "desktop" : "live-server";
            fromSums("https://releases.ubuntu.com/" + dist + "/", "SHA256SUMS",
                     QRegularExpression("^ubuntu-([0-9.]+)-" + kind + "-amd64\\.iso$"));
        });
    } else if (it.id == "debian") {
        fromSums("https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/", "SHA256SUMS",
                 QRegularExpression(R"(^debian-([0-9.]+)-amd64-netinst\.iso$)"));
    } else if (it.id == "debian-live") {
        fromSums("https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/", "SHA256SUMS",
                 QRegularExpression(R"(^debian-live-([0-9.]+)-amd64-gnome\.iso$)"));
    } else if (it.id == "kali") {
        fromSums("https://cdimage.kali.org/current/", "SHA256SUMS", QRegularExpression(R"(^kali-linux-([0-9.]+)-live-amd64\.iso$)"));
    } else if (it.id == "mint") {
        const QString base = "https://mirrors.edge.kernel.org/linuxmint/stable/";
        get(base, [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            QVersionNumber best;
            QString bestS;
            auto i = QRegularExpression(R"(href="([0-9]+(?:\.[0-9]+)*)/")").globalMatch(QString::fromUtf8(d));
            while (i.hasNext()) {
                const QString s = i.next().captured(1);
                const QVersionNumber v = QVersionNumber::fromString(s);
                if (bestS.isEmpty() || v > best) {
                    best = v;
                    bestS = s;
                }
            }
            if (bestS.isEmpty())
                return done({}, L("Could not find the latest release"));
            fromSums(base + bestS + "/", "sha256sum.txt", QRegularExpression(R"(^linuxmint-([0-9.]+)-cinnamon-64bit\.iso$)"));
        });
    } else if (it.id == "fedora") {
        get("https://fedoraproject.org/releases.json", [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            ResolvedImage best;
            int bestV = 0;
            for (const QJsonValue& x : QJsonDocument::fromJson(d).array()) {
                const QJsonObject o = x.toObject();
                const QString ver = o.value("version").toString();
                bool num = false;
                const int v = ver.toInt(&num);
                if (!num || o.value("arch").toString() != "x86_64" || o.value("variant").toString() != "Workstation" ||
                    !o.value("link").toString().endsWith(".iso"))
                    continue;
                if (v > bestV) {
                    bestV = v;
                    best.url = o.value("link").toString();
                    best.fileName = QUrl(best.url).fileName();
                    best.sha256 = o.value("sha256").toString().toLower();
                    best.version = ver;
                }
            }
            if (best.url.isEmpty())
                return done({}, L("Could not find the latest release"));
            done(best, {});
        });
    } else if (it.id == "alpine") {
        const QString base = "https://dl-cdn.alpinelinux.org/alpine/latest-stable/releases/x86_64/";
        get(base + "latest-releases.yaml", [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            bool inStd = false;
            ResolvedImage r;
            for (const QByteArray& raw : d.split('\n')) {
                const QString l = QString::fromUtf8(raw).trimmed();
                if (l.startsWith("- title:") || l.startsWith("-"))
                    inStd = false;
                if (l == "flavor: alpine-standard")
                    inStd = true;
                if (inStd && l.startsWith("iso:"))
                    r.fileName = l.mid(4).trimmed();
                if (inStd && l.startsWith("sha256:"))
                    r.sha256 = l.mid(7).trimmed().toLower();
                if (inStd && l.startsWith("version:"))
                    r.version = l.mid(8).trimmed();
            }
            if (r.fileName.isEmpty())
                return done({}, L("Could not find the latest release"));
            r.url = base + r.fileName;
            done(r, {});
        });
    } else if (it.id == "arch") {
        get("https://archlinux.org/releng/releases/json/", [=](const QByteArray& d, const QString& err) {
            if (!err.isEmpty())
                return done({}, err);
            for (const QJsonValue& x : QJsonDocument::fromJson(d).object().value("releases").toArray()) {
                const QJsonObject o = x.toObject();
                if (!o.value("available").toBool())
                    continue;
                const QString ver = o.value("version").toString();
                ResolvedImage r;
                r.version = ver;
                r.fileName = "archlinux-" + ver + "-x86_64.iso";
                r.url = "https://geo.mirror.pkgbuild.com/iso/" + ver + "/" + r.fileName;
                r.sha256 = o.value("sha256_sum").toString().toLower();
                return done(r, {});
            }
            done({}, L("Could not find the latest release"));
        });
    } else if (it.id == "win11-eval") {
        ResolvedImage r;
        r.url = "https://go.microsoft.com/fwlink/?linkid=2289031&clcid=0x409&culture=en-us&country=us";
        r.fileName = "Windows11_Enterprise_Evaluation_x64.iso";
        done(r, {});   // Microsoft publishes no checksum next to this link
    } else {
        done({}, L("Unknown image"));
    }
}

void DownloadDialog::startSelected()
{
    const int row = list_->currentRow();
    if (row < 0)
        return;
    const CatalogItem it = items_[size_t(row)];
    if (it.browserOnly) {
        QDesktopServices::openUrl(QUrl(it.pageUrl));
        status_->setText(L("Download the ISO on Microsoft's page, then choose it in BootAz."));
        return;
    }
    setBusy(true);
    aborted_ = false;
    bar_->setState(0);
    bar_->setProgress(-1);
    bar_->setText(L("Finding the latest version…"));
    status_->setText(it.name);
    resolve(it, [this](const ResolvedImage& r, const QString& err) {
        if (!err.isEmpty()) {
            setBusy(false);
            bar_->setState(2);
            bar_->setText(L("Failed"));
            status_->setText(err);
            return;
        }
        download(r);
    });
}

void DownloadDialog::download(const ResolvedImage& r)
{
    QDir().mkpath(folder_->text());
    const QString path = QDir(folder_->text()).filePath(r.fileName);
    if (QFileInfo::exists(path) && !r.sha256.isEmpty()) {
        status_->setText(L("The file is already there; checking it…"));
        verify(path, r.sha256);
        return;
    }
    out_ = new QFile(path + ".part", this);
    if (!out_->open(QIODevice::WriteOnly)) {
        setBusy(false);
        status_->setText(L("Cannot write to the folder"));
        return;
    }
    QNetworkRequest req{QUrl(r.url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, "BootAz/" BOOTAZ_VERSION);
    active_ = net_->get(req);
    auto* clock = new QElapsedTimer;
    clock->start();
    status_->setText(r.fileName + (r.version.isEmpty() ? "" : "  ·  " + r.version));
    connect(active_, &QNetworkReply::readyRead, this, [this] { out_->write(active_->readAll()); });
    connect(active_, &QNetworkReply::downloadProgress, this, [this, clock](qint64 got, qint64 total) {
        const double secs = clock->elapsed() / 1000.0;
        const double speed = secs > 0.5 ? got / secs : 0;
        bar_->setProgress(total > 0 ? double(got) / double(total) : -1);
        QString t = QString::fromStdString(bootaz::humanBytes(uint64_t(got)));
        if (total > 0)
            t += " / " + QString::fromStdString(bootaz::humanBytes(uint64_t(total)));
        if (speed > 0)
            t += "  ·  " + QString::fromStdString(bootaz::humanBytes(uint64_t(speed))) + "/s";
        bar_->setText(t);
    });
    connect(active_, &QNetworkReply::finished, this, [this, path, r, clock] {
        delete clock;
        active_->deleteLater();
        const bool ok = active_->error() == QNetworkReply::NoError;
        const QString err = active_->errorString();
        active_ = nullptr;
        out_->close();
        if (!ok) {
            out_->remove();
            setBusy(false);
            bar_->setState(2);
            bar_->setText(aborted_ ? L("Cancelled") : L("Failed"));
            status_->setText(aborted_ ? QString() : err);
            return;
        }
        QFile::remove(path);
        out_->rename(path);
        if (r.sha256.isEmpty()) {
            setBusy(false);
            bar_->setState(1);
            bar_->setProgress(1);
            bar_->setText(L("Downloaded"));
            result_ = path;
            accept();
            return;
        }
        verify(path, r.sha256);
    });
}

void DownloadDialog::verify(const QString& path, const QString& expected)
{
    bar_->setState(0);
    bar_->setProgress(-1);
    bar_->setText(L("Checking the SHA-256 checksum…"));
    auto* w = new QFutureWatcher<QString>(this);
    connect(w, &QFutureWatcher<QString>::finished, this, [this, w, path, expected] {
        const QString got = w->result();
        w->deleteLater();
        setBusy(false);
        if (got == expected) {
            bar_->setState(1);
            bar_->setProgress(1);
            bar_->setText(L("Downloaded and verified"));
            result_ = path;
            accept();
        } else {
            bar_->setState(2);
            bar_->setText(L("Checksum mismatch"));
            status_->setText(L("The downloaded file does not match the official checksum. It was kept but should not be used:") + " " + path);
        }
    });
    w->setFuture(QtConcurrent::run([path] {
        bootaz::JobContext ctx;
        try {
            const auto r = bootaz::hashFile(path.toStdString(), {bootaz::HashAlgo::Sha256}, ctx);
            return QString::fromStdString(r.front().second);
        } catch (...) {
            return QString();
        }
    }));
}
