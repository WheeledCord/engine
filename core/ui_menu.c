/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_menu.h"

#include "ui_internal.h"
#include "ui_containers.h"
#include "ui_layout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int MaxInt(int a, int b) { return a > b ? a : b; }

static bool EnsureMenuCapacity(UiContext *ui, size_t needed)
{
    UiState *state = ui->state;
    if (needed <= state->menuCapacity)
        return true;
    size_t capacity = state->menuCapacity ? state->menuCapacity * 2 : 4;
    while (capacity < needed)
        capacity *= 2;
    const UiMenu **paths = malloc(capacity * sizeof(*paths));
    UiRect *rects = malloc(capacity * sizeof(*rects));
    UiRect *drawRects = malloc(capacity * sizeof(*drawRects));
    int *selections = malloc(capacity * sizeof(*selections));
    if (!paths || !rects || !drawRects || !selections)
    {
        free(paths);
        free(rects);
        free(drawRects);
        free(selections);
        return false;
    }
    if (state->menuDepth)
    {
        memcpy(paths, state->menuPath, state->menuDepth * sizeof(*paths));
        memcpy(rects, state->menuRects, state->menuDepth * sizeof(*rects));
        memcpy(drawRects, state->menuDrawRects, state->menuDepth * sizeof(*drawRects));
        memcpy(selections, state->menuSelections, state->menuDepth * sizeof(*selections));
    }
    free(state->menuPath);
    free(state->menuRects);
    free(state->menuDrawRects);
    free(state->menuSelections);
    state->menuPath = paths;
    state->menuRects = rects;
    state->menuDrawRects = drawRects;
    state->menuSelections = selections;
    state->menuCapacity = capacity;
    return true;
}

static int MenuOuterInset(const UiContext *ui)
{
    return ui->theme.frameWidth + ui->theme.containerGap;
}

static int MenuContentInset(const UiContext *ui)
{
    return MenuOuterInset(ui) + ui->theme.indentWidth;
}

static int MenuContentTop(const UiContext *ui)
{
    return ui->theme.frameWidth + ui->theme.titleHeight + ui->theme.containerGap +
           ui->theme.indentWidth;
}

static int MenuButtonWidth(UiContext *ui, const UiMenu *menu)
{
    if (menu->width > 0)
        return menu->width;
    int width = 0;
    for (size_t i = 0; i < menu->count; i++)
    {
        int itemWidth = UiTextWidth(ui, menu->items[i].label) + ui->theme.padding * 2;
        if (menu->items[i].submenu)
            itemWidth += ui->theme.itemHeight;
        width = MaxInt(width, itemWidth);
    }
    return width;
}

static UiRect MenuRect(UiContext *ui, const UiMenu *menu, int x, int y)
{
    int inset = MenuContentInset(ui);
    return (UiRect){x, y, MenuButtonWidth(ui, menu) + inset * 2,
                    ui->theme.frameWidth * 2 + ui->theme.titleHeight +
                        ui->theme.containerGap * 2 + ui->theme.indentWidth * 2 +
                        (int)menu->count * ui->theme.itemHeight};
}

static UiRect ClampRoot(UiContext *ui, const UiMenu *menu, int x, int y)
{
    UiRect screen = ui->state->screen;
    UiRect rect = MenuRect(ui, menu, x, y);
    if (rect.width > screen.width)
        rect.width = screen.width;
    if (rect.height > screen.height)
        rect.height = screen.height;
    if (rect.x + rect.width > screen.x + screen.width)
        rect.x = screen.x + screen.width - rect.width;
    if (rect.y + rect.height > screen.y + screen.height)
        rect.y = screen.y + screen.height - rect.height;
    if (rect.x < screen.x)
        rect.x = screen.x;
    if (rect.y < screen.y)
        rect.y = screen.y;
    return rect;
}

