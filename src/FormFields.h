/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Interactive PDF form (AcroForm) filling: in-place editing of text fields.
// Checkbox / radio toggling lives in Annotation.cpp (ToggleFormButton).

struct MainWindow;
struct Annotation;
struct Gfx;

bool StartFormFieldEdit(MainWindow* win, Annotation* widget);
bool StartSignatureFieldSigning(MainWindow* win, Annotation* widget);
// Tab from the canvas: move to the next/previous form field after the last one
// edited. False when there is no remembered field or no next one.
bool TabToAdjacentFormField(MainWindow* win, bool forward);
// as above, but debounced so a Tab press (WM_KEYDOWN + WM_CHAR) advances once
bool MaybeTabToAdjacentFormField(MainWindow* win, bool forward);
// Space on the canvas toggles a Tab-focused checkbox / radio
bool ToggleFocusedFormField(MainWindow* win);
// true while a checkbox / radio is Tab-focused (it owns the Space key)
bool HasFocusedFormField();
// as above, but debounced so a Space press (WM_KEYDOWN + WM_CHAR) toggles once
bool MaybeToggleFocusedFormField(MainWindow* win);

void CommitFormFieldEdit(bool save);

void CancelFormFieldEditIfWidget(Annotation* widget);

bool IsFormFieldEditActive();
void PaintFormFieldHighlights(MainWindow* win, Gfx* gfx);
