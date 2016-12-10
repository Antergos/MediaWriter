/*
 * Fedora Media Writer
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "utilities.h"

#include <QDebug>
#include <QStandardPaths>
#include <QDir>
#include <QApplication>
#include <QAbstractEventDispatcher>
#include <QNetworkProxyFactory>

// TODO: everything Q_UNUSED

Progress::Progress(QObject *parent, qreal from, qreal to)
    : QObject(parent), m_from(from), m_to(to), m_value(from) {
    connect(this, &Progress::toChanged, this, &Progress::valueChanged);
}

qreal Progress::from() const {
    return m_from;
}

qreal Progress::to() const {
    return m_to;
}

qreal Progress::value() const {
    return m_value;
}

qreal Progress::ratio() const {
    return (value() - from()) / (to() - from());
}

void Progress::setTo(qreal v) {
    if (m_to != v) {
        m_to = v;
        emit toChanged();
    }
}

void Progress::setValue(qreal v) {
    if (m_value != v) {
        m_value = v;
        emit valueChanged();
    }
}

void Progress::setValue(qreal v, qreal to) {
    qreal computedValue = v / to * (m_to - m_from) + m_from;
    if (computedValue != m_value) {
        m_value = computedValue;
        emit valueChanged();
    }
}

void Progress::update(qreal value) {
    if (m_value != value) {
        m_value = value;
        emit valueChanged();
    }
}

void Progress::reset() {
    update(from());
}


DownloadManager *DownloadManager::_self = nullptr;

DownloadManager *DownloadManager::instance() {
    if (!_self)
        _self = new DownloadManager();
    return _self;
}

QString DownloadManager::dir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    return d;
}

/*
 * TODO explain how this works
 */
QString DownloadManager::downloadFile(DownloadReceiver *receiver, const QUrl &url, const QString &folder, Progress *progress) {

    QString bareFileName = QString("%1/%2").arg(folder).arg(url.fileName());

    QDir dir;
    dir.mkpath(folder);

    if (QFile::exists(bareFileName))
        return bareFileName;

    m_mirrorCache.clear();
    m_mirrorCache << url.toString();

    if (m_current)
        m_current->deleteLater();

    m_current = new Download(this, receiver, bareFileName, progress);
    connect(m_current, &QObject::destroyed, [&](){ m_current = nullptr; });
    fetchPageAsync(this, url.toString());

    return QString();
}

void DownloadManager::fetchPageAsync(DownloadReceiver *receiver, const QString &url) {
    QNetworkRequest request;
    request.setUrl(QUrl(url));
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

    auto reply = m_manager.get(request);
    auto download = new Download(this, receiver, QString());
    download->handleNewReply(reply);
    Q_UNUSED(download)
}

QString DownloadManager::fetchPage(const QString &url) {
    Q_UNUSED(url)
    return QString();
}

QNetworkReply *DownloadManager::tryAnotherMirror() {
    if (m_mirrorCache.isEmpty())
        return nullptr;
    if (!m_current)
        return nullptr;

    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setUrl(m_mirrorCache.first());
    request.setRawHeader("Range", QString("bytes=%1-").arg(m_current->bytesDownloaded()).toLocal8Bit());

    m_mirrorCache.removeFirst();
    return m_manager.get(request);
}

void DownloadManager::cancel() {
    if (m_current) {
        m_current->deleteLater();
        m_current = nullptr;
    }
}

void DownloadManager::onStringDownloaded(const QString &text) {
    if (!m_current)
        return;

    QStringList mirrors;
    for (const QString &i : text.split("\n")) {
        if (!i.trimmed().startsWith("#")) {
            mirrors.append(i.trimmed());
            if (mirrors.count() == 8)
                break;
        }
    }
    if (!mirrors.isEmpty())
        m_mirrorCache = mirrors;

    if (!m_current->hasCatchedUp())
        return;

    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setUrl(m_mirrorCache.first());
    request.setRawHeader("Range", QString("bytes=%1-").arg(m_current->bytesDownloaded()).toLocal8Bit());

    m_mirrorCache.removeFirst();
    m_current->handleNewReply(m_manager.get(request));
}

void DownloadManager::onDownloadError(const QString &message) {
    qWarning() << "Unable to get the mirror list:" << message;

    if (m_mirrorCache.isEmpty()) {
        m_current->handleNewReply(nullptr);
        return;
    }

    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setUrl(m_mirrorCache.first());
    request.setRawHeader("Range", QString("bytes=%1-").arg(m_current->bytesDownloaded()).toLocal8Bit());

    m_mirrorCache.removeFirst();
    m_current->handleNewReply(m_manager.get(request));
}

DownloadManager::DownloadManager() {
    QNetworkProxyFactory::setUseSystemConfiguration (true);
}


Download::Download(DownloadManager *parent, DownloadReceiver *receiver, const QString &filePath, Progress *progress)
    : QObject(parent)
    , m_receiver(receiver)
    , m_path(filePath)
    , m_progress(progress)
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &Download::onTimedOut);

    if (!m_path.isEmpty()) {
        m_file = new QFile(m_path + ".part", this);

        if (m_file->exists()) {
            m_bytesDownloaded = m_file->size();
            m_catchingUp = true;
        }
    }

    QTimer::singleShot(0, this, &Download::start);
}

DownloadManager *Download::manager() {
    return qobject_cast<DownloadManager*>(parent());
}