static UiRect CascadeRect(UiContext *ui, size_t parentDepth, const UiMenu *menu)
{
    UiState *state = ui->state;
    UiRect parent = state->menuRects[parentDepth];
    int selected = state->menuSelections[parentDepth];
    UiRect rect = MenuRect(ui, menu, parent.x + parent.width,
                           parent.y + MenuContentTop(ui) + selected * ui->theme.itemHeight);
    int screenRight = state->screen.x + state->screen.width;
    int overflow = rect.x + rect.width - screenRight;
    if (overflow > 0)
    {
        int available = state->menuRects[0].x - state->screen.x;
        int shift = overflow < available ? overflow : available;
        for (size_t i = 0; i <= parentDepth; i++)
            state->menuRects[i].x -= shift;
        rect.x = state->menuRects[parentDepth].x + state->menuRects[parentDepth].width;
    }
    if (rect.x + rect.width > screenRight)
        rect.x = screenRight - rect.width;
    if (rect.x < state->screen.x)
        rect.x = state->screen.x;
    if (rect.height > state->screen.height)
        rect.height = state->screen.height;
    if (rect.y + rect.height > state->screen.y + state->screen.height)
        rect.y = state->screen.y + state->screen.height - rect.height;
    if (rect.y < state->screen.y)
        rect.y = state->screen.y;
    return rect;
}

static void RetargetMenus(UiContext *ui)
{
    UiState *state = ui->state;
    state->menuRects[0] = ClampRoot(ui, state->menuRoot, (int)state->menuPosition.x,
                                   (int)state->menuPosition.y);
    for (size_t depth = 1; depth < state->menuDepth; depth++)
        state->menuRects[depth] = CascadeRect(ui, depth - 1, state->menuPath[depth]);
}

static bool StartMenu(UiContext *ui)
{
    UiState *state = ui->state;
    if (!EnsureMenuCapacity(ui, 1))
        return false;
    state->menuPath[0] = state->menuRoot;
    state->menuSelections[0] = -1;
    state->menuDepth = 1;
    state->menuRects[0] = ClampRoot(ui, state->menuRoot, (int)state->menuPosition.x,
                                   (int)state->menuPosition.y);
    state->menuDrawRects[0] = state->menuRects[0];
    return true;
}

void UiCloseMenus(UiContext *ui)
{
    if (!ui || !ui->state)
        return;
    ui->state->menuRoot = NULL;
    ui->state->menuContextTitle = NULL;
    ui->state->menuUser = NULL;
    ui->state->menuDepth = 0;
}

bool UiMenuIsOpen(const UiContext *ui)
{
    return ui && ui->state && ui->state->menuRoot;
}

void UiContextMenu(UiContext *ui, UiRect triggerArea, const char *contextName,
                   const UiMenu *menu, void *user)
{
    if (!ui || !ui->state || !menu || !menu->items || !menu->count)
        return;
    UiState *state = ui->state;
    if (!state->input.mousePressed[MOUSE_BUTTON_RIGHT] ||
        !UiHit(ui, triggerArea))
        return;
    state->menuRoot = menu;
    state->menuContextTitle = contextName;
    state->menuUser = user;
    state->menuPosition = state->input.mousePosition;
    UiMarkMouse(ui);
    if (!StartMenu(ui))
        UiCloseMenus(ui);
}

static void DrawArrow(UiContext *ui, UiRect item, Color color, int offset)
{
    int x = item.x + item.width - ui->theme.padding - 4 + offset;
    int y = item.y + item.height / 2 - 3 + offset;
    UiFill(ui, (UiRect){x, y, 1, 7}, color);
    UiFill(ui, (UiRect){x + 1, y + 1, 1, 5}, color);
    UiFill(ui, (UiRect){x + 2, y + 2, 1, 3}, color);
    UiFill(ui, (UiRect){x + 3, y + 3, 1, 1}, color);
}

static bool ToggleSubmenu(UiContext *ui, size_t depth, int item)
{
    UiState *state = ui->state;
    if (state->menuSelections[depth] == item && state->menuDepth > depth + 1)
    {
        state->menuSelections[depth] = -1;
        state->menuDepth = depth + 1;
        RetargetMenus(ui);
        return true;
    }

    const UiMenu *submenu = state->menuPath[depth]->items[item].submenu;
    if (!EnsureMenuCapacity(ui, depth + 2))
        return false;
    state->menuSelections[depth] = item;
    state->menuDepth = depth + 2;
    state->menuPath[depth + 1] = submenu;
    state->menuSelections[depth + 1] = -1;
    state->menuRects[depth + 1] = CascadeRect(ui, depth, submenu);
    state->menuDrawRects[depth + 1] = state->menuRects[depth + 1];
    state->menuDrawRects[depth + 1].x =
        state->menuDrawRects[depth].x + state->menuDrawRects[depth].width;
    RetargetMenus(ui);
    return true;
}

