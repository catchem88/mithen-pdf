/* Copyright 2026 the MithenPDF project authors. See AUTHORS file. */

// OCR of the page shown in the canvas: Windows.Media.Ocr for the text and
// quirc for the QR codes. The result is a list of document-space boxes; the
// canvas darkens the page and draws a bright box over each of them, a click on
// a box copies its text and Ctrl+C copies all of them.

#include "base/Base.h"
#include "base/Win.h"
#include "base/UITask.h"

#include <gdiplus.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>

// RoGetActivationFactory and the other WinRT entry points live in WindowsApp.lib
#pragma comment(lib, "WindowsApp.lib")

extern "C" {
#include "quirc.h"
}

#include "gui/UIModels.h"
#include "gui/Gfx.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "Notifications.h"
#include "SumatraConfig.h"
#include "Translations.h"
#include "SumatraPDF.h"
#include "WindowTab.h"
#include "Canvas.h"

// notifications of the OCR hint; removed by group, so no pointer is kept
static constexpr const char* kOcrHintGroup = "ocrHint";

// image coordinates, as reported by the recognizer / quirc
struct OcrRawBox {
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;
    Str text = {}; // owned
};

OcrState::~OcrState() {
    for (OcrBox& b : boxes) {
        str::Free(b.text);
    }
}

static void ShowOcrHint(MainWindow* win) {
    RemoveNotificationsForGroup(win->hwndCanvas, kOcrHintGroup);
    NotificationCreateArgs args;
    args.hwndParent = win->hwndCanvas;
    args.groupId = kOcrHintGroup;
    args.corner = NotifCorner::BottomBar;
    args.timeoutMs = kNotifNoTimeout;
    args.plainText = true;
    args.msg = Tr("Click a line to copy it, or press Ctrl+C to copy all. Esc or right-click to close");
    ShowNotification(args);
}

static void ShowOcrToast(MainWindow* win, Str msg) {
    ShowPlainNotification(win->hwndCanvas, msg, kNotifDefaultTimeOut);
}

static void ShowOcrWarning(MainWindow* win, Str msg) {
    ShowPlainWarningNotification(win->hwndCanvas, msg, kNotif5SecsTimeOut);
}

//----------------------------------------------------------------------------
// page capture

