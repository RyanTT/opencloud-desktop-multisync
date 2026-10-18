/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gui/thumbnailserver.h"

#include "gui/folder.h"
#include "gui/folderman.h"
#include "gui/networkinformation.h"
#include "libsync/common/shellextensionutils.h"
#include "libsync/networkjobs/simplenetworkjob.h"
#include "libsync/progressdispatcher.h"
#include "libsync/theme.h"
#include "libsync/vfs/vfs.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QStandardPaths>

#include <algorithm>
#include <memory>
#include <utility>

using namespace std::chrono_literals;
using namespace Qt::Literals::StringLiterals;

namespace {
Q_LOGGING_CATEGORY(lcThumbnailServer, "gui.thumbnailserver", QtInfoMsg)

OCC::ThumbnailServer *serverInstance = nullptr;

// the shell extension waits 6s for the whole round trip
constexpr auto networkTimeout = 4s;
// nobody is waiting for prefetched thumbnails
constexpr auto prefetchNetworkTimeout = 30s;
// drop connections that never send a complete request
constexpr auto connectionTimeout = 10s;
// when prefetching is paused (offline, metered) or the server asked us to come back later
constexpr auto prefetchRetryInterval = 2min;
// delay the initial scan, so it doesn't slow down the start of the client
constexpr auto initialScanDelay = 30s;
constexpr int maxPrefetchAttempts = 3;
constexpr int maxParallelPrefetches = 2;
// local thumbnails are generated on the GUI thread, give the event loop a chance in between
constexpr int maxLocalItemsPerRound = 20;
// files above this are not worth it, the server's thumbnailer has its own limits anyway
constexpr qint64 maxFileSize = 50 * 1024 * 1024;
constexpr int maxThumbnailSize = 1024;
constexpr qint64 largeCacheBytes = 32 * 1024 * 1024;
constexpr qint64 storeBytes = 2LL * 1024 * 1024 * 1024;

// what the OpenCloud thumbnails service can render (services/thumbnails/pkg/thumbnail/mimetypes*.go),
// image formats only, there are no video previews on the server
bool hasPreviewableExtension(const QString &path)
{
    static const QStringList extensions = {u"png"_s, u"jpg"_s, u"jpeg"_s, u"gif"_s, u"bmp"_s, u"tif"_s, u"tiff"_s, u"webp"_s};
    return extensions.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
}

bool isPrefetchCandidate(const OCC::SyncJournalFileRecord &record)
{
    return record.isValid() && !record.isDirectory() && record.size() <= maxFileSize && hasPreviewableExtension(record.path());
}

bool isNoPreviewStatus(int status)
{
    // the server has no preview for that file (unsupported or broken), anything else
    // (425 still processing, 429 busy, timeouts) is worth a retry
    return status == 400 || status == 403 || status == 404;
}

QString absolutePath(OCC::Folder *folder, const QString &relativePath)
{
    return QDir(folder->path()).filePath(relativePath);
}

// a hydrated file is on disk, render it instead of asking the server
QImage generateLocally(OCC::Folder *folder, const QString &relativePath, int size)
{
    QImageReader reader(absolutePath(folder, relativePath));
    reader.setAutoTransform(true);
    const auto sourceSize = reader.size();
    if (sourceSize.isValid() && (sourceSize.width() > size || sourceSize.height() > size)) {
        // lets the JPEG decoder skip most of the work
        reader.setScaledSize(sourceSize.scaled(size, size, Qt::KeepAspectRatio));
    }
    return reader.read();
}

bool isHydrated(OCC::Folder *folder, const QString &relativePath)
{
    // never read a dehydrated placeholder, that would download it
    return !folder->vfs().isDehydratedPlaceholder(absolutePath(folder, relativePath));
}
}

