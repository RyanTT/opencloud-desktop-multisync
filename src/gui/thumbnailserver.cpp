/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gui/thumbnailserver.h"

#include "gui/folder.h"
#include "gui/folderman.h"
#include "libsync/common/shellextensionutils.h"
#include "libsync/networkjobs/simplenetworkjob.h"
#include "libsync/theme.h"

#include <QBuffer>
#include <algorithm>
#include <memory>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QTimer>

using namespace std::chrono_literals;
using namespace Qt::Literals::StringLiterals;

namespace {
Q_LOGGING_CATEGORY(lcThumbnailServer, "gui.thumbnailserver", QtInfoMsg)

// the shell extension waits 6s for the whole round trip
constexpr auto networkTimeout = 4s;
// drop connections that never send a complete request
constexpr auto connectionTimeout = 10s;
// don't ask the server again for a file it had no preview for
constexpr auto noPreviewTimeout = 10min;
// files above this are not worth asking for, the server's thumbnailer has its own limits anyway
constexpr qint64 maxFileSize = 50 * 1024 * 1024;
constexpr int maxThumbnailSize = 1024;
constexpr qint64 cacheSizeBytes = 32 * 1024 * 1024;

// what the OpenCloud thumbnails service can render (services/thumbnails/pkg/thumbnail/mimetypes*.go),
// image formats only, there are no video previews on the server
bool hasPreviewableExtension(const QString &path)
{
    static const QStringList extensions = {u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"bmp"_s, u"tif"_s, u"tiff"_s, u"webp"_s};
    return extensions.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}
}

namespace OCC {

ThumbnailServer::ThumbnailServer(QObject *parent)
    : QObject(parent)
    , _cache(cacheSizeBytes)
{
    const auto name = VfsShellExtensions::serverNameForApplicationName(Theme::instance()->appName());
    // only the current user may talk to us
    _server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!_server.listen(name)) {
        qCWarning(lcThumbnailServer) << u"Failed to listen on" << name << _server.errorString() << u", Explorer won't show thumbnails for dehydrated files";
        return;
    }
    qCInfo(lcThumbnailServer) << u"Listening on" << _server.fullServerName();
    connect(&_server, &QLocalServer::newConnection, this, &ThumbnailServer::handleNewConnection);
}

ThumbnailServer::~ThumbnailServer() = default;

void ThumbnailServer::handleNewConnection()
{
    while (auto *socket = _server.nextPendingConnection()) {
        auto reader = std::make_shared<VfsShellExtensions::Protocol::MessageReader>();
        auto handled = std::make_shared<bool>(false);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(connectionTimeout, socket, [socket] { socket->abort(); });
        connect(socket, &QLocalSocket::readyRead, this, [this, socket, reader, handled] {
            // one request per connection
            if (*handled) {
                socket->readAll();
                return;
            }
            reader->append(socket->readAll());
            if (auto message = reader->takeMessage()) {
                *handled = true;
                handleRequest(socket, *message);
            } else if (reader->hasError()) {
                socket->abort();
            }
        });
    }
}

