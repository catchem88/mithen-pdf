/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "gui/Dpi.h"

#include <mupdf/pdf.h>

#include "gui/UIModels.h"
#include "gui/Gfx.h"

#include "Settings.h"
#include "AppSettings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "Annotation.h"
#include "SumatraPDF.h"
#include "Commands.h"
#include "Toolbar.h"
#include "SumatraDialogs.h"
#include "Translations.h"
#include "Notifications.h"
#include "FormFields.h"

// One field is edited at a time: either a text edit box or a choice list box
// floats over the page.
struct ActiveFormEdit {
    HWND hwnd = nullptr; // WC_EDITW (text) or LISTBOX (choice), child of hwndCanvas
    HFONT font = nullptr;
    Annotation* widget = nullptr;
    MainWindow* win = nullptr;
    bool multiline = false;
    bool isChoice = false;
};

static ActiveFormEdit gEdit;
static WNDPROC gDefCtrlProc = nullptr;
static bool gCommitting = false;

static TempStr WidgetFieldName(Annotation* widget); // defined with the date helpers

// Identity of the form field edited / focused last, so Tab can continue from it.
// Kept as the widget's PDF object number + page, not a pointer (committing an
// edit rebuilds the widget list) and not the field name (the radios of one group
// share a name, which made Tab stick on that group).
static int gLastFieldObjNum = 0;
static int gLastFieldPage = 0;
static int gFocusFieldObjNum = 0;
static int gFocusFieldPage = 0;

static void RememberEditedField(Annotation* widget) {
    if (!widget) {
        return;
    }
    int num = EngineMupdfGetWidgetObjNum(widget);
    if (num <= 0) {
        return;
    }
    gLastFieldObjNum = num;
    gLastFieldPage = widget->pageNo;
}

static void ClearFocusedField() {
    gFocusFieldObjNum = 0;
    gFocusFieldPage = 0;
}

// Focus a checkbox / radio (no editor window) or open the editor for a
// text / choice field. Returns false when the field can't be focused.
static bool FocusOrEditField(MainWindow* win, Annotation* w) {
    if (!win || !w) {
        return false;
    }
    int wt = GetWidgetType(w);
    if (wt == PDF_WIDGET_TYPE_TEXT || wt == PDF_WIDGET_TYPE_COMBOBOX || wt == PDF_WIDGET_TYPE_LISTBOX) {
        ClearFocusedField();
        return StartFormFieldEdit(win, w);
    }
    if (wt == PDF_WIDGET_TYPE_CHECKBOX || wt == PDF_WIDGET_TYPE_RADIOBUTTON) {
        int num = EngineMupdfGetWidgetObjNum(w);
        if (num <= 0) {
            return false;
        }
        gFocusFieldObjNum = num;
        gFocusFieldPage = w->pageNo;
        RememberEditedField(w);
        // keep keyboard focus on the frame so Tab continues to work
        if (win->hwndFrame) {
            HwndSetFocus(win->hwndFrame);
        }
        MainWindowRerender(win);
        return true;
    }
    return false;
}

// Tab: move to the next/previous fillable field after the last edited / focused
// one. Returns false when there is nothing to move to.
bool TabToAdjacentFormField(MainWindow* win, bool forward) {
    if (!win) {
        return false;
    }
    bool haveFocus = gFocusFieldObjNum > 0;
    int anchorNum = haveFocus ? gFocusFieldObjNum : gLastFieldObjNum;
    int anchorPage = haveFocus ? gFocusFieldPage : gLastFieldPage;
    if (anchorNum <= 0) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return false;
    }
    Annotation* next = EngineMupdfGetAdjacentWidgetByObjNum(engine, anchorPage, anchorNum, forward, true);
    if (!next) {
        // the cached widget list can be momentarily incomplete right after a
        // commit; resync the page and retry before giving up
        EngineMupdfResyncPageWidgets(engine, anchorPage);
        next = EngineMupdfGetAdjacentWidgetByObjNum(engine, anchorPage, anchorNum, forward, true);
    }
    if (!next) {
        return false;
    }
    return FocusOrEditField(win, next);
}

