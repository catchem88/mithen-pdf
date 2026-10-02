/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "Annotation.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Toolbar.h"
#include "Selection.h"
#include "Theme.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "Notifications.h"
#include "DarkMode.h"
#include "SumatraDialogs.h"

#include <gdiplus.h>

// ---- signature input box: freehand drawing, or an uploaded image preview ----

struct SigBox {
    HWND hwnd = nullptr;
    bool uploadMode = false;
    // ink is drawn into this memory bitmap so painting is a plain BitBlt
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ bmpOld = nullptr;
    int dx = 0;
    int dy = 0;
    bool drawing = false;
    bool hasInk = false;
    // last point of the stroke being drawn (reset on mouse-down so a new stroke
    // doesn't connect to the previous one)
    POINT lastPt{};
    // the uploaded image (previewed into memDC)
    Gdiplus::Image* image = nullptr;
    Str imagePath;
};

static SigBox* gSigBox = nullptr;

static void SigBoxResetSurface(SigBox* b) {
    if (!b->memDC) {
        return;
    }
    RECT rc = {0, 0, b->dx, b->dy};
    FillRect(b->memDC, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
}

// draw the uploaded image into the memory bitmap, fit and centered
static void SigBoxRenderImage(SigBox* b) {
    if (!b->image || !b->memDC || b->dx <= 0 || b->dy <= 0) {
        return;
    }
    float iw = (float)b->image->GetWidth();
    float ih = (float)b->image->GetHeight();
    if (iw <= 0 || ih <= 0) {
        return;
    }
    float scale = std::min((float)b->dx / iw, (float)b->dy / ih);
    float w = iw * scale;
    float h = ih * scale;
    Gdiplus::Graphics g(b->memDC);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.DrawImage(b->image, ((float)b->dx - w) / 2.f, ((float)b->dy - h) / 2.f, w, h);
}

static void SigBoxDropImage(SigBox* b) {
    delete b->image;
    b->image = nullptr;
    str::Free(b->imagePath);
    b->imagePath = {};
}

static void SigBoxPickImage(SigBox* b) {
    WCHAR file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = b->hwnd;
    ofn.lpstrFilter =
        L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.gif\0All files "
        L"(*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = dimof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    Gdiplus::Image* img = Gdiplus::Image::FromFile(file);
    if (!img || img->GetLastStatus() != Gdiplus::Ok) {
        delete img;
        return;
    }
    SigBoxDropImage(b);
    b->image = img;
    b->imagePath = str::Dup(ToUtf8Temp(WStr(file)));
    SigBoxResetSurface(b);
    SigBoxRenderImage(b);
    InvalidateRect(b->hwnd, nullptr, FALSE);
}

static void SigBoxCreateSurface(SigBox* b, int dx, int dy) {
    if (b->memDC) {
        SelectObject(b->memDC, b->bmpOld);
        DeleteDC(b->memDC);
        b->memDC = nullptr;
    }
    if (b->bmp) {
        DeleteObject(b->bmp);
        b->bmp = nullptr;
    }
    b->dx = dx;
    b->dy = dy;
    if (dx <= 0 || dy <= 0) {
        return;
    }
    HDC hdc = GetDC(b->hwnd);
    b->memDC = CreateCompatibleDC(hdc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = dx;
    bmi.bmiHeader.biHeight = -dy; // top-down
    bmi.bmiHeader.biPlanes = 1;
    // 24bpp so the stride matches what the PNG encoder (PixelFormat24bppRGB)
    // expects; a 32bpp DIB wrapped as 24bpp skews the rows
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    b->bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    b->bmpOld = SelectObject(b->memDC, b->bmp);
    ReleaseDC(b->hwnd, hdc);
    SigBoxResetSurface(b);
    SigBoxRenderImage(b);
}

static void SigBoxPaint(SigBox* b) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(b->hwnd, &ps);
    if (b->memDC) {
        BitBlt(hdc, 0, 0, b->dx, b->dy, b->memDC, 0, 0, SRCCOPY);
    }
    RECT rc = {0, 0, b->dx, b->dy};
    FrameRect(hdc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));
    if (b->uploadMode && !b->image) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0x70, 0x70, 0x70));
        WCHAR* hint = CWStrTemp(Tr("Click to select an image"));
        RECT r = rc;
        DrawTextW(hdc, hint, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(b->hwnd, &ps);
}

static void SigBoxStrokeTo(SigBox* b, POINT pt, POINT prev) {
    if (!b->memDC) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, DpiScale(2), RGB(0, 0, 0));
    HGDIOBJ oldPen = SelectObject(b->memDC, pen);
    MoveToEx(b->memDC, prev.x, prev.y, nullptr);
    LineTo(b->memDC, pt.x, pt.y);
    SelectObject(b->memDC, oldPen);
    DeleteObject(pen);
}

