/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cfapishellintegrationclassfactory.h"
#include "thumbnailprovider.h"
#include <comdef.h>
#include <new>

long dllReferenceCount = 0;
long dllObjectsCount = 0;

HINSTANCE instanceHandle = nullptr;

HRESULT ThumbnailProvider_CreateInstance(REFIID riid, void **ppv);

// the custom state (overlay) provider is not built, only thumbnails are served
const VfsShellExtensions::ClassObjectInit listClassesSupported[] = {
    {&__uuidof(VfsShellExtensions::ThumbnailProvider), ThumbnailProvider_CreateInstance}};

STDAPI_(BOOL) DllMain(HINSTANCE hInstance, DWORD dwReason, void *)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        instanceHandle = hInstance;
        DisableThreadLibraryCalls(hInstance);
    }

    return TRUE;
}

STDAPI DllCanUnloadNow()
{
    return (dllReferenceCount == 0 && dllObjectsCount == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **ppv)
{
    return VfsShellExtensions::CfApiShellIntegrationClassFactory::CreateInstance(clsid, listClassesSupported, ARRAYSIZE(listClassesSupported), riid, ppv);
}

HRESULT ThumbnailProvider_CreateInstance(REFIID riid, void **ppv)
{
    auto *thumbnailProvider = new (std::nothrow) VfsShellExtensions::ThumbnailProvider();
    if (!thumbnailProvider) {
        return E_OUTOFMEMORY;
    }
    const auto hresult = thumbnailProvider->QueryInterface(riid, ppv);
    thumbnailProvider->Release();
    return hresult;
}
