/*
 * SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

// GDI+ needs IStream/PROPID (objidl.h, left out with WIN32_LEAN_AND_MEAN) and min/max (disabled by NOMINMAX)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objidl.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>

#include "thumbnailprovider.h"
#include "thumbnailprovideripc.h"

#include <shlwapi.h>

extern long dllObjectsCount;

namespace VfsShellExtensions {

std::pair<HBITMAP, WTS_ALPHATYPE> hBitmapAndAlphaTypeFromData(const QByteArray &thumbnailData)
{
    if (thumbnailData.isEmpty()) {
        return {nullptr, WTSAT_UNKNOWN};
    }

    ULONG_PTR gdiPlusToken;
    Gdiplus::GdiplusStartupInput gdiPlusStartupInput;
    if (Gdiplus::GdiplusStartup(&gdiPlusToken, &gdiPlusStartupInput, nullptr) != Gdiplus::Status::Ok) {
        return {nullptr, WTSAT_UNKNOWN};
    }

    HBITMAP hBitmap = nullptr;
    auto hasAlpha = false;
    if (IStream *stream = ::SHCreateMemStream(reinterpret_cast<const BYTE *>(thumbnailData.constData()), static_cast<UINT>(thumbnailData.size()))) {
        // the bitmap reads from the stream, so it must be gone before the stream is released and GDI+ is shut down
        if (auto *gdiPlusBitmap = Gdiplus::Bitmap::FromStream(stream)) {
            if (gdiPlusBitmap->GetLastStatus() == Gdiplus::Status::Ok) {
                hasAlpha = Gdiplus::IsAlphaPixelFormat(gdiPlusBitmap->GetPixelFormat());
                if (gdiPlusBitmap->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBitmap) != Gdiplus::Status::Ok) {
                    hBitmap = nullptr;
                }
            }
            delete gdiPlusBitmap;
        }
        stream->Release();
    }

    Gdiplus::GdiplusShutdown(gdiPlusToken);

    if (!hBitmap) {
        return {nullptr, WTSAT_UNKNOWN};
    }
    return {hBitmap, hasAlpha ? WTSAT_ARGB : WTSAT_RGB};
}

ThumbnailProvider::ThumbnailProvider()
    : _referenceCount(1)
{
    InterlockedIncrement(&dllObjectsCount);
}

ThumbnailProvider::~ThumbnailProvider()
{
    InterlockedDecrement(&dllObjectsCount);
}
IFACEMETHODIMP ThumbnailProvider::QueryInterface(REFIID riid, void **ppv)
{
    static const QITAB qit[] = {
        QITABENT(ThumbnailProvider, IInitializeWithItem),
        QITABENT(ThumbnailProvider, IThumbnailProvider),
        {0},
    };
    return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::AddRef()
{
    return InterlockedIncrement(&_referenceCount);
}

IFACEMETHODIMP_(ULONG) ThumbnailProvider::Release()
{
    const auto refCount = InterlockedDecrement(&_referenceCount);
    if (refCount == 0) {
        delete this;
    }
    return refCount;
}

IFACEMETHODIMP ThumbnailProvider::Initialize(_In_ IShellItem *item, _In_ DWORD mode)
{
    Q_UNUSED(mode)
    HRESULT hresult = item->QueryInterface(__uuidof(_shellItem), reinterpret_cast<void **>(&_shellItem));
    if (FAILED(hresult)) {
        return hresult;
    }

    LPWSTR pszName = nullptr;
    hresult = _shellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszName);
    if (FAILED(hresult)) {
        return hresult;
    }

    _shellItemPath = QString::fromWCharArray(pszName);
    CoTaskMemFree(pszName);

    return S_OK;
}

IFACEMETHODIMP ThumbnailProvider::GetThumbnail(_In_ UINT cx, _Out_ HBITMAP *bitmap, _Out_ WTS_ALPHATYPE *alphaType)
{
    *bitmap = nullptr;
    *alphaType = WTSAT_UNKNOWN;

    // any failure makes Explorer fall back to the regular icon
    const auto thumbnailDataReceived = ThumbnailProviderIpc::fetchThumbnailForFile(_shellItemPath, static_cast<int>(cx));
    if (thumbnailDataReceived.isEmpty()) {
        return E_FAIL;
    }

    const auto bitmapAndAlphaType = hBitmapAndAlphaTypeFromData(thumbnailDataReceived);
    if (!bitmapAndAlphaType.first) {
        return E_FAIL;
    }
    *bitmap = bitmapAndAlphaType.first;
    *alphaType = bitmapAndAlphaType.second;

    return S_OK;
}
}