static LRESULT CALLBACK WndProcSigBox(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SigBox* b = (SigBox*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_SIZE:
            if (b) {
                SigBoxCreateSurface(b, LOWORD(lp), HIWORD(lp));
            }
            return 0;
        case WM_PAINT:
            if (b) {
                SigBoxPaint(b);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            if (b && b->uploadMode) {
                SigBoxPickImage(b);
            } else if (b && !b->drawing) {
                SetCapture(hwnd);
                b->drawing = true;
                b->hasInk = true;
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                b->lastPt = pt;
                SigBoxStrokeTo(b, pt, pt);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (!b || !b->drawing) {
                return 0;
            }
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SigBoxStrokeTo(b, pt, b->lastPt);
            b->lastPt = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP:
            if (b && b->drawing) {
                b->drawing = false;
                ReleaseCapture();
            }
            return 0;
        case WM_SETCURSOR:
            if (b && b->uploadMode) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static SigBox* SigBoxCreate(HWND parent) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcSigBox;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MithenSigBox";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    auto* b = new SigBox();
    HWND hwnd = CreateWindowExW(0, L"MithenSigBox", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, nullptr,
                                GetModuleHandleW(nullptr), b);
    if (!hwnd) {
        delete b;
        return nullptr;
    }
    b->hwnd = hwnd;
    gSigBox = b;
    return b;
}

static void SigBoxSetMode(SigBox* b, bool upload) {
    if (!b || b->uploadMode == upload) {
        return;
    }
    b->uploadMode = upload;
    if (upload) {
        // leaving draw mode: drop the ink
        SigBoxResetSurface(b);
        b->hasInk = false;
    } else {
        // leaving upload mode: drop the image
        SigBoxDropImage(b);
        SigBoxResetSurface(b);
    }
    InvalidateRect(b->hwnd, nullptr, FALSE);
}

static void SigBoxClear(SigBox* b) {
    if (!b) {
        return;
    }
    SigBoxDropImage(b);
    SigBoxResetSurface(b);
    b->hasInk = false;
    InvalidateRect(b->hwnd, nullptr, FALSE);
}

static bool SigBoxHasContent(SigBox* b) {
    if (!b) {
        return false;
    }
    return b->uploadMode ? (b->image != nullptr) : b->hasInk;
}

// PNG of what should be signed: the uploaded file, or the ink rendered to a
// temp PNG. Empty when there is nothing yet.
static TempStr SigBoxImagePathTemp(SigBox* b) {
    if (!b) {
        return {};
    }
    if (b->uploadMode) {
        return b->imagePath ? str::DupTemp(b->imagePath) : TempStr{};
    }
    if (!b->hasInk || !b->memDC || b->dx <= 0 || b->dy <= 0) {
        return {};
    }
    BITMAP bmi{};
    GetObjectW(b->bmp, sizeof(bmi), &bmi);
    // opaque 24bpp: the DIB's alpha bytes are 0 and a transparent PNG would
    // render invisible
    Gdiplus::Bitmap gbmp(b->dx, b->dy, bmi.bmWidthBytes, PixelFormat24bppRGB, (BYTE*)bmi.bmBits);
    TempStr filePath = GetTempFilePathTemp(StrL("MithenSig"));
    WCHAR fileW[MAX_PATH]{};
    WStr fw = ToWStrTemp(filePath);
    if (len(fw) < dimof(fileW)) {
        wcsncpy_s(fileW, dimof(fileW), fw.s, len(fw));
    }
    UINT n = 0;
    UINT sz = 0;
    Gdiplus::GetImageEncodersSize(&n, &sz);
    auto* encoders = (Gdiplus::ImageCodecInfo*)malloc(sz);
    bool ok = false;
    if (encoders) {
        Gdiplus::GetImageEncoders(n, sz, encoders);
        for (UINT i = 0; i < n; i++) {
            if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                ok = (gbmp.Save(fileW, &encoders[i].Clsid, nullptr) == Gdiplus::Ok);
                break;
            }
        }
        free(encoders);
    }
    if (!ok) {
        return {};
    }
    return filePath;
}

static Size SigBoxIdealSize() {
    return Size{DpiScale(380), DpiScale(130)};
}

static void SigBoxDestroy(SigBox* b) {
    if (!b) {
        return;
    }
    SigBoxDropImage(b);
    if (b->memDC) {
        SelectObject(b->memDC, b->bmpOld);
        DeleteDC(b->memDC);
        b->memDC = nullptr;
    }
    if (b->bmp) {
        DeleteObject(b->bmp);
        b->bmp = nullptr;
    }
    if (b->hwnd) {
        DestroyWindow(b->hwnd);
        b->hwnd = nullptr;
    }
    if (gSigBox == b) {
        gSigBox = nullptr;
    }
    delete b;
}

extern bool SaveAnnotationsToExistingFile(WindowTab*, bool reload = true);

// Fills in a signature field with a certificate from a .pfx / .p12 file.
// Same WindowBase layout pattern as Add Favorite / Change Color.
struct SignDocumentWnd : WindowBase {
    ~SignDocumentWnd() override { str::Free(preselectField); }

    MainWindow* win = nullptr;
    HWND hwndOwner = nullptr;
    bool modalDisablesOwner = false;
    // unsigned signature fields the document already has; the placement
    // drop-down lists them first, then "new signature on the current page"
    StrVec fieldNames;
    Vec<int> fieldPages;
    int currPageNo = 1;
    // field the user clicked, so the drop-down opens on it rather than on the
    // first unsigned field in the document (issue #5964). Empty = no preference
    Str preselectField;
    bool hasPreselect = false;

    // CurrentUser\MY certs that can sign; the drop-down lists these, then
    // "Certificate file..." which uses editCert / editPassword instead
    StrVec certThumbs;
    DropDown* ddCert = nullptr;
    Edit* editCert = nullptr;
    Edit* editPassword = nullptr;
    DropDown* ddPlacement = nullptr;
    VirtButton* btnBrowse = nullptr;
    VirtButton* btnCancel = nullptr;
    VirtButton* btnClear = nullptr;
    VirtButton* btnRemoveSig = nullptr;
    VirtButton* btnSign = nullptr;
    // appearance (issue #5963): which bits of the cert / labels to draw, and
    // an optional image that replaces the large name on the left
    Checkbox* cbShowLabels = nullptr;
    Checkbox* cbShowDN = nullptr;
    Checkbox* cbShowDate = nullptr;
    Checkbox* cbShowGraphicName = nullptr;
    // signature input: draw freehand (default) or upload an image
    Checkbox* radioDraw = nullptr;
    Checkbox* radioUpload = nullptr;
    SigBox* sigBox = nullptr;

    // the rows behind the "Advanced" toggle (everything but Image and
    // Where to sign); hidden until the user clicks the toggle
    VBox* advancedPanel = nullptr;
    VirtText* advancedToggle = nullptr;
    bool advancedOpen = false;
    PlatformFont* font = nullptr;
    bool isRtl = false;

    void ToggleAdvanced(VirtMouseEvent* ev = nullptr);
    void AddAdvancedSection();
    void Relayout();

    bool Create(MainWindow* win);
    void CollectFields();
    void FillCertificates();
    void FillPlacement();
    void FillAppearance();
    bool UsingCertFile() const;
    void UpdateCertFileEnabled();
    void UpdateGraphicNameEnabled();
    bool BuildSignArgs(PdfSignArgs& args);
    void DoSign(const PdfSignArgs& args);

    void OnModeChanged();
    void OnClearSignature(VirtMouseEvent* ev = nullptr);
    void OnRemoveSignature(VirtMouseEvent* ev = nullptr);
    void OnCertChanged();
    void OnBrowse(VirtMouseEvent* ev = nullptr);
    void OnCancel(VirtMouseEvent* ev = nullptr);
    void OnSign(VirtMouseEvent* ev = nullptr);
    // Tab moves focus through this order (the virtual Sign/Cancel buttons aren't
    // in it: they're painted, not HWNDs)
    void OnDialogKeyDown(KeyEvent* ev);
    void BuildFocusOrder();
    Vec<HWND> focusOrder;
};

// last image the user signed with (this process only; not written to settings)
static Str gLastImagePath;

static SignDocumentWnd* gSignDocumentWnd = nullptr;
// true while the Sign Document dialog is hidden and the next click / drag on
// the page will place a new signature (issue #5967)
static bool gPlacingSignature = false;
static Kind kNotifSignPlacement = "notifSignPlacement";
static void ClearSignaturePlacementNotif(MainWindow* win);

static void SetSignDocumentOwnerEnabled(SignDocumentWnd* wnd, bool enabled) {
    if (wnd && wnd->modalDisablesOwner && wnd->hwndOwner && IsWindow(wnd->hwndOwner)) {
        EnableWindow(wnd->hwndOwner, enabled);
    }
}

// Hand activation back to the main window. Must run while this process is still
// the foreground one (i.e. before the dialog is hidden / destroyed): hiding the
// active popup otherwise gives activation to whatever app is next in z-order,
// which pushed the document behind other windows.
static void ActivateMainWindow(MainWindow* win) {
    if (!win || !win->hwndFrame || !IsWindow(win->hwndFrame)) {
        return;
    }
    SetForegroundWindow(win->hwndFrame);
    SetActiveWindow(win->hwndFrame);
    HwndSetFocus(win->hwndCanvas);
}

// Default size of a new signature when the user clicks rather than dragging
// a rectangle. 2" x 0.75" at 72 pt/in — enough for name, date and reason.
constexpr float kDefaultSignatureDx = 144;
constexpr float kDefaultSignatureDy = 54;

static void ClearSignDocumentWnd() {
    gPlacingSignature = false;
    SignDocumentWnd* wnd = gSignDocumentWnd;
    if (wnd && wnd->hwnd && GetCurrentModelessDialog() == wnd->hwnd) {
        SetCurrentModelessDialog(nullptr);
    }
    SigBoxDestroy(gSigBox);
    SetSignDocumentOwnerEnabled(wnd, true);
    gSignDocumentWnd = nullptr;
    // closing the dialog can leave the main window behind other apps; bring it
    // back to the foreground
    if (wnd && wnd->win && wnd->win->hwndFrame && IsWindow(wnd->win->hwndFrame)) {
        SetForegroundWindow(wnd->win->hwndFrame);
        HwndSetFocus(wnd->win->hwndCanvas);
    }
}

void CloseSignDocumentDialog(MainWindow* win) {
    if (!gSignDocumentWnd) {
        return;
    }
    if (win && gSignDocumentWnd->win != win) {
        return;
    }
    gPlacingSignature = false;
    ClearSignaturePlacementNotif(gSignDocumentWnd->win);
    gSignDocumentWnd->ScheduleDelete();
}

static EngineBase* GetPdfEngine(MainWindow* win) {
    if (!IsMainWindowValidAndNotClosing(win) || !win->IsDocLoaded()) {
        return nullptr;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || !EngineMupdfSupportsAnnotations(engine)) {
        return nullptr;
    }
    return engine;
}

// Bounding box of the current selection. Sets pageNoOut to the page of the
// first non-empty piece. Empty if nothing is selected.
static RectF SelectionRect(WindowTab* tab, int* pageNoOut) {
    RectF res;
    if (!tab || !tab->selectionOnPage) {
        return res;
    }
    for (auto& sel : *tab->selectionOnPage) {
        if (sel.rect.IsEmpty()) {
            continue;
        }
        if (res.IsEmpty()) {
            if (pageNoOut) {
                *pageNoOut = sel.pageNo;
            }
            res = sel.rect;
        } else if (!pageNoOut || sel.pageNo == *pageNoOut) {
            res = res.Union(sel.rect);
        }
    }
    return res;
}

static void ClearSignaturePlacementNotif(MainWindow* win) {
    if (win && win->hwndCanvas) {
        RemoveNotificationsForGroup(win->hwndCanvas, kNotifSignPlacement);
    }
}

static void ShowSignaturePlacementNotif(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    NotificationCreateArgs args;
    args.hwndParent = win->hwndCanvas;
    args.msg = Tr("Click or drag on the page to place the signature. Esc to cancel.");
    args.timeoutMs = kNotifNoTimeout;
    args.groupId = kNotifSignPlacement;
    args.corner = NotifCorner::BottomBar;
    args.warning = true;
    args.tab = win->CurrentTab();
    ShowNotification(args);
}

bool IsPlacingSignature(MainWindow* win) {
    return gPlacingSignature && gSignDocumentWnd && gSignDocumentWnd->win == win;
}

// Leaves placement mode. Returns true if it was active. The hidden dialog is
// shown again so the user can change the certificate or cancel.
bool CancelPlacingSignature(MainWindow* win) {
    if (!IsPlacingSignature(win)) {
        if (gPlacingSignature) {
            gPlacingSignature = false;
        }
        return false;
    }
    gPlacingSignature = false;
    ClearSignaturePlacementNotif(win);
    if (gSignDocumentWnd) {
        SetSignDocumentOwnerEnabled(gSignDocumentWnd, false);
        gSignDocumentWnd->SetIsVisible(true);
        HwndSetFocus(gSignDocumentWnd->hwnd);
    }
    return true;
}

static void StartSignaturePlacement(SignDocumentWnd* wnd) {
    if (!wnd || !wnd->win) {
        return;
    }
    gPlacingSignature = true;
    wnd->SetIsVisible(false);
    SetSignDocumentOwnerEnabled(wnd, true);
    DeleteOldSelectionInfo(wnd->win, true);
    ShowSignaturePlacementNotif(wnd->win);
    HwndSetFocus(wnd->win->hwndFrame);
}

static RectF ClampRectToPage(RectF r, RectF page) {
    if (page.IsEmpty()) {
        return r;
    }
    if (r.dx > page.dx) {
        r.dx = page.dx;
    }
    if (r.dy > page.dy) {
        r.dy = page.dy;
    }
    if (r.x < page.x) {
        r.x = page.x;
    }
    if (r.y < page.y) {
        r.y = page.y;
    }
    if (r.x + r.dx > page.x + page.dx) {
        r.x = page.x + page.dx - r.dx;
    }
    if (r.y + r.dy > page.y + page.dy) {
        r.y = page.y + page.dy - r.dy;
    }
    return r;
}

// A default-size box centered on the click, kept on the page.
static RectF DefaultSignatureRectAt(DisplayModel* dm, int pageNo, PointF pt) {
    RectF r(pt.x - (kDefaultSignatureDx / 2), pt.y - (kDefaultSignatureDy / 2), kDefaultSignatureDx,
            kDefaultSignatureDy);
    PageInfo* pi = dm ? dm->GetPageInfo(pageNo) : nullptr;
    if (!pi || !IsMediaBoxKnown(pi->mediaBox)) {
        return r;
    }
    return ClampRectToPage(r, pi->mediaBox);
}

// The click or drag that places a new signature. aborted is a click (no drag).
// Returns true if this press belonged to placement (even if we keep waiting).
bool FinishSignaturePlacement(MainWindow* win, int x, int y, bool aborted) {
    if (!IsPlacingSignature(win) || !gSignDocumentWnd) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        CancelPlacingSignature(win);
        return true;
    }

    int pageNo = gSignDocumentWnd->currPageNo;
    RectF rect;
    if (aborted) {
        Point pt(x, y);
        pageNo = dm->GetPageNoByPoint(pt);
        if (!dm->ValidPageNo(pageNo)) {
            return true; // click off the page: keep waiting
        }
        rect = DefaultSignatureRectAt(dm, pageNo, dm->CvtFromScreen(pt, pageNo));
    } else {
        rect = SelectionRect(win->CurrentTab(), &pageNo);
        if (rect.IsEmpty() || rect.dx < 8 || rect.dy < 8) {
            Point pt(x, y);
            int clickPage = dm->GetPageNoByPoint(pt);
            if (!dm->ValidPageNo(clickPage)) {
                return true;
            }
            pageNo = clickPage;
            rect = DefaultSignatureRectAt(dm, pageNo, dm->CvtFromScreen(pt, pageNo));
        }
    }
    if (rect.IsEmpty()) {
        return true;
    }

    PdfSignArgs args;
    if (!gSignDocumentWnd->BuildSignArgs(args)) {
        CancelPlacingSignature(win);
        return true;
    }
    args.pageNo = pageNo;
    args.rect = rect;
    args.fieldName = {};

    gPlacingSignature = false;
    ClearSignaturePlacementNotif(win);
    DeleteOldSelectionInfo(win, true);
    SetSignDocumentOwnerEnabled(gSignDocumentWnd, false);
    gSignDocumentWnd->DoSign(args);
    return true;
}

void SignDocumentWnd::CollectFields() {
    fieldNames.Reset();
    VecReset(fieldPages);
    currPageNo = 1;
    EngineBase* engine = GetPdfEngine(win);
    if (!engine) {
        return;
    }
    if (win->ctrl) {
        currPageNo = win->ctrl->CurrentPageNo();
    }
    EngineMupdfGetUnsignedSignatureFields(engine, fieldNames, fieldPages);
}

void SignDocumentWnd::FillCertificates() {
    if (!ddCert) {
        return;
    }
    certThumbs.Reset();
    StrVec labels;
    ListWindowsSigningCertificates(certThumbs, labels);
    StrVec items;
    for (int i = 0; i < len(labels); i++) {
        items.Append(labels[i]);
    }
    items.Append(Tr("Certificate file..."));
    ddCert->SetItems(items);
    // a store cert if we have one; otherwise the file picker
    CbSetCurrentSelection(ddCert, len(certThumbs) > 0 ? 0 : len(items) - 1);
    UpdateCertFileEnabled();
}

bool SignDocumentWnd::UsingCertFile() const {
    int idx = CbGetCurrentSelection(ddCert);
    return idx < 0 || idx >= len(certThumbs);
}

void SignDocumentWnd::UpdateCertFileEnabled() {
    bool file = UsingCertFile();
    if (editCert) {
        editCert->SetIsEnabled(file);
    }
    if (btnBrowse) {
        btnBrowse->SetIsEnabled(file);
    }
    if (editPassword) {
        editPassword->SetIsEnabled(file);
    }
}

void SignDocumentWnd::OnCertChanged() {
    UpdateCertFileEnabled();
}

void SignDocumentWnd::UpdateGraphicNameEnabled() {
    bool hasImage = SigBoxHasContent(sigBox);
    if (cbShowGraphicName) {
        cbShowGraphicName->SetIsEnabled(!hasImage);
        if (hasImage) {
            cbShowGraphicName->SetIsChecked(false);
        }
    }
}

void SignDocumentWnd::OnClearSignature(VirtMouseEvent*) {
    SigBoxClear(sigBox);
    UpdateGraphicNameEnabled();
}

// Removes the signature from the field this dialog targets (the clicked one, or
// the current placement selection), then closes the dialog.
void SignDocumentWnd::OnRemoveSignature(VirtMouseEvent*) {
    EngineBase* engine = GetPdfEngine(win);
    Str fieldName = {};
    if (hasPreselect) {
        fieldName = preselectField;
    } else if (ddPlacement) {
        int sel = CbGetCurrentSelection(ddPlacement);
        if (sel >= 0 && sel < len(fieldNames)) {
            fieldName = fieldNames[sel];
        }
    }
    if (engine && len(fieldName) > 0) {
        if (EngineMupdfRemoveSignatureByName(engine, fieldName)) {
            MainWindowRerender(win);
            ToolbarUpdateStateForWindow(win, false);
        }
    }
    OnCancel(nullptr);
}

void SignDocumentWnd::OnModeChanged() {
    if (sigBox && radioUpload) {
        SigBoxSetMode(sigBox, radioUpload->IsChecked());
    }
    UpdateGraphicNameEnabled();
}

// Tab order is explicit so it never lands on the bare dialog (the virtual
// Sign/Cancel buttons are painted, not HWNDs, so IsDialogMessage can't include
// them and wraps focus to the dialog instead)
void SignDocumentWnd::BuildFocusOrder() {
    VecReset(focusOrder);
    auto add = [this](HWND h) {
        if (h) {
            VecAppend(focusOrder, h);
        }
    };
    add(radioDraw ? radioDraw->hwnd : nullptr);
    add(ddPlacement ? ddPlacement->hwnd : nullptr);
    add(ddCert ? ddCert->hwnd : nullptr);
    add(editCert ? editCert->hwnd : nullptr);
    add(editPassword ? editPassword->hwnd : nullptr);
    add(cbShowLabels ? cbShowLabels->hwnd : nullptr);
    add(cbShowDN ? cbShowDN->hwnd : nullptr);
    add(cbShowDate ? cbShowDate->hwnd : nullptr);
    add(cbShowGraphicName ? cbShowGraphicName->hwnd : nullptr);
}

void SignDocumentWnd::OnDialogKeyDown(KeyEvent* ev) {
    if (ev->vkey != VK_TAB || ev->isCtrl || ev->isAlt) {
        return;
    }
    int n = len(focusOrder);
    if (n == 0) {
        return;
    }
    HWND focus = GetFocus();
    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (focusOrder[i] == focus) {
            idx = i;
            break;
        }
    }
    int step = ev->isShift ? -1 : 1;
    for (int k = 1; k <= n; k++) {
        int j = (((idx + step * k) % n) + n) % n;
        HWND h = focusOrder[j];
        if (h && IsWindow(h) && IsWindowVisible(h) && IsWindowEnabled(h)) {
            HwndSetFocus(h);
            break;
        }
    }
    ev->didHandle = true;
}

