#pragma once

#include "Unattend.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class FlashBar;

// Rufus-style "Customize Windows installation".
class WindowsOptionsDialog : public QDialog {
    Q_OBJECT
public:
    WindowsOptionsDialog(const bootaz::WindowsOptions& current, bool windows11, QWidget* parent = nullptr);
    bootaz::WindowsOptions options() const;

private:
    QCheckBox *bypass_, *noOnline_, *local_, *privacy_, *bitlocker_, *locale_, *computer_;
    QLineEdit *localName_, *localeName_, *computerName_;
};

// Checksums of an image, compared with a pasted value or the published one.
class HashDialog : public QDialog {
    Q_OBJECT
public:
    HashDialog(const QString& path, QWidget* parent = nullptr);

private:
    void compare();
    QString path_;
    QLineEdit* fields_[4];
    QLineEdit* expected_;
    QLabel* verdict_;
    FlashBar* bar_;
    QString published_;
};

class LogDialog : public QDialog {
    Q_OBJECT
public:
    explicit LogDialog(QWidget* parent = nullptr);
    void append(const QString& line);
    void clear();

private:
    QPlainTextEdit* text_;
};

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
};

// The last question before a drive is erased.
bool confirmErase(QWidget* parent, const QStringList& drives, const QString& summary, const QStringList& warnings);
