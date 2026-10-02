/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/AutoWin.h"
#include "base/File.h"
#include "base/Pixmap.h"
#include "base/Win.h"
#include "base/GdiPlusUtil.h"
#include "gui/Dpi.h"

#include <gdiplus.h>

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/win/WebView.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/GuiColors.h"
#include "gui/VirtCtrl.h"
#include "gui/VirtHost.h"

#include "Settings.h"
#include "DocController.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "Commands.h"
#include "Accelerators.h"
#include "Translations.h"
#include "Version.h"
#include "Theme.h"
#include "AppSettings.h"
#include "AppTools.h"
#include "DarkMode.h"
#include "SvgIcons.h"
#include "PagePosition.h"
#include "resource.h"
#include "HomePage.h"

// how the shared tip code (TipText.cpp) opens a url link
static void OpenTipUrl(Str url) {
    SumatraLaunchBrowser(url);
}

// what the shared tip code (TipText.cpp) knows about our commands: it names
// them in (Key/Cmd...) and in link targets, but knows nothing about the command
// table itself
struct SumatraCommandsContext : CommandsContext {
    TempStr GetCommandShortcutTemp(Str cmdName) override {
        int cmdId = GetCommandIdByName(cmdName);
        if (cmdId <= 0) {
            return {}; // not a command: the markup stays literal text
        }
        TempStr accel = AppendAccelKeyToMenuStringTemp(StrL(""), cmdId);
        if (len(accel) == 0 || !*accel.s) {
            return str::DupTemp(cmdName); // a command, but unbound
        }
        // AppendAccelKeyToMenuStringTemp prepends 	, skip it
        if (accel.s[0] == '	') {
            return Str(accel.s + 1);
        }
        return accel;
    }

    // `cmd` can carry arguments (e.g. "CmdFixDefaultApp .pdf"); those go through
    // CreateCommandFromDefinition so FrameOnCommand sees a CustomCommand with args
    void ExecuteCommand(HWND hwnd, Str cmd) override {
        CustomCommand* custom = CreateCommandFromDefinition(cmd);
        if (custom) {
            HwndSendCommand(hwnd, custom->id);
            return;
        }
        int cmdId = GetCommandIdByName(cmd);
        if (cmdId > 0) {
            HwndSendCommand(hwnd, cmdId);
        }
    }
};

static SumatraCommandsContext gSumatraCommandsContext;

struct TipHookInstaller {
    TipHookInstaller() {
        gTipOpenUrl = OpenTipUrl;
        gCommandsContext = &gSumatraCommandsContext;
    }
};
static TipHookInstaller gTipHookInstaller;

#ifndef ABOUT_USE_LESS_COLORS
constexpr int kAboutLineOuterSize = 2;
#else
constexpr int kAboutLineOuterSize = 1;
#endif
constexpr int kAboutLineSepSize = 1;

static Str sumatraTips = StrL(R"tips(You can [customize scrollbar](CmdChangeScrollbar).
You can [customize keyboard shortcuts](Help/Customize-keyboard-shortcuts).
You can [customize toolbar](Help/Customize-toolbar).
Press (Key/CmdCommandPalette) to open [command palette](CmdCommandPalette).
You can [extract text from PDF file](Help/Tool-x-extract-text-from-pdf).
You can [toggle menu bar](CmdToggleMenuBar) with (Key/CmdToggleMenuBar).
You can [toggle toolbar](CmdToggleToolbar) with (Key/CmdToggleToolbar).
You can [edit PDF annotations](Help/Editing-annotations).
You can preview where a citation, figure or footnote link points by hovering it — [Toggle Citation Hover Preview](CmdToggleHoverPreview) or set CitationHoverDelay in [advanced settings](CmdAdvancedSettings).
)tips");

static Str sumatraPromos = StrL(R"promos(Try [Edna](https://edna.arslexis.io): a note taking web app for power users.
Try [MarkLexis](https://marklexis.arslexis.io): a bookmarking web application.
)promos");

static Str promoFromServer;

// the tip markup, one line each; the selected one is parsed by the tip band
static StrVec gTipLines;
static StrVec gPromoLines;
static bool gTipsParsed = false;
static bool gSelectedIsPromo = false;
static int gSelectedTipIdx = -1;

static void CollectTipsFromString(Str src, Str prefix, StrVec* out) {
    StrVec lines;
    Split(&lines, src, StrL("\n"));
    for (int i = 0; i < len(lines); i++) {
        Str line = lines[i];
        if (str::IsEmptyOrWhiteSpace(line)) {
            continue;
        }
        if (prefix) {
            out->Append(str::JoinTemp(prefix, line));
        } else {
            out->Append(line);
        }
    }
}

// the markup of the tip currently on show, {} when there is none
static Str SelectedTipLine() {
    if (!gSettings->showTips || gSelectedTipIdx < 0) {
        return {};
    }
    StrVec& v = gSelectedIsPromo ? gPromoLines : gTipLines;
    if (gSelectedTipIdx >= len(v)) {
        return {};
    }
    return v[gSelectedTipIdx];
}

static void PickRandomTipOrPromo() {
    bool pickPromo = (len(gPromoLines) > 0) && (rand() % 100 < 30);
    if (pickPromo) {
        gSelectedIsPromo = true;
        gSelectedTipIdx = rand() % len(gPromoLines);
    } else if (len(gTipLines) > 0) {
        gSelectedIsPromo = false;
        gSelectedTipIdx = rand() % len(gTipLines);
    }
}

static void EnsureTipsParsed() {
    if (gTipsParsed) {
        return;
    }
    CollectTipsFromString(sumatraTips, StrL("Tip: "), &gTipLines);
    CollectTipsFromString(sumatraPromos, {}, &gPromoLines);
    gTipsParsed = true;
    PickRandomTipOrPromo();
}

static void ClearHomeLayoutCache(MainWindow*);

void FreeHomePageTips() {
    if (gTipsParsed) {
        gTipLines.Reset();
        gPromoLines.Reset();
        gTipsParsed = false;
    }
    str::Free(promoFromServer);
    HomePageInvalidateLayoutCache();
}

static void PickAnotherRandomTip() {
    bool prevIsPromo = gSelectedIsPromo;
    int prev = gSelectedTipIdx;
    // keep picking until we get a different one
    int maxIter = 100;
    while (maxIter-- > 0) {
        PickRandomTipOrPromo();
        if (gSelectedIsPromo != prevIsPromo || gSelectedTipIdx != prev) {
            return;
        }
    }
}

constexpr Color kAboutBorderCol = kColBlack;

constexpr int kAboutLeftRightSpaceDx = 8;
constexpr int kAboutMarginDx = 10;
constexpr int kAboutBoxMarginDy = 6;
constexpr int kAboutTxtDy = 6;
constexpr int kAboutRectPadding = 8;

constexpr int kInnerPadding = 8;

static const Str kSumatraTxtFont = StrL("Arial Black");
constexpr int kSumatraTxtFontSize = 24;

constexpr int kLayoutLtr = 0;

static ATOM gAtomAbout;
static HWND gHwndAbout;
static VirtRoot* gAboutRoot = nullptr;
static Tooltip* gAboutTooltip = nullptr;
static Str gClickedURL;

// one row of the About screen's two-column table
struct AboutRow {
    Str leftTxt;
    Str rightTxt;
    Str url;
};

static AboutRow gAboutRows[] = {
    // a null rightTxt means "the app version", filled in by Sync() because it
    // isn't known until runtime (32/64-bit, debug)
    {StrL("version"), {}, {}},
    {StrL("built on"), StrL(__DATE__ " " __TIME__), {}},
    {StrL("website"), StrL("MithenPDF GitHub"), Str(kWebsiteURL)},
#ifdef GIT_COMMIT_ID_STR
    {StrL("last change"), StrL("git commit " GIT_COMMIT_ID_STR),
     StrL("https://github.com/sumatrapdfreader/sumatrapdf/commit/" GIT_COMMIT_ID_STR)},
#endif
    {StrL("based on"), StrL("SumatraPDF Team"), StrL("https://github.com/sumatrapdfreader/sumatrapdf")},
    {{}, {}, {}}};

// The About screen's two text columns: a Table (ILayout) whose left column
// is right-aligned and right column left-aligned. Rows with a url become
// VirtLink (owning the hit-testing, the hand cursor and the tooltip), the rest
// plain VirtText. Table is an ILayout child so ElementFromPoint walks the cells.
static Kind kindAboutCtrl = "aboutCtrl";

struct SumatraLogo;

struct AboutCtrl : VirtCtrl {
    // the two text columns; owned here (not a VirtCtrl child)
    Table* table = nullptr;
    // "Show frequently read", bottom right of the About page (not the window)
    VirtLink* showFreqRead = nullptr;
    // the colored app name on top of the box; hidden in the home-page dropdown
    SumatraLogo* logo = nullptr;
    // copies version / OS / machine info for bug reports (dialog and dropdown)
    VirtButton* copyInfoBtn = nullptr;
    bool hideLogo = false;

    // geometry, computed by UpdateLayout()
    Rect aboutRect;  // the framed box
    Size headerSize; // the "SumatraPDF" band on top of it
    int dividerX = 0;

    AboutCtrl();
    ~AboutCtrl() override;
    void Sync();
    void UpdateLayout(Rect clientRc);
    VirtText* LeftAt(int i);
    VirtText* RightAt(int i);
    void Paint(VirtPaintCtx&) override;
    void PaintChildren(VirtPaintCtx&) override;
    int LayoutChildCount() override;
    ILayout* LayoutChildAt(int) override;
};

static void OpenAboutUrl(VirtMouseEvent* ev) {
    auto* link = (VirtLink*)ev->target;
    OpenTipUrl(link->target);
}

void SetPromoString(Str s) {
    if (len(s) == 0) return;
    str::ReplaceWithCopy(&promoFromServer, s);
}

static TempStr GetAppVersionTemp() {
    TempStr s = str::DupTemp(StrL("v" CURR_VERSION_STRA));
    if (IsProcess64()) {
        s = str::JoinTemp(s, StrL(" 64-bit"));
    } else {
        s = str::JoinTemp(s, StrL(" 32-bit"));
    }
    return s;
}

constexpr Color kCol1 = MkRgb(196, 64, 50);
constexpr Color kCol2 = MkRgb(227, 107, 35);
constexpr Color kCol3 = MkRgb(93, 160, 40);
constexpr Color kCol4 = MkRgb(69, 132, 190);
constexpr Color kCol5 = MkRgb(112, 115, 207);

static Kind kindSumatraLogo = "sumatraLogo";

// the app name centered in its bounds, each letter in a different color (so it
// can't be a VirtText). The version isn't part of it: it is the first row of
// the About table
struct SumatraLogo : VirtCtrl {
    PlatformFont* font = nullptr; // not owned

    SumatraLogo();
    Size GetIdealSize() override;
    void Paint(VirtPaintCtx&) override;
};

SumatraLogo::SumatraLogo() {
    kind = kindSumatraLogo;
    flags |= vwfNoHitTest;
}

// the app logo (IDB_MITHEN_LOGO), loaded once into a Pixmap so either drawing
// backend can composite it
static Pixmap* GetMithenLogoPixmap() {
    static Pixmap* logo = nullptr;
    static bool tried = false;
    if (tried) {
        return logo;
    }
    tried = true;
    LoadedDataResource res;
    if (!LockDataResource(IDB_MITHEN_LOGO, &res, nullptr) || !res.data || res.dataSize <= 0) {
        return nullptr;
    }
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, res.dataSize);
    if (!hMem) {
        return nullptr;
    }
    void* pMem = GlobalLock(hMem);
    memcpy(pMem, res.data, (size_t)res.dataSize);
    GlobalUnlock(hMem);
    IStream* stream = nullptr;
    Gdiplus::Bitmap* bmp = nullptr;
    if (SUCCEEDED(CreateStreamOnHGlobal(hMem, TRUE, &stream)) && stream) {
        bmp = Gdiplus::Bitmap::FromStream(stream);
        stream->Release();
    }
    if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
        logo = PixmapFromGdiplus(bmp);
        if (logo) {
            logo->hasAlpha = true;
        }
    }
    delete bmp;
    return logo;
}

