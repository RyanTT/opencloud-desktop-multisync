/*
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "gui/thumbnailstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLoggingCategory>
#include <QSaveFile>

#include <algorithm>
#include <vector>

using namespace Qt::Literals::StringLiterals;

namespace {
Q_LOGGING_CATEGORY(lcThumbnailStore, "gui.thumbnailstore", QtInfoMsg)

// check the size of the store every so many writes
constexpr int limitCheckInterval = 200;
}

namespace OCC {

ThumbnailStore::ThumbnailStore(const QString &directory, qint64 maxBytes)
    : _directory(directory)
    , _maxBytes(maxBytes)
{
}

QString ThumbnailStore::keyFor(const QByteArray &fileId, const QString &etag)
{
    // the file id may contain characters that are not allowed in file names ($ and ! in OpenCloud ids)
    return QString::fromLatin1(QCryptographicHash::hash(QByteArray(fileId + '\n' + etag.toUtf8()), QCryptographicHash::Sha1).toHex());
}

QString ThumbnailStore::pathFor(const QString &key, const char *suffix) const
{
    // spread the entries over 256 folders
    return u"%1/%2/%3.%4"_s.arg(_directory, key.left(2), key, QString::fromLatin1(suffix));
}

std::optional<QImage> ThumbnailStore::load(const QString &key) const
{
    QImage image;
    if (!image.load(pathFor(key, "img"))) {
        return {};
    }
    return image;
}

bool ThumbnailStore::hasThumbnail(const QString &key) const
{
    return QFile::exists(pathFor(key, "img"));
}

bool ThumbnailStore::isMarkedNoPreview(const QString &key) const
{
    return QFile::exists(pathFor(key, "none"));
}

void ThumbnailStore::store(const QString &key, const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    const auto path = pathFor(key, "img");
    QDir().mkpath(QFileInfo(path).path());
    const auto scaled = (image.width() > StoredSize || image.height() > StoredSize)
        ? image.scaled(StoredSize, StoredSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
        : image;
    QSaveFile file(path);
    // JPEG is a fraction of the size of PNG for photos, keep PNG where transparency matters
    if (!file.open(QIODevice::WriteOnly) || !scaled.save(&file, scaled.hasAlphaChannel() ? "PNG" : "JPG", 85) || !file.commit()) {
        qCWarning(lcThumbnailStore) << u"Failed to store thumbnail" << path << file.errorString();
        return;
    }
    if (++_storedSinceLimitCheck >= limitCheckInterval) {
        enforceLimit();
    }
}

void ThumbnailStore::markNoPreview(const QString &key)
{
    const auto path = pathFor(key, "none");
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.close();
    }
}

void ThumbnailStore::remove(const QString &key)
{
    QFile::remove(pathFor(key, "img"));
    QFile::remove(pathFor(key, "none"));
}

void ThumbnailStore::enforceLimit()
{
    _storedSinceLimitCheck = 0;

    struct Entry
    {
        QString path;
        qint64 size;
        QDateTime modified;
    };
    std::vector<Entry> entries;
    qint64 total = 0;
    for (QDirIterator it(_directory, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
        const auto info = it.nextFileInfo();
        entries.push_back({info.filePath(), info.size(), info.lastModified()});
        total += info.size();
    }
    if (total <= _maxBytes) {
        return;
    }
    // drop the oldest entries until we are at 90% of the limit
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.modified < b.modified; });
    const auto target = _maxBytes / 10 * 9;
    qsizetype removed = 0;
    for (const auto &entry : entries) {
        if (total <= target) {
            break;
        }
        if (QFile::remove(entry.path)) {
            total -= entry.size;
            ++removed;
        }
    }
    qCInfo(lcThumbnailStore) << u"Removed" << removed << u"old thumbnails, the store now uses" << total << u"bytes";
}
}