void SignDocumentWnd::FillAppearance() {
    // every appearance option defaults to unchecked: the signature shows just
    // the drawn / uploaded image and nothing else
    if (cbShowLabels) {
        cbShowLabels->SetIsChecked(false);
    }
    if (cbShowDN) {
        cbShowDN->SetIsChecked(false);
    }
    if (cbShowDate) {
        cbShowDate->SetIsChecked(false);
    }
    if (cbShowGraphicName) {
        cbShowGraphicName->SetIsChecked(false);
    }
    UpdateGraphicNameEnabled();
}

void SignDocumentWnd::FillPlacement() {
    if (!ddPlacement) {
        return;
    }
    StrVec items;
    for (int i = 0; i < len(fieldNames); i++) {
        Str name = fieldNames[i];
        if (str::IsEmptyOrWhiteSpace(name)) {
            name = Tr("Signature");
        }
        items.Append(fmt(Tr("Signature field: %s (page %d)").s, name, fieldPages[i]));
    }
    items.Append(fmt(Tr("New signature on page %d").s, currPageNo));
    ddPlacement->SetItems(items);
    int sel = 0;
    if (hasPreselect) {
        for (int i = 0; i < len(fieldNames); i++) {
            if (str::Eq(fieldNames[i], preselectField)) {
                sel = i;
                break;
            }
        }
    }
    CbSetCurrentSelection(ddPlacement, sel);
}

