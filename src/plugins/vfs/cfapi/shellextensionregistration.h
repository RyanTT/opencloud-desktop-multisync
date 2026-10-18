/*
 * SPDX-FileCopyrightText: 2026 OpenCloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <QString>

namespace OCC::CfApiShellExtension {

/**
 * Registers the thumbnail handler COM server for the current user (HKCU, unpackaged builds only):
 * CLSID with InprocServer32 pointing to the DLL next to the executable, and an AppID with an empty
 * DllSurrogate, so Explorer hosts the DLL in dllhost.exe instead of loading it into its own process.
 * Returns false if the DLL is missing or the registry could not be written.
 */
bool registerThumbnailHandler();

/** Removes the COM registration again. */
void unregisterThumbnailHandler();

/**
 * Points the sync root at the thumbnail handler, a "ThumbnailProvider" value under
 * HKLM\...\Explorer\SyncRootManager\<syncRootId>, the key StorageProviderSyncRootManager::Register() created.
 */
bool attachThumbnailHandler(const QString &syncRootId);
void detachThumbnailHandler(const QString &syncRootId);

/** Whether any sync root id starting with syncRootIdPrefix is still registered. */
bool hasSyncRootsWithPrefix(const QString &syncRootIdPrefix);
}
