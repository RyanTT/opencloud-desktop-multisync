/*
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QImage>
#include <QString>

#include <optional>

namespace OCC {

/**
 * Thumbnails on disk, so they survive a restart and are available offline.
 *
 * One entry per file version (file id + etag), stored at StoredSize and scaled down on demand.
 * Files the server has no preview for get a marker, so they are not requested again.
 * The oldest entries are removed once the store grows beyond its limit.
 */
class ThumbnailStore
{
public:
    // edge length of the stored thumbnails, Explorer's largest icon view
    static constexpr int StoredSize = 256;

    explicit ThumbnailStore(const QString &directory, qint64 maxBytes);

    static QString keyFor(const QByteArray &fileId, const QString &etag);

    std::optional<QImage> load(const QString &key) const;
    bool hasThumbnail(const QString &key) const;
    bool isMarkedNoPreview(const QString &key) const;
    // true if there is nothing to fetch for that key anymore
    bool contains(const QString &key) const { return hasThumbnail(key) || isMarkedNoPreview(key); }

    void store(const QString &key, const QImage &image);
    void markNoPreview(const QString &key);
    void remove(const QString &key);

    // removes the oldest entries until the store is below its limit
    void enforceLimit();

private:
    QString pathFor(const QString &key, const char *suffix) const;

    QString _directory;
    qint64 _maxBytes;
    int _storedSinceLimitCheck = 0;
};
}