// Copies `rc` of `hwnd` (client coordinates) as 32-bit BGRA into `bgra`,
// downscaled when that is the only way to stay within the recognizer's limit.
// Returns the image size and the image-pixels-per-client-pixel scale.
static bool CaptureToBgra(HWND hwnd, Rect rc, Vec<u8>& bgra, int* dxOut, int* dyOut, double* scaleOut) {
    if (rc.dx <= 0 || rc.dy <= 0) {
        return false;
    }
    int maxDim = 0;
    try {
        maxDim = (int)winrt::Windows::Media::Ocr::OcrEngine::MaxImageDimension();
    } catch (...) {
        maxDim = 0;
    }
    if (maxDim <= 0) {
        maxDim = 2600;
    }

    double scale = 1.0;
    int longest = std::max(rc.dx, rc.dy);
    if (longest > maxDim) {
        scale = (double)maxDim / (double)longest;
    }
    int dx = std::max(1, (int)(rc.dx * scale));
    int dy = std::max(1, (int)(rc.dy * scale));

    HDC src = GetDC(hwnd);
    if (!src) {
        return false;
    }
    HDC mem = CreateCompatibleDC(src);
    if (!mem) {
        ReleaseDC(hwnd, src);
        return false;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = dx;
    bi.bmiHeader.biHeight = -dy; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(src, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(mem);
        ReleaseDC(hwnd, src);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(mem, dib);
    SetStretchBltMode(mem, HALFTONE);
    SetBrushOrgEx(mem, 0, 0, nullptr);
    BOOL ok = StretchBlt(mem, 0, 0, dx, dy, src, rc.x, rc.y, rc.dx, rc.dy, SRCCOPY);
    GdiFlush();

    bool captured = false;
    if (ok) {
        VecResize(bgra, dx * dy * 4);
        memcpy(bgra.els, bits, (size_t)dx * dy * 4);
        // the recognizer wants opaque BGRA; a window capture has alpha 0
        for (int i = 3; i < dx * dy * 4; i += 4) {
            bgra.els[i] = 255;
        }
        *dxOut = dx;
        *dyOut = dy;
        *scaleOut = (double)dx / (double)rc.dx;
        captured = true;
    }

    SelectObject(mem, oldBmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(hwnd, src);
    return captured;
}

//----------------------------------------------------------------------------
// recognition

static winrt::Windows::Media::Ocr::OcrEngine CreateOcrEngine() {
    using winrt::Windows::Media::Ocr::OcrEngine;
    // prefer a language from the user profile, then any installed OCR language
    try {
        OcrEngine profile = OcrEngine::TryCreateFromUserProfileLanguages();
        if (profile != nullptr) {
            return profile;
        }
        for (const winrt::Windows::Globalization::Language& lang : OcrEngine::AvailableRecognizerLanguages()) {
            OcrEngine e = OcrEngine::TryCreateFromLanguage(lang);
            if (e != nullptr) {
                return e;
            }
        }
    } catch (...) {
    }
    return nullptr;
}

// Recognizes the text lines in `bgra` and appends them to `boxes`.
// Returns false only when there is no OCR language at all.
static bool RecognizeText(const u8* bgra, int dx, int dy, Vec<OcrRawBox>& boxes) {
    winrt::Windows::Media::Ocr::OcrEngine engine = CreateOcrEngine();
    if (engine == nullptr) {
        return false;
    }

    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Security::Cryptography;
    using namespace winrt::Windows::Storage::Streams;

    IBuffer buf =
        CryptographicBuffer::CreateFromByteArray(winrt::array_view<const uint8_t>(bgra, bgra + (size_t)dx * dy * 4));
    SoftwareBitmap bitmap =
        SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, dx, dy, BitmapAlphaMode::Premultiplied);

    winrt::Windows::Media::Ocr::OcrResult res = engine.RecognizeAsync(bitmap).get();
    for (const winrt::Windows::Media::Ocr::OcrLine& line : res.Lines()) {
        Str text = ToUtf8Temp(WStr((WCHAR*)line.Text().c_str(), (int)line.Text().size()));
        str::TrimWsBoth(text);
        if (len(text) == 0) {
            continue;
        }

        // WinRT reports word boxes only; a line's box is the union of its words
        Rect box;
        bool first = true;
        for (const winrt::Windows::Media::Ocr::OcrWord& word : line.Words()) {
            winrt::Windows::Foundation::Rect wr = word.BoundingRect();
            Rect r((int)wr.X, (int)wr.Y, (int)wr.Width, (int)wr.Height);
            box = first ? r : box.Union(r);
            first = false;
        }
        if (first) {
            continue;
        }
        VecAppend(boxes, OcrRawBox{box.x, box.y, box.dx, box.dy, str::Dup(text)});
    }
    return true;
}

// Decodes QR codes in `bgra` with quirc and appends them to `boxes`.
static void DecodeQrCodes(const u8* bgra, int dx, int dy, Vec<OcrRawBox>& boxes) {
    Vec<u8> gray;
    VecResize(gray, dx * dy);
    for (int i = 0; i < dx * dy; i++) {
        const u8* p = bgra + (size_t)i * 4;
        // BGRA -> luma
        gray.els[i] = (u8)((p[2] * 299 + p[1] * 587 + p[0] * 114) / 1000);
    }

    struct quirc* qr = quirc_new();
    if (!qr) {
        return;
    }
    if (quirc_resize(qr, dx, dy) < 0) {
        quirc_destroy(qr);
        return;
    }
    int w = 0;
    int h = 0;
    u8* buf = quirc_begin(qr, &w, &h);
    if (buf) {
        for (int y = 0; y < h; y++) {
            memcpy(buf + (size_t)y * w, gray.els + (size_t)y * dx, (size_t)w);
        }
    }
    quirc_end(qr);

    int count = quirc_count(qr);
    for (int i = 0; i < count; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(qr, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) {
            continue;
        }
        if (data.payload_len <= 0) {
            continue;
        }
        int left = code.corners[0].x;
        int top = code.corners[0].y;
        int right = left;
        int bottom = top;
        for (int c = 1; c < 4; c++) {
            left = std::min(left, (int)code.corners[c].x);
            top = std::min(top, (int)code.corners[c].y);
            right = std::max(right, (int)code.corners[c].x);
            bottom = std::max(bottom, (int)code.corners[c].y);
        }
        if (right <= left || bottom <= top) {
            continue;
        }
        Str text = str::Dup(Str((char*)data.payload, data.payload_len));
        VecAppend(boxes, OcrRawBox{left, top, right - left, bottom - top, text});
    }
    quirc_destroy(qr);
}

//----------------------------------------------------------------------------
// background job

struct OcrRun {
    ~OcrRun() {
        for (OcrRawBox& b : boxes) {
            str::Free(b.text);
        }
    }

    MainWindow* win = nullptr;
    int epoch = 0;
    int pageNo = 0;
    Rect captureRc;     // client coordinates that were captured
    double scale = 1.0; // image pixels per client pixel
    Vec<u8> bgra;
    int dx = 0;
    int dy = 0;
    Vec<OcrRawBox> boxes;
    bool hasLanguage = true;
};

static void FinishOcrRun(OcrRun* d) {
    AutoDelete delRun(d);
    MainWindow* win = d->win;
    if (!IsMainWindowValidAndNotClosing(win)) {
        return;
    }
    OcrState& st = win->ocr;
    SafeCloseThreadHandle(&st.thread);
    if (d->epoch != st.epoch) {
        return; // cancelled (closed, or a newer run started)
    }
    st.running = false;
    KillTimer(win->hwndCanvas, kOcrSpinTimerID);

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        ScheduleRepaint(win, 0);
        return;
    }
    for (OcrRawBox& b : d->boxes) {
        Rect screen((int)(d->captureRc.x + b.x / d->scale), (int)(d->captureRc.y + b.y / d->scale),
                    (int)(b.dx / d->scale), (int)(b.dy / d->scale));
        OcrBox box;
        box.pageNo = d->pageNo;
        box.rect = dm->CvtFromScreen(screen, d->pageNo);
        box.text = b.text;
        b.text = {};
        if (box.rect.IsEmpty()) {
            str::Free(box.text);
            continue;
        }
        VecAppend(st.boxes, box);
    }

    if (len(st.boxes) == 0) {
        st.shown = false;
        st.tab = nullptr;
        logf("Ocr: no boxes, language=%d\n", (int)d->hasLanguage);
        if (!d->hasLanguage) {
            ShowOcrWarning(win, Tr("No OCR language is available. Add a language with OCR support in the Windows "
                                   "language settings."));
        } else {
            ShowOcrToast(win, Tr("OCR cannot find any text on this page"));
        }
    } else {
        st.shown = true;
        st.hover = -1;
        logf("Ocr: showing %d boxes\n", len(st.boxes));
        ShowOcrHint(win);
    }
    ScheduleRepaint(win, 0);
}

static void OcrThread(OcrRun* d) {
    try {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
            // the thread already has an apartment, which is fine
        }
        d->hasLanguage = RecognizeText(d->bgra.els, d->dx, d->dy, d->boxes);
        DecodeQrCodes(d->bgra.els, d->dx, d->dy, d->boxes);
    } catch (...) {
        VecReset(d->boxes);
    }
    logf("Ocr: recognized %d boxes (language=%d)\n", len(d->boxes), (int)d->hasLanguage);
    auto fn = MkFunc0<OcrRun>(FinishOcrRun, d);
    uitask::Post(fn, "FinishOcrRun");
}

