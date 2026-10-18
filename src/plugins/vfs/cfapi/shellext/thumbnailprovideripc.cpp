/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "thumbnailprovideripc.h"

#include "configvfscfapishellext.h"
#include "libsync/common/shellextensionutils.h"

#include <QDeadlineTimer>
#include <QLocalSocket>

using namespace std::chrono_literals;

namespace {
// connecting to a running client is instant, if it takes longer the client is not there or hangs
constexpr auto connectTimeout = 1s;
// the client fetches the preview from the server with a shorter network timeout, this is the hard upper bound
constexpr auto replyTimeout = 6s;
}

namespace VfsShellExtensions {

QString ThumbnailProviderIpc::overrideServerName;

QByteArray ThumbnailProviderIpc::fetchThumbnailForFile(const QString &filePath, int size)
{
    const auto serverName = overrideServerName.isEmpty() ? serverNameForApplicationName(QStringLiteral(CFAPI_SHELLEXT_APPLICATION_NAME)) : overrideServerName;

    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (!socket.waitForConnected(static_cast<int>(std::chrono::milliseconds(connectTimeout).count()))) {
        return {};
    }

    const auto request = Protocol::createMessage({{QString::fromLatin1(Protocol::ThumbnailProviderRequestKey),
        QVariantMap{{QString::fromLatin1(Protocol::FilePathKey), filePath}, {QString::fromLatin1(Protocol::ThumbnailProviderRequestSizeKey), size}}}});

    const QDeadlineTimer deadline(replyTimeout);
    socket.write(request);
    while (socket.bytesToWrite() > 0) {
        if (deadline.hasExpired() || !socket.waitForBytesWritten(static_cast<int>(deadline.remainingTime()))) {
            return {};
        }
    }

    Protocol::MessageReader reader;
    while (true) {
        if (socket.bytesAvailable() > 0) {
            reader.append(socket.readAll());
            if (const auto message = reader.takeMessage()) {
                socket.disconnectFromServer();
                if (!Protocol::validateProtocolVersion(*message)) {
                    return {};
                }
                return QByteArray::fromBase64(message->value(QString::fromLatin1(Protocol::ThumbnailProviderDataKey)).toByteArray());
            }
            if (reader.hasError()) {
                return {};
            }
        }
        if (deadline.hasExpired() || socket.state() != QLocalSocket::ConnectedState
            || !socket.waitForReadyRead(static_cast<int>(deadline.remainingTime()))) {
            // the server might have written the reply and closed the connection in one go
            if (socket.bytesAvailable() > 0) {
                continue;
            }
            return {};
        }
    }
}
}
