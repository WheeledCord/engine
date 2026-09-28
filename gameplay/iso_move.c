/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "iso_move.h"
#include <stdlib.h>
#include <string.h>

bool IsoPathInit(IsoPath *path, int capacity)
{
    *path = (IsoPath){0};
    if (capacity < 1)
        return false;
    path->hexes = calloc((size_t)capacity, sizeof(*path->hexes));
    if (!path->hexes)
        return false;
    path->capacity = capacity;
    return true;
}
void IsoPathFree(IsoPath *path)
{
    free(path->hexes);
    *path = (IsoPath){0};
}
void IsoPathClear(IsoPath *path)
{
    path->count = 0;
}

bool IsoPathfinderInit(IsoPathfinder *finder, int width, int height)
{
    *finder = (IsoPathfinder){0};
    if (width < 1 || height < 1 || width > 4096 || height > 4096)
        return false;
    size_t cells = (size_t)width * (size_t)height;
    finder->gScore = calloc(cells, sizeof(*finder->gScore));
    finder->fScore = calloc(cells, sizeof(*finder->fScore));
    finder->cameFrom = calloc(cells, sizeof(*finder->cameFrom));
    finder->heap = calloc(cells, sizeof(*finder->heap));
    finder->heapAt = calloc(cells, sizeof(*finder->heapAt));
    finder->seen = calloc(cells, sizeof(*finder->seen));
    finder->closed = calloc(cells, sizeof(*finder->closed));
    if (!finder->gScore || !finder->fScore || !finder->cameFrom || !finder->heap || !finder->heapAt ||
        !finder->seen || !finder->closed)
    {
        IsoPathfinderFree(finder);
        return false;
    }
    finder->width = width;
    finder->height = height;
    return true;
}
void IsoPathfinderFree(IsoPathfinder *finder)
{
    free(finder->gScore);
    free(finder->fScore);
    free(finder->cameFrom);
    free(finder->heap);
    free(finder->heapAt);
    free(finder->seen);
    free(finder->closed);
    *finder = (IsoPathfinder){0};
}
static bool InBounds(const IsoPathfinder *finder, IsoHex hex)
{
    return hex.x >= 0 && hex.y >= 0 && hex.x < finder->width && hex.y < finder->height;
}
static int Index(const IsoPathfinder *finder, IsoHex hex)
{
    return hex.y * finder->width + hex.x;
}
static void HeapSwap(IsoPathfinder *finder, int a, int b)
{
    int nodeA = finder->heap[a], nodeB = finder->heap[b];
    finder->heap[a] = nodeB;
    finder->heap[b] = nodeA;
    finder->heapAt[nodeB] = a;
    finder->heapAt[nodeA] = b;
}
static void HeapUp(IsoPathfinder *finder, int at)
{
    while (at > 0)
    {
        int parent = (at - 1) / 2;
        if (finder->fScore[finder->heap[parent]] <= finder->fScore[finder->heap[at]])
            break;
        HeapSwap(finder, at, parent);
        at = parent;
    }
}
static void HeapDown(IsoPathfinder *finder, int at)
{
    for (;;)
    {
        int left = at * 2 + 1, right = left + 1, best = at;
        if (left < finder->heapCount && finder->fScore[finder->heap[left]] < finder->fScore[finder->heap[best]])
            best = left;
        if (right < finder->heapCount &&
            finder->fScore[finder->heap[right]] < finder->fScore[finder->heap[best]])
            best = right;
        if (best == at)
            break;
        HeapSwap(finder, at, best);
        at = best;
    }
}
static void HeapPush(IsoPathfinder *finder, int node)
{
    finder->heap[finder->heapCount] = node;
    finder->heapAt[node] = finder->heapCount;
    finder->heapCount++;
    HeapUp(finder, finder->heapCount - 1);
}
static int HeapPop(IsoPathfinder *finder)
{
    int node = finder->heap[0];
    finder->heapCount--;
    if (finder->heapCount > 0)
    {
        finder->heap[0] = finder->heap[finder->heapCount];
        finder->heapAt[finder->heap[0]] = 0;
        HeapDown(finder, 0);
    }
    finder->heapAt[node] = -1;
    return node;
}
bool IsoPathfinderSolve(IsoPathfinder *finder, IsoPath *path, IsoHex from, IsoHex to,
                        IsoBlockedFn blocked, void *user)
{
    if (!finder->width || !path->capacity)
        return false;
    IsoPathClear(path);
    if (!InBounds(finder, from) || !InBounds(finder, to))
        return false;
    if (blocked && (blocked(user, from) || blocked(user, to)))
        return false;
    size_t cells = (size_t)finder->width * (size_t)finder->height;
    memset(finder->seen, 0, cells);
    memset(finder->closed, 0, cells);
    finder->heapCount = 0;
    int start = Index(finder, from), goal = Index(finder, to);
    finder->gScore[start] = 0;
    finder->fScore[start] = (float)IsoHexDistance(from, to);
    finder->cameFrom[start] = -1;
    finder->seen[start] = 1;
    HeapPush(finder, start);
    while (finder->heapCount > 0)
    {
        int node = HeapPop(finder);
        if (node == goal)
        {
            /* Walk the trail back, then reverse it, so the path reads in the order it is walked. */
            int length = 0;
            for (int at = node; at != -1; at = finder->cameFrom[at])
                length++;
            if (length > path->capacity)
                return false;
            int at = node;
            for (int i = length - 1; i >= 0; i--)
            {
                path->hexes[i] = (IsoHex){at % finder->width, at / finder->width};
                at = finder->cameFrom[at];
            }
            path->count = length;
            return true;
        }
        finder->closed[node] = 1;
        IsoHex here = {node % finder->width, node / finder->width};
        for (int dir = 0; dir < ISO_DIR_COUNT; dir++)
        {
            IsoHex next = IsoHexNeighbour(here, (IsoDir)dir);
            if (!InBounds(finder, next))
                continue;
            int index = Index(finder, next);
            if (finder->closed[index] || (blocked && blocked(user, next)))
                continue;
            float tentative = finder->gScore[node] + 1.0f;
            if (finder->seen[index] && tentative >= finder->gScore[index])
                continue;
            finder->gScore[index] = tentative;
            finder->fScore[index] = tentative + (float)IsoHexDistance(next, to);
            finder->cameFrom[index] = node;
            if (!finder->seen[index])
            {
                finder->seen[index] = 1;
                HeapPush(finder, index);
            }
            else
                HeapUp(finder, finder->heapAt[index]);
        }
    }
    return false;
}

