#include "Brand.h"
#include "MainWindow.h"
#include "Theme.h"

#include "I18n.h"

#include <QApplication>
#include <QFileInfo>
#include <QFont>
#include <QSettings>
#include <QTimer>

#include <functional>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("BootAz");
    QApplication::setApplicationDisplayName("BootAz");
    QApplication::setApplicationVersion(BOOTAZ_VERSION);
    QApplication::setOrganizationName("BootAz");
    QApplication::setDesktopFileName("bootaz");
    QApplication::setWindowIcon(brand::appIcon());
    QFont f = QApplication::font();
    f.setFamilies({"Inter", "Segoe UI", "Noto Sans", "Ubuntu", f.family()});
    QApplication::setFont(f);
    bootaz::i18n::setLanguage(bootaz::i18n::detectLanguage());
    // "BootAz file.iso" (file manager "Open with", desktop entry %f) selects the image.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1 && QFileInfo(args.at(1)).isFile())
        qputenv("BOOTAZ_IMAGE", QFileInfo(args.at(1)).absoluteFilePath().toUtf8());

    // Language and theme changes rebuild the window so every text and colour is fresh.
    MainWindow* window = nullptr;
    std::function<void()> open = [&] {
        QSettings s(QSettings::IniFormat, QSettings::UserScope, "BootAz", "BootAz");
        theme::apply(app, s.value("dark", true).toBool());
        auto* next = new MainWindow;
        if (window) {
            next->setGeometry(window->geometry());
            if (window->isMaximized())
                next->showMaximized();
            else
                next->show();
            window->hide();
            window->deleteLater();
        } else {
            next->show();
        }
        window = next;
        QObject::connect(next, &MainWindow::rebuildRequested, next, [&] { QTimer::singleShot(0, open); });
    };
    open();
    // BOOTAZ_SCREENSHOT=file.png: save the window after it settles and quit (docs and tests).
    if (!qEnvironmentVariableIsEmpty("BOOTAZ_SCREENSHOT")) {
        const int delay = qEnvironmentVariableIntValue("BOOTAZ_SCREENSHOT_DELAY");
        QTimer::singleShot(delay > 0 ? delay : 2500, &app, [&] {
            window->grab().save(qEnvironmentVariable("BOOTAZ_SCREENSHOT"));
            app.quit();
        });
    }
    const int code = app.exec();
    delete window;
    return code;
}
