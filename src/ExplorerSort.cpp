/* Copyright 2026 the MithenPDF project authors. See AUTHORS file. */

// Queries Explorer for the order items are displayed in a folder (whatever
// sort mode and direction the user has set - Name, Date, Size, Type, ...).
// Based on the same approach in mithen-view (qvwin32functions.cpp).

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "base/Dict.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>

#include "ExplorerSort.h"

// path -> backslashes + uppercase (temporary, for comparison)
static TempStr NormPathLower(Str s) {
    TempStr r = str::DupTemp(s);
    char* p = r.s;
    for (int i = 0; i < len(r); i++) {
        char c = p[i];
        if (c == '/') {
            p[i] = '\\';
        } else {
            p[i] = (char)(c & ~0x20); // toupper without locale: clear 0x20 for alpha
        }
    }
    return r;
}

// Walk one Explorer window (index `windowIndex`) and, if it shows `normDir`,
// return its item order.  The caller's lambda wraps the COM lifetimes.
static bool ResolveWindow(IShellWindows* pShellWindows, long windowIndex, Str normDir, StrVec& out) {
    VARIANT vIndex;
    VariantInit(&vIndex);
    vIndex.vt = VT_I4;
    vIndex.lVal = windowIndex;

    IDispatch* pDispatch = nullptr;
    HRESULT hr = pShellWindows->Item(vIndex, &pDispatch);
    VariantClear(&vIndex);
    if (FAILED(hr) || !pDispatch) {
        return false;
    }

    IWebBrowser2* pBrowser = nullptr;
    hr = pDispatch->QueryInterface(IID_IWebBrowser2, (void**)&pBrowser);
    if (FAILED(hr) || !pBrowser) {
        pDispatch->Release();
        return false;
    }

    IDispatch* pDocDispatch = nullptr;
    hr = pBrowser->get_Document(&pDocDispatch);
    if (FAILED(hr) || !pDocDispatch) {
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    IServiceProvider* pSvc = nullptr;
    hr = pDocDispatch->QueryInterface(IID_IServiceProvider, (void**)&pSvc);
    if (FAILED(hr) || !pSvc) {
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    IShellBrowser* pShellBrowser = nullptr;
    hr = pSvc->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&pShellBrowser);
    pSvc->Release();
    if (FAILED(hr) || !pShellBrowser) {
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    IShellView* pShellView = nullptr;
    hr = pShellBrowser->QueryActiveShellView(&pShellView);
    if (FAILED(hr) || !pShellView) {
        pShellBrowser->Release();
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    IFolderView* pFolderView = nullptr;
    hr = pShellView->QueryInterface(IID_IFolderView, (void**)&pFolderView);
    pShellView->Release();
    if (FAILED(hr) || !pFolderView) {
        pShellBrowser->Release();
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    // Get the folder this view is showing
    IPersistFolder2* pPersist = nullptr;
    hr = pFolderView->GetFolder(IID_IPersistFolder2, (void**)&pPersist);
    if (FAILED(hr) || !pPersist) {
        pFolderView->Release();
        pShellBrowser->Release();
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    LPITEMIDLIST pidl = nullptr;
    hr = pPersist->GetCurFolder(&pidl);
    if (FAILED(hr) || !pidl) {
        pPersist->Release();
        pFolderView->Release();
        pShellBrowser->Release();
        pDocDispatch->Release();
        pBrowser->Release();
        pDispatch->Release();
        return false;
    }

    WCHAR folderBuf[MAX_PATH]{};
    bool match = SHGetPathFromIDListW(pidl, folderBuf) && str::EqI(NormPathLower(ToUtf8Temp(WStr(folderBuf))), normDir);

    int itemCount = 0;
    if (match) {
        hr = pFolderView->ItemCount(SVGIO_ALLVIEW, &itemCount);
        if (FAILED(hr)) {
            itemCount = 0;
        }
    }

    for (int idx = 0; match && idx < itemCount; idx++) {
        LPITEMIDLIST pidlItem = nullptr;
        if (FAILED(pFolderView->Item(idx, &pidlItem)) || !pidlItem) {
            continue;
        }
        LPITEMIDLIST pidlFull = ILCombine(pidl, pidlItem);
        const LPITEMIDLIST pidlResolve = pidlFull ? pidlFull : pidlItem;
        WCHAR filePath[MAX_PATH]{};
        if (SHGetPathFromIDListW(pidlResolve, filePath)) {
            out.Append(ToUtf8Temp(WStr(filePath)));
        }
        if (pidlFull) {
            CoTaskMemFree(pidlFull);
        }
        CoTaskMemFree(pidlItem);
    }

    CoTaskMemFree(pidl);
    pPersist->Release();
    pFolderView->Release();
    pShellBrowser->Release();
    pDocDispatch->Release();
    pBrowser->Release();
    pDispatch->Release();
    return match && itemCount > 0;
}

bool GetExplorerSortOrder(Str dir, StrVec& out) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool comOwn = SUCCEEDED(hr); // S_FALSE = already initialized; don't uninit then

    IShellWindows* pSW = nullptr;
    hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows, (void**)&pSW);
    if (FAILED(hr) || !pSW) {
        if (comOwn) {
            CoUninitialize();
        }
        return false;
    }

    long count = 0;
    pSW->get_Count(&count);

    Str normDir = NormPathLower(dir);

    // Phase 1: cheap pre-filter via IWebBrowser2::LocationURL
    long candidateIndex = -1;
    for (long i = 0; i < count && candidateIndex < 0; i++) {
        VARIANT vIndex;
        VariantInit(&vIndex);
        vIndex.vt = VT_I4;
        vIndex.lVal = i;

        IDispatch* pDisp = nullptr;
        hr = pSW->Item(vIndex, &pDisp);
        VariantClear(&vIndex);
        if (FAILED(hr) || !pDisp) {
            continue;
        }
        IWebBrowser2* pBrw = nullptr;
        hr = pDisp->QueryInterface(IID_IWebBrowser2, (void**)&pBrw);
        if (FAILED(hr) || !pBrw) {
            pDisp->Release();
            continue;
        }
        BSTR url = nullptr;
        if (SUCCEEDED(pBrw->get_LocationURL(&url)) && url) {
            WCHAR path[MAX_PATH]{};
            DWORD pathLen = dimof(path);
            if (PathCreateFromUrlW(url, path, &pathLen, 0) == S_OK) {
                WStr viewPath(path);
                if (str::EqI(NormPathLower(ToUtf8Temp(viewPath)), normDir)) {
                    candidateIndex = i;
                }
            }
            SysFreeString(url);
        }
        pBrw->Release();
        pDisp->Release();
    }

    // Phase 2: resolve the candidate window first; if it yields nothing
    // fall back to walking all windows
    if (candidateIndex >= 0) {
        ResolveWindow(pSW, candidateIndex, normDir, out);
    }
    if (len(out) == 0) {
        for (long i = 0; i < count; i++) {
            if (candidateIndex == i) {
                continue; // already tried and failed
            }
            if (ResolveWindow(pSW, i, normDir, out)) {
                break;
            }
        }
    }

    pSW->Release();
    if (comOwn) {
        CoUninitialize();
    }
    logf("ExplorerSort: %s -> %d items\n", dir, len(out));
    return len(out) > 0;
}

struct ExplorerOrder {
    dict::MapStrToInt ranks;
};

ExplorerOrder* GetExplorerOrder(Str dir) {
    StrVec paths;
    if (!GetExplorerSortOrder(dir, paths) || len(paths) == 0) {
        return nullptr;
    }
    auto* order = new ExplorerOrder;
    int n = len(paths);
    for (int i = 0; i < n; i++) {
        TempStr name = path::GetBaseNameTemp(paths[i]);
        if (len(name) > 0) {
            order->ranks.Insert(name, i);
        }
    }
    return order;
}

void FreeExplorerOrder(ExplorerOrder* order) {
    delete order;
}

int ExplorerOrderRank(const ExplorerOrder* order, Str name) {
    if (!order) {
        return -1;
    }
    int rank = 0;
    if (!order->ranks.Get(name, &rank)) {
        return -1;
    }
    return rank;
}

// the order the comparators below rank against; set only around a Sort() call
static thread_local const ExplorerOrder* gSortOrder = nullptr;

// Explorer's position first, natural order for the entries it doesn't show, so
// one call orders the whole list
static bool StrLessByExplorerRank(Str a, Str b) {
    int ra = ExplorerOrderRank(gSortOrder, path::GetBaseNameTemp(a));
    int rb = ExplorerOrderRank(gSortOrder, path::GetBaseNameTemp(b));
    if (ra < 0) {
        ra = INT_MAX;
    }
    if (rb < 0) {
        rb = INT_MAX;
    }
    if (ra != rb) {
        return ra < rb;
    }
    return str::CmpNatural(a, b) < 0;
}

void SortPathsByExplorerOrder(StrVec& paths) {
    int n = len(paths);
    if (n < 2) {
        return;
    }
    // a same-named file in another folder would match this folder's order
    TempStr dir = path::GetDirTemp(paths[0]);
    for (int i = 1; i < n; i++) {
        if (!path::IsSame(path::GetDirTemp(paths[i]), dir)) {
            return;
        }
    }
    ExplorerOrder* order = GetExplorerOrder(dir);
    if (!order) {
        return;
    }
    gSortOrder = order;
    Sort(&paths, StrLessByExplorerRank);
    gSortOrder = nullptr;
    FreeExplorerOrder(order);
}