namespace OCC {

ThumbnailServer::ThumbnailServer(QObject *parent)
    : QObject(parent)
    , _store(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + u"/thumbnails"_s, storeBytes)
    , _largeCache(largeCacheBytes)
{
    serverInstance = this;

    const auto name = VfsShellExtensions::serverNameForApplicationName(Theme::instance()->appName());
    // only the current user may talk to us
    _server.setSocketOptions(QLocalServer::UserAccessOption);
    if (_server.listen(name)) {
        qCInfo(lcThumbnailServer) << u"Listening on" << _server.fullServerName();
        connect(&_server, &QLocalServer::newConnection, this, &ThumbnailServer::handleNewConnection);
    } else {
        qCWarning(lcThumbnailServer) << u"Failed to listen on" << name << _server.errorString() << u", Explorer won't show thumbnails for dehydrated files";
    }

    // prefetching: new and changed images after every sync, all of them once per session
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted, this, [this](Folder *folder, const SyncFileItemPtr &item) {
        if (folder && folder->prefetchThumbnails() && item->_status == SyncFileItem::Success && !item->isDirectory()
            && hasPreviewableExtension(item->destination())) {
            enqueue(folder, item->destination());
            processPrefetchQueue();
        }
    });
    connect(FolderMan::instance(), &FolderMan::folderListChanged, this, [this] { QTimer::singleShot(initialScanDelay, this, &ThumbnailServer::scanEnabledFolders); });
    _prefetchRetryTimer.setSingleShot(true);
    _prefetchRetryTimer.setInterval(prefetchRetryInterval);
    connect(&_prefetchRetryTimer, &QTimer::timeout, this, [this] {
        _prefetchQueue.append(std::exchange(_prefetchDeferred, QList<PrefetchItem>{}));
        processPrefetchQueue();
    });
    connect(NetworkInformation::instance(), &NetworkInformation::isMeteredChanged, this, &ThumbnailServer::processPrefetchQueue);
    // don't update the UI for every single thumbnail
    _progressTimer.setSingleShot(true);
    _progressTimer.setInterval(250ms);
    connect(&_progressTimer, &QTimer::timeout, this, &ThumbnailServer::updateProgress);

    QTimer::singleShot(initialScanDelay, this, [this] {
        scanEnabledFolders();
        _store.enforceLimit();
    });
}

ThumbnailServer::~ThumbnailServer()
{
    serverInstance = nullptr;
}

