#include "Engine.h"

#include "I18n.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

#ifndef _WIN32
#include <unistd.h>
#endif

static QString Q(const std::string& s) { return QString::fromStdString(s); }

Engine::Engine(QObject* parent) : QObject(parent) {}

Engine::~Engine()
{
    if (proc_ && proc_->state() != QProcess::NotRunning) {
        cancel();
        proc_->waitForFinished(3000);
    }
}

QString Engine::cliPath()
{
    const QString dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
    const QString name = "bootaz-cli.exe";
#else
    const QString name = "bootaz-cli";
#endif
    const QString local = QDir(dir).filePath(name);
    if (QFileInfo::exists(local))
        return local;
    const QString inPath = QStandardPaths::findExecutable(name);
    return inPath.isEmpty() ? local : inPath;
}

bool Engine::running() const { return proc_ && proc_->state() != QProcess::NotRunning; }

bool Engine::start(const QStringList& args)
{
    if (running())
        return false;
    delete proc_;
    proc_ = new QProcess(this);
    buffer_.clear();
    sawError_ = false;
    cancelled_ = false;
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &Engine::readOutput);
    connect(proc_, &QProcess::readyReadStandardError, this, [this] {
        const QString err = QString::fromUtf8(proc_->readAllStandardError()).trimmed();
        if (!err.isEmpty())
            emit logLine(QString(), err);
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        readOutput();
        const bool ok = st == QProcess::NormalExit && code == 0 && !sawError_;
#ifndef _WIN32
        // pkexec: 126 = the user dismissed the password dialog, 127 = not authorised.
        if (!ok && (code == 126 || code == 127) && !sawError_)
            emit failed(QString(), Q(T("Administrator rights were not granted")));
#endif
        emit finished(ok, cancelled_ || code == 3);
    });

    QStringList full = args;
    full << "--json" << "--lang" << bootaz::i18n::code(bootaz::i18n::language());
    const QString cli = cliPath();
#ifdef _WIN32
    proc_->start(cli, full);   // the GUI itself runs as Administrator (manifest)
#else
    // Already root, or a test run against image files (BOOTAZ_NO_ELEVATE): no password prompt.
    if (::geteuid() == 0 || qEnvironmentVariableIsSet("BOOTAZ_NO_ELEVATE")) {
        proc_->start(cli, full);
    } else {
        const QString pkexec = QStandardPaths::findExecutable("pkexec");
        if (pkexec.isEmpty()) {
            emit failed(QString(), Q(T("pkexec was not found; run BootAz with sudo")));
            return false;
        }
        full.prepend(QFileInfo(cli).absoluteFilePath());
        proc_->start(pkexec, full);
    }
#endif
    if (!proc_->waitForStarted(5000)) {
        emit failed(QString(), Q(T("Cannot start bootaz-cli")) + " (" + cli + ")");
        return false;
    }
    return true;
}

void Engine::cancel()
{
    if (!running())
        return;
    cancelled_ = true;
    // The job runs as another user under pkexec, so it is asked through its standard input.
    proc_->write("cancel\n");
}

void Engine::readOutput()
{
    if (!proc_)
        return;
    buffer_ += proc_->readAllStandardOutput();
    int nl;
    while ((nl = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(nl).trimmed();
        buffer_.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            emit logLine(QString(), QString::fromUtf8(line));
            continue;
        }
        const QJsonObject o = doc.object();
        const QString type = o.value("type").toString();
        const QString dev = o.value("device").toString();
        if (type == "progress")
            emit progress(dev, o.value("stage").toString(), qint64(o.value("done").toDouble()), qint64(o.value("total").toDouble()),
                          o.value("speed").toDouble());
        else if (type == "log")
            emit logLine(dev, o.value("text").toString());
        else if (type == "plan")
            emit planned(dev, o);
        else if (type == "error") {
            sawError_ = true;
            emit failed(dev, o.value("message").toString());
        } else
            emit report(dev, o);
    }
}
