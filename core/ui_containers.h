/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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

/* Floating dock apps. Layout/interaction only: callers own content, theme and persistence.
   A body click activates; a body drag detaches one app; a grip drag moves its attached bar.
   Equal nonzero group values mean attached. IDs must be unique and positive. */
typedef struct UiDockApp
{
    int id, group;
    UiRect rect;
    int edgeX, edgeY; /* -1: near edge, 0: floating, +1: far edge */
    bool groupHandle; /* drag anywhere on this app to move its group; click still activates */
} UiDockApp;
typedef struct UiDockBoard
{
    UiDockApp *apps;
    size_t count;
    int snapDistance, dragThreshold, gripHeight;
    bool edgeOnly, verticalStack; /* optional constrained docks; vertical stacks dock left/right */
    UiDockApp *dragBackup; /* caller-owned count-element scratch; required for edgeOnly */
    size_t dragBackupCount;
    int active; /* private interaction state, zero-initialize */
    bool dragging, wholeGroup;
    Vector2 pressed, previous;
    UiRect viewport;
} UiDockBoard;
typedef struct UiDockEvent
{
    int activated; /* app ID, or zero */
    bool mouseCaptured, layoutChanged;
} UiDockEvent;

/** Advances a caller-owned dock layout, without drawing or allocating. Invalid layouts return
    an empty event. Release after a drag never activates. Snaps whole bounds to edges/neighbours.
    edgeOnly rejects floating/overlapping drops, restoring the complete pre-drag layout. A group
    handle stays first in a vertical stack. Scratch must not alias apps and must remain alive. */
UiDockEvent UiDockBoardStep(UiDockBoard *board, const EngineInput *input, UiRect viewport);
/** Runs the same interaction in the current UI frame and routes pointer capture through UiCapture.
    Draw the resulting rectangles with ordinary widgets or UiDrawCustom. */
UiDockEvent UiDockBoardControl(UiContext *ui, UiDockBoard *board);

#endif