ThumbnailServer *ThumbnailServer::instance()
{
    return serverInstance;
}

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
        sendReply(socket, {}, 0);
        return;
    }
    const auto request = message.value(QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderRequestKey)).toMap();
    const auto path = QDir::fromNativeSeparators(request.value(QString::fromLatin1(VfsShellExtensions::Protocol::FilePathKey)).toString());
    const auto size = std::min(request.value(QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderRequestSizeKey)).toInt(), maxThumbnailSize);
    if (path.isEmpty() || size <= 0 || !hasPreviewableExtension(path)) {
        sendReply(socket, {}, 0);
        return;
    }

    // works for every sync folder, including the ones in custom locations
    QString relativePath;
    auto *folder = FolderMan::instance()->folderForPath(path, &relativePath);
    if (!folder || relativePath.isEmpty() || !folder->isReady()) {
        sendReply(socket, {}, 0);
        return;
    }

    const auto record = folder->journalDb()->getFileRecord(relativePath);
    if (!isPrefetchCandidate(record)) {
        sendReply(socket, {}, 0);
        return;
    }

    // the file id is only used to identify the file locally, requests are built from the path
    const auto key = ThumbnailStore::keyFor(record.fileId(), record.etag());
    const bool isStoredSize = size <= ThumbnailStore::StoredSize;
    const auto largeKey = u"%1:%2"_s.arg(key, QString::number(size));
    if (isStoredSize) {
        if (const auto image = _store.load(key)) {
            sendReply(socket, *image, size);
            return;
        }
        if (_store.isMarkedNoPreview(key)) {
            sendReply(socket, {}, 0);
            return;
        }
    } else if (const auto *image = _largeCache.object(largeKey)) {
        sendReply(socket, *image, size);
        return;
    }

    if (isHydrated(folder, relativePath)) {
        const auto image = generateLocally(folder, relativePath, std::max(size, ThumbnailStore::StoredSize));
        if (!image.isNull()) {
            _store.store(key, image);
        }
        sendReply(socket, image, size);
        return;
    }

    if (!folder->accountState()->isConnected()) {
        sendReply(socket, {}, 0);
        return;
    }

    // small thumbnails are fetched at the stored size, so the next size Explorer asks for is already there
    const auto fetchSize = isStoredSize ? ThumbnailStore::StoredSize : size;
    const auto pendingKey = u"%1:%2"_s.arg(key, QString::number(fetchSize));
    auto &waiting = _pending[pendingKey];
    waiting.append({socket, size});
    if (waiting.size() > 1) {
        return;
    }
    fetchFromServer(folder, relativePath, record.etag(), fetchSize, false, [this, key, pendingKey, isStoredSize, largeKey](const QImage &image, int status) {
        if (!image.isNull()) {
            if (isStoredSize) {
                _store.store(key, image);
            } else {
                _largeCache.insert(largeKey, new QImage(image), image.sizeInBytes());
            }
        } else if (isNoPreviewStatus(status)) {
            _store.markNoPreview(key);
        }
        const auto waitingRequests = _pending.take(pendingKey);
        for (const auto &request : waitingRequests) {
            if (request.socket) {
                sendReply(request.socket, image, request.size);
            }
        }
    });
}

void ThumbnailServer::fetchFromServer(
    Folder *folder, const QString &relativePath, const QString &etag, int size, bool background, const std::function<void(const QImage &, int)> &done)
{
    // Same request as the web client (web-pkg previewService): GET <space dav url>/<path>?preview=1&x=..&y=..
    // The server only evaluates x, y and processor, c (the etag) makes sure a changed file isn't served from a cache.
    // webDavUrl() is the Space root plus the sub path of a custom folder, the relative path is percent encoded by QUrl.
    auto *job = new SimpleNetworkJob(folder->accountState()->account(), folder->webDavUrl(), relativePath, "GET",
        SimpleNetworkJob::UrlQuery{{u"preview"_s, u"1"_s}, {u"scalingup"_s, u"0"_s}, {u"a"_s, u"1"_s}, {u"c"_s, QString(etag).remove(u'"')},
            {u"x"_s, QString::number(size)}, {u"y"_s, QString::number(size)}},
        {}, this);
    job->setTimeout(background ? prefetchNetworkTimeout : networkTimeout);
    if (background) {
        // never compete with the sync or with what Explorer is waiting for
        job->setPriority(QNetworkRequest::LowPriority);
    }
    connect(job, &SimpleNetworkJob::finishedSignal, this, [job, done] {
        QImage image;
        const auto status = job->httpStatusCode();
        if (status == 200 && job->reply()->header(QNetworkRequest::ContentTypeHeader).toString().startsWith("image/"_L1)) {
            image = QImage::fromData(job->reply()->readAll());
        } else {
            qCDebug(lcThumbnailServer) << u"No thumbnail for" << job->url() << status << job->reply()->errorString();
        }
        done(image, status);
    });
    job->start();
}

void ThumbnailServer::sendReply(QLocalSocket *socket, const QImage &image, int size)
{
    QByteArray png;
    if (!image.isNull()) {
        // Explorer expects the thumbnail to fit into a square of the requested size
        const auto scaled = (image.width() > size || image.height() > size) ? image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        scaled.save(&buffer, "PNG");
    }
    socket->write(VfsShellExtensions::Protocol::createMessage(
        {{QString::fromLatin1(VfsShellExtensions::Protocol::ThumbnailProviderDataKey), QString::fromLatin1(png.toBase64())}}));
    // flushes the pending data before the connection is closed
    socket->disconnectFromServer();
}

