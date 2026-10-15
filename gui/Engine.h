#pragma once
// Runs bootaz-cli for the jobs that need administrator rights (Linux: through pkexec) and turns
// its JSON event stream into Qt signals. Read-only work (listing drives, analysing images) is
// done in-process with the core library.

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject* parent = nullptr);
    ~Engine() override;

    // Starts `bootaz-cli <args> --json --lang xx` with administrator rights. One job at a time.
    bool start(const QStringList& args);
    void cancel();
    bool running() const;
    static QString cliPath();

signals:
    void progress(const QString& device, const QString& stage, qint64 done, qint64 total, double speed);
    void logLine(const QString& device, const QString& text);
    void planned(const QString& device, const QJsonObject& plan);
    void report(const QString& device, const QJsonObject& event);   // badblocks / hashes results
    void failed(const QString& device, const QString& message);
    void finished(bool ok, bool cancelled);

private:
    void readOutput();

    QProcess* proc_ = nullptr;
    QByteArray buffer_;
    bool sawError_ = false;
    bool cancelled_ = false;
};
