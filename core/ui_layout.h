/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_LAYOUT_H
#define CORE_UI_LAYOUT_H

#include "ui.h"

typedef enum UiLayoutAxis
{
    UI_LAYOUT_HORIZONTAL,
    UI_LAYOUT_VERTICAL
} UiLayoutAxis;

typedef struct UiLayout
{
    UiRect rect;
    UiLayoutAxis axis;
    int cursor;
    int gap;
} UiLayout;

UiLayout UiColumn(UiContext *ui, UiRect rect);
UiLayout UiRow(UiContext *ui, UiRect rect);
UiRect UiLayoutNext(UiContext *ui, UiLayout *layout, int pixels);
UiRect UiLayoutRemaining(const UiLayout *layout);
UiRect UiLayoutCell(UiRect rect, UiLayoutAxis axis, int index, int count, int gap);

// Standard group: face gap outside, one indent, contents flush inside it.
void UiGroup(UiContext *ui, UiRect rect, UiContentFn draw, void *user);

#endif
