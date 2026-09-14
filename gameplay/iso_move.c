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
