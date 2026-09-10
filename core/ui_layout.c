/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_layout.h"

static int MaxInt(int a, int b) { return a > b ? a : b; }

static UiLayout Layout(UiContext *ui, UiRect rect, UiLayoutAxis axis)
{
    return (UiLayout){rect, axis, 0, ui ? ui->theme.containerGap : 0};
}

UiLayout UiColumn(UiContext *ui, UiRect rect)
{
    return Layout(ui, rect, UI_LAYOUT_VERTICAL);
}

UiLayout UiRow(UiContext *ui, UiRect rect)
{
    return Layout(ui, rect, UI_LAYOUT_HORIZONTAL);
}

UiRect UiLayoutNext(UiContext *ui, UiLayout *layout, int pixels)
{
    if (!layout)
        return (UiRect){0};
    if (pixels <= 0)
        pixels = ui ? ui->theme.itemHeight : 0;
    UiRect result = layout->rect;
    if (layout->axis == UI_LAYOUT_VERTICAL)
    {
        result.y += layout->cursor;
        result.height = MaxInt(0, pixels);
    }
    else
    {
        result.x += layout->cursor;
        result.width = MaxInt(0, pixels);
    }
    layout->cursor += pixels + layout->gap;
    return result;
}

UiRect UiLayoutRemaining(const UiLayout *layout)
{
    if (!layout)
        return (UiRect){0};
    UiRect result = layout->rect;
    if (layout->axis == UI_LAYOUT_VERTICAL)
    {
        result.y += layout->cursor;
        result.height = MaxInt(0, result.height - layout->cursor);
    }
    else
    {
        result.x += layout->cursor;
        result.width = MaxInt(0, result.width - layout->cursor);
    }
    return result;
}

UiRect UiLayoutCell(UiRect rect, UiLayoutAxis axis, int index, int count, int gap)
{
    if (count <= 0 || index < 0 || index >= count)
        return (UiRect){0};
    if (gap < 0)
        gap = 0;
    int extent = axis == UI_LAYOUT_VERTICAL ? rect.height : rect.width;
    int available = MaxInt(0, extent - gap * (count - 1));
    int start = available * index / count + gap * index;
    int end = available * (index + 1) / count + gap * index;
    if (axis == UI_LAYOUT_VERTICAL)
        return (UiRect){rect.x, rect.y + start, rect.width, end - start};
    return (UiRect){rect.x + start, rect.y, end - start, rect.height};
}

void UiGroup(UiContext *ui, UiRect rect, UiContentFn draw, void *user)
{
    if (!ui)
        return;
    UiRect well = UiRectInset(rect, ui->theme.containerGap);
    UiIndent(ui, well, draw, user);
}
