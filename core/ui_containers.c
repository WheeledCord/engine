/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_containers.h"

#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static UiRect DockBounds(const UiDockBoard *board, int group)
{
    UiRect bounds={0}; bool first=true;
    for (size_t i=0;i<board->count;i++) if (board->apps[i].group==group)
    {
        UiRect r=board->apps[i].rect;
        if (first) { bounds=r; first=false; continue; }
        int right=bounds.x+bounds.width, bottom=bounds.y+bounds.height;
        if (r.x<bounds.x) bounds.x=r.x;
        if (r.y<bounds.y) bounds.y=r.y;
        if (r.x+r.width>right) right=r.x+r.width;
        if (r.y+r.height>bottom) bottom=r.y+r.height;
        bounds.width=right-bounds.x; bounds.height=bottom-bounds.y;
    }
    return bounds;
}
static void DockMove(UiDockBoard *board, int group, int dx, int dy)
{
    for (size_t i=0;i<board->count;i++) if (board->apps[i].group==group)
    { board->apps[i].rect.x+=dx; board->apps[i].rect.y+=dy; }
}
static bool DockFirst(const UiDockBoard *board, size_t index)
{
    for (size_t j=0;j<index;j++) if (board->apps[j].group==board->apps[index].group) return false;
    return true;
}
static bool DockOverlap(UiRect a, UiRect b)
{
    return a.x<b.x+b.width && a.x+a.width>b.x && a.y<b.y+b.height && a.y+a.height>b.y;
}
static bool DockHasHandle(const UiDockBoard *board, int group)
{
    for (size_t i=0;i<board->count;i++)
        if (board->apps[i].group==group && board->apps[i].groupHandle) return true;
    return false;
}
static bool DockDrop(UiDockBoard *board, int group, UiRect viewport)
{
    UiRect a=DockBounds(board,group);
    int snap=board->snapDistance, best=snap*snap+1, dx=0,dy=0,join=0;
    for (size_t i=0;i<board->count;i++)
    {
        if (board->apps[i].group==group || !DockFirst(board,i)) continue;
        UiRect b=DockBounds(board,board->apps[i].group);
        const int offsets[4][2]={{b.x+b.width-a.x,b.y-a.y},{b.x-a.width-a.x,b.y-a.y},
                                 {b.x-a.x,b.y+b.height-a.y},{b.x-a.x,b.y-a.height-a.y}};
        for (int k=0;k<4;k++)
        {
            if (board->verticalStack && (k<2 ||
                (k==3 && DockHasHandle(board,board->apps[i].group)) ||
                (k==2 && DockHasHandle(board,group)))) continue;
            int x=offsets[k][0],y=offsets[k][1],d=x*x+y*y;
            if (abs(x)>snap || abs(y)>snap || d>=best) continue;
            UiRect proposed={a.x+x,a.y+y,a.width,a.height}; bool clear=true;
            if (proposed.x<viewport.x || proposed.y<viewport.y ||
                proposed.x+proposed.width>viewport.x+viewport.width ||
                proposed.y+proposed.height>viewport.y+viewport.height) continue;
            for (size_t j=0;j<board->count;j++)
                if (board->apps[j].group!=group && DockOverlap(proposed,board->apps[j].rect)) clear=false;
            if (clear) { best=d; dx=x; dy=y; join=board->apps[i].group; }
        }
    }
    if (join)
    {
        DockMove(board,group,dx,dy);
        for (size_t i=0;i<board->count;i++) if (board->apps[i].group==group) board->apps[i].group=join;
        group=join; a=DockBounds(board,group);
    }
    int edgeX=0,edgeY=0; dx=0; dy=0;
    if (abs(a.x-viewport.x)<=snap) { dx=viewport.x-a.x; edgeX=-1; }
    else if (abs(a.x+a.width-viewport.x-viewport.width)<=snap)
    { dx=viewport.x+viewport.width-a.width-a.x; edgeX=1; }
    if (abs(a.y-viewport.y)<=snap) { dy=viewport.y-a.y; edgeY=-1; }
    else if (abs(a.y+a.height-viewport.y-viewport.height)<=snap)
    { dy=viewport.y+viewport.height-a.height-a.y; edgeY=1; }
    DockMove(board,group,dx,dy);
    for (size_t i=0;i<board->count;i++) if (board->apps[i].group==group)
    { board->apps[i].edgeX=edgeX; board->apps[i].edgeY=edgeY; }
    if (board->edgeOnly)
    {
        if ((!edgeX && !edgeY) || (board->verticalStack && !edgeX)) return false;
        for (size_t i=0;i<board->count;i++) if (board->apps[i].group==group)
            for (size_t j=0;j<board->count;j++)
                if (board->apps[j].group!=group && DockOverlap(board->apps[i].rect,board->apps[j].rect))
                    return false;
    }
    return true;
}
UiDockEvent UiDockBoardStep(UiDockBoard *board, const EngineInput *input, UiRect viewport)
{
    UiDockEvent event={0};
    if (!board || !input || !board->apps || !board->count || viewport.width<=0 || viewport.height<=0 ||
        board->snapDistance<0 || board->snapDistance>1024 || board->dragThreshold<0) return event;
    if (board->edgeOnly && (!board->dragBackup || board->dragBackup==board->apps ||
                           board->dragBackupCount<board->count)) return event;
    for (size_t i=0;i<board->count;i++)
    {
        UiDockApp *app=&board->apps[i];
        if (app->id<=0 || app->rect.width<=0 || app->rect.height<=0) return event;
        for (size_t j=0;j<i;j++) if (board->apps[j].id==app->id) return event;
        if (!app->group) app->group=app->id;
    }
    if (board->active<0 || (size_t)board->active>board->count) board->active=0;
    for (size_t i=0;i<board->count;i++) if (DockFirst(board,i))
    {
        UiDockApp *app=&board->apps[i]; UiRect r=DockBounds(board,app->group);
        int x=r.x,y=r.y;
        if (app->edgeX<0) x=viewport.x;
        if (app->edgeX>0) x=viewport.x+viewport.width-r.width;
        if (app->edgeY<0) y=viewport.y;
        if (app->edgeY>0) y=viewport.y+viewport.height-r.height;
        x=ClampInt(x,viewport.x,viewport.x+viewport.width-r.width);
        y=ClampInt(y,viewport.y,viewport.y+viewport.height-r.height);
        DockMove(board,app->group,x-r.x,y-r.y);
        event.layoutChanged |= x!=r.x || y!=r.y;
    }
    board->viewport=viewport;
    int hot=-1;
    for (size_t i=0;i<board->count;i++) if (UiPointInRect(input->mousePosition,board->apps[i].rect)) hot=(int)i;
    event.mouseCaptured=hot>=0 || board->active!=0;
    if (!board->active && hot>=0 && input->mousePressed[MOUSE_BUTTON_LEFT])
    {
        board->active=hot+1; board->dragging=false;
        board->wholeGroup=board->apps[hot].groupHandle ||
            (!board->edgeOnly && input->mousePosition.y<board->apps[hot].rect.y+board->gripHeight);
        board->pressed=board->previous=input->mousePosition;
        if (board->edgeOnly) memcpy(board->dragBackup,board->apps,board->count*sizeof(*board->apps));
    }
    if (board->active)
    {
        UiDockApp *app=&board->apps[board->active-1];
        float px=input->mousePosition.x-board->pressed.x,py=input->mousePosition.y-board->pressed.y;
        if (!board->dragging && input->mouseDown[MOUSE_BUTTON_LEFT] &&
            px*px+py*py>(float)(board->dragThreshold*board->dragThreshold))
        {
            board->dragging=true;
            if (!board->wholeGroup)
            {
                int oldGroup=app->group;
                UiRect removed=app->rect;
                bool vertical=true,horizontal=true;
                for (size_t i=0;i<board->count;i++) if (board->apps[i].group==oldGroup)
                {
                    vertical &= board->apps[i].rect.x==removed.x;
                    horizontal &= board->apps[i].rect.y==removed.y;
                }
                int fresh=app->id;
                for (size_t i=0;i<board->count;i++) if (board->apps[i].group>=fresh) fresh=board->apps[i].group+1;
                app->group=fresh;
                for (size_t i=0;i<board->count;i++) if (board->apps[i].group==oldGroup)
                {
                    UiRect *r=&board->apps[i].rect;
                    if (vertical && r->y>removed.y) r->y-=removed.height;
                    else if (horizontal && r->x>removed.x) r->x-=removed.width;
                    // Keep the root stationary when removing an app from an edge-pinned bar.
                    if (vertical && board->apps[i].edgeY>0) board->apps[i].edgeY=0;
                    if (horizontal && board->apps[i].edgeX>0) board->apps[i].edgeX=0;
                }
            }
            for (size_t i=0;i<board->count;i++) if (board->apps[i].group==app->group)
                board->apps[i].edgeX=board->apps[i].edgeY=0;
        }
        if (board->dragging && input->mouseDown[MOUSE_BUTTON_LEFT])
        {
            UiRect r=DockBounds(board,app->group);
            int dx=(int)lroundf(input->mousePosition.x-board->previous.x);
            int dy=(int)lroundf(input->mousePosition.y-board->previous.y);
            dx=ClampInt(r.x+dx,viewport.x,viewport.x+viewport.width-r.width)-r.x;
            dy=ClampInt(r.y+dy,viewport.y,viewport.y+viewport.height-r.height)-r.y;
            DockMove(board,app->group,dx,dy); event.layoutChanged=true;
            board->previous=input->mousePosition;
        }
        if (input->mouseReleased[MOUSE_BUTTON_LEFT] || !input->mouseDown[MOUSE_BUTTON_LEFT])
        {
            if (board->dragging)
            {
                if (!DockDrop(board,app->group,viewport) && board->edgeOnly)
                    memcpy(board->apps,board->dragBackup,board->count*sizeof(*board->apps));
                event.layoutChanged=true;
            }
            else if ((!board->wholeGroup || app->groupHandle) && hot==board->active-1 && input->mouseReleased[MOUSE_BUTTON_LEFT])
                event.activated=app->id;
            board->active=0; board->dragging=false;
        }
    }
    return event;
}
UiDockEvent UiDockBoardControl(UiContext *ui, UiDockBoard *board)
{
    if (!ui || !ui->state || !UiInputAllowed(ui)) return (UiDockEvent){0};
    UiDockEvent event=UiDockBoardStep(board,&ui->state->input,ui->state->screen);
    if (event.mouseCaptured) UiMarkMouse(ui);
    return event;
}