void Download::handleNewReply(QNetworkReply *reply) {
    if (!reply) {
        m_receiver->onDownloadError(tr("Unable to fetch the requested image."));
        return;
    }

    if (m_reply)
        m_reply->deleteLater();
    m_reply = reply;
    // have only a 64MB buffer in case the user is on a very fast network
    m_reply->setReadBufferSize(64*1024*1024);
    m_reply->setParent(this);

    connect(m_reply, &QNetworkReply::readyRead, this, &Download::onReadyRead);
    connect(m_reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error), this, &Download::onError);
    connect(m_reply, &QNetworkReply::sslErrors, this, &Download::onSslErrors);
    connect(m_reply, &QNetworkReply::finished, this, &Download::onFinished);
    if (m_progress) {
        connect(reply, &QNetworkReply::downloadProgress, this, &Download::onDownloadProgress);
    }

    m_timer.start(15000);

    if (m_reply->bytesAvailable() > 0)
        onReadyRead();

}

qint64 Download::bytesDownloaded() {
    return m_bytesDownloaded;
}

bool Download::hasCatchedUp() {
    return !m_catchingUp;
}

void Download::start() {
    if (m_catchingUp) {
        // first precompute the SHA hash of the already downloaded part
        m_file->open(QIODevice::ReadOnly);
        m_previousSize = 0;

        QTimer::singleShot(0, this, &Download::catchUp);
    }
    else if (!m_path.isEmpty()) {
        m_file->open(QIODevice::WriteOnly);
    }
}

void Download::catchUp() {

    QByteArray buffer = m_file->read(1024*1024);
    m_previousSize += buffer.size();
    m_hash.addData(buffer);
    if (m_progress && m_previousSize < m_progress->to())
        m_progress->setValue(m_previousSize);
    m_bytesDownloaded = m_previousSize;

    if (!m_file->atEnd()) {
        QTimer::singleShot(0, this, &Download::catchUp);
    }
    else {
        m_file->close();
        m_file->open(QIODevice::Append);
        m_catchingUp = false;
        auto mirror = DownloadManager::instance()->tryAnotherMirror();
        if (mirror)
            handleNewReply(mirror);
    }
}

void Download::onReadyRead() {
    QByteArray buf = m_reply->read(1024*64);
    if (m_reply->error() == QNetworkReply::NoError && buf.size() > 0) {

        m_bytesDownloaded += buf.size();

        if (m_progress && m_reply->header(QNetworkRequest::ContentLengthHeader).isValid())
            m_progress->setTo(m_reply->header(QNetworkRequest::ContentLengthHeader).toULongLong() + m_previousSize);

        if (m_progress)
            m_progress->setValue(m_bytesDownloaded);

        if (m_timer.isActive())
            m_timer.start(15000);

        if (m_file) {
            if (m_file->exists() && m_file->isOpen() && m_file->isWritable()) {
                m_hash.addData(buf);
                m_file->write(buf);
            }
            else {
                m_receiver->onDownloadError(tr("The download file is not writable."));
                deleteLater();
            }
        }
        else {
            m_buf.append(buf);
        }
    }
    if (m_reply->bytesAvailable() > 0) {
        QTimer::singleShot(0, this, &Download::onReadyRead);
    }
}

void Download::onError(QNetworkReply::NetworkError code) {
    qWarning() << "Error" << code << "reading from" << m_reply->url() << ":" << m_reply->errorString();
    if (m_path.isEmpty())
        return;

    QNetworkReply *reply = manager()->tryAnotherMirror();
    if (reply)
        handleNewReply(reply);
    else
        m_receiver->onDownloadError(m_reply->errorString());
}

void Download::onSslErrors(const QList<QSslError> errors) {
    qWarning() << "Error reading from" << m_reply->url() << ":" << m_reply->errorString();
    for (auto i : errors) {
        qWarning() << "SSL error" << i.errorString();
    }
    m_receiver->onDownloadError(m_reply->errorString());
}

void Download::onFinished() {
    m_timer.stop();
    if (m_reply->error() != 0) {
        if (m_file && m_file->size() == 0) {
            m_file->remove();
        }
    }
    else {
        while (m_reply->bytesAvailable() > 0) {
            onReadyRead();
            qApp->eventDispatcher()->processEvents(QEventLoop::ExcludeSocketNotifiers);
        }
        if (m_file) {
            m_file->rename(m_path);  // move the .part file to the final path
            m_receiver->onFileDownloaded(m_file->fileName(), m_hash.result().toHex());
            m_reply->deleteLater();
            m_reply = nullptr;
            deleteLater();
        }
        else {
            m_receiver->onStringDownloaded(m_buf);
            m_reply->deleteLater();
            m_reply = nullptr;
            deleteLater();
        }
    }
}

void Download::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    Q_UNUSED(bytesReceived);
    Q_UNUSED(bytesTotal);
    /*
    m_bytesDownloaded = m_previousSize + bytesReceived;
    if (bytesTotal > 0)
        m_progress->setValue(m_bytesDownloaded, m_previousSize + bytesTotal);
    else
        m_progress->setValue(m_bytesDownloaded);
    m_timer.start(15000);
    */
}

void Download::onTimedOut() {
    qWarning() << m_reply->url() << "timed out. Trying another mirror.";
    m_reply->deleteLater();
    if (m_path.isEmpty())
        return;

    QNetworkReply *reply = manager()->tryAnotherMirror();
    if (reply)
        handleNewReply(reply);
    else
        m_receiver->onDownloadError(tr("Connection timed out"));
}
