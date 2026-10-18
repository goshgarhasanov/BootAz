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

// Crescent and eight-pointed star, sized for a flag of height h with the crescent centred on c.
void drawEmblem(QPainter& p, QPointF c, double h, const QColor& color)
{
    QPainterPath outer, inner;
    outer.addEllipse(c, 0.15 * h, 0.15 * h);
    inner.addEllipse(QPointF(c.x() + 0.037 * h, c.y()), 0.125 * h, 0.125 * h);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(outer.subtracted(inner));
    p.drawPath(star8(QPointF(c.x() + 0.105 * h, c.y()), 0.072 * h, 0.034 * h));
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
    drawEmblem(p, QPointF(r.center().x() - 0.03 * h, r.center().y()), h, Qt::white);
    p.restore();
}

void drawLogo(QPainter& p, const QRectF& rect)
{
    const double s = std::min(rect.width(), rect.height());
    const QRectF r(rect.center().x() - s / 2, rect.center().y() - s / 2, s, s);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);

    // Tile: the whole flag of Azerbaijan, with a soft gloss.
    QPainterPath tile;
    tile.addRoundedRect(r, s * 0.22, s * 0.22);
    p.save();
    p.setClipPath(tile);
    p.setPen(Qt::NoPen);
    const double band = s / 3.0;
    p.fillRect(QRectF(r.left(), r.top(), s, band + 0.5), kBlue);
    p.fillRect(QRectF(r.left(), r.top() + band, s, band + 0.5), kRed);
    p.fillRect(QRectF(r.left(), r.top() + 2 * band, s, s - 2 * band), kGreen);
    QLinearGradient shine(r.topLeft(), r.bottomLeft());
    shine.setColorAt(0, QColor(255, 255, 255, 50));
    shine.setColorAt(0.5, QColor(255, 255, 255, 0));
    shine.setColorAt(1, QColor(0, 0, 0, 40));
    p.fillRect(r, shine);
    p.restore();

    // White USB stick standing upright, slightly tilted, carrying the crescent and star.
    p.translate(r.center());
    p.rotate(-12);
    const double bw = s * 0.40, bh = s * 0.50;   // body
    const QRectF body(-bw / 2, -bh / 2 + s * 0.10, bw, bh);
    const double cw = bw * 0.62, ch = s * 0.19;  // metal connector
    const QRectF conn(-cw / 2, body.top() - ch + s * 0.01, cw, ch);
    QPainterPath connPath, bodyPath;
    connPath.addRoundedRect(conn, s * 0.025, s * 0.025);
    bodyPath.addRoundedRect(body, s * 0.07, s * 0.07);

    // drop shadow
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawPath(connPath.united(bodyPath).translated(s * 0.015, s * 0.03));

    QLinearGradient metal(conn.topLeft(), conn.topRight());
    metal.setColorAt(0, QColor(0xb8, 0xc0, 0xcc));
    metal.setColorAt(0.5, QColor(0xf2, 0xf4, 0xf7));
    metal.setColorAt(1, QColor(0x9a, 0xa3, 0xb2));
    p.fillPath(connPath, metal);
    p.setBrush(QColor(0x2a, 0x30, 0x3d));
    const double hole = cw * 0.2;
    p.drawRect(QRectF(conn.left() + cw * 0.18, conn.top() + ch * 0.28, hole, hole * 0.8));
    p.drawRect(QRectF(conn.right() - cw * 0.18 - hole, conn.top() + ch * 0.28, hole, hole * 0.8));

    QLinearGradient white(body.topLeft(), body.bottomRight());
    white.setColorAt(0, QColor(0xff, 0xff, 0xff));
    white.setColorAt(1, QColor(0xe4, 0xe9, 0xf0));
    p.fillPath(bodyPath, white);
    drawEmblem(p, QPointF(-0.035 * s, body.top() + bh * 0.42), bw * 1.55, kRed);

    // Status light at the bottom, in the flag's green.
    const QPointF led(0, body.top() + bh * 0.83);
    QRadialGradient ledGlow(led, s * 0.06);
    ledGlow.setColorAt(0, QColor(0x6c, 0xd1, 0x3f));
    ledGlow.setColorAt(1, QColor(0x6c, 0xd1, 0x3f, 0));
    p.setBrush(ledGlow);
    p.drawEllipse(led, s * 0.06, s * 0.06);
    p.setBrush(kGreen);
    p.drawEllipse(led, s * 0.022, s * 0.022);
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
