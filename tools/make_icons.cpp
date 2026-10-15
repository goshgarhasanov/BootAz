// Writes the BootAz icons and logos into a folder (default: resources):
//   cmake --build build --target make_icons && build/make_icons resources
#include "Brand.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QPixmap>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("resources");
    QDir().mkpath(dir);
    const QList<int> sizes = {16, 24, 32, 48, 64, 128, 256};
    QList<QByteArray> pngs;
    for (int s : sizes) {
        const QPixmap pm = brand::iconPixmap(s);
        QByteArray png;
        QBuffer b(&png);
        b.open(QIODevice::WriteOnly);
        pm.save(&b, "PNG");
        pngs << png;
        if (s >= 48)
            pm.save(QDir(dir).filePath(QString("bootaz-%1.png").arg(s)));
    }
    brand::iconPixmap(512).save(QDir(dir).filePath("bootaz.png"));
    brand::wordmarkPixmap(160, true).save(QDir(dir).filePath("logo.png"));
    brand::wordmarkPixmap(160, false).save(QDir(dir).filePath("logo-light.png"));

    // .ico with PNG-compressed entries (Windows Vista and later).
    QFile ico(QDir(dir).filePath("bootaz.ico"));
    if (!ico.open(QIODevice::WriteOnly))
        return 1;
    auto u16 = [&](quint16 v) { ico.write(reinterpret_cast<const char*>(&v), 2); };
    auto u32 = [&](quint32 v) { ico.write(reinterpret_cast<const char*>(&v), 4); };
    u16(0);
    u16(1);
    u16(quint16(sizes.size()));
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (int i = 0; i < sizes.size(); ++i) {
        const int s = sizes[i];
        ico.putChar(char(s >= 256 ? 0 : s));
        ico.putChar(char(s >= 256 ? 0 : s));
        ico.putChar(0);
        ico.putChar(0);
        u16(1);
        u16(32);
        u32(quint32(pngs[i].size()));
        u32(offset);
        offset += quint32(pngs[i].size());
    }
    for (const QByteArray& p : pngs)
        ico.write(p);
    return 0;
}
