/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_containers.h"

#include "ui_internal.h"

#include <math.h>

static int ClampInt(int value, int minimum, int maximum)
{
    if (maximum < minimum)
        return minimum;
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

UiRect UiWindowContentRect(const UiContext *ui, UiRect rect)
{
    if (!ui)
        return (UiRect){0};
    return (UiRect){rect.x + ui->theme.frameWidth,
                    rect.y + ui->theme.frameWidth + ui->theme.titleHeight,
                    rect.width - ui->theme.frameWidth * 2,
                    rect.height - ui->theme.frameWidth * 2 - ui->theme.titleHeight};
}

void UiDrawWindow(UiContext *ui, UiRect rect, const char *title, UiContentFn draw, void *user)
{
    if (!ui || !ui->state || rect.width <= 0 || rect.height <= 0)
        return;
    UiDrawFrame(ui, rect);
    UiRect titleRect = {rect.x + ui->theme.frameWidth, rect.y + ui->theme.frameWidth,
                        rect.width - ui->theme.frameWidth * 2, ui->theme.titleHeight};
    UiDrawTitleBar(ui, titleRect, title);
    UiRect content = UiWindowContentRect(ui, rect);
    UiClipState previous = UiPushClip(ui, content);
    if (draw && content.width > 0 && content.height > 0)
        draw(ui, content, user);
    UiRestoreClip(ui, previous);
}

void UiDrawPanel(UiContext *ui, UiPanel *panel, const char *title, UiContentFn draw, void *user)
{
    if (!ui || !ui->state || !panel || panel->rect.width <= 0 || panel->rect.height <= 0)
        return;
    UiState *state = ui->state;
    uint64_t id = UiPointerId(panel, 0x50414e454cULL);
    UiRect titleRect = {panel->rect.x + ui->theme.frameWidth,
                        panel->rect.y + ui->theme.frameWidth,
                        panel->rect.width - ui->theme.frameWidth * 2,
                        ui->theme.titleHeight};
    bool titleHot = UiHit(ui, titleRect);
    if (UiHit(ui, panel->rect))
        UiMarkMouse(ui);
    if (panel->draggable && UiInputAllowed(ui) && titleHot &&
        state->input.mousePressed[MOUSE_BUTTON_LEFT] && !state->activeId)
    {
        state->activeId = id;
        state->dragOffset =
            (Vector2){state->input.mousePosition.x - panel->rect.x,
                      state->input.mousePosition.y - panel->rect.y};
    }
    if (state->activeId == id && state->input.mouseDown[MOUSE_BUTTON_LEFT])
    {
        UiRect screen = state->screen;
        int x = (int)lroundf(state->input.mousePosition.x - state->dragOffset.x);
        int y = (int)lroundf(state->input.mousePosition.y - state->dragOffset.y);
        panel->rect.x = ClampInt(x, screen.x, screen.x + screen.width - panel->rect.width);
        panel->rect.y = ClampInt(y, screen.y, screen.y + screen.height - panel->rect.height);
    }

    UiDrawWindow(ui, panel->rect, title, draw, user);
}

void UiDrawDock(UiContext *ui, UiDock *dock)
{
    if (!ui || !ui->state || !dock || !dock->tiles || dock->tileSize <= 0)
        return;
    for (size_t i = 0; i < dock->tileCount; i++)
    {
        UiDockTile *tile = &dock->tiles[i];
        tile->rect = (UiRect){dock->x, dock->y + (int)i * dock->tileSize, dock->tileSize,
                              dock->tileSize};
        UiDrawFrame(ui, tile->rect);
        if (UiHit(ui, tile->rect))
            UiMarkMouse(ui);
        UiRect content = UiRectInset(tile->rect, ui->theme.frameWidth);
        UiClipState previous = UiPushClip(ui, content);
        if (tile->draw && content.width > 0 && content.height > 0)
            tile->draw(ui, content, tile->user);
        UiRestoreClip(ui, previous);
    }
}