// Tab arrives both as WM_KEYDOWN and as the WM_CHAR the loop derives from it;
// advancing on both would skip a field. Fire once per press.
bool MaybeTabToAdjacentFormField(MainWindow* win, bool forward) {
    static DWORD lastTabTick = 0;
    DWORD now = GetTickCount();
    if (now - lastTabTick < 120) {
        return false;
    }
    if (!TabToAdjacentFormField(win, forward)) {
        return false;
    }
    lastTabTick = now;
    return true;
}

// True while a checkbox / radio is Tab-focused (it owns the Space key).
bool HasFocusedFormField() {
    return gFocusFieldObjNum > 0;
}

// Space on the canvas toggles the focused checkbox / radio. Returns true if a
// focused button was toggled.
bool ToggleFocusedFormField(MainWindow* win) {
    if (!win || gFocusFieldObjNum <= 0) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return false;
    }
    Annotation* w = EngineMupdfGetWidgetByObjNum(engine, gFocusFieldPage, gFocusFieldObjNum);
    if (!w) {
        return false;
    }
    bool ok = ToggleFormButton(w);
    if (ok) {
        MainWindowRerender(win);
        ToolbarUpdateStateForWindow(win, false);
    }
    return ok;
}

// Space arrives both as WM_KEYDOWN and as the WM_CHAR the message loop derives
// from it; toggling on both would flip the button twice (net no change). Fire
// once and ignore the second event of the same press.
bool MaybeToggleFocusedFormField(MainWindow* win) {
    static DWORD lastToggleTick = 0;
    DWORD now = GetTickCount();
    if (now - lastToggleTick < 120) {
        return false;
    }
    if (!ToggleFocusedFormField(win)) {
        return false;
    }
    lastToggleTick = now;
    return true;
}

// True while a form field is being edited in place.
bool IsFormFieldEditActive() {
    return gEdit.hwnd != nullptr;
}

// Acrobat / Chrome pale blue, translucent so the page still shows through.
constexpr Color kFormFieldHighlightCol = MkRgb(166, 202, 240);
constexpr u8 kFormFieldHighlightAlpha = 96;
// outline drawn around a checkbox / radio focused with Tab
constexpr Color kFormFieldFocusCol = MkRgb(0x1e, 0x88, 0xe5);

// Tint empty fillable fields so they are visible without hovering (issue #5966),
// plus a focus outline around a Tab-focused checkbox / radio.
void PaintFormFieldHighlights(MainWindow* win, Gfx* gfx) {
    if (!win || !win->IsDocLoaded() || !gfx) {
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!EngineMupdfIsPdf(engine)) {
        return;
    }
    // focus outline is independent of the highlight setting
    if (gFocusFieldObjNum > 0) {
        Annotation* w = EngineMupdfGetWidgetByObjNum(engine, gFocusFieldPage, gFocusFieldObjNum);
        if (w) {
            Rect rc = dm->CvtToScreen(w->pageNo, w->bounds);
            if (!rc.IsEmpty()) {
                gfx->DrawRect(rc, kFormFieldFocusCol, DpiScale(2));
            }
        }
    }
    if (!gSettings || !gSettings->highlightFormFields) {
        return;
    }
    Vec<Rect> screenRects;
    int pageCount = dm->PageCount();
    for (int pageNo = 1; pageNo <= pageCount; pageNo++) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        if (!pi || !pi->isShown || pi->visibleRatio == 0) {
            continue;
        }
        Vec<RectF> pageRects;
        EngineMupdfGetFormFieldHighlightRects(engine, pageNo, gEdit.widget, pageRects);
        for (RectF& pr : pageRects) {
            Rect rc = dm->CvtToScreen(pageNo, pr);
            if (!rc.IsEmpty()) {
                VecAppend(screenRects, rc);
            }
        }
    }
    if (len(screenRects) > 0) {
        gfx->FillRects(screenRects.els, len(screenRects), kFormFieldHighlightCol, kFormFieldHighlightAlpha);
    }
}

// Cancel the active form edit if it is for this widget (no save). Safe no-op
// when no edit is active or the widget does not match.
void CancelFormFieldEditIfWidget(Annotation* widget) {
    if (!widget || !gEdit.hwnd || gEdit.widget != widget) {
        return;
    }
    CommitFormFieldEdit(false);
}

