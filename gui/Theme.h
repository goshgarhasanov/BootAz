#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace theme {

struct Palette {
    bool dark = true;
    QColor bg, surface, surface2, border, text, muted, accent, accent2, good, warn, bad;
};

const Palette& current();
void apply(QApplication& app, bool dark);
QString chipStyle(const QColor& c);   // small rounded badge in a colour

} // namespace theme