bool IsoMoverInit(IsoMover *mover, IsoHex start, int pathCapacity)
{
    *mover = (IsoMover){0};
    if (!IsoPathInit(&mover->path, pathCapacity))
        return false;
    mover->hex = start;
    mover->facing = ISO_SE;
    mover->hexesPerSecond = 4.0f;
    return true;
}
void IsoMoverFree(IsoMover *mover)
{
    IsoPathFree(&mover->path);
}
void IsoMoverStop(IsoMover *mover)
{
    IsoPathClear(&mover->path);
    mover->step = 0;
    mover->progress = 0;
}
bool IsoMoverMoving(const IsoMover *mover)
{
    return mover->path.count > 0 && mover->step < mover->path.count;
}
int IsoMoverRemainingSteps(const IsoMover *mover)
{
    return IsoMoverMoving(mover) ? mover->path.count - mover->step : 0;
}
bool IsoMoverGoTo(IsoMover *mover, IsoPathfinder *finder, IsoHex target, IsoBlockedFn blocked, void *user)
{
    IsoMoverStop(mover);
    if (!IsoPathfinderSolve(finder, &mover->path, mover->hex, target, blocked, user))
        return false;
    if (mover->path.count < 2)
    {
        IsoMoverStop(mover);
        return false;
    }
    mover->step = 1; /* hexes[0] is where it already stands */
    mover->progress = 0;
    mover->facing = IsoHexDirection(mover->hex, mover->path.hexes[1]);
    return true;
}
void IsoMoverTruncate(IsoMover *mover, int maxSteps)
{
    if (maxSteps < 0 || !IsoMoverMoving(mover))
        return;
    if (mover->path.count - 1 > maxSteps)
        mover->path.count = maxSteps + 1;
    if (mover->path.count < 2)
        IsoMoverStop(mover);
}
void IsoMoverUpdate(IsoMover *mover, float dt)
{
    if (!IsoMoverMoving(mover) || dt <= 0)
        return;
    mover->progress += mover->hexesPerSecond * dt;
    while (mover->progress >= 1.0f)
    {
        mover->hex = mover->path.hexes[mover->step];
        mover->step++;
        mover->progress -= 1.0f;
        if (mover->step >= mover->path.count)
        {
            IsoMoverStop(mover);
            return;
        }
        mover->facing = IsoHexDirection(mover->hex, mover->path.hexes[mover->step]);
    }
}
Vector2 IsoMoverScreen(const IsoMover *mover)
{
    Vector2 here = IsoHexToScreen(mover->hex);
    if (!IsoMoverMoving(mover))
        return here;
    Vector2 next = IsoHexToScreen(mover->path.hexes[mover->step]);
    float t = mover->progress;
    return (Vector2){here.x + (next.x - here.x) * t, here.y + (next.y - here.y) * t};
}

// ---- as engine types -----------------------------------------------------------------------------
#define ISO_ROUTE_STEP_CAP 4096 // the longest route a pathfinder object keeps