// Commit (save=true) or cancel (save=false) the active form-field edit, if any.
void CommitFormFieldEdit(bool save) {
    if (!gEdit.hwnd || gCommitting) {
        return;
    }
    gCommitting = true;
    HWND h = gEdit.hwnd;
    HFONT font = gEdit.font;
    Annotation* widget = gEdit.widget;
    MainWindow* win = gEdit.win;
    bool isChoice = gEdit.isChoice;

    Str text;
    if (save) {
        if (isChoice) {
            int sel = LbGetCurrentSelection(h);
            if (sel < 0) {
                save = false; // nothing selected
            } else {
                TempWStr buf = LbGetTextTemp(h, sel);
                text = ToUtf8Temp(buf);
            }
        } else {
            text = str::DupTemp(HwndGetTextTemp(h));
        }
    }
    // clear state and unsubclass *before* destroying so the destroy-time
    // WM_KILLFOCUS doesn't re-enter the commit path
    gEdit = {};
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)gDefCtrlProc);
    DestroyWindow(h);
    if (font) {
        DeleteObject(font);
    }
    bool changed = false;
    if (save && widget) {
        changed = isChoice ? SetWidgetChoiceValue(widget, text) : SetWidgetTextValue(widget, text);
    }
    if (win) {
        HwndSetFocus(win->hwndFrame);
        if (changed) {
            MainWindowRerender(win);
            // refresh the tab's unsaved-changes (red dot) indicator and toolbar
            // state now, otherwise it only updates on the next repaint trigger
            // (tab switch, resize)
            ToolbarUpdateStateForWindow(win, false);
        }
    }
    gCommitting = false;
}

static LRESULT CALLBACK WndProcFormCtrl(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    bool isChoice = gEdit.isChoice;
    switch (msg) {
        case WM_GETDLGCODE:
            // we handle Tab / Enter / Esc ourselves
            return DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                CommitFormFieldEdit(false);
                return 0;
            }
            if (wp == VK_TAB) {
                // commit, then move to the next/previous field. Identity-based:
                // committing can rebuild the widget list.
                bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                MainWindow* win = gEdit.win;
                CommitFormFieldEdit(true);
                if (win) {
                    TabToAdjacentFormField(win, !back);
                }
                return 0;
            }
            if (wp == VK_RETURN && (isChoice || !gEdit.multiline)) {
                CommitFormFieldEdit(true);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (isChoice) {
                // let the listbox finalize the clicked selection, then commit it
                LRESULT r = CallWindowProcW(gDefCtrlProc, hwnd, msg, wp, lp);
                CommitFormFieldEdit(true);
                return r;
            }
            break;
        case WM_KILLFOCUS:
            // text: clicking elsewhere commits; choice: clicking away cancels
            CommitFormFieldEdit(!isChoice);
            return 0;
    }
    return CallWindowProcW(gDefCtrlProc, hwnd, msg, wp, lp);
}

// --- date fields: a month-calendar popup instead of the plain edit box ---

// field /T name; date-picking fields are named after what they hold
static TempStr WidgetFieldName(Annotation* widget) {
    if (!AnnotationIsLive(widget) || widget->type != AnnotationType::Widget) {
        return {};
    }
    EngineMupdf* e = widget->engine;
    auto* a = widget->pdfannot;
    auto* ctx = e->Ctx();
    AutoUnlockRecursiveMutex cs(&e->docLock);
    TempStr res;
    fz_try(ctx) {
        char* name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, a));
        res = name ? str::DupTemp(Str(name)) : StrL("");
        fz_free(ctx, name);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return res;
}