ThumbnailServer::PrefetchEstimate ThumbnailServer::estimatePrefetch(Folder *folder) const
{
    PrefetchEstimate estimate;
    folder->journalDb()->getFilesBelowPath(QString(), [&estimate, this](const SyncJournalFileRecord &record) {
        if (isPrefetchCandidate(record)) {
            ++estimate.images;
            if (!_store.contains(ThumbnailStore::keyFor(record.fileId(), record.etag()))) {
                ++estimate.missing;
            }
        }
    });
    return estimate;
}

void ThumbnailServer::setPrefetchEnabled(Folder *folder, bool enabled)
{
    if (enabled) {
        qCInfo(lcThumbnailServer) << u"Keeping thumbnails available for" << folder->path();
        scanFolder(folder);
        return;
    }

    qCInfo(lcThumbnailServer) << u"No longer keeping thumbnails available for" << folder->path();
    _scannedFolders.removeIf([folder](const QPointer<Folder> &scanned) { return scanned.data() == folder; });
    _progress.remove(folder);
    folder->setThumbnailProgress(Folder::ThumbnailState::Idle, 0, 0);
    // queued items of the folder are dropped when they come up, remove what is stored
    folder->journalDb()->getFilesBelowPath(QString(), [this](const SyncJournalFileRecord &record) {
        if (isPrefetchCandidate(record)) {
            _store.remove(ThumbnailStore::keyFor(record.fileId(), record.etag()));
        }
    });
}

void ThumbnailServer::scanEnabledFolders()
{
    _scannedFolders.removeIf([](const QPointer<Folder> &scanned) { return scanned.isNull(); });
    for (auto *folder : FolderMan::instance()->folders()) {
        if (folder->isReady() && folder->prefetchThumbnails() && !isScanned(folder)) {
            scanFolder(folder);
        }
    }
}

bool ThumbnailServer::isScanned(Folder *folder) const
{
    return std::any_of(_scannedFolders.cbegin(), _scannedFolders.cend(), [folder](const QPointer<Folder> &scanned) { return scanned.data() == folder; });
}

void ThumbnailServer::scanFolder(Folder *folder)
{
    if (!isScanned(folder)) {
        _scannedFolders.append(folder);
    }
    // whether a thumbnail is needed is checked when the item comes up, the scan is a single query
    folder->journalDb()->getFilesBelowPath(QString(), [this, folder](const SyncJournalFileRecord &record) {
        if (isPrefetchCandidate(record)) {
            enqueue(folder, record.path());
        }
    });
    qCInfo(lcThumbnailServer) << u"Thumbnail prefetch queue:" << _prefetchQueue.size() << u"images";
    processPrefetchQueue();
}

void ThumbnailServer::enqueue(Folder *folder, const QString &relativePath)
{
    const auto id = absolutePath(folder, relativePath);
    if (!_prefetchQueued.contains(id)) {
        _prefetchQueued.insert(id);
        _prefetchQueue.append({folder, relativePath});
        ++_progress[folder].total;
        scheduleProgressUpdate();
    }
}

