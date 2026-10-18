/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QCache>
#include <QDeadlineTimer>
#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QPointer>

class QLocalSocket;

namespace OCC {
class Folder;

/**
 * Serves thumbnails to the Windows Explorer shell extension (src/plugins/vfs/cfapi/shellext).
 *
 * Explorer asks for a thumbnail of a file in a sync root, including dehydrated placeholders.
 * We map the local path to its folder (any sync folder, custom locations included), look the
 * file up in the journal and fetch a preview from the server's WebDAV endpoint
 * (<space dav url>/<path>?preview=1&x=..&y=..), the same mechanism the web client uses.
 * The file content is never downloaded.
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

private:
    struct Request
    {
        QPointer<QLocalSocket> socket;
        int size = 0;
    };

    void handleNewConnection();
    void handleRequest(QLocalSocket *socket, const QVariantMap &message);
    void fetchThumbnail(Folder *folder, const QString &relativePath, const QString &etag, int size, const QString &key);
    void sendReply(QLocalSocket *socket, const QByteArray &png);

    QLocalServer _server;
    // PNG data by file id, etag, size and requested edge length, the cost is the size in bytes
    QCache<QString, QByteArray> _cache;
    // files the server has no preview for, so scrolling through a folder doesn't ask again and again
    QHash<QString, QDeadlineTimer> _noPreview;
    // requests waiting for a running fetch of the same key
    QHash<QString, QList<Request>> _pending;
};
}
