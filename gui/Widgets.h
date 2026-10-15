#pragma once

#include <QFrame>
#include <QString>
#include <QWidget>

class QLabel;
class QVBoxLayout;
class QVariantAnimation;
class QTimer;

// Big progress bar: the fill glides to each value, a shimmer runs while busy, text in the middle.
class FlashBar : public QWidget {
    Q_OBJECT
public:
    explicit FlashBar(QWidget* parent = nullptr);
    void setProgress(double fraction);   // 0..1, or < 0 for "working, unknown amount"
    void setText(const QString& t);
    void setState(int state);            // 0 normal, 1 success, 2 error
    QSize sizeHint() const override { return {400, 40}; }
    QSize minimumSizeHint() const override { return {200, 34}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double target_ = 0, shown_ = 0, phase_ = 0;
    bool busy_ = false;
    int state_ = 0;
    QString text_;
    QVariantAnimation* anim_;
    QTimer* shimmer_;
};

// A rounded panel with an icon, a title and a body layout.
class Card : public QFrame {
    Q_OBJECT
public:
    Card(const QString& icon, const QString& title, QWidget* parent = nullptr);
    QVBoxLayout* body() const { return body_; }

private:
    QVBoxLayout* body_;
};

QLabel* chip(const QString& text, const QColor& color);
QLabel* hint(const QString& text);   // small grey explanation under a control

#include <QLayout>

// Lays widgets out left to right and wraps to the next line when the row is full (image chips).
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6);
    ~FlowLayout() override;
    void addItem(QLayoutItem* item) override;
    int count() const override { return int(items_.size()); }
    QLayoutItem* itemAt(int i) const override { return i >= 0 && i < count() ? items_[size_t(i)] : nullptr; }
    QLayoutItem* takeAt(int i) override;
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return doLayout(QRect(0, 0, w, 0), true); }
    void setGeometry(const QRect& r) override;
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override;

private:
    int doLayout(const QRect& r, bool testOnly) const;
    std::vector<QLayoutItem*> items_;
    int space_;
};
