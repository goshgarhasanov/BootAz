#pragma once
// Built-in catalog of operating system images, resolved from each project's own "latest release"
// index so links do not go stale, then downloaded and checked against the published SHA-256.

#include <QDialog>
#include <QString>
#include <functional>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;
class QListWidget;
class QLabel;
class QPushButton;
class QLineEdit;
class FlashBar;
class QFile;

struct CatalogItem {
    QString id;
    QString name;          // "Ubuntu Desktop"
    QString note;          // short description (translated at display)
    QString icon;          // emoji / glyph
    bool browserOnly = false;   // opens the vendor's page (e.g. retail Windows)
    QString pageUrl;
};

struct ResolvedImage {
    QString url, fileName, sha256, version;
};

class DownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadDialog(QWidget* parent = nullptr);
    QString downloadedFile() const { return result_; }

private:
    void startSelected();
    void resolve(const CatalogItem& item, const std::function<void(const ResolvedImage&, const QString& err)>& done);
    void get(const QString& url, const std::function<void(const QByteArray&, const QString& err)>& done);
    void download(const ResolvedImage& r);
    void verify(const QString& path, const QString& expected);
    void setBusy(bool busy);

    QNetworkAccessManager* net_;
    std::vector<CatalogItem> items_;
    QListWidget* list_;
    QLineEdit* folder_;
    QLabel* status_;
    FlashBar* bar_;
    QPushButton* go_;
    QPushButton* cancel_;
    QNetworkReply* active_ = nullptr;
    QFile* out_ = nullptr;
    QString result_;
    bool aborted_ = false;
};