// Turns the dialog state into what the engine needs; false if something the
// user has to fix is missing.
bool SignDocumentWnd::BuildSignArgs(PdfSignArgs& args) {
    if (UsingCertFile()) {
        TempStr certPath = editCert ? editCert->GetTextTemp() : Str{};
        str::TrimWSInPlace(certPath, str::TrimOpt::Both);
        if (len(certPath) == 0) {
            MessageBoxWarning(hwnd, Tr("Please choose the certificate file to sign with."), Tr("Sign Document"));
            return false;
        }
        if (!file::Exists(certPath)) {
            MessageBoxWarning(hwnd, fmt(Tr("Certificate file %s doesn't exist.").s, certPath), Tr("Sign Document"));
            return false;
        }
        args.certPath = certPath;
        args.certPassword = editPassword ? editPassword->GetTextTemp() : Str{};
    } else {
        int idx = CbGetCurrentSelection(ddCert);
        args.certThumbprint = certThumbs[idx];
    }
    args.reason = {};
    args.location = {};

    TempStr imagePath = SigBoxImagePathTemp(sigBox);
    if (len(imagePath) == 0) {
        MessageBoxWarning(hwnd, Tr("Please draw your signature or select an image."), Tr("Sign Document"));
        return false;
    }
    args.imagePath = imagePath;

    int flags = 0;
    if (cbShowLabels && cbShowLabels->IsChecked()) {
        flags |= kPdfSignShowLabels;
    }
    if (cbShowDN && cbShowDN->IsChecked()) {
        flags |= kPdfSignShowDN;
    }
    if (cbShowDate && cbShowDate->IsChecked()) {
        flags |= kPdfSignShowDate;
    }
    if (cbShowGraphicName && cbShowGraphicName->IsChecked()) {
        flags |= kPdfSignShowGraphicName;
    }
    // the "Digitally signed by ..." text is never drawn; with the required
    // image that is all the appearance needs
    args.appearanceFlags = flags;

    int idx = CbGetCurrentSelection(ddPlacement);
    if (idx >= 0 && idx < len(fieldNames)) {
        args.fieldName = fieldNames[idx];
        args.pageNo = fieldPages[idx];
        return true;
    }
    // a new field: use the selection if there is one, otherwise the caller
    // will ask the user to click or drag on the page (issue #5967)
    args.pageNo = currPageNo;
    int selPage = currPageNo;
    args.rect = SelectionRect(win ? win->CurrentTab() : nullptr, &selPage);
    if (!args.rect.IsEmpty()) {
        args.pageNo = selPage;
    }
    return true;
}