void ThumbnailServer::processPrefetchQueue()
{
    int localItems = 0;
    while (_prefetchRunning < maxParallelPrefetches && !_prefetchQueue.isEmpty()) {
        if (localItems >= maxLocalItemsPerRound) {
            // continue in the next event loop iteration
            QTimer::singleShot(0, this, &ThumbnailServer::processPrefetchQueue);
            return;
        }

        auto item = _prefetchQueue.takeFirst();
        Folder *folder = item.folder;
        if (!folder || !folder->prefetchThumbnails() || !folder->isReady()) {
            if (folder) {
                _prefetchQueued.remove(absolutePath(folder, item.relativePath));
                itemFinished(folder);
            }
            continue;
        }
        const auto id = absolutePath(folder, item.relativePath);
        const auto record = folder->journalDb()->getFileRecord(item.relativePath);
        if (!isPrefetchCandidate(record)) {
            _prefetchQueued.remove(id);
            itemFinished(folder);
            continue;
        }
        const auto key = ThumbnailStore::keyFor(record.fileId(), record.etag());
        if (_store.contains(key)) {
            _prefetchQueued.remove(id);
            itemFinished(folder);
            continue;
        }

        if (isHydrated(folder, item.relativePath)) {
            ++localItems;
            _prefetchQueued.remove(id);
            itemFinished(folder);
            const auto image = generateLocally(folder, item.relativePath, ThumbnailStore::StoredSize);
            if (image.isNull()) {
                _store.markNoPreview(key);
            } else {
                _store.store(key, image);
            }
            continue;
        }

        if (!folder->accountState()->isConnected() || NetworkInformation::instance()->isMetered()) {
            // keep the item, try again later
            _prefetchQueue.prepend(item);
            _prefetchRetryTimer.start();
            scheduleProgressUpdate();
            return;
        }

        ++_prefetchRunning;
        ++item.attempts;
        fetchFromServer(folder, item.relativePath, record.etag(), ThumbnailStore::StoredSize, true, [this, item, id, key](const QImage &image, int status) {
            --_prefetchRunning;
            if (!image.isNull()) {
                _store.store(key, image);
                _prefetchQueued.remove(id);
                itemFinished(item.folder);
            } else if (isNoPreviewStatus(status)) {
                _store.markNoPreview(key);
                _prefetchQueued.remove(id);
                itemFinished(item.folder);
            } else if (item.attempts < maxPrefetchAttempts) {
                // e.g. 425 while the server is still processing the file, or 429
                _prefetchDeferred.append(item);
                _prefetchRetryTimer.start();
            } else {
                _prefetchQueued.remove(id);
                itemFinished(item.folder);
            }
            scheduleProgressUpdate();
            if (_prefetchQueue.isEmpty() && _prefetchDeferred.isEmpty() && _prefetchRunning == 0) {
                qCInfo(lcThumbnailServer) << u"Thumbnail prefetch queue is done";
            }
            processPrefetchQueue();
        });
    }
}

void ThumbnailServer::itemFinished(Folder *folder)
{
    if (!folder) {
        return;
    }
    if (const auto it = _progress.find(folder); it != _progress.end()) {
        ++it->done;
        scheduleProgressUpdate();
    }
}

void ThumbnailServer::scheduleProgressUpdate()
{
    if (!_progressTimer.isActive()) {
        _progressTimer.start();
    }
}

void ThumbnailServer::updateProgress()
{
    const auto &folders = FolderMan::instance()->folders();
    for (auto it = _progress.begin(); it != _progress.end();) {
        auto *folder = it.key();
        if (!folders.contains(folder)) {
            it = _progress.erase(it);
            continue;
        }
        const auto remaining = it->total - it->done;
        if (remaining <= 0) {
            // the pass is complete, the next new image starts a new one
            folder->setThumbnailProgress(Folder::ThumbnailState::Idle, 0, 0);
            it = _progress.erase(it);
            continue;
        }
        const auto deferred = std::count_if(_prefetchDeferred.cbegin(), _prefetchDeferred.cend(), [folder](const PrefetchItem &item) { return item.folder.data() == folder; });
        auto state = Folder::ThumbnailState::Running;
        if (NetworkInformation::instance()->isMetered()) {
            state = Folder::ThumbnailState::PausedMetered;
        } else if (!folder->accountState()->isConnected()) {
            state = Folder::ThumbnailState::PausedOffline;
        } else if (deferred >= remaining) {
            state = Folder::ThumbnailState::WaitingForServer;
        }
        folder->setThumbnailProgress(state, it->done, it->total);
        ++it;
    }
}
}
