#pragma once
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

// Downloads a list of (URL → local path) pairs sequentially.
// Each file is first written to a .part file and renamed on success
// so an interrupted download never leaves a corrupt file behind.

class ModelDownloader : public QObject
{
    Q_OBJECT

public:
    struct Item {
        QUrl    url;
        QString localPath;  // absolute path for the finished file
    };

    explicit ModelDownloader(QObject *parent = nullptr);
    ~ModelDownloader() override;

    void download(const QList<Item> &items);
    void abort();

signals:
    // fileIndex / fileCount let the UI show "file 2 of 5" style progress.
    // bytesTotal is -1 when the server does not send Content-Length.
    void progress(int fileIndex, int fileCount,
                  qint64 bytesReceived, qint64 bytesTotal);
    void fileComplete(const QString &localPath);
    void finished();
    void failed(const QString &error);

private slots:
    void onReadyRead();
    void onDownloadProgress(qint64 received, qint64 total);
    void onFinished();

private:
    void startNext();
    void discardPart();

    QNetworkAccessManager *m_nam;
    QNetworkReply         *m_reply = nullptr;
    QFile                 *m_file  = nullptr;
    QList<Item>            m_items;
    int                    m_index = 0;
};