void SignDocumentWnd::OnBrowse(VirtMouseEvent*) {
    WCHAR fileName[MAX_PATH + 1]{};
    TempStr curr = editCert ? editCert->GetTextTemp() : Str{};
    if (len(curr) > 0 && len(curr) < MAX_PATH) {
        wstr::BufSet(WStr(fileName, dimof(fileName)), ToWStrTemp(curr));
    }

    str::Builder fileFilter;
    fileFilter.Reserve(256);
    fileFilter.Append(Tr("Certificate files"));
    fileFilter.Append(StrL("\1*.pfx;*.p12\1"));
    fileFilter.Append(Tr("All files"));
    fileFilter.Append(StrL("\1*.*\1"));
    Str fileFilterStr = ToStr(fileFilter);
    str::TransCharsInPlace(fileFilterStr, StrL("\1"), StrL("\0"));

    OPENFILENAME ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = dimof(fileName);
    ofn.lpstrFilter = CWStrTemp(fileFilterStr);
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) {
        return;
    }
    if (editCert) {
        editCert->SetText(ToUtf8Temp(WStr(fileName)));
    }
    EditSetFocus(editPassword);
}

void SignDocumentWnd::OnCancel(VirtMouseEvent*) {
    gPlacingSignature = false;
    ClearSignaturePlacementNotif(win);
    // re-enable the owner before activating it (a disabled window can't be
    // activated), then hand it the foreground before the dialog goes away
    SetSignDocumentOwnerEnabled(this, true);
    ActivateMainWindow(win);
    ScheduleDelete();
}