//----------------------------------------------------------------------------
// public API

void OcrClose(MainWindow* win) {
    if (!win) {
        return;
    }
    OcrState& st = win->ocr;
    if (!st.running && !st.shown) {
        return;
    }
    st.epoch++;
    st.running = false;
    st.shown = false;
    st.hover = -1;
    st.tab = nullptr;
    for (OcrBox& b : st.boxes) {
        str::Free(b.text);
    }
    VecReset(st.boxes);
    RemoveNotificationsForGroup(win->hwndCanvas, kOcrHintGroup);
    KillTimer(win->hwndCanvas, kOcrSpinTimerID);
    logf("Ocr: closed\n");
    SetCanvasCursor(win, IDC_ARROW);
    ScheduleRepaint(win, 0);
}

bool OcrIsRunning(MainWindow* win) {
    return win && win->ocr.running;
}

bool OcrIsShown(MainWindow* win) {
    return win && win->ocr.shown;
}

void OcrStart(MainWindow* win) {
    if (!win || gPluginMode) {
        return;
    }
    OcrState& st = win->ocr;
    if (st.running || st.shown) {
        return; // Esc / right-click close the results
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm || !win->IsDocLoaded()) {
        return;
    }

    // the page in the middle of the canvas, else the current page
    Rect canvasRc = HwndClientRect(win->hwndCanvas);
    int pageNo = dm->GetPageNoByPoint(Point(canvasRc.dx / 2, canvasRc.dy / 2));
    if (pageNo < 0 || !dm->PageVisible(pageNo)) {
        pageNo = dm->CurrentPageNo();
    }
    if (pageNo < 0 || !dm->PageVisible(pageNo)) {
        ShowOcrWarning(win, Tr("OCR needs a page on screen"));
        return;
    }
    PageInfo* pi = dm->GetPageInfo(pageNo);
    if (!pi) {
        return;
    }

    Rect pageRc = pi->pageOnScreen;
    HwndInvalidate(win->hwndCanvas);
    UpdateWindow(win->hwndCanvas);

    auto* d = new OcrRun;
    d->win = win;
    d->epoch = ++st.epoch;
    d->pageNo = pageNo;
    d->captureRc = pageRc;
    if (!CaptureToBgra(win->hwndCanvas, pageRc, d->bgra, &d->dx, &d->dy, &d->scale)) {
        delete d;
        logf("Ocr: capture failed for page %d\n", pageNo);
        ShowOcrWarning(win, Tr("Could not capture the page"));
        return;
    }
    logf("Ocr: page %d capture %dx%d scale=%.2f\n", pageNo, d->dx, d->dy, d->scale);

    st.running = true;
    st.shown = false;
    st.hover = -1;
    st.spinPhase = 0;
    st.tab = win->CurrentTab();

    NotificationCreateArgs args;
    args.hwndParent = win->hwndCanvas;
    args.groupId = kOcrHintGroup;
    args.corner = NotifCorner::BottomBar;
    args.timeoutMs = kNotifNoTimeout;
    args.plainText = true;
    args.msg = Tr("Recognizing text...");
    ShowNotification(args);

    SetTimer(win->hwndCanvas, kOcrSpinTimerID, 60, nullptr);
    auto fn = MkFunc0<OcrRun>(OcrThread, d);
    st.thread = StartThread(fn, StrL("OcrThread"));
    ScheduleRepaint(win, 0);
}