void ThumbnailServer::handleRequest(QLocalSocket *socket, const QVariantMap &message)
{
    if (!VfsShellExtensions::Protocol::validateProtocolVersion(message)) {
        qCWarning(lcThumbnailServer) << u"Unsupported shell extension protocol version" << message.value(QString::fromLatin1(VfsShellExtensions::Protocol::VersionKey));
        sendReply(socket, {});
        return;
    }
    const auto request = message.value(QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderRequestKey)).toMap();
    const auto path = QDir::fromNativeSeparators(request.value(QString::fromLatin1(VfsShellExtensions::Protocol::FilePathKey)).toString());
    const auto size = std::min(request.value(QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderRequestSizeKey)).toInt(), maxThumbnailSize);
    if (path.isEmpty() || size <= 0 || !hasPreviewableExtension(path)) {
        sendReply(socket, {});
        return;
    }

    // works for every sync folder, including the ones in custom locations
    QString relativePath;
    auto *folder = FolderMan::instance()->folderForPath(path, &relativePath);
    if (!folder || relativePath.isEmpty() || !folder->isReady() || !folder->accountState()->isConnected()) {
        sendReply(socket, {});
        return;
    }

    const auto record = folder->journalDb()->getFileRecord(relativePath);
    if (!record.isValid() || record.type() == ItemTypeDirectory || record.size() > maxFileSize) {
        sendReply(socket, {});
        return;
    }

    // the file id is only used to identify the file locally, the request is built from the path
    const auto key = u"%1:%2:%3:%4"_s.arg(QString::fromUtf8(record.fileId()), record.etag(), QString::number(record.size()), QString::number(size));
    if (const auto *png = _cache.object(key)) {
        sendReply(socket, *png);
        return;
    }
    if (const auto it = _noPreview.constFind(key); it != _noPreview.cend()) {
        if (!it->hasExpired()) {
            sendReply(socket, {});
            return;
        }
        _noPreview.erase(it);
    }

    auto &waiting = _pending[key];
    waiting.append({socket, size});
    if (waiting.size() == 1) {
        fetchThumbnail(folder, relativePath, record.etag(), size, key);
    }
}

void ThumbnailServer::fetchThumbnail(Folder *folder, const QString &relativePath, const QString &etag, int size, const QString &key)
{
    // Same request as the web client (web-pkg previewService): GET <space dav url>/<path>?preview=1&x=..&y=..
    // The server only evaluates x, y and processor, c (the etag) makes sure a changed file isn't served from a cache.
    // webDavUrl() is the Space root plus the sub path of a custom folder, the relative path is percent encoded by QUrl.
    auto *job = new SimpleNetworkJob(folder->accountState()->account(), folder->webDavUrl(), relativePath, "GET",
        SimpleNetworkJob::UrlQuery{{u"preview"_s, u"1"_s}, {u"scalingup"_s, u"0"_s}, {u"a"_s, u"1"_s}, {u"c"_s, QString(etag).remove(u'"')},
            {u"x"_s, QString::number(size)}, {u"y"_s, QString::number(size)}},
        {}, this);
    job->setTimeout(networkTimeout);
    connect(job, &SimpleNetworkJob::finishedSignal, this, [this, job, key, size] {
        QByteArray png;
        const auto status = job->httpStatusCode();
        if (status == 200 && job->reply()->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("image/"_L1)) {
            auto image = QImage::fromData(job->reply()->readAll());
            if (!image.isNull()) {
                // Explorer expects the thumbnail to fit into a square of the requested size
                if (image.width() > size || image.height() > size) {
                    image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }
                QBuffer buffer(&png);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "PNG");
            }
        }

        if (!png.isEmpty()) {
            _cache.insert(key, new QByteArray(png), png.size());
        } else if (status == 404 || status == 400 || status == 403) {
            // no preview for that file (unsupported or broken), anything else (425 still processing, 429, timeouts) is worth a retry
            if (_noPreview.size() > 10000) {
                _noPreview.removeIf([](const auto &it) { return it.value().hasExpired(); });
            }
            _noPreview.insert(key, QDeadlineTimer(noPreviewTimeout));
        } else {
            qCDebug(lcThumbnailServer) << u"No thumbnail for" << job->url() << status << job->reply()->errorString();
        }

        const auto waiting = _pending.take(key);
        for (const auto &request : waiting) {
            if (request.socket) {
                sendReply(request.socket, png);
            }
        }
    });
    job->start();
}

void ThumbnailServer::sendReply(QLocalSocket *socket, const QByteArray &png)
{
    socket->write(VfsShellExtensions::Protocol::createMessage({{QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderDataKey), QString::fromLatin1(png.toBase64())}}));
    // flushes the pending data before the connection is closed
    socket->disconnectFromServer();
}
}