// mupdf reports a certificate it can't open as a raw Win32 failure
// ("PFXImportCertStore failed (gle=86)"). A mistyped password is by far the
// most likely cause of gle=86 (ERROR_INVALID_PASSWORD), so say that instead.
static TempStr SignErrorMessageTemp(Str err) {
    if (len(err) == 0) {
        return Tr("Could not sign the document.");
    }
    if (str::Contains(err, StrL("PFXImportCertStore"))) {
        if (str::Contains(err, StrL("gle=86"))) {
            return Tr("Wrong password for the certificate file.");
        }
        return fmt(Tr("Could not read the certificate file: %s").s, err);
    }
    if (str::Contains(err, StrL("not found in the Windows certificate store")) ||
        str::Contains(err, StrL("invalid certificate thumbprint"))) {
        return Tr("Could not use that certificate from the Windows certificate store.");
    }
    if (str::Contains(err, StrL("could not read signature image")) || str::Contains(err, StrL("cannot create image")) ||
        str::Contains(err, StrL("unknown image format"))) {
        return Tr("Could not read the signature image.");
    }
    return str::DupTemp(err);
}

void SignDocumentWnd::DoSign(const PdfSignArgs& args) {
    EngineBase* engine = GetPdfEngine(win);
    if (!engine) {
        ScheduleDelete();
        return;
    }

    Str err;
    bool ok = EngineMupdfSignDocument(engine, args, &err);
    if (!ok) {
        SetIsVisible(true);
        HwndSetFocus(hwnd);
        MessageBoxWarning(hwnd, SignErrorMessageTemp(err), Tr("Sign Document"));
        str::Free(err);
        return;
    }
    str::Free(err);

    str::ReplaceWithCopy(&gLastImagePath, args.imagePath);

    // the signature is only computed while saving, so write the document out
    // now. Save in place and don't reload: reloading can close the document,
    // and the engine already dropped the page's cached rendering, so a repaint
    // shows the new signature right away.
    // Apply the signature to the in-memory document and show it; don't write
    // the file. The user saves when they're done (the signature is completed
    // during that save).
    SetSignDocumentOwnerEnabled(this, true);
    ActivateMainWindow(win);
    SetIsVisible(false);
    ScheduleDelete();
    MainWindowRerender(win);
    ToolbarUpdateStateForWindow(win, false);
    ActivateMainWindow(win);
}

void SignDocumentWnd::OnSign(VirtMouseEvent*) {
    EngineBase* engine = GetPdfEngine(win);
    if (!engine) {
        ScheduleDelete();
        return;
    }
    PdfSignArgs args;
    if (!BuildSignArgs(args)) {
        return;
    }
    // a new field with nowhere to put it: hide this dialog and let the user
    // click or drag on the page (issue #5967)
    if (len(args.fieldName) == 0 && args.rect.IsEmpty()) {
        StartSignaturePlacement(this);
        return;
    }
    DoSign(args);
}

static void OnClose(WindowBase::CloseEvent* /*ev*/) {
    if (gSignDocumentWnd) {
        gSignDocumentWnd->OnCancel();
    }
}

static void OnDestroy(WindowBase::DestroyEvent* /*ev*/) {
    if (gSignDocumentWnd) {
        gSignDocumentWnd->ScheduleDelete();
    }
}

// label above a field, matching the spacing the other dialogs use
static VirtText* AddLabel(VBox* vbox, Str s, PlatformFont* font, bool isRtl, int padTop) {
    auto* c = NewVirtText({
        .s = s,
        .font = font,
        .isRtl = isRtl,
        .prefix = true,
        .padding = DpiScaledInsets(padTop, 0, 4, 0),
    });
    vbox->AddChild(c);
    return c;
}

