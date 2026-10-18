/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QString>

namespace VfsShellExtensions {
/**
 * Asks the running client for a thumbnail.
 * Everything is blocking with a short deadline, Explorer must never wait long on us.
 */
class ThumbnailProviderIpc
{
public:
    // returns PNG data or an empty array if the client is not running or has no thumbnail
    static QByteArray fetchThumbnailForFile(const QString &filePath, int size);

    // for tests
    static QString overrideServerName;
};
}
