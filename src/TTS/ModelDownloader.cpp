/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

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
    // A new batch supersedes whatever was in flight. Without this the old reply
    // keeps writing into the abandoned .part file, and both it and the QFile are
    // orphaned until this object is destroyed.
    abort();

    m_items = items;
    m_index = 0;
    startNext();
}

void ModelDownloader::abort()
{
    // QNetworkReply::abort() emits finished() SYNCHRONOUSLY. Sever the wires
    // first so it cannot re-enter onFinished(), which would clear m_reply
    // underneath us (leaving this call to dereference null) and then advance
    // m_index into the next item mid-abort. Nulling the member before touching
    // the reply matches AzureTTSEngine::stop() and STTBackendAzure.
    if (m_reply) {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
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
    if (m_reply && m_file)
        m_file->write(m_reply->readAll());
}

void ModelDownloader::onDownloadProgress(qint64 received, qint64 total)
{
    emit progress(m_index, m_items.size(), received, total);
}

void ModelDownloader::onFinished()
{
    // abort() disconnects before tearing the reply down, so this slot should not
    // run without one — the guard keeps any future re-entrant path honest.
    if (!m_reply)
        return;

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