static Edit* AddEdit(VBox* vbox, HWND hwnd, PlatformFont* font, bool isRtl, bool isPassword) {
    Edit::CreateArgs args;
    args.parent = hwnd;
    args.font = font;
    args.withBorder = true;
    args.isRtl = isRtl;
    args.isPassword = isPassword;
    auto* c = new Edit();
    c->Create(args);
    vbox->AddChild(c);
    return c;
}

static Checkbox* AddCheckbox(VBox* vbox, HWND hwnd, Str text, bool isRtl, int padTop) {
    Checkbox::CreateArgs args;
    args.parent = hwnd;
    args.text = text;
    args.isRtl = isRtl;
    auto* c = new Checkbox();
    c->SetInsetsPt(padTop, 0, 0, 0);
    c->Create(args);
    vbox->AddChild(c);
    return c;
}

// the advanced panel mixes real HWND controls (edits, combos, checkboxes) and
// painted virtual ones (the labels); collapsing the VBox shrinks the dialog but
// neither kind disappears on its own, so show / hide both explicitly
static void ForEachControl(ILayout* l, bool visible) {
    for (int i = 0; i < l->LayoutChildCount(); i++) {
        ILayout* c = l->LayoutChildAt(i);
        if (ControlBase* cb = c->AsControl()) {
            ShowWindow(cb->hwnd, visible ? SW_SHOW : SW_HIDE);
        }
        if (VirtCtrl* vc = c->AsVirtCtrl()) {
            vc->SetIsVisible(visible);
        }
        ForEachControl(c, visible);
    }
}
bool SignDocumentWnd::Create(MainWindow* mainWin) {
    win = mainWin;
    hwndOwner = win ? win->hwndFrame : nullptr;
    CollectFields();

    {
        CreateCustomArgs args;
        args.owner = hwndOwner;
        args.title = Tr("Sign Document");
        args.visible = false;
        args.style = WS_POPUPWINDOW | WS_CAPTION;
        args.font = GetFont();
        args.icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(GetAppIconID()));
        CreateCustom(args);
    }
    if (!hwnd) {
        return false;
    }
    isRtl = IsUIRtl();
    font = GetFont();

    auto* vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;

    // the image IS the signature: it comes first and is required
    // signature input: draw freehand (default) or upload an image, with a
    // drawable / preview area below
    {
        auto* row = new HBox();
        row->alignMain = MainAxisAlign::MainStart;
        row->alignCross = CrossAxisAlign::CrossCenter;
        row->gap = DpiScale(16);

        Checkbox::CreateArgs r1;
        r1.parent = hwnd;
        r1.text = Tr("&Draw signature");
        r1.isRtl = isRtl;
        r1.isRadio = true;
        radioDraw = new Checkbox();
        radioDraw->Create(r1);
        radioDraw->SetIsChecked(true);
        row->AddChild(radioDraw);

        Checkbox::CreateArgs r2;
        r2.parent = hwnd;
        r2.text = Tr("&Upload an image");
        r2.isRtl = isRtl;
        r2.isRadio = true;
        radioUpload = new Checkbox();
        radioUpload->Create(r2);
        row->AddChild(radioUpload);

        radioDraw->onStateChanged = MkMethod0<SignDocumentWnd, &SignDocumentWnd::OnModeChanged>(this);
        radioUpload->onStateChanged = MkMethod0<SignDocumentWnd, &SignDocumentWnd::OnModeChanged>(this);
        vbox->AddChild(row);
    }
    {
        sigBox = SigBoxCreate(hwnd);
        Size sz = SigBoxIdealSize();
        if (sigBox) {
            vbox->AddChild(new HwndSlot(sigBox->hwnd, sz.dx, sz.dy));
        }
    }

    AddLabel(vbox, Tr("&Where to sign:"), font, isRtl, 8);
    {
        DropDown::CreateArgs args;
        args.parent = hwnd;
        args.font = font;
        args.isRtl = isRtl;
        auto* c = new DropDown();
        c->Create(args);
        ddPlacement = c;
        vbox->AddChild(c);
        FillPlacement();
    }

    // clickable "Advanced" text reveals the certificate / options rows
    auto* adv = NewVirtText({
        .s = Tr("Advanced..."),
        .font = font,
        .isRtl = isRtl,
        .prefix = true,
        .padding = DpiScaledInsets(10, 0, 4, 0),
    });
    adv->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::ToggleAdvanced>(this);
    // plain VirtText is not hit-tested by default; make it a clickable link
    adv->SetFlag(vwfNoHitTest, false);
    adv->cursor = CursorId::Hand;
    advancedToggle = adv;
    vbox->AddChild(adv);

    advancedPanel = new VBox();
    advancedPanel->alignMain = MainAxisAlign::MainStart;
    advancedPanel->alignCross = CrossAxisAlign::Stretch;
    if (!advancedOpen) {
        advancedPanel->SetVisibility(Visibility::Collapse);
    }
    vbox->AddChild(advancedPanel);
    AddAdvancedSection();
    if (!advancedOpen) {
        ForEachControl(advancedPanel, false);
    }
    FillAppearance();

    {
        auto* hbox = new HBox();
        hbox->alignMain = MainAxisAlign::MainEnd;
        hbox->alignCross = CrossAxisAlign::CrossCenter;
        hbox->gap = font->averageCharWidth;
        auto pad = Insets{12, 0, 4, 0};

        // Remove Signature drops an existing signature from the document and
        // closes the dialog; Clear only empties the input box
        btnRemoveSig = NewThemedButton(hwnd, Tr("Remove Signature"), font, false);
        btnRemoveSig->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::OnRemoveSignature>(this);
        hbox->AddChild(new Padding(btnRemoveSig, pad));
        btnClear = NewThemedButton(hwnd, Tr("Clear"), font, false);
        btnClear->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::OnClearSignature>(this);
        hbox->AddChild(new Padding(btnClear, pad));
        btnCancel = NewThemedButton(hwnd, Tr("Cancel"), font, false);
        btnCancel->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::OnCancel>(this);
        hbox->AddChild(new Padding(btnCancel, pad));
        btnSign = NewThemedButton(hwnd, Tr("Sign"), font, true);
        btnSign->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::OnSign>(this);
        hbox->AddChild(new Padding(btnSign, pad));
        vbox->AddChild(hbox);
    }

    auto* padding = new Padding(vbox, DpiScaledInsets(4, 8));
    layout = padding;

    int dx = DpiScale(460);
    LayoutAndSizeToContent(layout, dx, 0, hwnd);
    DoLayout(HwndClientRect(hwnd).Size());
    HwndCenterDialog(hwnd, win ? win->hwndFrame : nullptr);
    UpdateTheme();

    SetIsVisible(true);
    // register as the current modeless dialog so the main message loop routes
    // Tab through IsDialogMessage, which is what moves focus between the real
    // controls (edits, combos, checkboxes)
    SetCurrentModelessDialog(hwnd);
    onKeyDown = MkMethod1<SignDocumentWnd, KeyEvent*, &SignDocumentWnd::OnDialogKeyDown>(this);
    BuildFocusOrder();
    if (radioDraw) {
        HwndSetFocus(radioDraw->hwnd);
    }
    return true;
}

