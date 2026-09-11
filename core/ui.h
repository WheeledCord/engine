/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_H
#define CORE_UI_H

#include "engine.h"

#include <stdbool.h>
#include <stddef.h>

#define CORE_UI_DEFAULT_FONT_PATH "core/fonts/unifont-17.0.04.bdf"

typedef struct UiRect
{
    int x;
    int y;
    int width;
    int height;
} UiRect;

typedef enum UiAlign
{
    UI_ALIGN_START,  // left or top
    UI_ALIGN_CENTER,
    UI_ALIGN_END     // right or bottom
} UiAlign;

typedef struct UiTextAlignment
{
    UiAlign horizontal;
    UiAlign vertical;
} UiTextAlignment;

typedef struct UiTheme
{
    Color face;
    Color white;
    Color darkGrey;
    Color black;
    // A supplied Font is borrowed. Otherwise UiInit loads fontPath and owns it.
    Font font;
    const char *fontPath;
    const int *fontCodepoints; // Initial atlas; owned fonts load additional glyphs as text needs them.
    int fontGlyphCount;
    int fontSize;
    int textSpacing;
    int textOffsetY;
    int textShadowOffset;
    int frameWidth;
    int indentWidth;
    int outsetWidth;
    int padding;
    int containerGap;
    int itemHeight;
    int titleHeight;
    int checkboxSize;
    int sliderKnobWidth;
    int editorHandleSize;
    float menuSlideSpeed;
} UiTheme;

typedef struct UiState UiState;
typedef struct UiContext
{
    UiTheme theme;
    UiState *state;
} UiContext;

typedef void (*UiContentFn)(UiContext *ui, UiRect content, void *user);

typedef enum UiButtonFlags
{
    UI_BUTTON_DEFAULT = 0,
    UI_BUTTON_BARE = 1 << 0,
    UI_BUTTON_DISABLED = 1 << 1,
    UI_BUTTON_DOWN = 1 << 2,
    UI_BUTTON_TOGGLE_VISUAL = 1 << 3,
    UI_BUTTON_NO_INPUT = 1 << 4
} UiButtonFlags;

UiTheme UiThemeDefault(void);
bool UiInit(UiContext *ui, UiTheme theme);
void UiFree(UiContext *ui);
bool UiSetTheme(UiContext *ui, UiTheme theme);
void UiBeginFrame(UiContext *ui, const EngineInput *input, UiRect screen);
void UiEndFrame(UiContext *ui);
bool UiConsumesMouse(const UiContext *ui);
/* Optional: interact once before simulation, then UiRender inside the drawing pass.
   Commands copy text and retain buffer capacity between frames. UiEndFrame still required. */
void UiBeginDeferredFrame(UiContext *ui, const EngineInput *input, UiRect screen);
EngineInputCapture UiCapture(const UiContext *ui);
bool UiRender(UiContext *ui);
/* Draw-only custom content; user data/resources must live until UiRender. Uses the current clip. */
void UiDrawCustom(UiContext *ui, UiRect rect, UiContentFn draw, void *user);

UiRect UiRectInset(UiRect rect, int amount);
void UiDrawFrame(UiContext *ui, UiRect rect);
void UiDrawIndent(UiContext *ui, UiRect rect);
void UiDrawOutset(UiContext *ui, UiRect rect);
void UiDrawShadowText(UiContext *ui, int x, int y, const char *text, Color color);
// Ensures owned-font glyphs are loaded before measuring, using the same metrics as drawing.
int UiTextWidth(UiContext *ui, const char *text);

bool UiButton(UiContext *ui, UiRect rect, const char *label);
bool UiButtonEx(UiContext *ui, UiRect rect, const char *label, UiButtonFlags flags);
bool UiButtonAligned(UiContext *ui, UiRect rect, const char *label, UiButtonFlags flags,
                     UiTextAlignment alignment);
bool UiButtonBare(UiContext *ui, UiRect rect, const char *label);
bool UiToggleButton(UiContext *ui, UiRect rect, const char *label, bool *value);
bool UiSlider(UiContext *ui, UiRect rect, float *value, float minimum, float maximum);
bool UiCheckbox(UiContext *ui, UiRect rect, const char *label, bool *value);
/* interactive=false draws the identical widget without claiming input or the pointer. */
bool UiSliderEx(UiContext *ui, UiRect rect, float *value, float minimum, float maximum,
                bool interactive);
bool UiCheckboxEx(UiContext *ui, UiRect rect, const char *label, bool *value, bool interactive);
bool UiTextField(UiContext *ui, UiRect rect, char *text, size_t capacity);
/* True while a text field holds keyboard focus, so callers can suppress their own shortcuts. */
bool UiTextFieldActive(const UiContext *ui);
/* True while this particular buffer is the focused field. */
bool UiTextFieldFocused(const UiContext *ui, const char *text);
void UiLabel(UiContext *ui, UiRect rect, const char *text);
void UiLabelAligned(UiContext *ui, UiRect rect, const char *text, UiTextAlignment alignment);
void UiIndent(UiContext *ui, UiRect rect, UiContentFn draw, void *user);

#endif
