#include "Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace theme {

namespace {
Palette gPal;

Palette makeDark()
{
    Palette p;
    p.dark = true;
    p.bg = QColor(0x0d, 0x11, 0x1a);
    p.surface = QColor(0x15, 0x1b, 0x27);
    p.surface2 = QColor(0x1c, 0x24, 0x34);
    p.border = QColor(0x27, 0x30, 0x42);
    p.text = QColor(0xe8, 0xec, 0xf3);
    p.muted = QColor(0x8d, 0x97, 0xab);
    p.accent = QColor(0x00, 0xb5, 0xe2);    // flag blue
    p.accent2 = QColor(0x50, 0x9e, 0x2f);   // flag green
    p.good = QColor(0x34, 0xd3, 0x99);
    p.warn = QColor(0xf5, 0xb0, 0x4a);
    p.bad = QColor(0xef, 0x33, 0x40);       // flag red
    return p;
}

Palette makeLight()
{
    Palette p;
    p.dark = false;
    p.bg = QColor(0xf3, 0xf5, 0xf9);
    p.surface = QColor(0xff, 0xff, 0xff);
    p.surface2 = QColor(0xf0, 0xf3, 0xf8);
    p.border = QColor(0xdd, 0xe3, 0xec);
    p.text = QColor(0x16, 0x20, 0x2b);
    p.muted = QColor(0x5f, 0x6b, 0x7c);
    p.accent = QColor(0x00, 0x8f, 0xbf);
    p.accent2 = QColor(0x3f, 0x86, 0x22);
    p.good = QColor(0x15, 0x9a, 0x6a);
    p.warn = QColor(0xc2, 0x7a, 0x0e);
    p.bad = QColor(0xd1, 0x24, 0x31);
    return p;
}

QString rgba(const QColor& c, double a) { return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(a); }
} // namespace

const Palette& current() { return gPal; }

QString chipStyle(const QColor& c)
{
    return QString("background:%1; color:%2; border:1px solid %3; border-radius:10px; padding:3px 10px; font-weight:600; font-size:9.5pt;")
        .arg(rgba(c, 0.14), (gPal.dark ? c.lighter(120) : c.darker(110)).name(), rgba(c, 0.5));
}

void apply(QApplication& app, bool dark)
{
    gPal = dark ? makeDark() : makeLight();
    const Palette& p = gPal;
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette pal;
    pal.setColor(QPalette::Window, p.bg);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.surface2);
    pal.setColor(QPalette::AlternateBase, p.surface);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.surface2);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::Highlight, p.accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, p.surface);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::PlaceholderText, p.muted);
    pal.setColor(QPalette::Link, p.accent);
    pal.setColor(QPalette::Disabled, QPalette::Text, p.muted);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.muted);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.muted);
    app.setPalette(pal);

    QString css = R"(
QWidget { color: %TEXT%; font-size: 10.5pt; }
QMainWindow, #Root { background: %BG%; }
QDialog { background: %BG%; }
#Header { background: %SURFACE%; border-bottom: 1px solid %BORDER%; }
#Subtitle { color: %MUTED%; }
#Card { background: %SURFACE%; border: 1px solid %BORDER%; border-radius: 14px; }
#CardTitle { font-size: 12pt; font-weight: 700; }
#CardIcon { color: %ACCENT%; font-size: 13pt; }
#Muted, #Hint { color: %MUTED%; }
#Hint { font-size: 9.5pt; }
#Footer { color: %MUTED%; font-size: 9.5pt; }
#Status { font-weight: 600; }
#Warn { color: %WARN%; }
#Bad { color: %BAD%; font-weight: 600; }
#Good { color: %GOOD%; font-weight: 600; }

QLineEdit, QComboBox, QSpinBox, QPlainTextEdit, QListWidget {
    background: %SURFACE2%; border: 1px solid %BORDER%; border-radius: 8px; padding: 6px 9px;
    selection-background-color: %ACCENT%;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QPlainTextEdit:focus { border-color: %ACCENT%; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: %SURFACE%; border: 1px solid %BORDER%; selection-background-color: %ACCENT_SOFT%; padding: 4px; }
QListWidget::item { padding: 6px; border-radius: 6px; }
QListWidget::item:selected { background: %ACCENT_SOFT%; color: %TEXT%; }

QPushButton { background: %SURFACE2%; border: 1px solid %BORDER%; border-radius: 9px; padding: 7px 14px; }
QPushButton:hover { border-color: %ACCENT%; }
QPushButton:pressed { background: %BORDER%; }
QPushButton:disabled { color: %MUTED%; border-color: %BORDER%; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: %MUTED%; background: %SURFACE%; }
QCheckBox:disabled { color: %MUTED%; }
QLabel:disabled { color: %MUTED%; }
QPushButton[variant="primary"] {
    border: none; color: white; font-weight: 700; font-size: 12pt; padding: 10px 28px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00a3d4, stop:1 #3f9a2c);
}
QPushButton[variant="primary"]:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1bb7e6, stop:1 #4fb03a); }
QPushButton[variant="primary"]:disabled { background: %BORDER%; color: %MUTED%; }
QPushButton[variant="danger"] { border: none; color: white; font-weight: 700; background: %BAD%; padding: 10px 22px; }
QPushButton[variant="flat"] { background: transparent; border: 1px solid transparent; padding: 6px 10px; }
QPushButton[variant="flat"]:hover { background: %SURFACE2%; border-color: %BORDER%; }

QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 18px; height: 18px; border-radius: 5px; border: 1px solid %BORDER%; background: %SURFACE2%; }
QCheckBox::indicator:checked { background: %ACCENT%; border-color: %ACCENT%; image: url(:/bootaz/check.png); }
QCheckBox::indicator:checked:disabled { background: %BORDER%; border-color: %BORDER%; }
QSlider::groove:horizontal { height: 6px; background: %BORDER%; border-radius: 3px; }
QSlider::sub-page:horizontal { background: %ACCENT%; border-radius: 3px; }
QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; border-radius: 8px; background: white; border: 2px solid %ACCENT%; }

QToolTip { background: %SURFACE%; color: %TEXT%; border: 1px solid %BORDER%; border-radius: 6px; padding: 6px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %BORDER%; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }
QMessageBox { background: %BG%; }
QProgressBar { background: %SURFACE2%; border: 1px solid %BORDER%; border-radius: 8px; text-align: center; height: 18px; }
QProgressBar::chunk { border-radius: 7px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00b5e2, stop:1 #509e2f); }
)";
    css.replace("%BG%", p.bg.name());
    css.replace("%SURFACE2%", p.surface2.name());
    css.replace("%SURFACE%", p.surface.name());
    css.replace("%BORDER%", p.border.name());
    css.replace("%TEXT%", p.text.name());
    css.replace("%MUTED%", p.muted.name());
    css.replace("%ACCENT_SOFT%", rgba(p.accent, 0.22));
    css.replace("%ACCENT%", p.accent.name());
    css.replace("%WARN%", p.warn.name());
    css.replace("%BAD%", p.bad.name());
    css.replace("%GOOD%", p.good.name());
    app.setStyleSheet(css);
}

} // namespace theme
