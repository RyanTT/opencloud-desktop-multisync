/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

// Shared between the client (ThumbnailServer) and the Windows shell extension DLL (ThumbnailProviderIpc).
// Header only on purpose: the shell extension does not link libsync.

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QString>
#include <QVariantMap>
#include <QtEndian>

#include <optional>

namespace VfsShellExtensions {

/**
 * Name of the local server (a named pipe on Windows) the shell extension talks to.
 * It is per application and per user, so a stable and a beta client or two users on one machine don't collide.
 */
inline QString serverNameForApplicationName(const QString &applicationName)
{
    const QString user = qEnvironmentVariable("USERDOMAIN") + QLatin1Char('\\') + qEnvironmentVariable("USERNAME");
    const auto userTag = QString::fromLatin1(QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    return QStringLiteral("%1-VfsShellExtensions-%2").arg(applicationName, userTag);
}

namespace Protocol {
    // version 1 of the OpenCloud protocol, not compatible with Nextcloud's "2.0" (it adds framing)
    constexpr int Version = 1;

    constexpr auto VersionKey = "version";
    constexpr auto FilePathKey = "filePath";
    constexpr auto ThumbnailProviderRequestKey = "thumbnailProviderRequest";
    // the edge length in pixels Explorer asked for, the thumbnail must fit into a square of that size
    constexpr auto ThumbnailProviderRequestSizeKey = "size";
    // base64 encoded PNG, missing or empty if no thumbnail is available
    constexpr auto ThumbnailProviderDataKey = "thumbnailData";

    // a message is a 4 byte big endian length followed by that many bytes of compact JSON
    constexpr qsizetype HeaderSize = 4;
    constexpr qsizetype MaxMessageSize = 16 * 1024 * 1024;

    inline QByteArray createMessage(const QVariantMap &message)
    {
        QVariantMap messageCopy = message;
        messageCopy[QString::fromLatin1(VersionKey)] = Version;
        const auto json = QJsonDocument::fromVariant(messageCopy).toJson(QJsonDocument::Compact);
        QByteArray out(HeaderSize, Qt::Uninitialized);
        qToBigEndian<quint32>(static_cast<quint32>(json.size()), out.data());
        return out + json;
    }

    inline bool validateProtocolVersion(const QVariantMap &message)
    {
        return message.value(QString::fromLatin1(VersionKey)).toInt() == Version;
    }

    /**
     * Collects incoming bytes until a complete message is available.
     */
    class MessageReader
    {
    public:
        void append(const QByteArray &data) { _buffer.append(data); }

        // true if the peer sent something that can never become a valid message
        bool hasError() const { return _error; }

        std::optional<QVariantMap> takeMessage()
        {
            if (_error || _buffer.size() < HeaderSize) {
                return {};
            }
            const auto size = static_cast<qsizetype>(qFromBigEndian<quint32>(_buffer.constData()));
            if (size > MaxMessageSize) {
                _error = true;
                return {};
            }
            if (_buffer.size() < HeaderSize + size) {
                return {};
            }
            QJsonParseError parseError;
            const auto doc = QJsonDocument::fromJson(_buffer.mid(HeaderSize, size), &parseError);
            _buffer.remove(0, HeaderSize + size);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                _error = true;
                return {};
            }
            return doc.toVariant().toMap();
        }

    private:
        QByteArray _buffer;
        bool _error = false;
    };
}
}
