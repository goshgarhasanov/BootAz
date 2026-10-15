#pragma once
// The BootAz logo: a USB stick wearing the flag of Azerbaijan, drawn in code at any size.

#include <QIcon>
#include <QPixmap>

class QPainter;
class QRectF;

namespace brand {

void drawFlag(QPainter& p, const QRectF& r);
void drawLogo(QPainter& p, const QRectF& r);   // square app icon
QPixmap iconPixmap(int size);
QIcon appIcon();
// Icon plus the "BootAz" word mark, for README and about box.
QPixmap wordmarkPixmap(int height, bool darkBackground);

} // namespace brand
