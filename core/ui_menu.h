/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_MENU_H
#define CORE_UI_MENU_H

#include "ui.h"

#include <stddef.h>

typedef struct UiMenu UiMenu;
typedef void (*UiMenuActionFn)(void *user);

typedef struct UiMenuItem
{
    const char *label;
    bool enabled;
    const UiMenu *submenu;
    UiMenuActionFn action;
} UiMenuItem;

struct UiMenu
{
    const UiMenuItem *items;
    size_t count;
    int width;
    const char *title;
};

void UiContextMenu(UiContext *ui, UiRect triggerArea, const char *contextName,
                   const UiMenu *menu, void *user);
void UiDrawMenus(UiContext *ui);
void UiCloseMenus(UiContext *ui);
bool UiMenuIsOpen(const UiContext *ui);

#endif