typedef struct MenuDraw
{
    size_t depth;
    bool activated;
    UiMenuActionFn action;
    void *actionUser;
} MenuDraw;

static void DrawMenuButtons(UiContext *ui, UiRect content, void *user)
{
    MenuDraw *draw = user;
    size_t depth = draw->depth;
    UiState *state = ui->state;
    const UiMenu *menu = state->menuPath[depth];
    for (size_t i = 0; i < menu->count; i++)
    {
        const UiMenuItem *entry = &menu->items[i];
        UiRect item = {content.x, content.y + (int)i * ui->theme.itemHeight, content.width,
                       ui->theme.itemHeight};
        bool down = entry->submenu && state->menuSelections[depth] == (int)i &&
                    state->menuDepth > depth + 1;
        bool clicked = UiButtonControl(ui, item, entry->label,
                                      UiPointerId(entry, 0x4d454e554954454dULL), entry->enabled,
                                      entry->enabled, down, true, entry->submenu != NULL,
                                      (UiTextAlignment){UI_ALIGN_CENTER, UI_ALIGN_CENTER});
        if (entry->submenu)
            DrawArrow(ui, item, entry->enabled ? ui->theme.black : ui->theme.darkGrey, down ? 1 : 0);
        if (!clicked)
            continue;
        if (entry->submenu)
            ToggleSubmenu(ui, depth, (int)i);
        else
        {
            draw->activated = true;
            draw->action = entry->action;
            draw->actionUser = state->menuUser;
        }
    }
}

static void DrawMenuGroup(UiContext *ui, UiRect content, void *user)
{
    UiGroup(ui, content, DrawMenuButtons, user);
}

static bool DrawMenu(UiContext *ui, size_t depth)
{
    UiState *state = ui->state;
    const UiMenu *menu = state->menuPath[depth];
    UiRect rect = state->menuDrawRects[depth];
    const char *title = NULL;
    if (depth == 0)
        title = menu->title ? menu->title : state->menuContextTitle;
    else
    {
        int parentItem = state->menuSelections[depth - 1];
        title = state->menuPath[depth - 1]->items[parentItem].label;
    }
    MenuDraw draw = {.depth = depth};
    UiDrawWindow(ui, rect, title ? title : "Menu", DrawMenuGroup, &draw);
    if (draw.activated)
    {
        UiCloseMenus(ui);
        if (draw.action)
            draw.action(draw.actionUser);
    }
    return draw.activated;
}

static int MoveToward(int current, int target, int step)
{
    if (current < target)
        return current + step < target ? current + step : target;
    if (current > target)
        return current - step > target ? current - step : target;
    return current;
}

static void AdvanceMenus(UiContext *ui)
{
    UiState *state = ui->state;
    int step = (int)ceilf(ui->theme.menuSlideSpeed * state->frameDt);
    if (step < 1)
        step = 1;
    for (size_t i = 0; i < state->menuDepth; i++)
    {
        state->menuDrawRects[i].x = MoveToward(state->menuDrawRects[i].x, state->menuRects[i].x, step);
        state->menuDrawRects[i].y = MoveToward(state->menuDrawRects[i].y, state->menuRects[i].y, step);
    }
}

static bool PointInMenus(const UiContext *ui, Vector2 point)
{
    for (size_t i = 0; i < ui->state->menuDepth; i++)
        if (UiPointInRect(point, ui->state->menuDrawRects[i]))
            return true;
    return false;
}

void UiDrawMenus(UiContext *ui)
{
    if (!ui || !ui->state || !ui->state->menuRoot)
        return;
    UiState *state = ui->state;
    UiMarkMouse(ui);
    if (state->input.mousePressed[MOUSE_BUTTON_LEFT] &&
        !PointInMenus(ui, state->input.mousePosition))
    {
        UiCloseMenus(ui);
        return;
    }

    AdvanceMenus(ui);
    for (size_t depth = 0; depth < state->menuDepth; depth++)
        if (DrawMenu(ui, depth))
            return;
}
