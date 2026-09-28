/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_ISO_MOVE_H
#define GAMEPLAY_ISO_MOVE_H

#include "core/iso_grid.h"
#include "core/object.h"

/* Routing and walking over the hex grid in core/iso_grid.h.

   Nothing here knows what blocks a hex, what a move costs, or whether there are turns at all. The
   caller answers what is blocked through IsoBlockedFn, so walls, scenery, other characters and shut
   doors are all the same question asked of the project. A game that rations movement plans a walk
   and then cuts it to what it will allow with IsoMoverTruncate; what the allowance is, and what it
   is called, stays with the game. */

typedef bool (*IsoBlockedFn)(void *user, IsoHex hex);

typedef struct IsoPath
{
    IsoHex *hexes; /* hexes[0] is where the walk starts; the rest are entered in order */
    int count, capacity;
} IsoPath;

bool IsoPathInit(IsoPath *path, int capacity);
void IsoPathFree(IsoPath *path);
void IsoPathClear(IsoPath *path);

/* A* over a bounded hex grid. The workspace is sized once and reused, because a search that
   allocates is a search a turn cannot afford to run often. */
typedef struct IsoPathfinder
{
    int width, height;
    float *gScore, *fScore;
    int *cameFrom, *heap, *heapAt;
    unsigned char *seen, *closed;
    int heapCount;
} IsoPathfinder;

bool IsoPathfinderInit(IsoPathfinder *finder, int width, int height);
void IsoPathfinderFree(IsoPathfinder *finder);
/* Fills path with the route from `from` to `to` inclusive. False when there is no route, when
   either end lies off the grid or is blocked, or when the route is longer than the path can hold. */
bool IsoPathfinderSolve(IsoPathfinder *finder, IsoPath *path, IsoHex from, IsoHex to,
                        IsoBlockedFn blocked, void *user);

typedef struct IsoMover
{
    IsoHex hex; /* the hex it occupies; it is never between two of them as far as the grid cares */
    IsoDir facing;
    IsoPath path;
    int step;       /* index in path of the hex being entered */
    float progress; /* 0..1 from path[step-1] to path[step], for drawing only */
    float hexesPerSecond;
} IsoMover;

bool IsoMoverInit(IsoMover *mover, IsoHex start, int pathCapacity);
void IsoMoverFree(IsoMover *mover);
/* Plans a walk to the target and starts it. The whole route is walked unless the caller cuts it. */
bool IsoMoverGoTo(IsoMover *mover, IsoPathfinder *finder, IsoHex target, IsoBlockedFn blocked,
                  void *user);
/* Cuts the planned walk to at most this many hexes, for a game that rations movement. Called after
   IsoMoverGoTo; a walk already cut this short is left alone. */
void IsoMoverTruncate(IsoMover *mover, int maxSteps);
/* Hexes still to be entered on the current walk. */
int IsoMoverRemainingSteps(const IsoMover *mover);
void IsoMoverStop(IsoMover *mover);
bool IsoMoverMoving(const IsoMover *mover);
void IsoMoverUpdate(IsoMover *mover, float dt);
/* Interpolated position for drawing, between the hex left and the hex being entered. */
Vector2 IsoMoverScreen(const IsoMover *mover);

/* A pathfinder that keeps its own map of blocked hexes and the route it last solved, so a script
   can route without writing an IsoBlockedFn. The "pathfinder" engine type, created with a grid
   width and height: block!, unblock!, blocked?, solve!, route-length and route-hex. */
typedef struct IsoRouter
{
    IsoPathfinder finder;
    IsoPath route;
    bool *blocked; /* width * height, row-major; off the grid counts as blocked */
    int width, height;
} IsoRouter;

/* The engine types. "mover" is created on a starting hex, optionally with a route capacity: go-to!
   through a pathfinder, truncate! and stop!; hex, screen, moving? and remaining. A mover walks as
   its pool steps and emits "arrived" with the hex it stopped on when a walk ends. */
extern const EngineType IsoRouterType;
extern const EngineType IsoMoverType;
#endif
