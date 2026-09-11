/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_INTERNAL_H
#define CORE_UI_INTERNAL_H

#include "ui.h"

#include <stddef.h>
#include <stdint.h>

struct UiMenu;

typedef struct UiClipState
{
    bool active;
    UiRect rect;
} UiClipState;

struct UiState
{
    bool deferred, commandFailed, keyboardConsumed;
    struct UiCommand *commands;
    size_t commandCount, commandCapacity;
    char *commandText;
    size_t textCount, textCapacity;
    bool ownsFont;
    int *fontCodepoints; // Requested glyphs, including misses, so absent glyphs are tried only once.
    int fontGlyphCount;
    EngineInput input;
    UiRect screen;
    UiClipState clip;
    uint64_t activeId;
    uint64_t focusedId;
    bool focusClaimed;
    int caret;     // byte offset of the insertion point in the focused field
    int selection; // byte offset of the other end of the selection
    int scroll;    // pixels the focused field is scrolled by, so the caret stays visible
    bool selecting;
    Vector2 dragOffset;
    bool mouseConsumed;
    const struct UiMenu *menuRoot;
    const char *menuContextTitle;
    void *menuUser;
    Vector2 menuPosition;
    const struct UiMenu **menuPath;
    UiRect *menuRects;
    UiRect *menuDrawRects;
    int *menuSelections;
    size_t menuDepth;
    size_t menuCapacity;
    float frameDt;
};

void UiFill(UiContext *ui, UiRect rect, Color color);
void UiQueueText(UiContext *ui, int x, int y, const char *text, Color color);
bool UiPointInRect(Vector2 point, UiRect rect);
UiRect UiIntersectRect(UiRect a, UiRect b);
uint64_t UiPointerId(const void *pointer, uint64_t salt);
uint64_t UiWidgetId(const char *label, UiRect rect, uint64_t salt);
bool UiInputAllowed(const UiContext *ui);
void UiMarkMouse(UiContext *ui);
UiClipState UiPushClip(UiContext *ui, UiRect rect);
void UiRestoreClip(UiContext *ui, UiClipState previous);
void UiDrawTextRaw(UiContext *ui, int x, int y, const char *text, Color color);
bool UiButtonControl(UiContext *ui, UiRect rect, const char *label, uint64_t id,
                     bool enabled, bool interactive, bool forcedDown, bool allowWhileMenuOpen,
                     bool toggleVisual, UiTextAlignment alignment);
void UiDrawTitleBar(UiContext *ui, UiRect rect, const char *title);

#endif