int OcrUpdateHover(MainWindow* win, Point pt) {
    if (!win || !win->ocr.shown) {
        return -1;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return -1;
    }
    int found = -1;
    int foundArea = 0;
    for (int i = 0; i < len(win->ocr.boxes); i++) {
        OcrBox& b = win->ocr.boxes[i];
        Rect rc = dm->CvtToScreen(b.pageNo, b.rect);
        if (!rc.Contains(pt)) {
            continue;
        }
        int area = rc.dx * rc.dy;
        if (found < 0 || area < foundArea) {
            found = i;
            foundArea = area;
        }
    }
    if (found != win->ocr.hover) {
        win->ocr.hover = found;
        ScheduleRepaint(win, 0);
    }
    return found;
}

bool OcrClickCopy(MainWindow* win, Point pt) {
    if (!win || !win->ocr.shown) {
        return false;
    }
    int idx = OcrUpdateHover(win, pt);
    if (idx < 0) {
        return false;
    }
    CopyTextToClipboard(win->ocr.boxes[idx].text);
    ShowOcrToast(win, Tr("Text copied"));
    return true;
}

bool OcrCopyAll(MainWindow* win) {
    if (!win || !win->ocr.shown || len(win->ocr.boxes) == 0) {
        return false;
    }
    StrVec lines;
    for (OcrBox& b : win->ocr.boxes) {
        lines.Append(b.text);
    }
    TempStr all = JoinTemp(&lines, StrL("\n"));
    CopyTextToClipboard(all);
    ShowOcrToast(win, Tr("Copied all text"));
    return true;
}

void OcrTickSpin(MainWindow* win) {
    if (!win || !win->ocr.running) {
        KillTimer(win->hwndCanvas, kOcrSpinTimerID);
        return;
    }
    win->ocr.spinPhase = (win->ocr.spinPhase + 1) % 15;
    ScheduleRepaint(win, 0);
}

void OcrPaint(MainWindow* win, Gfx* gfx, HDC hdc) {
    OcrState& st = win->ocr;
    if (!st.running && !st.shown) {
        return;
    }
    if (st.shown && st.tab != win->CurrentTab()) {
        // the tab changed under us; drop the overlay on the next loop
        auto fn = MkFunc0<MainWindow>(OcrClose, win);
        uitask::Post(fn, "OcrCloseOnTabChange");
        return;
    }

    Rect canvasRc = HwndClientRect(win->hwndCanvas);
    if (canvasRc.IsEmpty()) {
        return;
    }
    gfx->FillRects(&canvasRc, 1, kColBlack, st.shown ? 160 : 128);

    DisplayModel* dm = win->AsFixed();
    if (st.shown && dm) {
        for (int i = 0; i < len(st.boxes); i++) {
            Rect rc = dm->CvtToScreen(st.boxes[i].pageNo, st.boxes[i].rect);
            if (rc.IsEmpty()) {
                continue;
            }
            gfx->FillRects(&rc, 1, kColWhite, i == st.hover ? 120 : 50, 1);
        }
    }

    if (st.running) {
        // a rotating arc in the middle of the canvas
        int size = 44;
        int x = canvasRc.x + (canvasRc.dx - size) / 2;
        int y = canvasRc.y + (canvasRc.dy - size) / 2;
        Gdiplus::Graphics gs(hdc);
        gs.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen back(Gdiplus::Color(120, 255, 255, 255), 4.0f);
        gs.DrawEllipse(&back, x, y, size, size);
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), 4.0f);
        gs.DrawArc(&pen, x, y, size, size, (Gdiplus::REAL)(st.spinPhase * 24), 90.0f);
    }
}