Size SumatraLogo::GetIdealSize() {
    int dy = DpiScale(60);
    int dx = dy;
    Pixmap* logo = GetMithenLogoPixmap();
    if (logo && logo->height > 0) {
        dx = (logo->width * dy) / logo->height;
    }
    return Size{dx, dy};
}

void SumatraLogo::Paint(VirtPaintCtx& ctx) {
    Pixmap* logo = GetMithenLogoPixmap();
    if (!logo || logo->width <= 0 || logo->height <= 0) {
        return;
    }
    Rect r = ctx.bounds;
    float aspect = (float)logo->width / (float)logo->height;
    int dy = r.dy;
    int dx = (int)(dy * aspect);
    if (dx > r.dx) {
        dx = r.dx;
        dy = (int)(dx / aspect);
    }
    int x = r.x + ((r.dx - dx) / 2);
    int y = r.y + ((r.dy - dy) / 2);
    ctx.gfx->DrawPixmap(logo, Rect(x, y, dx, dy));
}

static TempStr TrimGitTemp(Str s) {
    if (gitCommidId && str::EndsWith(s, gitCommidId)) {
        int sLen = len(s);
        int gitLen = len(gitCommidId);
        return str::DupTemp(Str(s.s, sLen - gitLen - 7));
    }
    return s;
}

// the About screen's virtual controls for one HWND. Positions come from
// AboutCtrl::UpdateLayout(), so the root must not run a layout of its own
static AboutCtrl* EnsureAboutCtrl(VirtRoot** rootPtr, HWND hwnd, Rect clientRc) {
    VirtRoot* root = *rootPtr;
    if (!root) {
        root = new VirtRoot(hwnd);
        *rootPtr = root;
    }
    if (!IsVirtCtrlOfKind(root->owned, kindAboutCtrl)) {
        root->SetChild(new AboutCtrl());
    }
    root->bounds = clientRc;
    root->needsLayout = false;
    auto* about = (AboutCtrl*)root->owned;
    about->SetBounds(clientRc);
    return about;
}

static int AboutRowCount() {
    int n = 0;
    for (AboutRow* el = gAboutRows; el->leftTxt; el++) {
        n++;
    }
    return n;
}

AboutCtrl::AboutCtrl() {
    kind = kindAboutCtrl;
    flags |= vwfNoHitTest;
    table = new Table();
    logo = new SumatraLogo();
    AddChild(logo);
}

AboutCtrl::~AboutCtrl() {
    delete table;
    table = nullptr;
}

VirtText* AboutCtrl::LeftAt(int i) {
    return (VirtText*)table->GetCell(i, 0);
}

VirtText* AboutCtrl::RightAt(int i) {
    return (VirtText*)table->GetCell(i, 1);
}

// framed box: title band, body, then children; no border in MithenPDF
void AboutCtrl::Paint(VirtPaintCtx& ctx) {
    Rect rect = aboutRect;
    if (rect.IsEmpty()) {
        return;
    }
    Color bgCol = ThemeMainWindowBackgroundColor();
    ctx.gfx->FillRect(rect, bgCol);
}

// paint logo (VirtCtrl children) and the table's VirtText / VirtLink cells
void AboutCtrl::PaintChildren(VirtPaintCtx& ctx) {
    VirtCtrl::PaintChildren(ctx);
    if (table) {
        for (int i = 0; i < table->LayoutChildCount(); i++) {
            VirtCtrl* v = table->LayoutChildAt(i)->AsVirtCtrl();
            if (!v) {
                continue;
            }
            v->SetRoot(root);
            // cells were given absolute window coords by Table::SetBounds
            v->PaintTree(ctx.gfx, {0, 0}, ctx.clip);
        }
    }

    if (table && !table->lastBounds.IsEmpty()) {
        Color lineCol = ThemeWindowTextColor();
        Rect t = table->lastBounds;
        ctx.gfx->DrawLine({dividerX, t.y, 0, t.dy}, lineCol, kAboutLineSepSize);
    }
}

// table is not a VirtCtrl child; expose it so ElementFromPoint walks the cells
int AboutCtrl::LayoutChildCount() {
    return VirtCtrl::LayoutChildCount() + (table ? 1 : 0);
}

ILayout* AboutCtrl::LayoutChildAt(int i) {
    int n = VirtCtrl::LayoutChildCount();
    if (i < n) {
        return VirtCtrl::LayoutChildAt(i);
    }
    return table;
}

// build the table once, then keep text, fonts and colors in step with the theme
// and the DPI. Sizing happens in UpdateLayout(), which measures what we set here
void AboutCtrl::Sync() {
    int n = AboutRowCount();
    bool canAccessDisk = CanAccessDisk();
    if (table->rows != n) {
        table->SetSize(n, 2);
        for (int i = 0; i < n; i++) {
            AboutRow* el = &gAboutRows[i];
            TableCell& left = table->SetCell(i, 0, new VirtText(el->leftTxt));
            // the left column is flush against the divider line
            left.alignH = CrossAxisAlign::CrossEnd;
            left.alignV = CrossAxisAlign::CrossCenter;

            VirtText* rightTxt;
            if (el->url) {
                auto* link = new VirtLink(el->rightTxt);
                link->SetTarget(el->url);
                link->SetTooltip(el->url);
                link->withUnderline = true;
                // the underline sat 3px above the bottom of the text box
                link->underlineOffsetY = -3;
                link->onClick = MkFunc1Void(OpenAboutUrl);
                rightTxt = link;
            } else {
                rightTxt = new VirtText(el->rightTxt);
            }
            TableCell& right = table->SetCell(i, 1, rightTxt);
            right.alignV = CrossAxisAlign::CrossCenter;
        }
    }

    logo->font = GetUserGuiFont(kSumatraTxtFont, DpiScale(kSumatraTxtFontSize));

    PlatformFont* fontLeftTxt = GetUserGuiFont(Str(kLeftTextFont), DpiScale(kLeftTextFontSize));
    PlatformFont* fontRightTxt = GetUserGuiFont(Str(kRightTextFont), DpiScale(kRightTextFontSize));
    Color colText = ThemeWindowTextColor();
    Color colLink = ThemeWindowLinkColor();

    for (int i = 0; i < n; i++) {
        AboutRow* el = &gAboutRows[i];
        VirtText* left = LeftAt(i);
        left->font = fontLeftTxt;

        VirtText* right = RightAt(i);
        right->font = fontRightTxt;
        bool isLink = canAccessDisk && el->url;
        // the right column is a link when the row has a url we can open
        right->SetColor(kColText, isLink ? colLink : colText);
        // without disk access the url can't be opened, so it isn't a link
        right->withUnderline = isLink;
        right->SetFlag(vwfNoHitTest, !isLink);
        right->SetText(el->rightTxt ? TrimGitTemp(el->rightTxt) : Str(GetAppVersionTemp()));
    }
}

