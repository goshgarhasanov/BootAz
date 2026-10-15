#include "Brand.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <cmath>

namespace brand {

namespace {

constexpr double kPi = 3.14159265358979323846;
const QColor kBlue(0x00, 0xb5, 0xe2);    // flag of Azerbaijan
const QColor kRed(0xef, 0x33, 0x40);
const QColor kGreen(0x50, 0x9e, 0x2f);

QPainterPath star8(QPointF c, double outer, double inner)
{
    QPainterPath path;
    for (int i = 0; i < 16; ++i) {
        const double a = kPi / 8.0 * i - kPi / 2.0;
        const double r = (i % 2 == 0) ? outer : inner;
        const QPointF pt(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    path.closeSubpath();
    return path;
}

} // namespace

void drawFlag(QPainter& p, const QRectF& r)
{
    const double h = r.height(), band = h / 3.0;
    p.save();
    p.setPen(Qt::NoPen);
    p.fillRect(QRectF(r.left(), r.top(), r.width(), band + 0.5), kBlue);
    p.fillRect(QRectF(r.left(), r.top() + band, r.width(), band + 0.5), kRed);
    p.fillRect(QRectF(r.left(), r.top() + 2 * band, r.width(), r.height() - 2 * band), kGreen);
    const QPointF c(r.center().x() - 0.03 * h, r.center().y());
    QPainterPath outer, inner;
    outer.addEllipse(c, 0.15 * h, 0.15 * h);
    inner.addEllipse(QPointF(c.x() + 0.037 * h, c.y()), 0.125 * h, 0.125 * h);
    p.setBrush(Qt::white);
    p.drawPath(outer.subtracted(inner));
    p.drawPath(star8(QPointF(c.x() + 0.105 * h, c.y()), 0.072 * h, 0.034 * h));
    p.restore();
}

void drawLogo(QPainter& p, const QRectF& rect)
{
    const double s = std::min(rect.width(), rect.height());
    const QRectF r(rect.center().x() - s / 2, rect.center().y() - s / 2, s, s);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    // Tile: deep navy with a soft glow.
    QLinearGradient bg(r.topLeft(), r.bottomRight());
    bg.setColorAt(0, QColor(0x1d, 0x27, 0x45));
    bg.setColorAt(1, QColor(0x0d, 0x12, 0x20));
    QPainterPath tile;
    tile.addRoundedRect(r, s * 0.22, s * 0.22);
    p.fillPath(tile, bg);
    QRadialGradient glow(QPointF(r.center().x(), r.top() + s * 0.25), s * 0.7);
    glow.setColorAt(0, QColor(0x00, 0xb5, 0xe2, 70));
    glow.setColorAt(1, QColor(0x00, 0xb5, 0xe2, 0));
    p.fillPath(tile, glow);

    // USB stick standing upright, slightly tilted.
    p.translate(r.center());
    p.rotate(-12);
    const double bw = s * 0.40, bh = s * 0.52;   // body
    const QRectF body(-bw / 2, -bh / 2 + s * 0.08, bw, bh);
    const double cw = bw * 0.62, ch = s * 0.20;  // metal connector
    const QRectF conn(-cw / 2, body.top() - ch + s * 0.01, cw, ch);

    QLinearGradient metal(conn.topLeft(), conn.topRight());
    metal.setColorAt(0, QColor(0xb8, 0xc0, 0xcc));
    metal.setColorAt(0.5, QColor(0xf2, 0xf4, 0xf7));
    metal.setColorAt(1, QColor(0x9a, 0xa3, 0xb2));
    QPainterPath connPath;
    connPath.addRoundedRect(conn, s * 0.025, s * 0.025);
    p.fillPath(connPath, metal);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2a, 0x30, 0x3d));
    const double hole = cw * 0.2;
    p.drawRect(QRectF(conn.left() + cw * 0.18, conn.top() + ch * 0.28, hole, hole * 0.8));
    p.drawRect(QRectF(conn.right() - cw * 0.18 - hole, conn.top() + ch * 0.28, hole, hole * 0.8));

    // Body with the flag.
    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, s * 0.07, s * 0.07);
    p.save();
    p.setClipPath(bodyPath);
    drawFlag(p, QRectF(body.left() - 1, body.top() + bh * 0.10, body.width() + 2, bh * 0.66));
    // dark caps above and below the flag
    p.fillRect(QRectF(body.left(), body.top(), body.width(), bh * 0.10 + 0.5), QColor(0x14, 0x18, 0x21));
    p.fillRect(QRectF(body.left(), body.top() + bh * 0.76, body.width(), bh * 0.24 + 1), QColor(0x14, 0x18, 0x21));
    // gloss
    QLinearGradient gloss(body.topLeft(), body.topRight());
    gloss.setColorAt(0, QColor(255, 255, 255, 60));
    gloss.setColorAt(0.45, QColor(255, 255, 255, 0));
    p.fillRect(body, gloss);
    p.restore();
    p.setPen(QPen(QColor(255, 255, 255, 40), s * 0.012));
    p.setBrush(Qt::NoBrush);
    p.drawPath(bodyPath);

    // Status light / power symbol at the bottom.
    const QPointF led(0, body.top() + bh * 0.88);
    QRadialGradient ledGlow(led, s * 0.07);
    ledGlow.setColorAt(0, QColor(0x5e, 0xea, 0xd4));
    ledGlow.setColorAt(1, QColor(0x5e, 0xea, 0xd4, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(ledGlow);
    p.drawEllipse(led, s * 0.07, s * 0.07);
    p.setBrush(QColor(0xd9, 0xff, 0xf8));
    p.drawEllipse(led, s * 0.02, s * 0.02);
    p.restore();
}

QPixmap iconPixmap(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    drawLogo(p, QRectF(0, 0, size, size));
    return pm;
}

QIcon appIcon()
{
    QIcon icon;
    for (int s : {16, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(iconPixmap(s));
    return icon;
}

QPixmap wordmarkPixmap(int height, bool dark)
{
    QFont f("Inter");
    f.setPixelSize(int(height * 0.62));
    f.setWeight(QFont::Black);
    QFontMetricsF fm(f);
    const QString boot = "Boot", az = "Az";
    const double gap = height * 0.22;
    const double w = height + gap + fm.horizontalAdvance(boot) + fm.horizontalAdvance(az) + height * 0.1;
    QPixmap pm(int(std::ceil(w)), height);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    drawLogo(p, QRectF(0, 0, height, height));
    p.setFont(f);
    const double base = height / 2.0 + (fm.ascent() - fm.descent()) / 2.0;
    double x = height + gap;
    p.setPen(dark ? Qt::white : QColor(0x16, 0x20, 0x2b));
    p.drawText(QPointF(x, base), boot);
    x += fm.horizontalAdvance(boot);
    // "Az" in the flag's colours
    QLinearGradient g(QPointF(x, 0), QPointF(x + fm.horizontalAdvance(az), 0));
    g.setColorAt(0, kBlue);
    g.setColorAt(0.5, kRed);
    g.setColorAt(1, kGreen);
    QPainterPath t;
    t.addText(QPointF(x, base), f, az);
    p.fillPath(t, g);
    return pm;
}

} // namespace brand