static IsoHex HexOf(Vector2 v) { return (IsoHex){(int)v.x, (int)v.y}; }
static Vector2 VectorOf(IsoHex h) { return (Vector2){(float)h.x, (float)h.y}; }

static bool RouterBlocked(void *user, IsoHex hex)
{
    const IsoRouter *router = user;
    if (hex.x < 0 || hex.x >= router->width || hex.y < 0 || hex.y >= router->height)
        return true;
    return router->blocked[hex.y * router->width + hex.x];
}

static bool RouterCreate(EngineCall *call)
{
    IsoRouter *router = call->data;
    int width = call->arguments[0].as.integer, height = call->arguments[1].as.integer;
    if (width <= 0 || height <= 0)
    {
        call->error = "a pathfinder needs a width and height above zero";
        return false;
    }
    int steps = width * height < ISO_ROUTE_STEP_CAP ? width * height : ISO_ROUTE_STEP_CAP;
    router->blocked = calloc((size_t)width * (size_t)height, sizeof *router->blocked);
    if (!router->blocked || !IsoPathfinderInit(&router->finder, width, height))
    {
        free(router->blocked);
        call->error = "out of memory";
        return false;
    }
    if (!IsoPathInit(&router->route, steps))
    {
        IsoPathfinderFree(&router->finder);
        free(router->blocked);
        call->error = "out of memory";
        return false;
    }
    router->width = width;
    router->height = height;
    return true;
}

static void RouterDestroy(void *data)
{
    IsoRouter *router = data;
    IsoPathfinderFree(&router->finder);
    IsoPathFree(&router->route);
    free(router->blocked);
}

static bool *RouterCell(IsoRouter *router, IsoHex hex)
{
    if (hex.x < 0 || hex.x >= router->width || hex.y < 0 || hex.y >= router->height)
        return NULL;
    return &router->blocked[hex.y * router->width + hex.x];
}

static bool RouterMark(EngineCall *call, bool blocked)
{
    bool *cell = RouterCell(call->data, HexOf(call->arguments[0].as.vector2));
    if (cell)
        *cell = blocked;
    call->result = EngineBool(cell != NULL);
    return true;
}
static bool RouterBlock(EngineCall *call) { return RouterMark(call, true); }
static bool RouterUnblock(EngineCall *call) { return RouterMark(call, false); }
static bool RouterIsBlocked(EngineCall *call)
{
    call->result = EngineBool(RouterBlocked(call->data, HexOf(call->arguments[0].as.vector2)));
    return true;
}
static bool RouterSolve(EngineCall *call)
{
    IsoRouter *router = call->data;
    IsoPathClear(&router->route);
    bool found = IsoPathfinderSolve(&router->finder, &router->route,
                                    HexOf(call->arguments[0].as.vector2),
                                    HexOf(call->arguments[1].as.vector2), RouterBlocked, router);
    if (!found)
        IsoPathClear(&router->route);
    call->result = EngineInt(router->route.count);
    return true;
}
static bool RouterHex(EngineCall *call)
{
    IsoRouter *router = call->data;
    int at = call->arguments[0].as.integer;
    if (at < 0 || at >= router->route.count)
    {
        call->error = "that step is not on the route";
        return false;
    }
    call->result = EngineVector2(VectorOf(router->route.hexes[at]));
    return true;
}
static bool RouterLength(const void *object, EngineValue *out)
{
    *out = EngineInt(((const IsoRouter *)object)->route.count);
    return true;
}
static const EngineProperty routerProperties[] = {
    ENGINE_COMPUTED("route-length", ENGINE_INT, ENGINE_PROPERTY_READ_ONLY, RouterLength, NULL,
                    "hexes in the route the last solve! found, zero for none"),
};
static const EngineMethod routerMethods[] = {
    {"block!", ENGINE_BOOL, {ENGINE_VECTOR2}, 1, RouterBlock, "mark a hex impassable; false off the grid"},
    {"unblock!", ENGINE_BOOL, {ENGINE_VECTOR2}, 1, RouterUnblock, "mark a hex passable; false off the grid"},
    {"blocked?", ENGINE_BOOL, {ENGINE_VECTOR2}, 1, RouterIsBlocked, "whether a hex is impassable"},
    {"solve!", ENGINE_INT, {ENGINE_VECTOR2, ENGINE_VECTOR2}, 2, RouterSolve,
     "route from one hex to another; answers the route's length, zero for none"},
    {"route-hex", ENGINE_VECTOR2, {ENGINE_INT}, 1, RouterHex, "one hex of the last route, 0 first"},
};
const EngineType IsoRouterType = {
    .name = "pathfinder",
    .size = sizeof(IsoRouter),
    .properties = routerProperties,
    .propertyCount = 1,
    .methods = routerMethods,
    .methodCount = sizeof routerMethods / sizeof routerMethods[0],
    .createArguments = {ENGINE_INT, ENGINE_INT},
    .createArgumentCount = 2,
    .createRequired = 2,
    .create = RouterCreate,
    .destroy = RouterDestroy,
    .help = "A* over a hex grid with its own map of blocked hexes",
};