// the About box is the title band above the two-column table. This sizes it from
// the table, centers it in clientRc and positions the table inside it
void AboutCtrl::UpdateLayout(Rect clientRc) {
    bool showLogo = logo && logo->GetVisibility() != Visibility::Collapse;
    bool showCopy = copyInfoBtn && copyInfoBtn->GetVisibility() != Visibility::Collapse;
    headerSize = showLogo ? logo->GetIdealSize() : Size{};

    int leftRightSpaceDx = DpiScale(kAboutLeftRightSpaceDx);
    int marginDx = DpiScale(kAboutMarginDx);
    int aboutTxtDy = DpiScale(kAboutTxtDy);

    table->colGap = 2 * leftRightSpaceDx;
    table->rowGap = aboutTxtDy;
    Size tableSize = table->Layout(ExpandInf());

    Size btnSz{};
    int gap = DpiScale(12);
    int padBottom = DpiScale(kAboutRectPadding);
    int copyBlockDy = 0;
    if (showCopy) {
        btnSz = copyInfoBtn->GetIdealSize();
        copyBlockDy = gap + btnSz.dy + padBottom;
    }

    Rect r;
    // the divider line is drawn inside the gap between the two columns
    r.dx = std::max(tableSize.dx + kAboutLineSepSize, headerSize.dx) + (2 * kAboutLineOuterSize) + (2 * marginDx);
    if (showCopy) {
        r.dx = std::max(r.dx, btnSz.dx + (2 * marginDx) + (2 * kAboutLineOuterSize));
    }
    // one extra row gap so the last row isn't flush against the frame
    r.dy = headerSize.dy + tableSize.dy + aboutTxtDy + (2 * kAboutLineOuterSize) + 4 + copyBlockDy;
    r.x = clientRc.x + ((clientRc.dx - r.dx) / 2);
    if (hideLogo) {
        r.y = clientRc.y;
    } else if (showCopy) {
        r.y = clientRc.y + DpiScale(kAboutRectPadding);
    } else {
        r.y = clientRc.y + ((clientRc.dy - r.dy) / 2);
    }
    aboutRect = r;

    if (showLogo) {
        logo->SetBounds({r.x + ((r.dx - headerSize.dx) / 2), r.y, headerSize.dx, headerSize.dy});
    }

    int x = r.x + kAboutLineOuterSize + marginDx;
    int y = r.y + (showLogo ? headerSize.dy : kAboutLineOuterSize) + 4;
    table->SetBounds({x, y, tableSize.dx, tableSize.dy});
    dividerX = table->CellRect(0, 1).x - leftRightSpaceDx;

    if (showCopy) {
        int btnX = r.x + ((r.dx - btnSz.dx) / 2);
        int btnY = r.y + r.dy - padBottom - btnSz.dy;
        copyInfoBtn->SetBounds({btnX, btnY, btnSz.dx, btnSz.dy});
    }
}

// Version, OS, WebView2, memory and similar facts for a bug report.
static void AppendBugReportInfo(str::Builder& s) {
    s.Append(fmt("SumatraPDF %s\n", GetAppVersionTemp()));
    s.Append(fmt("Built on: %s %s\n", StrL(__DATE__), StrL(__TIME__)));
    if (gitCommidId) {
        s.Append(fmt("Git: %s\n", gitCommidId));
    }
    Str exeType = IsDllBuild() ? StrL("dll") : StrL("static");
    Str instType = IsRunningInPortableMode() ? StrL("portable") : StrL("installed");
    s.Append(fmt("Type: %s, %s\n", exeType, instType));
    if (gIsPreReleaseBuild) {
        s.Append(StrL("Pre-release: yes\n"));
    }
    if (gIsAsanBuild) {
        s.Append(StrL("ASan: yes\n"));
    }

    OSVERSIONINFOEX ver{};
    if (GetOsVersion(ver)) {
        TempStr os = OsNameFromVerTemp(ver);
        int buildNumber = (int)ver.dwBuildNumber & 0xFFFF;
        Str arch = StrL("64-bit");
        if (IsProcess32()) {
            arch = IsRunningInWow64() ? StrL("32-bit (Wow64)") : StrL("32-bit");
        }
        s.Append(fmt("OS: Windows %s, build %d, %s\n", os, buildNumber, arch));
    }
    if (IsOs64()) {
        s.Append(StrL("OS architecture: 64-bit\n"));
    } else {
        s.Append(StrL("OS architecture: 32-bit\n"));
    }

    TempStr wv = GetWebView2VersionTemp();
    if (len(wv) == 0) {
        s.Append(StrL("WebView2: not installed\n"));
    } else {
        s.Append(fmt("WebView2: %s\n", wv));
    }

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        float physMemGB = (float)ms.ullTotalPhys / (float)(1024 * 1024 * 1024);
        s.Append(fmt("Physical memory: %.2f GB (%d%% in use)\n", physMemGB, (int)ms.dwMemoryLoad));
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    s.Append(fmt("Processors: %d\n", (int)si.dwNumberOfProcessors));
    TempStr cpuName = ReadRegStrTemp(HKEY_LOCAL_MACHINE, StrL(R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)"),
                                     StrL("ProcessorNameString"));
    if (cpuName) {
        s.Append(fmt("Processor: %s\n", cpuName));
    }

    int screenDx = GetSystemMetrics(SM_CXSCREEN);
    int screenDy = GetSystemMetrics(SM_CYSCREEN);
    int dpi = DpiGet();
    s.Append(fmt("Screen: %dx%d, DPI %d (%d%%)\n", screenDx, screenDy, dpi, MulDiv(dpi, 100, 96)));

    char country[32] = {}, lang[32]{};
    GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, country, dimof(country) - 1);
    GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SISO639LANGNAME, lang, dimof(lang) - 1);
    s.Append(fmt("Locale: %s-%s\n", Str(lang), Str(country)));

    Str theme = ThemeGetNameAt(ThemeGetCurrentIndex());
    if (theme) {
        s.Append(fmt("Theme: %s\n", theme));
    }
    if (IsRunningOnWine()) {
        s.Append(StrL("Wine: yes\n"));
    }
}

static void CopyAboutInfoToClipboard() {
    str::Builder info;
    info.Reserve(1024);
    AppendBugReportInfo(info);
    CopyTextToClipboard(ToStr(info));
}

static void OnCopyProgramInfo(VirtMouseEvent*) {
    CopyAboutInfoToClipboard();
}

// prepares the About tree for hwnd and computes its geometry
static AboutCtrl* UpdateAboutLayout(VirtRoot** rootPtr, HWND hwnd, Rect clientRc, bool hover = false) {
    DpiSetFromHwnd(hwnd);
    AboutCtrl* about = EnsureAboutCtrl(rootPtr, hwnd, clientRc);
    about->hideLogo = hover;
    if (about->logo) {
        about->logo->SetVisibility(hover ? Visibility::Collapse : Visibility::Visible);
    }
    about->Sync();
    bool showCopy = hover || (hwnd == gHwndAbout);
    if (showCopy) {
        if (!about->copyInfoBtn) {
            about->copyInfoBtn =
                NewThemedButton(hwnd, Tr("Copy program and machine info to clipboard"), GetAppFont(), false);
            about->copyInfoBtn->onClick = MkFunc1Void(OnCopyProgramInfo);
            about->AddChild(about->copyInfoBtn);
        }
        about->copyInfoBtn->font = GetAppFont();
        about->copyInfoBtn->SetVisibility(Visibility::Visible);
    } else if (about->copyInfoBtn) {
        about->copyInfoBtn->SetVisibility(Visibility::Collapse);
    }
    about->UpdateLayout(clientRc);
    return about;
}

/* Draws the about screen. The text columns are painted by the AboutCtrl tree;
   this draws the frame around them. It transcribes the design I did in graphics
   software - hopeless to understand without seeing the design. */
static void DrawAbout(Gfx* gfx, VirtRoot* root, Rect clientRc) {
    auto* about = (AboutCtrl*)root->owned;
    Color bgCol = ThemeMainWindowBackgroundColor();
    gfx->FillRect(clientRc, bgCol);

#ifdef ABOUT_USE_LESS_COLORS
    Color lineCol = ThemeWindowTextColor();
    Rect titleRect(about->aboutRect.TL(), about->headerSize);
    Rect titleBgBand(0, about->aboutRect.y, clientRc.dx, titleRect.dy);
    gfx->FillRect(titleBgBand, bgCol);
    gfx->DrawLine(Rect(0, about->aboutRect.y, clientRc.dx, 0), lineCol);
    gfx->DrawLine(Rect(0, about->aboutRect.y + titleRect.dy, clientRc.dx, 0), lineCol);
#endif

    root->Paint(gfx, clientRc);
}

static void OnPaintAbout(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    SetLayout(hdc, kLayoutLtr);
    Rect clientRc = HwndClientRect(hwnd);
    UpdateAboutLayout(&gAboutRoot, hwnd, clientRc);
    Gfx* gfx = GfxCreate(hdc);
    DrawAbout(gfx, gAboutRoot, clientRc);
    delete gfx;
    EndPaint(hwnd, &ps);
}

static void CreateInfotipForLink(Str tooltip, const Rect& rc) {
    if (gAboutTooltip != nullptr) {
        return;
    }

    Tooltip::CreateArgs args;
    args.parent = gHwndAbout;
    args.font = GetAppFont();
    args.isRtl = IsUIRtl();

    gAboutTooltip = new Tooltip();
    gAboutTooltip->Create(args);
    gAboutTooltip->SetSingle(tooltip, rc, false);
}

static void DeleteInfotip() {
    if (gAboutTooltip == nullptr) {
        return;
    }
    // gAboutTooltip->Hide();
    delete gAboutTooltip;
    gAboutTooltip = nullptr;
}

