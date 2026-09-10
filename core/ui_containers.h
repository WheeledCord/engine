#ifndef CORE_UI_CONTAINERS_H
#define CORE_UI_CONTAINERS_H

#include "ui.h"

#include <stddef.h>

typedef struct UiPanel
{
    UiRect rect;
    bool draggable;
} UiPanel;

typedef struct UiDockTile
{
    UiRect rect;
    UiContentFn draw;
    void *user;
} UiDockTile;

typedef struct UiDock
{
    int x;
    int y;
    int tileSize;
    UiDockTile *tiles;
    size_t tileCount;
} UiDock;

UiRect UiWindowContentRect(const UiContext *ui, UiRect rect);
void UiDrawWindow(UiContext *ui, UiRect rect, const char *title, UiContentFn draw, void *user);
void UiDrawPanel(UiContext *ui, UiPanel *panel, const char *title, UiContentFn draw, void *user);
void UiDrawDock(UiContext *ui, UiDock *dock);

#endif
