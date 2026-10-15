#pragma once

#include "Disk.h"
#include "Image.h"
#include "Jobs.h"
#include "Unattend.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QMainWindow>
#include <QMap>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QWidget;
class QHBoxLayout;
class FlowLayout;
class FlashBar;
class Engine;
class LogDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

signals:
    void rebuildRequested();   // language or theme changed

protected:
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    QWidget* buildHeader();
    QWidget* buildDeviceCard();
    QWidget* buildImageCard();
    QWidget* buildBootCard();
    QWidget* buildFormatCard();
    QWidget* buildActionBar();

    void refreshDrives(bool keepSelection = true);
    void pollDrives();
    std::vector<bootaz::DriveInfo> selectedDrives() const;
    void updateDriveInfo();
    void chooseImage();
    void setImage(const QString& path);
    void showImageInfo();
    void updateControls();
    bootaz::WriteOptions collectOptions(const bootaz::DriveInfo& d) const;
    void start();
    void runTool(const QString& what);
    void testInVm();
    void setRunning(bool running, const QString& title = QString());
    void onProgress(const QString& dev, const QString& stage, qint64 done, qint64 total, double speed);
    void onFinished(bool ok, bool cancelled);
    void log(const QString& line);
    void setStatus(const QString& text, int kind = 0);   // 0 normal, 1 good, 2 bad, 3 warn

    // devices
    std::vector<bootaz::DriveInfo> drives_;
    QString driveSignature_;
    QComboBox* driveCombo_ = nullptr;
    QCheckBox* multiCheck_ = nullptr;
    QListWidget* multiList_ = nullptr;
    QCheckBox* showFixed_ = nullptr;
    QLabel* driveInfo_ = nullptr;

    // image
    QComboBox* bootCombo_ = nullptr;   // image / non-bootable
    QLineEdit* imagePath_ = nullptr;
    QPushButton* selectBtn_ = nullptr;
    QPushButton* downloadBtn_ = nullptr;
    QPushButton* hashBtn_ = nullptr;
    QPushButton* vmBtn_ = nullptr;
    QWidget* chipsBox_ = nullptr;
    FlowLayout* chips_ = nullptr;
    QLabel* imageNote_ = nullptr;
    bootaz::ImageInfo image_;
    bool haveImage_ = false;
    bool analysing_ = false;

    // boot
    QComboBox* modeCombo_ = nullptr;
    QComboBox* schemeCombo_ = nullptr;
    QComboBox* targetCombo_ = nullptr;
    QLabel* targetHint_ = nullptr;
    QWidget* winLayoutRow_ = nullptr;
    QComboBox* winLayoutCombo_ = nullptr;
    QWidget* persistRow_ = nullptr;
    QSlider* persistSlider_ = nullptr;
    QLabel* persistLabel_ = nullptr;
    QWidget* winRow_ = nullptr;
    QPushButton* winBtn_ = nullptr;
    QLabel* winSummary_ = nullptr;
    bootaz::WindowsOptions winOpts_;
    bool winOptsSet_ = false;

    // format
    QLineEdit* labelEdit_ = nullptr;
    QComboBox* fsCombo_ = nullptr;
    QComboBox* clusterCombo_ = nullptr;
    QCheckBox* quickCheck_ = nullptr;
    QCheckBox* verifyCheck_ = nullptr;
    QCheckBox* ejectCheck_ = nullptr;
    QCheckBox* autorunCheck_ = nullptr;
    QComboBox* badCombo_ = nullptr;

    // actions
    FlashBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QPushButton* toolsBtn_ = nullptr;
    QPushButton* logBtn_ = nullptr;
    QList<QWidget*> lockWhileRunning_;

    Engine* engine_ = nullptr;
    LogDialog* logDlg_ = nullptr;
    QTimer* pollTimer_ = nullptr;
    QString jobTitle_;
    QMap<QString, double> devProgress_;
    QElapsedTimer jobClock_;
    QString lastError_;
    QJsonObject lastReport_;
    bool running_ = false;
};
