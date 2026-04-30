#include "ModelDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

ModelDownloader::ModelDownloader(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

ModelDownloader::~ModelDownloader()
{
    abort();
}

void ModelDownloader::download(const QList<Item> &items)
{
    m_items = items;
    m_index = 0;
    startNext();
}

void ModelDownloader::abort()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    discardPart();
}

void ModelDownloader::startNext()
{
    if (m_index >= m_items.size()) {
        emit finished();
        return;
    }

    const Item &item = m_items.at(m_index);

    QDir().mkpath(QFileInfo(item.localPath).absolutePath());

    m_file = new QFile(item.localPath + QStringLiteral(".part"), this);
    if (!m_file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(tr("Cannot write to %1").arg(item.localPath));
        discardPart();
        return;
    }

    QNetworkRequest req(item.url);

    m_reply = m_nam->get(req);
    connect(m_reply, &QNetworkReply::readyRead,
            this, &ModelDownloader::onReadyRead);
    connect(m_reply, &QNetworkReply::downloadProgress,
            this, &ModelDownloader::onDownloadProgress);
    connect(m_reply, &QNetworkReply::finished,
            this, &ModelDownloader::onFinished);
}

void ModelDownloader::onReadyRead()
{
    if (m_file)
        m_file->write(m_reply->readAll());
}

void ModelDownloader::onDownloadProgress(qint64 received, qint64 total)
{
    emit progress(m_index, m_items.size(), received, total);
}

void ModelDownloader::onFinished()
{
    if (m_reply->error() != QNetworkReply::NoError) {
        const QString err = m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        discardPart();
        emit failed(err);
        return;
    }

    if (m_file) {
        m_file->write(m_reply->readAll());
        m_file->close();

        const QString partPath  = m_file->fileName();
        const QString finalPath = m_items.at(m_index).localPath;
        QFile::remove(finalPath);
        QFile::rename(partPath, finalPath);

        delete m_file;
        m_file = nullptr;
        emit fileComplete(finalPath);
    }

    m_reply->deleteLater();
    m_reply = nullptr;
    ++m_index;
    startNext();
}

void ModelDownloader::discardPart()
{
    if (m_file) {
        const QString partPath = m_file->fileName();
        m_file->close();
        QFile::remove(partPath);
        delete m_file;
        m_file = nullptr;
    }
}
