/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "gui/thumbnailstore.h"

#include <QCache>
#include <QHash>
#include <QImage>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <functional>

class QLocalSocket;

namespace OCC {
class Folder;

/**
 * Serves thumbnails to the Windows Explorer shell extension (src/plugins/vfs/cfapi/shellext).
 *
 * Explorer asks for a thumbnail of a file in a sync root, including dehydrated placeholders.
 * We map the local path to its folder (any sync folder, custom locations included) and look the
 * file up in the journal. Thumbnails come from, in that order:
 *  - the ThumbnailStore on disk, so they survive restarts and work offline
 *  - the local file, if it is hydrated (no network, and right even before an edit is uploaded)
 *  - the server's WebDAV preview (<space dav url>/<path>?preview=1&x=..&y=..), the same mechanism
 *    the web client uses. The file content is never downloaded.
 *
 * Folders can opt in to prefetching: thumbnails of all their images are fetched in the
 * background, and new or changed images after every sync.
 *
 * The shell extension gives up after a few seconds, so everything here is asynchronous and
 * any problem results in an empty reply, Explorer then shows the regular icon.
 */
class ThumbnailServer : public QObject
{
    Q_OBJECT
public:
    explicit ThumbnailServer(QObject *parent = nullptr);
    ~ThumbnailServer() override;

    static ThumbnailServer *instance();

    struct PrefetchEstimate
    {
        qint64 images = 0;
        // images without a thumbnail on disk yet
        qint64 missing = 0;
    };
    PrefetchEstimate estimatePrefetch(Folder *folder) const;

    // call after Folder::setPrefetchThumbnails(), disabling removes the folder's stored thumbnails
    void setPrefetchEnabled(Folder *folder, bool enabled);

private:
    struct Request
    {
        QPointer<QLocalSocket> socket;
        int size = 0;
    };

    struct PrefetchItem
    {
        QPointer<Folder> folder;
        QString relativePath;
        int attempts = 0;
    };

    void handleNewConnection();
    void handleRequest(QLocalSocket *socket, const QVariantMap &message);
    void sendReply(QLocalSocket *socket, const QImage &image, int size);

    // done is called with a null image on failure, status is the HTTP status (0 for network errors)
    void fetchFromServer(Folder *folder, const QString &relativePath, const QString &etag, int size, bool background,
        const std::function<void(const QImage &image, int status)> &done);

    void scanFolder(Folder *folder);
    void scanEnabledFolders();
    void enqueue(Folder *folder, const QString &relativePath);
    void processPrefetchQueue();

    // progress per folder, shown in the folder list next to the sync progress
    void itemFinished(Folder *folder);
    void scheduleProgressUpdate();
    void updateProgress();

    QLocalServer _server;
    ThumbnailStore _store;
    // thumbnails larger than the stored size (Explorer's preview pane), in memory only, the cost is the size in bytes
    QCache<QString, QImage> _largeCache;
    // requests waiting for a running fetch of the same thumbnail
    QHash<QString, QList<Request>> _pending;

    QList<PrefetchItem> _prefetchQueue;
    // items to try again once _prefetchRetryTimer fires
    QList<PrefetchItem> _prefetchDeferred;
    // folder path + relative path of the queued items
    QSet<QString> _prefetchQueued;
    int _prefetchRunning = 0;
    QTimer _prefetchRetryTimer;
    QList<QPointer<Folder>> _scannedFolders;

    struct FolderProgress
    {
        qint64 done = 0;
        qint64 total = 0;
    };
    // the current pass per folder, a pass ends when everything queued for the folder is handled
    QHash<Folder *, FolderProgress> _progress;
    QTimer _progressTimer;
};
}
