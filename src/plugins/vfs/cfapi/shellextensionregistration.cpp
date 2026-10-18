/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "shellextensionregistration.h"

#include "configvfscfapishellext.h"
#include "common/utility_win.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <windows.h>

using namespace Qt::Literals::StringLiterals;

Q_LOGGING_CATEGORY(lcCfApiShellExt, "sync.vfs.cfapi.shellext", QtInfoMsg)

namespace {
const auto classesKey = uR"(Software\Classes\)"_s;
const auto syncRootManagerKey = uR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager\)"_s;

const QString thumbnailHandlerClsid = QStringLiteral(CFAPI_SHELLEXT_THUMBNAIL_HANDLER_CLASS_ID_REG);
const QString appId = QStringLiteral(CFAPI_SHELLEXT_APPID_REG);

const wchar_t *w(const QString &s)
{
    return reinterpret_cast<const wchar_t *>(s.utf16());
}

// valueName empty: the default value of the key
bool setString(HKEY root, const QString &subKey, const QString &valueName, const QString &value)
{
    const auto result = RegSetKeyValueW(root, w(subKey), valueName.isEmpty() ? nullptr : w(valueName), REG_SZ, w(value),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (result != ERROR_SUCCESS) {
        qCWarning(lcCfApiShellExt) << u"Failed to write registry value" << subKey << valueName << OCC::Utility::formatWinError(result);
        return false;
    }
    return true;
}

void deleteTree(HKEY root, const QString &subKey)
{
    const auto result = RegDeleteTreeW(root, w(subKey));
    if (result == ERROR_SUCCESS) {
        // RegDeleteTreeW only deletes the content, remove the key itself
        RegDeleteKeyW(root, w(subKey));
    } else if (result != ERROR_FILE_NOT_FOUND) {
        qCWarning(lcCfApiShellExt) << u"Failed to delete registry key" << subKey << OCC::Utility::formatWinError(result);
    }
}

QString dllPath()
{
    return QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + u"/"_s + QStringLiteral(CFAPI_SHELL_EXTENSIONS_LIB_NAME) + u".dll"_s);
}
}

namespace OCC::CfApiShellExtension {

bool registerThumbnailHandler()
{
    const auto path = dllPath();
    if (!QFileInfo::exists(path)) {
        qCWarning(lcCfApiShellExt) << u"Not registering the thumbnail handler, the shell extension is missing:" << path;
        return false;
    }

    const QString appIdKey = classesKey + u"AppID\\"_s + appId;
    const QString clsidKey = classesKey + u"CLSID\\"_s + thumbnailHandlerClsid;
    // an empty DllSurrogate makes COM host the in process server in the default surrogate (dllhost.exe)
    const bool ok = setString(HKEY_CURRENT_USER, appIdKey, {}, QStringLiteral(CFAPI_SHELLEXT_APPID_DISPLAY_NAME))
        && setString(HKEY_CURRENT_USER, appIdKey, u"DllSurrogate"_s, {})
        && setString(HKEY_CURRENT_USER, clsidKey, {}, QStringLiteral(CFAPI_SHELLEXT_THUMBNAIL_HANDLER_DISPLAY_NAME))
        && setString(HKEY_CURRENT_USER, clsidKey, u"AppID"_s, appId)
        && setString(HKEY_CURRENT_USER, clsidKey + u"\\InprocServer32"_s, {}, path)
        && setString(HKEY_CURRENT_USER, clsidKey + u"\\InprocServer32"_s, u"ThreadingModel"_s, u"Apartment"_s);
    if (ok) {
        qCInfo(lcCfApiShellExt) << u"Registered thumbnail handler" << thumbnailHandlerClsid << path;
    }
    return ok;
}

void unregisterThumbnailHandler()
{
    deleteTree(HKEY_CURRENT_USER, classesKey + u"CLSID\\"_s + thumbnailHandlerClsid);
    deleteTree(HKEY_CURRENT_USER, classesKey + u"AppID\\"_s + appId);
    qCInfo(lcCfApiShellExt) << u"Unregistered thumbnail handler" << thumbnailHandlerClsid;
}

bool attachThumbnailHandler(const QString &syncRootId)
{
    if (syncRootId.isEmpty()) {
        return false;
    }
    // The key belongs to the current user's sync root, so this works without elevation. If it doesn't,
    // syncing is unaffected, Explorer just shows icons instead of previews for dehydrated files.
    return setString(HKEY_LOCAL_MACHINE, syncRootManagerKey + syncRootId, u"ThumbnailProvider"_s, thumbnailHandlerClsid);
}

void detachThumbnailHandler(const QString &syncRootId)
{
    if (syncRootId.isEmpty()) {
        return;
    }
    const auto result = RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, w(syncRootManagerKey + syncRootId), L"ThumbnailProvider");
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
        qCWarning(lcCfApiShellExt) << u"Failed to remove the thumbnail handler from" << syncRootId << OCC::Utility::formatWinError(result);
    }
}

bool hasSyncRootsWithPrefix(const QString &syncRootIdPrefix)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, w(syncRootManagerKey), 0, KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS) {
        return false;
    }
    bool found = false;
    wchar_t name[MAX_PATH];
    for (DWORD i = 0; !found; ++i) {
        DWORD nameSize = MAX_PATH;
        if (RegEnumKeyExW(key, i, name, &nameSize, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        found = QString::fromWCharArray(name, static_cast<qsizetype>(nameSize)).startsWith(syncRootIdPrefix, Qt::CaseInsensitive);
    }
    RegCloseKey(key);
    return found;
}
}
