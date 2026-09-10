/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_EDITOR_H
#define CORE_UI_EDITOR_H

#include "ui.h"

#include <stddef.h>

typedef enum UiHandle
{
    UI_HANDLE_NONE,
    UI_HANDLE_MOVE,
    UI_HANDLE_LEFT,
    UI_HANDLE_RIGHT,
    UI_HANDLE_TOP,
    UI_HANDLE_BOTTOM,
    UI_HANDLE_TOP_LEFT,
    UI_HANDLE_TOP_RIGHT,
    UI_HANDLE_BOTTOM_LEFT,
    UI_HANDLE_BOTTOM_RIGHT
} UiHandle;

typedef struct UiRectEditor
{
    UiHandle handle;
    Vector2 grab;
    UiRect origin;
} UiRectEditor;

// What an edited rectangle is allowed to line up with. Callers own the policy: step and threshold
// of zero disable grid and alignment snapping respectively.
typedef struct UiSnapConfig
{
    UiRect bounds;
    const UiRect *others;
    size_t otherCount;
    int step;      // grid pitch
    int threshold; // how far a snap reaches, in pixels
    int gap;       // conventional spacing between neighbours
    int rowHeight; // conventional row height
} UiSnapConfig;

typedef struct UiGuide
{
    bool active;
    int position;
    int start;
    int end;
} UiGuide;

typedef struct UiSnapResult
{
    UiGuide vertical;
    UiGuide horizontal;
} UiSnapResult;

int UiPickRect(const UiRect *rects, size_t count, Vector2 point);
UiHandle UiHandleAt(const UiContext *ui, UiRect rect, Vector2 point);
UiRect UiClampRect(UiRect rect, UiRect bounds, int minimumSize);
// Adjusts rect in place; the returned guides describe the lines it locked onto.
UiSnapResult UiSnapRect(UiRect *rect, UiHandle handle, UiSnapConfig config);
bool UiEditRect(UiContext *ui, UiRect bounds, UiRect *rect, UiRectEditor *editor,
                const UiSnapConfig *snap, UiSnapResult *guides);
void UiDrawRectSelection(UiContext *ui, UiRect rect);
void UiDrawGuides(UiContext *ui, UiSnapResult guides, Color color);

#endif
