// Runs the desktop app's job engine against an image file used as a "drive": checks that the
// CLI it starts writes a bootable FAT32 drive and that progress and completion arrive as signals.
//   bootaz-engine-test <path-to-bootaz-cli-dir> <image.iso>
#include "Engine.h"

#include "Disk.h"
#include "Fat32.h"
#include "Partition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2 || !QFile::exists(argv[1])) {
        std::printf("engine test skipped: no image given\n");
        return 0;
    }
    qputenv("BOOTAZ_NO_ELEVATE", "1");
    QTemporaryDir tmp;
    const QString target = tmp.filePath("usb.img");
    {
        QFile f(target);
        if (!f.open(QIODevice::WriteOnly))
            return 1;
        f.resize(qint64(600) * 1024 * 1024);
    }
    Engine engine;
    int progressEvents = 0;
    QString error;
    bool ok = false, done = false;
    QObject::connect(&engine, &Engine::progress, [&](const QString&, const QString&, qint64, qint64, double) { ++progressEvents; });
    QObject::connect(&engine, &Engine::failed, [&](const QString&, const QString& m) { error = m; });
    QObject::connect(&engine, &Engine::finished, [&](bool o, bool) {
        ok = o;
        done = true;
        app.quit();
    });
    if (!engine.start({"write", "--device", target, "--allow-file", "--image", QString::fromLocal8Bit(argv[1]), "--mode", "iso",
                       "--scheme", "gpt", "--yes"})) {
        std::printf("FAIL: engine did not start\n");
        return 1;
    }
    QTimer::singleShot(120000, &app, [&] { app.quit(); });
    app.exec();
    int failed = 0;
    auto check = [&](bool c, const char* what) {
        std::printf("%s %s\n", c ? "ok  " : "FAIL", what);
        failed += !c;
    };
    check(done, "job finished");
    check(ok, ("job succeeded " + error.toStdString()).c_str());
    check(progressEvents > 0, "progress events arrived");
    // The file now holds a GPT disk with one FAT32 partition.
    bootaz::DriveInfo d;
    bootaz::findDrive(target.toStdString(), d, true);
    bootaz::JobContext ctx;
    auto disk = bootaz::Disk::open(d, false, ctx);
    bootaz::Scheme scheme = bootaz::Scheme::Mbr;
    const auto parts = bootaz::readPartitionTable(*disk, &scheme);
    check(scheme == bootaz::Scheme::Gpt && parts.size() == 1, "GPT with one partition");
    if (!parts.empty()) {
        bootaz::SubTarget part(*disk, parts[0].offset, parts[0].size);
        const auto info = bootaz::readFat32Info(part);
        check(info.ok, "FAT32 file system");
    }
    return failed ? 1 : 0;
}
