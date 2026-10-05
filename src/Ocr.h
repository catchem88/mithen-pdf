/* Copyright 2026 the MithenPDF project authors. See AUTHORS file. */

// Recognizes the text (Windows.Media.Ocr) and the QR codes (quirc) of the page
// shown in the canvas, then shows the result as click-to-copy boxes over a
// darkened page. See Ocr.cpp.

struct MainWindow;
struct WindowTab;
struct Gfx;

struct OcrBox {
    int pageNo = 0;
    RectF rect; // document coordinates
    Str text;   // owned
};

struct OcrState {
    ~OcrState();

    bool running = false;
    bool shown = false;
    Vec<OcrBox> boxes;
    WindowTab* tab = nullptr;
    ThreadHandle thread = nullptr;
    int hover = -1;
    int epoch = 0;
    int spinPhase = 0;
};

// canvas timer that animates the spinner while a recognition runs
constexpr UINT_PTR kOcrSpinTimerID = 17;

// starts recognizing the page shown in the canvas; ignored while a recognition
// runs or its results are already shown (Esc / right-click close those)
void OcrStart(MainWindow* win);
// closes the results and cancels a running recognition
void OcrClose(MainWindow* win);
bool OcrIsRunning(MainWindow* win);
bool OcrIsShown(MainWindow* win);
// copies the recognized line under `pt` (canvas coordinates); false if none
bool OcrClickCopy(MainWindow* win, Point pt);
// re-hit-tests the line under `pt`; returns its index, or -1
int OcrUpdateHover(MainWindow* win, Point pt);
// copies every recognized line; false when no results are shown
bool OcrCopyAll(MainWindow* win);
// advances the spinner animation
void OcrTickSpin(MainWindow* win);
// paints the darkening, the recognized lines and the spinner
void OcrPaint(MainWindow* win, Gfx* gfx, HDC hdc);