static LRESULT CALLBACK WndProcAbout(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Point pt;

    // the links are VirtLinks: let the tree hit-test, click and set the
    // cursor. Its GetTooltipTemp() drives this window's own Tooltip
    if (gAboutRoot && gAboutRoot->owned) {
        LRESULT res = 0;
        switch (msg) {
            case WM_MOUSEMOVE:
            case WM_MOUSELEAVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
                if (gAboutRoot->OnMessage(msg, wp, lp, res)) {
                    return res;
                }
                break;
            case WM_SETCURSOR: {
                pt = HwndGetCursorPos(hwnd);
                Point ptLocal{0, 0};
                ILayout* el = ElementFromPoint(gAboutRoot, pt, &ptLocal);
                VirtCtrl* w = el ? el->AsVirtCtrl() : nullptr;
                if (w && w->OnSetCursor(ptLocal)) {
                    TempStr tip = w->GetTooltipTemp(ptLocal);
                    if (tip && *tip.s) {
                        Rect r = w->BoundsInWindow();
                        CreateInfotipForLink(tip, r);
                    }
                    return TRUE;
                }
                DeleteInfotip();
                return DefWindowProc(hwnd, msg, wp, lp);
            }
        }
    }

    switch (msg) {
        case WM_CREATE:
            ReportIf(gHwndAbout);
            DarkModeApplyToTitleBar(hwnd);
            break;

        case WM_ERASEBKGND:
            // do nothing, helps to avoid flicker
            return TRUE;

        case WM_PAINT:
            OnPaintAbout(hwnd);
            break;

        case WM_SETCURSOR:
            DeleteInfotip();
            return DefWindowProc(hwnd, msg, wp, lp);

        case WM_CHAR:
            if (VK_ESCAPE == wp) {
                DestroyWindow(hwnd);
            }
            break;

        case WM_KEYDOWN:
            if ('C' == wp && IsCtrlPressed()) {
                CopyAboutInfoToClipboard();
            }
            break;

        case WM_COMMAND:
            if (CmdCopySelection == LOWORD(wp)) {
                CopyAboutInfoToClipboard();
            }
            break;

        case WM_DESTROY:
            DeleteInfotip();
            delete gAboutRoot;
            gAboutRoot = nullptr;
            ReportIf(!gHwndAbout);
            gHwndAbout = nullptr;
            break;

        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

constexpr const WCHAR* kAboutClassName = L"SUMATRA_PDF_ABOUT";

void ShowAboutWindow(MainWindow* win) {
    if (gHwndAbout) {
        SetActiveWindow(gHwndAbout);
        return;
    }

    if (!gAtomAbout) {
        WNDCLASSEX wcex;
        FillWndClassEx(wcex, kAboutClassName, WndProcAbout);
        HMODULE h = GetModuleHandleW(nullptr);
        wcex.hIcon = LoadIcon(h, MAKEINTRESOURCE(GetAppIconID()));
        gAtomAbout = RegisterClassEx(&wcex);
        ReportIf(!gAtomAbout);
    }

    WCHAR* title = CWStrTemp(Tr("About MithenPDF"));
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    int dx = CW_USEDEFAULT;
    int dy = CW_USEDEFAULT;
    HINSTANCE h = GetModuleHandleW(nullptr);
    gHwndAbout = CreateWindowExW(0, kAboutClassName, title, style, x, y, dx, dy, nullptr, nullptr, h, nullptr);
    if (!gHwndAbout) {
        return;
    }

    HwndSetRtl(gHwndAbout, IsUIRtl());

    // get the dimensions required for the about box's content
    AboutCtrl* about = UpdateAboutLayout(&gAboutRoot, gHwndAbout, HwndClientRect(gHwndAbout));
    int rectPadding = DpiScale(kAboutRectPadding);
    dx = about->aboutRect.dx + (2 * rectPadding);
    dy = about->aboutRect.dy + (2 * rectPadding);

    // resize the new window to just match these dimensions
    Rect wRc = HwndWindowRect(gHwndAbout);
    Rect cRc = HwndClientRect(gHwndAbout);
    wRc.dx += dx - cRc.dx;
    wRc.dy += dy - cRc.dy;
    MoveWindow(gHwndAbout, wRc.x, wRc.y, wRc.dx, wRc.dy, FALSE);

    HwndPositionInCenterOf(gHwndAbout, win->hwndFrame);
    ShowWindow(gHwndAbout, SW_SHOW);
}

static void ShowFrequentlyRead(VirtMouseEvent* ev) {
    auto* win = (MainWindow*)ev->target->userData;
    gSettings->showStartPage = true;
    HomePageRelayout(win);
    win->RedrawAll(true);
}

void DrawAboutPage(MainWindow* win, Gfx* gfx) {
    HWND hwnd = win->hwndCanvas;
    Rect clientRc = HwndClientRect(hwnd);
    AboutCtrl* about = UpdateAboutLayout(&win->homeRoot, hwnd, clientRc);

    bool showLink = HasPermission(Perm::SavePreferences | Perm::DiskAccess) && SettingsRememberOpenedFiles();
    if (showLink && !about->showFreqRead) {
        auto* link = new VirtLink(Tr("Show frequently read"));
        link->withUnderline = true;
        link->isRtl = IsUIRtl();
        link->userData = (uintptr_t)win;
        link->onClick = MkFunc1Void(ShowFrequentlyRead);
        about->showFreqRead = link;
        about->AddChild(link);
    }
    if (about->showFreqRead) {
        VirtLink* link = about->showFreqRead;
        link->visibility = showLink ? Visibility::Visible : Visibility::Collapse;
        link->font = GetUserGuiFont(StrL("MS Shell Dlg"), DpiScale(16));
        link->sz = {0, 0}; // re-measure: the font may have changed with the DPI
        Size txtSize = link->GetIdealSize(true);
        Rect r = {0, 0, txtSize.dx, txtSize.dy};
        PositionRB(clientRc, r);
        MoveXY(r, -DpiScale(kInnerPadding), -DpiScale(kInnerPadding));
        link->SetBounds(r);
    }
    DrawAbout(gfx, win->homeRoot, clientRc);
}

/* alternate static page to display when no document is loaded */

// HomePageViewMode setting ("thumbnails" or "list")
bool HomePageIsListView() {
    return gSettings && str::EqI(gSettings->homePageViewMode, StrL("list"));
}

void SetHomePageListView(bool listView) {
    Str mode = listView ? StrL("list") : StrL("thumbnails");
    str::ReplaceWithCopy(&gSettings->homePageViewMode, mode);
}

struct HomePageLayout {
    // args in
    Gfx* gfx = nullptr;
    Rect rc;
    MainWindow* win = nullptr;

    Rect rcIconOpen;
    Rect rcLogo;

    VirtText* freqRead = nullptr;
    VirtText* openDoc = nullptr;
    VirtText* hideShowFreqRead = nullptr;
    Rect rcSearchBorder; // border rect drawn around the edit control

    // tip layout
    Rect rcTip;     // background rect for tip area
    Rect rcTipText; // where the markup goes inside the band
    bool hasTip = false;

    ~HomePageLayout();
};

// freqRead / openDoc are borrowed from the persistent chrome tree (owned by
// win->homeRoot), not created per layout
HomePageLayout::~HomePageLayout() = default;

// --- home page chrome as a VirtCtrl tree ---
// The chrome (header, view-mode buttons, "Open a document..." link, logo row)
// lives for as long as the window, so hover / pressed state survives the
// repaints that scrolling and filtering cause. Geometry still comes from
// LayoutHomePage(): HomePageSyncChrome() just feeds it into the tree. The
// [palette] logo [help] row is an HBox of virt controls.

// Leaf home-page controls: no MainWindow*. Wire onClick / hwndForCmds when
// building the chrome so the same VirtCtrl types stay reusable.

struct HomeOpenDocCtrl : VirtCtrl {
    Pixmap* pixmap = nullptr; // not owned, from GetCachedPixmapForSvg()
    VirtText* text = nullptr; // child
    // icon position, relative to our bounds
    Rect rcIconLocal;

    HomeOpenDocCtrl();
    void Paint(VirtPaintCtx&) override;
};

// white-circle home-page buttons: "?" keyboard help and the command palette
struct HomeCircleBtnCtrl : VirtCtrl {
    Pixmap* pixmap = nullptr; // not owned, from GetCachedPixmapForSvg(); null → glyph
    Str glyph;                // not owned; used when pixmap is null ("?")

    HomeCircleBtnCtrl();
    Size GetIdealSize() override;
    void Paint(VirtPaintCtx&) override;
};

// [command palette] SumatraPDF [keyboard shortcuts] along the top of the home
// page. An HBox sizes and places the three virt controls; they are also our
// VirtCtrl children so paint / hit-test / delete stay on this tree.
struct HomeLogoRow : VirtCtrl {
    HBox* box = nullptr;

    HomeLogoRow();
    ~HomeLogoRow() override;

    void AddItem(VirtCtrl*);
    Size GetIdealSize() override;
    void SetBounds(Rect) override;
};

// the tip band at the bottom. The markup is its VirtRichText child, which draws
// itself and runs its own links; clicking the band anywhere else picks another
// tip
struct HomeTipCtrl : VirtCtrl {
    // for link commands inside the tip markup (like VirtRichText)
    HWND hwndForCmds = nullptr;
    // onClick (VirtCtrl): band click outside a link picks another tip
    VirtRichText* rich = nullptr; // owned, as our only child
    Str richFor;                  // owned, the markup `rich` was parsed from

    ~HomeTipCtrl() override;
    void SetTipLine(Str line, PlatformFont* font);
    void Sync(const Rect& rcTip, const Rect& rcText);
    void Paint(VirtPaintCtx&) override;
};

// paints the border and background around the home search edit (the edit
// itself is a real HWND on top). Decoration only, never a click target
struct HomeSearchBorderCtrl : VirtCtrl {
    HomeSearchBorderCtrl();
    void Paint(VirtPaintCtx&) override;
};

static Kind kindHomeChromeCtrl = "homeChromeCtrl";

struct HomeChromeCtrl : VirtCtrl {
    HomeTipCtrl* tip = nullptr;
    HomeSearchBorderCtrl* searchBorder = nullptr;
    VirtText* hdr = nullptr;
    HomeLogoRow* logoRow = nullptr;
    SumatraLogo* logo = nullptr;
    HomeOpenDocCtrl* openDoc = nullptr;
    HomeCircleBtnCtrl* paletteBtn = nullptr;
    HomeCircleBtnCtrl* helpBtn = nullptr;
    // chrome-less About dropdown under the logo; shown after the tooltip delay
    VirtHost* aboutHover = nullptr;

    ~HomeChromeCtrl() override;
};

static HomeChromeCtrl* EnsureHomeChrome(MainWindow* win);
static HomeChromeCtrl* HomeChrome(MainWindow* win);
static void HideHomeAboutHover(MainWindow* win);
static void ShowHomeAboutHover(MainWindow* win);
static void HomePageSyncChrome(HomePageLayout& l);

static int HomePageIconSize() {
    int sz = DpiScale(gSettings->toolbarSize);
    if (sz < 1) {
        sz = DpiScale(16);
    }
    return RoundUp(sz, 4);
}

constexpr int kSearchEditDy = 28;

static PlatformFont* HomePageFont(int size) {
    return GetUserGuiFont(StrL("MS Shell Dlg"), DpiScale(size));
}

struct HomeSearchEdit : Edit {
    MainWindow* win = nullptr;

    void WndProc(ControlBase::WndProcEvent* ev) {
        if (ev->msg == WM_KEYDOWN && ev->wparam == VK_ESCAPE) {
            SetText(StrL(""));
            if (win) {
                HwndSetFocus(win->hwndCanvas);
                win->RedrawAll(true);
            }
            ev->result = 0;
            ev->didHandle = true;
            return;
        }
        if (ev->msg == WM_MOUSEWHEEL) {
            // the home page scrolls, not the one-line edit
            ev->result = SendMessageW(GetParent(ev->hwnd), ev->msg, ev->wparam, ev->lparam);
            ev->didHandle = true;
            return;
        }
        Edit::WndProc(ev);
    }
};

// Cue banner when the search field is empty.
static void UpdateHomeSearchCueBanner(MainWindow* win) {
    if (!win || !win->homeSearch) {
        return;
    }
    // Tr returns Str; pass it as an object into type-safe fmt.
    TempStr cue = fmt("%s", Tr("Search (Ctrl + F)"));
    EditSetCueText(win->homeSearch, cue);
}

static void HomeSearchTextChanged(MainWindow* win) {
    win->homePageScrollY = 0;
    HomePageRelayout(win);
    HwndInvalidate(win->hwndCanvas);
}

// repaint when focus enters/leaves the search box
static void HomeSearchFocusChanged(MainWindow* win) {
    HwndInvalidate(win->hwndCanvas);
}

static void PlaceHomeSearchEdit(MainWindow* win, const Rect& rcSearchBorder) {
    if (!win || !win->homeSearchLayout || rcSearchBorder.IsEmpty()) {
        return;
    }
    int searchEditDy = DpiScale(kSearchEditDy);
    Rect rcEdit = {rcSearchBorder.x + 1, rcSearchBorder.y + 1, rcSearchBorder.dx - 2, searchEditDy};
    LayoutToSize(win->homeSearchLayout, rcEdit.Size());
    win->homeSearchLayout->SetBounds(rcEdit);
}

static void EnsureHomeSearchCreated(MainWindow* win) {
    if (win->homeSearch) {
        UpdateHomeSearchCueBanner(win);
        return;
    }
    HWND parent = win->hwndCanvas;
    PlatformFont* font = HomePageFont(14);

    Edit::CreateArgs args;
    args.parent = parent;
    args.font = font;
    // the home page draws the box around it, so the edit has no border of its own
    auto* e = new HomeSearchEdit();
    e->win = win;
    e->Create(args);
    // Edit::Create wired Edit::WndProc; re-route to HomeSearchEdit for Esc/wheel
    e->onWndProc = MkMethod1<HomeSearchEdit, ControlBase::WndProcEvent*, &HomeSearchEdit::WndProc>(e);
    e->SetColors(ThemeWindowTextColor(), ThemeControlBackgroundColor());
    e->onTextChanged = MkFunc0(HomeSearchTextChanged, win);
    e->onFocus = MkFunc0(HomeSearchFocusChanged, win);
    e->onKillFocus = MkFunc0(HomeSearchFocusChanged, win);
    win->homeSearch = e;
    UpdateHomeSearchCueBanner(win);
    // add left/right padding so text doesn't overlap the border
    int margin = DpiScale(6);
    EditSetMargins(e, margin, margin);
    // restore the query from before the edit control was destroyed
    // (e.g. by switching to a document tab and back)
    if (len(win->homeSearchQuery) > 0) {
        e->SetText(win->homeSearchQuery);
    }
    // the box is kSearchEditDy tall but the edit is only as tall as its text,
    // so a one-child HBox centers it in there instead of us doing that by hand
    auto* box = new HBox();
    box->alignCross = CrossAxisAlign::CrossCenter;
    box->AddChild(e, 1);
    win->homeSearchLayout = box;
    e->SetIsVisible(false);
}

void HomePageHideSearch(MainWindow* win) {
    if (win && win->homeSearch) {
        win->homeSearch->SetIsVisible(false);
    }
}

void HomePageDestroySearch(MainWindow* win) {
    if (!win || !win->homeSearch) {
        return;
    }
    TempStr query = win->homeSearch->GetTextTemp();
    str::ReplaceWithCopy(&win->homeSearchQuery, query);
    // destroying the edit's window pumps messages, and the canvas answers most
    // of them by calling us again (see WndProcCanvas), so drop our pointers
    // before deleting - otherwise the re-entered call deletes the tree twice
    ILayout* layout = win->homeSearchLayout;
    win->homeSearchLayout = nullptr;
    win->homeSearch = nullptr;
    // the layout owns the edit
    delete layout;
}

// after a theme change; the edit paints itself from these (see
// Edit::OnMessageReflect and the reflection in WndProcCanvas)
void HomePageUpdateSearchColors(MainWindow* win) {
    if (win->homeSearch) {
        win->homeSearch->SetColors(ThemeWindowTextColor(), ThemeControlBackgroundColor());
    }
}

// The search edit survives while the frame moves between monitors. Its HFONT
// and text margins do not follow WM_DPICHANGED automatically, and the page's
// cached rectangles were measured at the previous DPI.
void HomePageOnDpiChanged(MainWindow* win, int dpi) {
    HideHomeAboutHover(win);
    ClearHomeLayoutCache(win);
    if (!win || dpi <= 0) {
        return;
    }
    if (win->homeSearch) {
        int fontSize = DpiScaleByDpi(dpi, 14);
        win->homeSearch->SetFont(GetUserGuiFont(StrL("MS Shell Dlg"), fontSize));
        int margin = DpiScaleByDpi(dpi, 6);
        EditSetMargins(win->homeSearch, margin, margin);
    }
    HomePageRelayout(win);
    if (win->hwndCanvas) {
        HwndInvalidate(win->hwndCanvas, true);
    }
}

void PickAnotherRandomPromotion() {
    PickAnotherRandomTip();
}

// --- layout cache: full LayoutHomePage only when content/size changes ---
struct HomePageLayoutCache {
    bool valid = false;
    int dpi = 0;
    Rect canvasRc;
    bool showTips = false;
    bool isRtl = false;
    int tipIdx = -1;
    bool tipIsPromo = false;

    Rect rcSearchBorder;
    Rect rcIconOpen;
    Rect rcLogo;
    Rect rcTip;
    Rect rcFreqRead;
    Rect rcOpenDoc;
    Rect rcTipText;
    bool hasTip = false;
};

// The layout belongs to the window: its home chrome entries are laid out and
// painted from these rects, so a second window must not overwrite them.
static HomePageLayoutCache& HomeLayout(MainWindow* win) {
    if (!win->homeLayout) {
        win->homeLayout = new HomePageLayoutCache();
    }
    return *win->homeLayout;
}

static void ClearHomeLayoutCache(MainWindow* win) {
    if (!win || !win->homeLayout) {
        return;
    }
    auto& c = *win->homeLayout;
    c.valid = false;
    c.hasTip = false;
}

// Layout caches belong to their window. Settings reloads and theme changes
// drop them; they are rebuilt on the next HomePageRelayout.
void HomePageInvalidateLayoutCache() {
    for (MainWindow* win : gWindows) {
        ClearHomeLayoutCache(win);
    }
}

void HomePageFocusSearch(MainWindow* win) {
    EnsureHomeSearchCreated(win);
    HomePageRelayout(win);
    if (win->homeSearch) {
        win->homeSearch->SetIsVisible(true);
        EditSetFocus(win->homeSearch);
    }
}

static bool HomeLayoutCacheMatches(MainWindow* win, const Rect& rc) {
    auto& c = HomeLayout(win);
    if (!c.valid) {
        return false;
    }
    if (c.dpi != DpiGet()) {
        return false;
    }
    if (c.canvasRc != rc) {
        return false;
    }
    if (c.showTips != (gSettings && gSettings->showTips)) {
        return false;
    }
    if (c.isRtl != IsUIRtl()) {
        return false;
    }
    if (c.tipIdx != gSelectedTipIdx || c.tipIsPromo != gSelectedIsPromo) {
        return false;
    }
    return true;
}

static void SaveHomeLayoutCache(const HomePageLayout& l) {
    auto& c = HomeLayout(l.win);
    c.valid = true;
    c.dpi = DpiGet();
    c.canvasRc = l.rc;
    c.showTips = gSettings && gSettings->showTips;
    c.isRtl = IsUIRtl();
    c.tipIdx = gSelectedTipIdx;
    c.tipIsPromo = gSelectedIsPromo;
    c.rcSearchBorder = l.rcSearchBorder;
    c.rcIconOpen = l.rcIconOpen;
    c.rcLogo = l.rcLogo;
    c.rcTip = l.rcTip;
    c.rcFreqRead = l.freqRead ? l.freqRead->lastBounds : Rect{};
    c.rcOpenDoc = l.openDoc ? l.openDoc->lastBounds : Rect{};
    c.rcTipText = l.rcTipText;
    c.hasTip = l.hasTip;
}

// rebuild chrome VirtText + copy cached geometry into l (no full layout)
static void ApplyHomeLayoutCache(HomePageLayout& l) {
    auto* win = l.win;
    auto& c = HomeLayout(win);
    bool isRtl = IsUIRtl();

    l.rcSearchBorder = c.rcSearchBorder;
    l.rcIconOpen = c.rcIconOpen;
    l.rcLogo = c.rcLogo;
    l.rcTip = c.rcTip;
    l.rcTipText = c.rcTipText;
    l.hasTip = c.hasTip;

    PlatformFont* hdrFont = HomePageFont(24);
    PlatformFont* fontText = HomePageFont(14);

    // the section title is no longer shown; empty bounds keep it unpainted
    HomeChromeCtrl* chrome = EnsureHomeChrome(win);
    VirtText* hdr = chrome->hdr;
    hdr->font = hdrFont;
    hdr->isRtl = isRtl;
    hdr->SetBounds(c.rcFreqRead);
    l.freqRead = hdr;

    TempStr openTxt = str::DupTemp(Tr("&Open..."));
    str::RemoveCharsInPlace(openTxt, StrL("&"));
    VirtText* openDoc = chrome->openDoc->text;
    openDoc->SetText(openTxt);
    openDoc->font = fontText;
    openDoc->isRtl = isRtl;
    openDoc->withUnderline = true;
    openDoc->SetBounds(c.rcOpenDoc);
    l.openDoc = openDoc;
}

static void LayoutHomePage(HomePageLayout& l) {
    EnsureTipsParsed();

    auto rc = l.rc;
    auto* win = l.win;
    bool isRtl = IsUIRtl();
    PlatformFont* fontText = HomePageFont(14);

    int innerPad = DpiScale(kInnerPadding);
    int contentX = rc.x + innerPad;
    int contentW = rc.dx - (2 * innerPad);

    HomeChromeCtrl* chrome = EnsureHomeChrome(win);
    // the section title is no longer shown; empty bounds keep it unpainted
    VirtText* hdr = chrome->hdr;
    hdr->SetBounds({});
    l.freqRead = hdr;

    int searchEditDy = DpiScale(kSearchEditDy);
    int borderDy = searchEditDy + 2; // 1px border on each side

    // [command palette] SumatraPDF [keyboard shortcuts], centered
    chrome->logo->font = GetUserGuiFont(kSumatraTxtFont, DpiScale(kSumatraTxtFontSize));
    Size logoRowSize = chrome->logoRow->GetIdealSize();
    int logoY = DpiScale(8);
    int logoX = contentX + ((contentW - logoRowSize.dx) / 2);
    if (logoX < innerPad) {
        logoX = innerPad;
    }
    l.rcLogo = {logoX, logoY, logoRowSize.dx, logoRowSize.dy};

    int hdrY = logoY + logoRowSize.dy + DpiScale(8);
    // every row item (link, search box) is centered on the row's vertical centerline
    Rect rcIconOpen(0, 0, 0, 0);
    rcIconOpen.dx = rcIconOpen.dy = HomePageIconSize();
    int rowDy = std::max(rcIconOpen.dy, borderDy);
    int centerY = hdrY + (rowDy / 2);

    /* "Open..." link at the left edge */
    TempStr openTxt = str::DupTemp(Tr("&Open..."));
    str::RemoveCharsInPlace(openTxt, StrL("&"));
    VirtText* openDoc = chrome->openDoc->text;
    openDoc->SetText(openTxt);
    openDoc->font = fontText;
    openDoc->isRtl = isRtl;
    openDoc->withUnderline = true;
    Size txtSize = openDoc->GetIdealSize(true);
    int openGroupDx = rcIconOpen.dx + 3 + txtSize.dx;

    rcIconOpen.x = contentX;
    rcIconOpen.y = centerY - (rcIconOpen.dy / 2);
    Rect rcOpenDoc(rcIconOpen.x + rcIconOpen.dx + 3, centerY - (txtSize.dy / 2), txtSize.dx, txtSize.dy);

    // the search box takes what is left of the content width; the open link
    // pads its left side so the box sits centered
    int rowGapX = DpiScale(16);
    int flankDx = openGroupDx + rowGapX;
    int borderDx = std::max(contentW - (2 * flankDx), DpiScale(200));
    int borderX = contentX + ((contentW - borderDx) / 2);
    l.rcSearchBorder = {borderX, hdrY + ((rowDy - borderDy) / 2), borderDx, borderDy};

    if (isRtl) {
        auto mirrorX = [&rc](Rect& r) { r.x = rc.dx - r.x - r.dx; };
        mirrorX(l.rcLogo);
        mirrorX(rcIconOpen);
        mirrorX(rcOpenDoc);
        mirrorX(l.rcSearchBorder);
    }
    l.rcIconOpen = rcIconOpen;
    openDoc->SetBounds(rcOpenDoc);
    l.openDoc = openDoc;

    // --- tip area at the bottom ---
    PlatformFont* fontTip = HomePageFont(16);
    HomeTipCtrl* tipCtrl = EnsureHomeChrome(l.win)->tip;
    tipCtrl->SetTipLine(SelectedTipLine(), fontTip);
    VirtRichText* tip = tipCtrl->rich;
    if (tip) {
        int tipPadding = DpiScale(8);
        int tipHeight = tip->MinIntrinsicHeight(contentW) + (2 * tipPadding);
        Rect rcClient = HwndClientRect(win->hwndCanvas);
        int tipY = rcClient.dy - tipHeight;
        // background spans full window width
        l.rcTip = {0, tipY, rcClient.dx, tipHeight};
        l.hasTip = true;

        // text area aligned with the header content
        l.rcTipText = {contentX, tipY + tipPadding, contentW, tip->MinIntrinsicHeight(contentW)};
    }
}

// a white circle with either a centered SVG or a black glyph ("?"). Drawn with
// GDI+ so the disc edge is smooth; nothing is painted outside it, so the page
// background shows through.
static void DrawHomeCircleButton(Gfx* gfx, Rect r, Pixmap* icon, Str glyph) {
    gfx->FillEllipse(r, kColWhite);
    if (icon) {
        int x = r.x + ((r.dx - icon->width) / 2);
        int y = r.y + ((r.dy - icon->height) / 2);
        gfx->DrawPixmap(icon, {x, y, icon->width, icon->height});
        return;
    }
    PlatformFont* font = HomePageFont(14);
    gfx->DrawText(glyph, r, gfxTextCenter | gfxTextVCenter, font, kColBlack);
}

static TempStr RectCsvTemp(const Rect& r) {
    return fmt("%d,%d,%d,%d", r.x, r.y, r.dx, r.dy);
}

// The home page's keyboard state. A test driving the home page with keys waits
// on this instead of sleeping after each key, which is what made
// tests/issue-1136 flaky: a key posted while focus was still moving went to the
// wrong window.
TempStr HomeSelectionResultTemp(int* exitCodeOut) {
    auto finish = [&](int code, TempStr s) -> TempStr {
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return s;
    };
    MainWindow* win = len(gWindows) > 0 ? gWindows[0] : nullptr;
    if (!win) {
        return finish(2, str::DupTemp(StrL("NOTREADY no-window")));
    }
    auto& c = HomeLayout(win);
    if (!c.valid) {
        return finish(2, str::DupTemp(StrL("NOTREADY no-layout")));
    }
    // the home page has no file entries anymore, so there is no selection
    bool searchFocus = win->homeSearch && GetFocus() == win->homeSearch->hwnd;
    return finish(
        0, fmt("OK sel=-1 entries=0 searchFocus=%d searchBox=%d search=%s path= listView=%d", searchFocus ? 1 : 0,
               win->homeSearch ? 1 : 0, RectCsvTemp(c.rcSearchBorder), HomePageIsListView() ? 1 : 0));
}

// What the home page list drew for each row. There is no file list anymore, so
// this always reports zero rows once the layout cache is valid. Used by
// tests/issue-5870.ts.
TempStr HomeListRowsResultTemp(int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](int code) -> TempStr {
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    MainWindow* win = len(gWindows) > 0 ? gWindows[0] : nullptr;
    if (!win) {
        out.Append(StrL("NOTREADY no-window\n"));
        return finish(2);
    }
    auto& c = HomeLayout(win);
    if (!c.valid) {
        out.Append(StrL("NOTREADY no-layout\n"));
        return finish(2);
    }
    out.Append(StrL("OK rows=0\n"));
    return finish(0);
}

//--- home page chrome VirtCtrls

HomeOpenDocCtrl::HomeOpenDocCtrl() {
    cursor = CursorId::Hand;
}

void HomeOpenDocCtrl::Paint(VirtPaintCtx& ctx) {
    if (!pixmap) {
        return;
    }
    Rect r = {ctx.bounds.x + rcIconLocal.x, ctx.bounds.y + rcIconLocal.y, pixmap->width, pixmap->height};
    ctx.gfx->DrawPixmap(pixmap, r);
}

HomeCircleBtnCtrl::HomeCircleBtnCtrl() {
    cursor = CursorId::Hand;
}

Size HomeCircleBtnCtrl::GetIdealSize() {
    int d = DpiScale(30);
    return {d, d};
}

void HomeCircleBtnCtrl::Paint(VirtPaintCtx& ctx) {
    DrawHomeCircleButton(ctx.gfx, ctx.bounds, pixmap, glyph);
}

HomeLogoRow::HomeLogoRow() {
    flags |= vwfNoHitTest;
    box = new HBox();
    box->alignMain = MainAxisAlign::MainStart;
    box->alignCross = CrossAxisAlign::CrossCenter;
}

HomeLogoRow::~HomeLogoRow() {
    // the buttons and logo are VirtCtrl children; don't let HBox delete them
    if (box) {
        VecClear(box->children);
        delete box;
        box = nullptr;
    }
}

void HomeLogoRow::AddItem(VirtCtrl* c) {
    AddChild(c);
    box->AddChild(c);
}

Size HomeLogoRow::GetIdealSize() {
    box->gap = DpiScale(10);
    return {box->MinIntrinsicWidth(0), box->MinIntrinsicHeight(0)};
}

void HomeLogoRow::SetBounds(Rect r) {
    VirtCtrl::SetBounds(r);
    box->gap = DpiScale(10);
    box->Layout(Tight(r.Size()));
    box->SetBounds(r);
}

HomeSearchBorderCtrl::HomeSearchBorderCtrl() {
    SetFlag(vwfNoHitTest, true);
}

// border and fill around the search edit; the edit HWND sits inside, so only
// the 1px frame and the padding around it are actually visible
void HomeSearchBorderCtrl::Paint(VirtPaintCtx& ctx) {
    Color bgCol = ThemeControlBackgroundColor();
    ctx.gfx->FillRect(ctx.bounds, bgCol);
    ctx.gfx->DrawRect(ctx.bounds, AccentColor(bgCol, 40));
}

//--- tip links

HomeTipCtrl::~HomeTipCtrl() {
    str::Free(richFor);
}

// re-parses when the markup changes; the parse is what the band draws
void HomeTipCtrl::SetTipLine(Str line, PlatformFont* font) {
    if (rich && str::Eq(richFor, line)) {
        rich->font = font;
        rich->hwndForCmds = hwndForCmds;
        return;
    }
    if (rich) {
        RemoveChild(rich, true);
        rich = nullptr;
    }
    str::ReplaceWithCopy(&richFor, line);
    if (len(line) == 0) {
        return;
    }
    rich = ParseTip(line);
    rich->font = font;
    rich->hwndForCmds = hwndForCmds;
    AddChild(rich);
}

// the band's background; the markup (VirtRichText child) paints over it
void HomeTipCtrl::Paint(VirtPaintCtx& ctx) {
    ctx.gfx->FillRect(ctx.bounds, ThemeControlBackgroundColor());
}

// rcTip is the whole band (its background), rcText where the markup goes
void HomeTipCtrl::Sync(const Rect& rcTip, const Rect& rcText) {
    if (!rich || rcTip.IsEmpty()) {
        visibility = Visibility::Collapse;
        return;
    }
    visibility = Visibility::Visible;
    SetBounds(rcTip);
    rich->SetBounds(rcText);
}

//--- home page click handlers

static void HomeOpenDocClicked(MainWindow* win, VirtMouseEvent*) {
    HwndSendCommand(win->hwndFrame, CmdOpenFile);
}

static void HomeHelpClicked(MainWindow* win, VirtMouseEvent*) {
    HwndSendCommand(win->hwndFrame, CmdToggleKeyboardHelp);
}

static void HomePaletteClicked(MainWindow* win, VirtMouseEvent*) {
    HwndSendCommand(win->hwndFrame, CmdCommandPalette);
}

static void HomeTipBandClicked(MainWindow* win, VirtMouseEvent*) {
    PickAnotherRandomPromotion();
    win->RedrawAll(true);
}

// TOOLTIPS_CLASS default for TTDT_INITIAL
static int TooltipInitialDelayMs() {
    int ms = (int)GetDoubleClickTime();
    return ms > 0 ? ms : 500;
}

constexpr UINT_PTR kHomeAboutHoverTimerID = 100;

static HomeChromeCtrl* HomeChrome(MainWindow* win) {
    if (!win || !win->homeRoot) {
        return nullptr;
    }
    if (!IsVirtCtrlOfKind(win->homeRoot->owned, kindHomeChromeCtrl)) {
        return nullptr;
    }
    return (HomeChromeCtrl*)win->homeRoot->owned;
}

static Rect HomeLogoScreenRect(HomeChromeCtrl* chrome) {
    if (!chrome || !chrome->logo) {
        return {};
    }
    HWND hwnd = chrome->GetHwnd();
    if (!hwnd) {
        return {};
    }
    Rect logo = chrome->logo->BoundsInWindow();
    Point origin = HwndClientToScreen(hwnd, Point());
    return {origin.x + logo.x, origin.y + logo.y, logo.dx, logo.dy};
}

static bool CursorOverHomeLogo(HomeChromeCtrl* chrome) {
    return HomeLogoScreenRect(chrome).Contains(GetCursorPosition());
}

static bool CursorOverAboutHover(HomeChromeCtrl* chrome) {
    return chrome && chrome->aboutHover && chrome->aboutHover->IsVisible() &&
           chrome->aboutHover->ScreenRect().Contains(GetCursorPosition());
}

static void CancelHomeAboutHoverTimer(MainWindow* win) {
    if (win && win->hwndCanvas) {
        KillTimer(win->hwndCanvas, kHomeAboutHoverTimerID);
    }
}

static void HideHomeAboutHover(MainWindow* win) {
    CancelHomeAboutHoverTimer(win);
    HomeChromeCtrl* chrome = HomeChrome(win);
    if (chrome && chrome->aboutHover) {
        chrome->aboutHover->Show(false);
    }
}

static void OnHomeAboutHoverLeave(MainWindow* win) {
    // left the dropdown; keep it only if the cursor is back on the title
    if (CursorOverHomeLogo(HomeChrome(win))) {
        return;
    }
    HideHomeAboutHover(win);
}

static void CALLBACK HomeAboutHoverTimerProc(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    MainWindow* win = FindMainWindowByHwnd(hwnd);
    if (!win || !IsMainWindowValidAndNotClosing(win)) {
        return;
    }
    ShowHomeAboutHover(win);
}

static void OnHomeLogoEnter(MainWindow* win) {
    if (!win || !win->hwndCanvas || !win->IsCurrentTabAbout()) {
        return;
    }
    HomeChromeCtrl* chrome = HomeChrome(win);
    if (chrome && chrome->aboutHover && chrome->aboutHover->IsVisible()) {
        return;
    }
    CancelHomeAboutHoverTimer(win);
    SetTimer(win->hwndCanvas, kHomeAboutHoverTimerID, (UINT)TooltipInitialDelayMs(), HomeAboutHoverTimerProc);
}

static void OnHomeLogoLeave(MainWindow* win) {
    // still on the title→dropdown path (cursor already over the popup)
    if (CursorOverAboutHover(HomeChrome(win))) {
        return;
    }
    HideHomeAboutHover(win);
}

// chrome-less About box under the home-page logo; links stay clickable
static void ShowHomeAboutHover(MainWindow* win) {
    if (!win || !IsMainWindowValidAndNotClosing(win) || !win->IsCurrentTabAbout()) {
        return;
    }
    HomeChromeCtrl* chrome = HomeChrome(win);
    if (!chrome || !chrome->logo || !CursorOverHomeLogo(chrome)) {
        return;
    }

    if (!chrome->aboutHover) {
        VirtHost::CreateArgs args;
        args.parent = win->hwndFrame;
        args.className = WStrL(L"SUMATRA_ABOUT_HOVER");
        args.isPopup = true;
        args.noActivate = true;
        args.visible = false;
        args.bgColor = ThemeMainWindowBackgroundColor();
        args.userData = win;
        chrome->aboutHover = VirtHost::Create(args);
        if (!chrome->aboutHover) {
            return;
        }
        chrome->aboutHover->onMouseLeave = MkFunc0(OnHomeAboutHoverLeave, win);
        ULONG_PTR cls = GetClassLongPtrW(chrome->aboutHover->native, GCL_STYLE);
        SetClassLongPtrW(chrome->aboutHover->native, GCL_STYLE, (LONG_PTR)(cls | CS_DROPSHADOW));
    }

    VirtHost* host = chrome->aboutHover;
    host->bgColor = ThemeMainWindowBackgroundColor();
    DpiSetFromHwnd(host->native);

    Rect measureRc{0, 0, 2000, 2000};
    AboutCtrl* about = UpdateAboutLayout(&host->vroot, host->native, measureRc, true);
    Size box = about->aboutRect.Size();
    if (box.IsEmpty()) {
        return;
    }

    Rect logoScreen = HomeLogoScreenRect(chrome);
    Rect pos{logoScreen.x + ((logoScreen.dx - box.dx) / 2), logoScreen.Bottom(), box.dx, box.dy};
    pos = ShiftRectToWorkArea(pos, win->hwndCanvas, true);
    host->SetPos(pos, true);
    UpdateAboutLayout(&host->vroot, host->native, {0, 0, box.dx, box.dy}, true);
    host->Invalidate();
}

HomeChromeCtrl::~HomeChromeCtrl() {
    HWND hwnd = GetHwnd();
    if (hwnd) {
        KillTimer(hwnd, kHomeAboutHoverTimerID);
    }
    delete aboutHover;
    aboutHover = nullptr;
}

// e.g. "Command Palette (Ctrl + K)"
static TempStr AppendCmdAccel(Str base, int cmd) {
    TempStr accel = AppendAccelKeyToMenuStringTemp({}, cmd);
    if (len(accel) == 0) {
        return base;
    }
    return str::JoinTemp(base, fmt(" (%s)", Str(accel.s + 1, len(accel) - 1))); // +1 skips the leading \t
}

// created once per window so that hover / pressed state survives the repaints
// that scrolling and filtering cause
static HomeChromeCtrl* EnsureHomeChrome(MainWindow* win) {
    // the canvas root holds either the home page's chrome or the About page's
    // controls, depending on which one is showing
    if (win->homeRoot && IsVirtCtrlOfKind(win->homeRoot->owned, kindHomeChromeCtrl)) {
        return (HomeChromeCtrl*)win->homeRoot->owned;
    }
    HWND hwnd = win->hwndCanvas;
    if (!win->homeRoot) {
        win->homeRoot = new VirtRoot(hwnd);
    }

    auto* chrome = new HomeChromeCtrl();
    chrome->kind = kindHomeChromeCtrl;
    chrome->flags |= vwfNoHitTest;

    // first, so that the rest of the chrome hit-tests and paints on top of the
    // entries. Below everything else: the tip band sits at the bottom of the page
    chrome->tip = new HomeTipCtrl();
    chrome->tip->hwndForCmds = win->hwndFrame;
    chrome->tip->onClick = MkFunc1(HomeTipBandClicked, win);
    chrome->AddChild(chrome->tip);

    chrome->searchBorder = new HomeSearchBorderCtrl();
    chrome->AddChild(chrome->searchBorder);

    chrome->hdr = new VirtText(StrL(""));
    chrome->AddChild(chrome->hdr);

    // [command palette] SumatraPDF [keyboard shortcuts] in one HBox at the top.
    // The title is hit-testable so hovering it can open the About dropdown.
    chrome->logoRow = new HomeLogoRow();
    chrome->paletteBtn = new HomeCircleBtnCtrl();
    chrome->paletteBtn->glyph = StrL(">");
    chrome->paletteBtn->SetTooltip(AppendCmdAccel(Tr("Command Palette"), CmdCommandPalette));
    chrome->paletteBtn->onClick = MkFunc1(HomePaletteClicked, win);
    chrome->logo = new SumatraLogo();
    chrome->logo->SetFlag(vwfNoHitTest, false);
    chrome->logo->onMouseEnter = MkFunc0(OnHomeLogoEnter, win);
    chrome->logo->onMouseLeave = MkFunc0(OnHomeLogoLeave, win);
    chrome->helpBtn = new HomeCircleBtnCtrl();
    chrome->helpBtn->glyph = StrL("?");
    chrome->helpBtn->SetTooltip(AppendCmdAccel(Tr("Keyboard Shortcuts"), CmdToggleKeyboardHelp));
    chrome->helpBtn->onClick = MkFunc1(HomeHelpClicked, win);
    chrome->logoRow->AddItem(chrome->paletteBtn);
    chrome->logoRow->AddItem(chrome->logo);
    chrome->logoRow->AddItem(chrome->helpBtn);
    chrome->AddChild(chrome->logoRow);

    chrome->openDoc = new HomeOpenDocCtrl();
    chrome->openDoc->text = new VirtText(StrL(""));
    chrome->openDoc->text->withUnderline = true;
    chrome->openDoc->AddChild(chrome->openDoc->text);
    chrome->openDoc->onClick = MkFunc1(HomeOpenDocClicked, win);
    chrome->AddChild(chrome->openDoc);

    win->homeRoot->SetChild(chrome);
    return chrome;
}

void HomePageDestroyChrome(MainWindow* win) {
    ClearHomeLayoutCache(win);
    delete win->homeLayout;
    win->homeLayout = nullptr;
    CancelHomeAboutHoverTimer(win);
    delete win->homeRoot;
    win->homeRoot = nullptr;
}

// gives the home page's virtual controls first shot at the canvas messages.
// Returns true when the event was consumed and the caller should stop
bool HomePageOnCanvasMessage(MainWindow* win, UINT msg, WPARAM wp, LPARAM lp, LRESULT& res) {
    VirtRoot* root = win->homeRoot;
    if (!root || !root->owned) {
        return false;
    }
    // Hover feedback (highlight, ✕ button, tooltips) must stay quiet while
    // another window is in front. The mouse still moves over the home page when
    // e.g. the command palette or the theme window is up, and putting a tooltip
    // over them steals activation: the palette closes on kill-focus and the
    // theme window ends up behind the main window. Clicks are exempt: clicking
    // a background window is meant to activate it.
    bool isHoverMsg = (msg == WM_MOUSEMOVE) || (msg == WM_SETCURSOR);
    if (isHoverMsg && GetForegroundWindow() != win->hwndFrame) {
        // thumbnail hover / tooltips stay quiet so they don't steal activation
        // from e.g. the command palette. The About dropdown is WS_EX_NOACTIVATE,
        // so hovering the logo still shows it.
        if (msg == WM_MOUSEMOVE) {
            Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ILayout* el = ElementFromPoint(root, pt, nullptr);
            VirtCtrl* w = el ? el->AsVirtCtrl() : nullptr;
            if (w && IsVirtCtrlOfKind(w, kindSumatraLogo)) {
                LRESULT ignored = 0;
                root->OnMessage(msg, wp, lp, ignored);
            } else {
                HideHomeAboutHover(win);
            }
        }
        return false;
    }
    if (msg != WM_SETCURSOR) {
        bool didHandle = root->OnMessage(msg, wp, lp, res);
        // canvas got the mouse and it is not on the title: left the SumatraPDF area
        if (msg == WM_MOUSEMOVE && !IsVirtCtrlOfKind(root->hovered, kindSumatraLogo)) {
            HideHomeAboutHover(win);
        }
        return didHandle;
    }
    Point pt = HwndGetCursorPos(win->hwndCanvas);
    Point ptLocal{0, 0};
    ILayout* el = ElementFromPoint(root, pt, &ptLocal);
    VirtCtrl* w = el ? el->AsVirtCtrl() : nullptr;
    if (!w || !w->OnSetCursor(ptLocal)) {
        return false;
    }
    // no tooltip of its own means "leave the tooltip alone", not "hide it"
    TempStr tip = w->GetTooltipTemp(ptLocal);
    if (tip && *tip.s) {
        Rect r = w->BoundsInWindow();
        win->ShowToolTip(tip, r);
    }
    res = TRUE;
    return true;
}

// feeds the geometry LayoutHomePage() computed into the persistent chrome tree
static void HomePageSyncChrome(HomePageLayout& l) {
    MainWindow* win = l.win;
    HomeChromeCtrl* chrome = EnsureHomeChrome(win);
    VirtRoot* root = win->homeRoot;
    // the chrome positions its children itself, so don't let the root re-layout
    root->bounds = l.rc;
    root->needsLayout = false;
    chrome->SetBounds(l.rc);

    chrome->tip->Sync(l.rcTip, l.rcTipText);

    chrome->searchBorder->visibility = l.rcSearchBorder.IsEmpty() ? Visibility::Collapse : Visibility::Visible;
    chrome->searchBorder->SetBounds(l.rcSearchBorder);

    // re-apply: the bounds were set before the parent was positioned
    chrome->hdr->SetBounds(l.freqRead->lastBounds);

    // font also set here so the cached-layout path (ApplyHomeLayoutCache)
    // repaints the logo without a full relayout
    chrome->logo->font = GetUserGuiFont(kSumatraTxtFont, DpiScale(kSumatraTxtFontSize));
    int iconSz = DpiScale(16);
    chrome->paletteBtn->pixmap = GetCachedPixmapForSvg(Str(gIconCommandPalette), iconSz, iconSz, kColBlack, kColWhite);
    chrome->logoRow->SetBounds(l.rcLogo);
    if (chrome->aboutHover && chrome->aboutHover->IsVisible()) {
        Size sz = chrome->aboutHover->ScreenRect().Size();
        Rect logoScreen = HomeLogoScreenRect(chrome);
        Rect want{logoScreen.x + ((logoScreen.dx - sz.dx) / 2), logoScreen.Bottom(), sz.dx, sz.dy};
        want = ShiftRectToWorkArea(want, l.win->hwndCanvas, true);
        if (want != chrome->aboutHover->ScreenRect()) {
            chrome->aboutHover->SetPos(want, true);
        }
    }

    // one click target covering the icon and the link text
    Rect rcOpen = l.rcIconOpen.Union(l.openDoc->lastBounds);
    rcOpen.Inflate(10, 10);
    HomeOpenDocCtrl* od = chrome->openDoc;
    od->pixmap = GetCachedPixmapForSvg(Str(gIconFileOpen), l.rcIconOpen.dx, l.rcIconOpen.dy);
    od->SetBounds(rcOpen);
    od->rcIconLocal = {l.rcIconOpen.x - rcOpen.x, l.rcIconOpen.y - rcOpen.y, l.rcIconOpen.dx, l.rcIconOpen.dy};
    // "Open a document" acts as a link, so it is drawn in the link color
    od->text->SetColor(kColText, gColsLink[kColText]);
    // re-apply now that the parent moved: bounds are relative to it
    od->text->SetBounds(l.openDoc->lastBounds);
}

static void DrawHomePageLayout(HomePageLayout& l) {
    Gfx* gfx = l.gfx;

    gfx->FillRect(l.rc, ThemeMainWindowBackgroundColor());

    // the chrome tree paints everything: search border, tip band, header
    // (palette / logo / help), and "Open a document..."
    l.win->homeRoot->Paint(gfx, l.rc);
}

static bool HomePageShouldShow(MainWindow* win) {
    if (!win || !win->IsCurrentTabAbout()) {
        return false;
    }
    if (!HasPermission(Perm::SavePreferences | Perm::DiskAccess)) {
        return false;
    }
    return gSettings && SettingsRememberOpenedFiles() && gSettings->showStartPage;
}

static void UpdateHomeOverlayScrollbar(MainWindow* win) {
    // the page no longer has scrollable content
    OverlayScrollbarShow(win->overlayScrollV, false);
}

void HomePageCreate(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    EnsureHomeChrome(win);
    EnsureHomeSearchCreated(win);
}

void HomePageRelayout(MainWindow* win) {
    if (!win || !win->hwndCanvas) {
        return;
    }
    if (!HomePageShouldShow(win)) {
        HomePageHideSearch(win);
        return;
    }
    EnsureHomeChrome(win);
    EnsureHomeSearchCreated(win);

    HomePageLayout l;
    l.rc = HwndClientRect(win->hwndCanvas);
    l.win = win;
    if (l.rc.IsEmpty()) {
        HomePageHideSearch(win);
        return;
    }

    bool usedCache = false;
    if (HomeLayoutCacheMatches(win, l.rc)) {
        ApplyHomeLayoutCache(l);
        usedCache = true;
    }
    if (!usedCache) {
        LayoutHomePage(l);
        SaveHomeLayoutCache(l);
    }
    HomePageSyncChrome(l);
    PlaceHomeSearchEdit(win, l.rcSearchBorder);
    if (win->homeSearch) {
        win->homeSearch->SetIsVisible(true);
    }
    UpdateHomeSearchCueBanner(win);
    UpdateHomeOverlayScrollbar(win);
}

void DrawHomePage(MainWindow* win, Gfx* gfx) {
    if (!HomeLayout(win).valid || !win->homeRoot) {
        HomePageRelayout(win);
    }
    if (!HomeLayout(win).valid || !win->homeRoot) {
        return;
    }

    auto& c = HomeLayout(win);
    HomePageLayout l;
    l.win = win;
    l.gfx = gfx;
    l.rc = c.canvasRc;
    l.rcSearchBorder = c.rcSearchBorder;
    l.rcTip = c.rcTip;
    l.hasTip = c.hasTip;
    DrawHomePageLayout(l);
}

// select the first entry, e.g. after the filter changed the list
void HomePageSelectFirst(MainWindow* win) {
    win->homePageSelIdx = 0;
    win->homePageSearchReturnCol = 0;
}

// hide keyboard-selection tip on deactivate
void HomePageOnWindowActivate(MainWindow* win, bool active) {
    if (!win) {
        return;
    }
    if (!active || IsIconic(win->hwndFrame)) {
        win->DeleteToolTip();
        HideHomeAboutHover(win);
    }
}

// mouse left the canvas: drop hover state
void HomePageClearActiveEntry(MainWindow* win) {
    if (win && win->homeRoot) {
        win->homeRoot->ClearHover();
    }
}

// File of the entry at (x,y). The home page has no file entries anymore, so
// this always returns empty; kept for the canvas context-menu path.
Str HomePageFilePathAtTemp(MainWindow*, int, int) {
    return {};
}

// Mouse over a file entry: there are no entries anymore, never handled.
bool HomePageOnHover(MainWindow*, int, int) {
    return false;
}

// file of the keyboard-selected entry, empty if there's no selection
Str HomePageSelectedFilePathTemp(MainWindow*) {
    return {};
}

// keyboard navigation of the file list: the list is gone, nothing to navigate
void HomePageMoveSelection(MainWindow*, int, int) {}

// the page content never overflows, so scrolling is a no-op; keep the scroll
// position valid for the layout code that still reads it
void HomePageOnVScroll(MainWindow* win, WPARAM) {
    win->homePageScrollY = 0;
}

void HomePageOnMouseWheel(MainWindow* win, int) {
    win->homePageScrollY = 0;
}