static bool MoverCreate(EngineCall *call)
{
    int capacity = call->count > 1 && call->arguments[1].as.integer > 0 ? call->arguments[1].as.integer
                                                                       : ISO_ROUTE_STEP_CAP;
    return IsoMoverInit(call->data, HexOf(call->arguments[0].as.vector2), capacity);
}
static void MoverDestroy(void *data) { IsoMoverFree(data); }
static void MoverStep(EngineObjects *objects, EngineObjectId self, void *data, float dt)
{
    IsoMover *mover = data;
    if (!IsoMoverMoving(mover))
        return;
    IsoMoverUpdate(mover, dt);
    if (!IsoMoverMoving(mover))
        EngineObjectEmit(objects, self, "arrived", (EngineValue[]){EngineVector2(VectorOf(mover->hex))}, 1);
}
static bool MoverGoTo(EngineCall *call)
{
    IsoRouter *router = EngineCallObject(call, 0, &IsoRouterType);
    if (!router)
    {
        call->error = "go-to! needs a pathfinder";
        return false;
    }
    call->result = EngineBool(IsoMoverGoTo(call->data, &router->finder,
                                           HexOf(call->arguments[1].as.vector2), RouterBlocked, router));
    return true;
}
static bool MoverTruncate(EngineCall *call)
{
    IsoMoverTruncate(call->data, call->arguments[0].as.integer);
    return true;
}
static bool MoverStop(EngineCall *call)
{
    IsoMoverStop(call->data);
    return true;
}
static bool MoverHex(const void *object, EngineValue *out)
{
    *out = EngineVector2(VectorOf(((const IsoMover *)object)->hex));
    return true;
}
static bool MoverScreen(const void *object, EngineValue *out)
{
    *out = EngineVector2(IsoMoverScreen(object));
    return true;
}
static bool MoverMoving(const void *object, EngineValue *out)
{
    *out = EngineBool(IsoMoverMoving(object));
    return true;
}
static bool MoverRemaining(const void *object, EngineValue *out)
{
    *out = EngineInt(IsoMoverRemainingSteps(object));
    return true;
}
static const EngineProperty moverProperties[] = {
    ENGINE_COMPUTED("hex", ENGINE_VECTOR2, ENGINE_PROPERTY_READ_ONLY, MoverHex, NULL,
                    "the hex it occupies"),
    ENGINE_COMPUTED("screen", ENGINE_VECTOR2, ENGINE_PROPERTY_READ_ONLY, MoverScreen, NULL,
                    "where to draw it, between the hex left and the hex being entered"),
    ENGINE_COMPUTED("moving?", ENGINE_BOOL, ENGINE_PROPERTY_READ_ONLY, MoverMoving, NULL,
                    "whether a walk is underway"),
    ENGINE_COMPUTED("remaining", ENGINE_INT, ENGINE_PROPERTY_READ_ONLY, MoverRemaining, NULL,
                    "hexes still to be entered"),
    ENGINE_FIELD("speed", IsoMover, hexesPerSecond, ENGINE_FLOAT, 0, "hexes walked per second"),
};
static const EngineMethod moverMethods[] = {
    {"go-to!", ENGINE_BOOL, {ENGINE_OBJECT, ENGINE_VECTOR2}, 2, MoverGoTo,
     "plan a walk to a hex through a pathfinder and set off"},
    {"truncate!", ENGINE_NONE, {ENGINE_INT}, 1, MoverTruncate, "cut the walk to at most so many hexes"},
    {"stop!", ENGINE_NONE, {ENGINE_NONE}, 0, MoverStop, "stop where it stands"},
};
static const char *const moverSignals[] = {"arrived"};
const EngineType IsoMoverType = {
    .name = "mover",
    .size = sizeof(IsoMover),
    .properties = moverProperties,
    .propertyCount = sizeof moverProperties / sizeof moverProperties[0],
    .methods = moverMethods,
    .methodCount = sizeof moverMethods / sizeof moverMethods[0],
    .signals = moverSignals,
    .signalCount = 1,
    .createArguments = {ENGINE_VECTOR2, ENGINE_INT},
    .createArgumentCount = 2,
    .createRequired = 1,
    .create = MoverCreate,
    .destroy = MoverDestroy,
    .step = MoverStep,
    .help = "walks a planned route across the hex grid a hex at a time",
};