// the field's own format script (/AA /F /JS), e.g. AFDate_FormatEx("mm/dd/yyyy")
static TempStr WidgetFormatScript(Annotation* widget) {
    if (!AnnotationIsLive(widget) || widget->type != AnnotationType::Widget) {
        return {};
    }
    EngineMupdf* e = widget->engine;
    auto* a = widget->pdfannot;
    auto* ctx = e->Ctx();
    AutoUnlockRecursiveMutex cs(&e->docLock);
    TempStr res;
    fz_try(ctx) {
        pdf_obj* field = pdf_annot_obj(ctx, a);
        pdf_obj* aa = pdf_dict_get_inheritable(ctx, field, PDF_NAME(AA));
        pdf_obj* js = pdf_dict_get(ctx, pdf_dict_get(ctx, aa, PDF_NAME(F)), PDF_NAME(JS));
        if (js) {
            char* s = pdf_load_stream_or_string_as_utf8(ctx, js);
            res = s ? str::DupTemp(Str(s)) : StrL("");
            fz_free(ctx, s);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return res;
}

// the value's date layout, when it is one: separator and token order
// (e.g. "2024-01-31" -> sep '-', year first; "24.01.2026" -> year last)
struct DateLayout {
    char sep = '-';
    bool yearFirst = true;
};

static DateLayout GuessLayoutFromValue(Str value) {
    DateLayout l;
    const char* s = value.s;
    int n = len(value);
    int firstLen = 0;
    while (firstLen < n && s[firstLen] != '-' && s[firstLen] != '/' && s[firstLen] != '.') {
        firstLen++;
    }
    if (firstLen == 0) {
        return l;
    }
    l.yearFirst = (firstLen == 4);
    if (str::IndexOfChar(value, '-') >= 0) {
        l.sep = '-';
    } else if (str::IndexOfChar(value, '/') >= 0) {
        l.sep = '/';
    } else if (str::IndexOfChar(value, '.') >= 0) {
        l.sep = '.';
    }
    return l;
}

static bool ParseDateValue(Str value, DateLayout* layout, SYSTEMTIME* st) {
    const char* s = value.s;
    int n = len(value);
    int idx = 0;
    char sep = 0;
    int tok[3] = {0, 0, 0};
    int tokLen[3] = {0, 0, 0};
    for (int t = 0; t < 3; t++) {
        int start = idx;
        while (idx < n && s[idx] != '-' && s[idx] != '/' && s[idx] != '.') {
            if (s[idx] < '0' || s[idx] > '9') {
                return false;
            }
            idx++;
        }
        tokLen[t] = idx - start;
        if (tokLen[t] == 0 || (t < 2 && idx >= n) || t < 2 && tokLen[t] > 4) {
            return false;
        }
        tok[t] = 0;
        for (int i = 0; i < tokLen[t]; i++) {
            tok[t] = tok[t] * 10 + (s[start + i] - '0');
        }
        if (t < 2) {
            if (sep == 0) {
                sep = s[idx];
            } else if (s[idx] != sep) {
                return false;
            }
            idx++;
        }
    }
    if (idx != n) {
        return false;
    }
    if (layout) {
        *layout = GuessLayoutFromValue(value);
    }
    if (st) {
        ZeroMemory(st, sizeof(*st));
        if (tok[1] < 1 || tok[1] > 12 || tok[2] < 1 || tok[2] > 31) {
            return false;
        }
        bool yearFirst = layout ? layout->yearFirst : (tokLen[0] == 4);
        if (yearFirst) {
            st->wYear = (WORD)tok[0];
            st->wMonth = (WORD)tok[1];
            st->wDay = (WORD)tok[2];
        } else if (tok[0] > 12 || tokLen[0] > 2) {
            // dd/mm/yyyy
            st->wDay = (WORD)tok[0];
            st->wMonth = (WORD)tok[1];
            st->wYear = (WORD)tok[2];
        } else {
            st->wMonth = (WORD)tok[0];
            st->wDay = (WORD)tok[1];
            st->wYear = (WORD)tok[2];
        }
        if (st->wDay < 1 || st->wDay > 31 || st->wMonth < 1 || st->wMonth > 12 || st->wYear < 1) {
            return false;
        }
    }
    return true;
}

static bool IsDateField(Annotation* widget) {
    TempStr name = WidgetFieldName(widget);
    if (str::ContainsI(name, StrL("date")) || str::ContainsI(name, StrL("dob")) ||
        str::ContainsI(name, StrL("birth"))) {
        return true;
    }
    if (str::ContainsI(WidgetFormatScript(widget), StrL("AFDate"))) {
        return true;
    }
    Str value = GetWidgetValue(widget);
    if (len(value) > 0) {
        return ParseDateValue(value, nullptr, nullptr);
    }
    return false;
}

// Layout of a picked date, taken from the field's format script when it has one
// (AFDate_FormatEx("dd/mm/yyyy")): the order of d / m / y, the separator and
// whether the year has 2 or 4 digits
struct PickLayout {
    char order[3] = {'y', 'm', 'd'};
    char sep = '-';
    int yearDigits = 4;
    bool found = false;
};

static PickLayout LayoutFromFormatScript(Str script) {
    PickLayout l;
    int q = str::IndexOfChar(script, '"');
    if (q < 0) {
        return l;
    }
    const char* s = script.s + q + 1;
    int n = len(script) - q - 1;
    int nTok = 0;
    char order[3] = {};
    char sep = 0;
    int yearDigits = 0;
    for (int i = 0; i < n && s[i] != '"' && nTok <= 3; i++) {
        char c = s[i];
        char lc = (char)(c | 0x20);
        if (lc != 'd' && lc != 'm' && lc != 'y') {
            if (nTok > 0 && !sep) {
                sep = c;
            }
            continue;
        }
        // a run of the same letter is one token (dd, mm, yyyy)
        int run = 1;
        while (i + run < n && (char)(s[i + run] | 0x20) == lc) {
            run++;
        }
        i += run - 1;
        if (nTok < 3) {
            order[nTok] = lc;
        }
        if (lc == 'y') {
            yearDigits = run;
        }
        nTok++;
    }
    if (nTok != 3) {
        return l;
    }
    for (int i = 0; i < 3; i++) {
        l.order[i] = order[i];
    }
    l.sep = sep ? sep : '-';
    l.yearDigits = yearDigits == 2 ? 2 : 4;
    l.found = true;
    return l;
}

static TempStr FormatByLayout(const PickLayout& l, SYSTEMTIME st) {
    TempStr parts[3];
    for (int i = 0; i < 3; i++) {
        if (l.order[i] == 'y') {
            parts[i] = l.yearDigits == 2 ? fmt("%02d", (int)(st.wYear % 100)) : fmt("%04d", (int)st.wYear);
        } else if (l.order[i] == 'm') {
            parts[i] = fmt("%02d", (int)st.wMonth);
        } else {
            parts[i] = fmt("%02d", (int)st.wDay);
        }
    }
    return fmt("%s%c%s%c%s", parts[0], l.sep, parts[1], l.sep, parts[2]);
}

// writes the picked date back in the field's own layout: its format script if
// it has one, else the current value's layout, else ISO. month/day pad to two
// digits, the year to four; fmt() has no %0*d width arg, so widths are fixed
static TempStr FormatDateValue(Annotation* widget, SYSTEMTIME st) {
    PickLayout scripted = LayoutFromFormatScript(WidgetFormatScript(widget));
    if (scripted.found) {
        return FormatByLayout(scripted, st);
    }
    Str value = GetWidgetValue(widget);
    DateLayout l;
    if (len(value) > 0) {
        l = GuessLayoutFromValue(value);
    }
    if (l.yearFirst) {
        return fmt("%04d%c%02d%c%02d", st.wYear, l.sep, st.wMonth, l.sep, st.wDay);
    }
    return fmt("%02d%c%02d%c%04d", st.wMonth, l.sep, st.wDay, l.sep, st.wYear);
}

struct DateEditWnd;
static DateEditWnd* gDateEdit = nullptr;
// original wndproc of the month-calendar child, which is subclassed so Esc
// closes the popup even though the calendar itself has the keyboard focus
static WNDPROC gDateMonthOrigProc = nullptr;
static void DatePickerTab(HWND picker, bool back); // defined after DateEditWnd

static LRESULT CALLBACK WndProcDateMonth(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        HWND picker = GetParent(hwnd);
        if (picker) {
            DestroyWindow(picker);
        }
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_TAB) {
        HWND picker = GetParent(hwnd);
        if (picker) {
            DatePickerTab(picker, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        }
        return 0;
    }
    return CallWindowProcW(gDateMonthOrigProc, hwnd, msg, wp, lp);
}

struct DateEditWnd {
    HWND hwnd = nullptr;
    HWND hwndMonth = nullptr;
    MainWindow* win = nullptr;
    Annotation* widget = nullptr;
};

// Tab while the calendar is open: close it and move to the next/previous field
static void DatePickerTab(HWND picker, bool back) {
    DateEditWnd* wnd = (DateEditWnd*)GetWindowLongPtrW(picker, GWLP_USERDATA);
    MainWindow* win = wnd ? wnd->win : nullptr;
    if (wnd) {
        RememberEditedField(wnd->widget);
    }
    DestroyWindow(picker);
    if (win) {
        TabToAdjacentFormField(win, !back);
    }
}

static LRESULT CALLBACK WndProcDatePick(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DateEditWnd* wnd = (DateEditWnd*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            break;
        }
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            // MCN_SELECT's value depends on the SDK the OS's monthcal control
            // was built with (MCN_FIRST differs across Windows versions), so
            // match either the compile-time constant or the modern -746.
            if ((int)hdr->code == (int)MCN_SELECT || hdr->code == (int)(0U - 746U)) {
                auto* sel = (LPNMSELCHANGE)lp;
                SYSTEMTIME st = sel->stSelStart;
                Str text = FormatDateValue(wnd->widget, st);
                bool changed = SetWidgetTextValue(wnd->widget, text);
                MainWindow* win = wnd->win;
                RememberEditedField(wnd->widget);
                // close either way: a rejected write must not leave the calendar stuck
                if (win) {
                    HwndSetFocus(win->hwndFrame);
                }
                if (changed && win) {
                    MainWindowRerender(win);
                    ToolbarUpdateStateForWindow(win, false);
                } else if (win) {
                    ShowTemporaryNotification(win->hwndCanvas, Tr("Could not write the date into the field"));
                }
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_ACTIVATE:
            // clicking away (another window / app) dismisses the calendar, so it
            // can never be left stuck on screen
            if (LOWORD(wp) == WA_INACTIVE && wnd) {
                HWND now = (HWND)lp;
                if (now != wnd->hwnd && now != wnd->hwndMonth && !IsChild(wnd->hwnd, now)) {
                    DestroyWindow(hwnd);
                    return 0;
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) {
                HwndSetFocus(wnd->win ? wnd->win->hwndFrame : nullptr);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_DESTROY:
            gDateEdit = nullptr;
            delete wnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// opens a month calendar for a date-looking text field, writing the picked
// date into the field (rc = canvas-client coords of the field)
static bool StartDateEdit(MainWindow* win, Annotation* widget, Rect rc) {
    if (gDateEdit) {
        return false;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcDatePick;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MithenDatePick";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    POINT pt = {rc.x, rc.y + rc.dy};
    if (win->hwndCanvas) {
        ClientToScreen(win->hwndCanvas, &pt);
    }
    // sized to a month calendar; opens below the field, above it if that would
    // fall off the screen
    int dx = DpiScale(240);
    int dy = DpiScale(190);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR hm = MonitorFromPoint({pt.x, pt.y}, MONITOR_DEFAULTTONEAREST);
    if (hm && GetMonitorInfoW(hm, &mi)) {
        if ((int)(pt.y + dy) > (int)mi.rcWork.bottom && (int)(pt.y - dy - rc.dy) >= (int)mi.rcWork.top) {
            pt.y -= dy + rc.dy;
        }
        pt.x = std::max((int)mi.rcWork.left, std::min((int)pt.x, (int)(mi.rcWork.right - dx)));
    }
    auto* wnd = new DateEditWnd();
    wnd->win = win;
    wnd->widget = widget;
    DWORD style = WS_POPUP;
    wnd->hwnd = CreateWindowExW(WS_EX_TOPMOST, L"MithenDatePick", L"", style, pt.x, pt.y, dx, dy, win->hwndCanvas,
                                nullptr, GetModuleHandleW(nullptr), wnd);
    if (!wnd->hwnd) {
        delete wnd;
        return false;
    }
    // the calendar draws its own thin frame; size the popup to exactly it so no
    // white margin is left around it
    DWORD mstyle = WS_CHILD | WS_VISIBLE | WS_BORDER | MCS_NOTODAY;
    wnd->hwndMonth = CreateWindowExW(0, MONTHCAL_CLASSW, L"", mstyle, 0, 0, dx, dy, wnd->hwnd, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
    if (!wnd->hwndMonth) {
        DestroyWindow(wnd->hwnd);
        return false;
    }
    gDateMonthOrigProc = (WNDPROC)SetWindowLongPtrW(wnd->hwndMonth, GWLP_WNDPROC, (LONG_PTR)WndProcDateMonth);
    RECT need{};
    if (MonthCal_GetMinReqRect(wnd->hwndMonth, &need)) {
        int nx = need.right - need.left;
        int ny = need.bottom - need.top;
        SetWindowPos(wnd->hwndMonth, nullptr, 0, 0, nx, ny, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(wnd->hwnd, nullptr, 0, 0, nx, ny, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }
    // preselect the field's current date when it parses
    Str value = GetWidgetValue(widget);
    SYSTEMTIME st{};
    if (len(value) > 0 && ParseDateValue(value, nullptr, &st) && st.wYear >= 1601) {
        MonthCal_SetCurSel(wnd->hwndMonth, &st);
    }
    ShowWindow(wnd->hwnd, SW_SHOW);
    HwndSetFocus(wnd->hwndMonth);
    gDateEdit = wnd;
    return true;
}

static HFONT MakeFieldFont(int fontPx) {
    fontPx = std::max(8, fontPx);
    // negative height => character height in pixels
    return CreateFontW(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Arial");
}

// the field's on-screen font height in pixels: the /DA font size (PDF points)
// scaled to the page's current zoom, or a height-derived fallback for
// auto-sized (/DA size 0) fields.
static int FieldFontPx(Annotation* widget, Rect rc) {
    float daSize = GetWidgetFontSize(widget);
    float pageDy = widget->bounds.dy; // field height in page (PDF) units
    if (daSize > 0 && pageDy > 0) {
        float scale = (float)rc.dy / pageDy; // screen px per PDF unit
        return std::max(8, (int)(daSize * scale));
    }
    return std::max(8, (int)((float)rc.dy * 0.7f));
}

static bool StartTextEdit(MainWindow* win, Annotation* widget, Rect rc, int flags) {
    bool multiline = (flags & PDF_TX_FIELD_IS_MULTILINE) != 0;
    bool password = (flags & PDF_TX_FIELD_IS_PASSWORD) != 0;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL;
    if (multiline) {
        style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
    }
    if (password) {
        style |= ES_PASSWORD;
    }
    HMODULE hmod = GetModuleHandleW(nullptr);
    HWND hEdit =
        CreateWindowExW(0, WC_EDITW, L"", style, rc.x, rc.y, rc.dx, rc.dy, win->hwndCanvas, nullptr, hmod, nullptr);
    if (!hEdit) {
        return false;
    }
    HFONT font = MakeFieldFont(FieldFontPx(widget, rc));
    SetWindowFont(hEdit, font, TRUE);
    int margin = DpiScale(2);
    SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(margin, margin));
    int maxLen = GetWidgetMaxLen(widget); // comb / limited fields (e.g. SSN)
    if (maxLen > 0) {
        SendMessageW(hEdit, EM_SETLIMITTEXT, (WPARAM)maxLen, 0);
    }
    HwndSetText(hEdit, GetWidgetValue(widget));

    gDefCtrlProc = (WNDPROC)GetWindowLongPtrW(hEdit, GWLP_WNDPROC);
    SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)WndProcFormCtrl);

    gEdit.hwnd = hEdit;
    gEdit.font = font;
    gEdit.widget = widget;
    gEdit.win = win;
    gEdit.multiline = multiline;
    gEdit.isChoice = false;

    HwndSetFocus(hEdit);
    EditSelectAll(hEdit);
    return true;
}

static bool StartChoiceEdit(MainWindow* win, Annotation* widget, Rect rc) {
    StrVec opts;
    GetWidgetChoiceOptions(widget, opts);
    int n = len(opts);
    if (n == 0) {
        return false;
    }
    int fontPx = FieldFontPx(widget, rc);
    int itemDy = fontPx + DpiScale(6);
    int visN = std::min(n, 8);
    int listDy = (visN * itemDy) + DpiScale(4);
    int listDx = std::max(rc.dx, DpiScale(120));
    // drop down just below the field, or above if it would fall off the canvas
    Rect canvasRc = HwndClientRect(win->hwndCanvas);
    int x = rc.x;
    int y = rc.y + rc.dy;
    if (y + listDy > canvasRc.dy && rc.y - listDy >= 0) {
        y = rc.y - listDy;
    }
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS;
    HMODULE hmod = GetModuleHandleW(nullptr);
    HWND hLb =
        CreateWindowExW(0, L"LISTBOX", L"", style, x, y, listDx, listDy, win->hwndCanvas, nullptr, hmod, nullptr);
    if (!hLb) {
        return false;
    }
    HFONT font = MakeFieldFont(fontPx);
    SetWindowFont(hLb, font, TRUE);
    LbSetItemHeight(hLb, 0, itemDy);

    Str cur = GetWidgetValue(widget);
    int curIdx = -1;
    for (int i = 0; i < n; i++) {
        Str o = opts[i];
        LbAddString(hLb, o);
        if (curIdx < 0 && str::Eq(o, cur)) {
            curIdx = i;
        }
    }
    LbSetCurrentSelection(hLb, curIdx);

    gDefCtrlProc = (WNDPROC)GetWindowLongPtrW(hLb, GWLP_WNDPROC);
    SetWindowLongPtrW(hLb, GWLP_WNDPROC, (LONG_PTR)WndProcFormCtrl);

    gEdit.hwnd = hLb;
    gEdit.font = font;
    gEdit.widget = widget;
    gEdit.win = win;
    gEdit.multiline = false;
    gEdit.isChoice = true;

    HwndSetFocus(hLb);
    return true;
}

// Clicking a signature field the document's author left unsigned opens Sign
// Document with that field selected. Signed fields are left alone (clicking one
// shouldn't offer to overwrite it), and so is everything else (issue #5964).
bool StartSignatureFieldSigning(MainWindow* win, Annotation* widget) {
    if (!win || !AnnotationIsLive(widget)) {
        return false;
    }
    if (GetWidgetType(widget) != PDF_WIDGET_TYPE_SIGNATURE) {
        return false;
    }
    if (GetWidgetFieldFlags(widget) & PDF_FIELD_IS_READ_ONLY) {
        return false;
    }
    // signing rewrites the PDF, so it needs the same engine support annotations
    // do - and the same gate that decides whether the Sign Document command is
    // shown at all, so a click can't reach a dialog the menu is hiding
    DisplayModel* dm = win->AsFixed();
    if (!dm || !EngineSupportsAnnotations(dm->GetEngine()) || win->isFullScreen) {
        return false;
    }
    TempStr fieldName;
    // a signed field can be re-signed (the signature is replaced)
    if (!IsUnsignedSignatureWidget(widget, &fieldName, false)) {
        return false;
    }
    CommitFormFieldEdit(true); // don't leave an in-place edit hanging
    ShowSignDocumentDialog(win, fieldName, true);
    return true;
}

bool StartFormFieldEdit(MainWindow* win, Annotation* widget) {
    if (!win || !AnnotationIsLive(widget)) {
        return false;
    }
    int wt = GetWidgetType(widget);
    bool isText = (wt == PDF_WIDGET_TYPE_TEXT);
    bool isChoice = (wt == PDF_WIDGET_TYPE_COMBOBOX) || (wt == PDF_WIDGET_TYPE_LISTBOX);
    if (!isText && !isChoice) {
        return false;
    }
    int flags = GetWidgetFieldFlags(widget);
    if (flags & PDF_FIELD_IS_READ_ONLY) {
        return false;
    }
    CommitFormFieldEdit(true); // commit any prior edit
    RememberEditedField(widget);

    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    Rect rc = dm->CvtToScreen(widget->pageNo, widget->bounds); // canvas-client coords
    // scroll the field into view if it's off-screen (e.g. Tab moved past the
    // fold), then recompute its on-screen rect
    if (dm->ScrollScreenToRect(widget->pageNo, rc)) {
        rc = dm->CvtToScreen(widget->pageNo, widget->bounds);
    }
    if (rc.dx < 4 || rc.dy < 4) {
        return false;
    }
    if (isText && IsDateField(widget)) {
        return StartDateEdit(win, widget, rc);
    }
    if (isChoice) {
        return StartChoiceEdit(win, widget, rc);
    }
    return StartTextEdit(win, widget, rc, flags);
}
