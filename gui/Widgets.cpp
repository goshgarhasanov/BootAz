#include "Widgets.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

FlashBar::FlashBar(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    anim_ = new QVariantAnimation(this);
    anim_->setDuration(350);
    anim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        shown_ = v.toDouble();
        update();
    });
    shimmer_ = new QTimer(this);
    shimmer_->setInterval(30);
    connect(shimmer_, &QTimer::timeout, this, [this] {
        phase_ += 0.012;
        if (phase_ > 1.4)
            phase_ = -0.4;
        update();
    });
}

void FlashBar::setProgress(double f)
{
    if (f < 0) {
        busy_ = true;
        if (!shimmer_->isActive())
            shimmer_->start();
        update();
        return;
    }
    busy_ = f < 1.0 && state_ == 0;
    if (busy_ && !shimmer_->isActive())
        shimmer_->start();
    if (!busy_)
        shimmer_->stop();
    target_ = std::clamp(f, 0.0, 1.0);
    anim_->stop();
    anim_->setStartValue(shown_);
    anim_->setEndValue(target_);
    anim_->start();
}

void FlashBar::setText(const QString& t)
{
    text_ = t;
    update();
}

void FlashBar::setState(int s)
{
    state_ = s;
    if (s != 0) {
        busy_ = false;
        shimmer_->stop();
    }
    update();
}

void FlashBar::paintEvent(QPaintEvent*)
{
    const theme::Palette& pal = theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double rad = r.height() / 2.4;
    QPainterPath track;
    track.addRoundedRect(r, rad, rad);
    p.fillPath(track, pal.surface2);
    p.setPen(QPen(pal.border, 1));
    p.drawPath(track);

    const double w = r.width() * shown_;
    if (w > 1) {
        QPainterPath fill;
        fill.addRoundedRect(QRectF(r.left(), r.top(), std::max(w, rad * 2), r.height()), rad, rad);
        QLinearGradient g(r.topLeft(), r.topRight());
        if (state_ == 2) {
            g.setColorAt(0, pal.bad.darker(110));
            g.setColorAt(1, pal.bad);
        } else if (state_ == 1) {
            g.setColorAt(0, pal.good.darker(115));
            g.setColorAt(1, pal.good);
        } else {
            g.setColorAt(0, QColor(0x00, 0xb5, 0xe2));
            g.setColorAt(0.5, QColor(0x2a, 0xa9, 0x9a));
            g.setColorAt(1, QColor(0x50, 0x9e, 0x2f));
        }
        p.fillPath(fill, g);
    }
    if (busy_) {
        // A soft light sweeping across the bar while work is running.
        const double x = r.left() + r.width() * phase_;
        QLinearGradient s(QPointF(x - 90, 0), QPointF(x + 90, 0));
        s.setColorAt(0, QColor(255, 255, 255, 0));
        s.setColorAt(0.5, QColor(255, 255, 255, pal.dark ? 45 : 70));
        s.setColorAt(1, QColor(255, 255, 255, 0));
        p.save();
        p.setClipPath(track);
        p.fillRect(r, s);
        p.restore();
    }
    if (!text_.isEmpty()) {
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(shown_ > 0.45 || state_ != 0 ? Qt::white : pal.text);
        p.drawText(r, Qt::AlignCenter, text_);
    }
}

Card::Card(const QString& icon, const QString& title, QWidget* parent) : QFrame(parent)
{
    setObjectName("Card");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 14, 18, 16);
    v->setSpacing(10);
    auto* head = new QHBoxLayout;
    head->setSpacing(10);
    auto* ic = new QLabel(icon);
    ic->setObjectName("CardIcon");
    auto* t = new QLabel(title);
    t->setObjectName("CardTitle");
    head->addWidget(ic);
    head->addWidget(t, 1);
    v->addLayout(head);
    body_ = new QVBoxLayout;
    body_->setSpacing(8);
    v->addLayout(body_);
}

QLabel* chip(const QString& text, const QColor& color)
{
    auto* l = new QLabel(text);
    l->setStyleSheet(theme::chipStyle(color));
    l->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return l;
}

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName("Hint");
    l->setWordWrap(true);
    return l;
}

FlowLayout::FlowLayout(QWidget* parent, int spacing) : QLayout(parent), space_(spacing) { setContentsMargins(0, 0, 0, 0); }

FlowLayout::~FlowLayout()
{
    while (QLayoutItem* it = takeAt(0))
        delete it;
}

void FlowLayout::addItem(QLayoutItem* item) { items_.push_back(item); }

QLayoutItem* FlowLayout::takeAt(int i)
{
    if (i < 0 || i >= count())
        return nullptr;
    QLayoutItem* it = items_[size_t(i)];
    items_.erase(items_.begin() + i);
    return it;
}

void FlowLayout::setGeometry(const QRect& r)
{
    QLayout::setGeometry(r);
    doLayout(r, false);
}

QSize FlowLayout::minimumSize() const
{
    QSize s;
    for (QLayoutItem* it : items_)
        s = s.expandedTo(it->minimumSize());
    return s;
}

int FlowLayout::doLayout(const QRect& r, bool testOnly) const
{
    int x = r.x(), y = r.y(), line = 0;
    for (QLayoutItem* it : items_) {
        const QSize sz = it->sizeHint();
        if (x + sz.width() > r.right() + 1 && line > 0) {
            x = r.x();
            y += line + space_;
            line = 0;
        }
        if (!testOnly)
            it->setGeometry(QRect(QPoint(x, y), sz));
        x += sz.width() + space_;
        line = std::max(line, sz.height());
    }
    return y + line - r.y();
}