// The certificate and appearance rows the main dialog hides behind "Advanced".
void SignDocumentWnd::AddAdvancedSection() {
    AddLabel(advancedPanel, Tr("&Certificate:"), font, isRtl, 0);
    {
        DropDown::CreateArgs args;
        args.parent = hwnd;
        args.font = font;
        args.isRtl = isRtl;
        auto* c = new DropDown();
        c->Create(args);
        ddCert = c;
        c->onSelectionChanged = MkMethod0<SignDocumentWnd, &SignDocumentWnd::OnCertChanged>(this);
        advancedPanel->AddChild(c);
        FillCertificates();
    }
    {
        auto* row = new HBox();
        row->alignMain = MainAxisAlign::MainStart;
        row->alignCross = CrossAxisAlign::CrossCenter;
        row->gap = font->averageCharWidth;

        Edit::CreateArgs args;
        args.parent = hwnd;
        args.font = font;
        args.withBorder = true;
        args.isRtl = isRtl;
        args.idealWidthChars = 40;
        auto* e = new Edit();
        e->Create(args);
        editCert = e;
        row->AddChild(e);

        btnBrowse = NewThemedButton(hwnd, Tr("&Browse..."), font, false);
        btnBrowse->onClick = MkMethod1<SignDocumentWnd, VirtMouseEvent*, &SignDocumentWnd::OnBrowse>(this);
        row->AddChild(btnBrowse);
        advancedPanel->AddChild(row);
        UpdateCertFileEnabled();
    }

    AddLabel(advancedPanel, Tr("&Password:"), font, isRtl, 8);
    editPassword = AddEdit(advancedPanel, hwnd, font, isRtl, true);
    UpdateCertFileEnabled();

    AddLabel(advancedPanel, Tr("Appearance:"), font, isRtl, 8);
    cbShowLabels = AddCheckbox(advancedPanel, hwnd, Tr("Show &labels"), isRtl, 2);
    cbShowDN = AddCheckbox(advancedPanel, hwnd, Tr("Show &DN"), isRtl, 2);
    cbShowDate = AddCheckbox(advancedPanel, hwnd, Tr("Show da&te"), isRtl, 2);
    cbShowGraphicName = AddCheckbox(advancedPanel, hwnd, Tr("Show name as &graphic"), isRtl, 2);
}

void SignDocumentWnd::Relayout() {
    if (!hwnd) {
        return;
    }
    // re-run the layout so the dialog grows / shrinks with the advanced panel
    int dx = DpiScale(460);
    LayoutAndSizeToContent(layout, dx, 0, hwnd);
    DoLayout(HwndClientRect(hwnd).Size());
}

void SignDocumentWnd::ToggleAdvanced(VirtMouseEvent*) {
    advancedOpen = !advancedOpen;
    if (advancedPanel) {
        advancedPanel->SetVisibility(advancedOpen ? Visibility::Visible : Visibility::Collapse);
        ForEachControl(advancedPanel, advancedOpen);
    }
    Relayout();
    HwndCenterDialog(hwnd, win ? win->hwndFrame : nullptr);
}

// fieldName selects that signature field in the placement drop-down; pass
// hasField = false (the default) to leave the choice at the first unsigned
// field, as the Sign Document command does.
void ShowSignDocumentDialog(MainWindow* win, Str fieldName, bool hasField) {
    if (!GetPdfEngine(win)) {
        return;
    }
    if (gSignDocumentWnd) {
        if (gPlacingSignature) {
            CancelPlacingSignature(win);
        }
        if (hasField) {
            str::ReplaceWithCopy(&gSignDocumentWnd->preselectField, fieldName);
            gSignDocumentWnd->hasPreselect = true;
            gSignDocumentWnd->FillPlacement();
        }
        gSignDocumentWnd->SetIsVisible(true);
        HwndSetFocus(gSignDocumentWnd->hwnd);
        return;
    }
    auto* wnd = new SignDocumentWnd();
    if (hasField) {
        str::ReplaceWithCopy(&wnd->preselectField, fieldName);
        wnd->hasPreselect = true;
    }
    wnd->closeOnEsc = true;
    wnd->onBeforeDelete = MkFunc0Void(ClearSignDocumentWnd);
    wnd->onClose = MkFunc1Void<WindowBase::CloseEvent*>(OnClose);
    wnd->onDestroy = MkFunc1Void<WindowBase::DestroyEvent*>(OnDestroy);
    wnd->SetFont(GetAppFont());
    bool ok = wnd->Create(win);
    if (!ok) {
        delete wnd;
        return;
    }
    gSignDocumentWnd = wnd;
    wnd->modalDisablesOwner = wnd->hwndOwner && IsWindowEnabled(wnd->hwndOwner);
    SetSignDocumentOwnerEnabled(wnd, false);
}
