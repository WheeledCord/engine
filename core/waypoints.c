/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "waypoints.h"

#include "raymath.h"

void CoreWaypointsClear(CoreWaypoints *w)
{
    if (!w)
        return;
    w->count = 0;
}

int CoreWaypointsAdd(CoreWaypoints *w, Vector3 position)
{
    if (!w || w->count >= CORE_WAYPOINTS_MAX)
        return -1;
    int i = w->count++;
    w->position[i] = position;
    for (int k = 0; k < CORE_WAYPOINT_LINKS; k++)
        w->links[i][k] = -1;
    return i;
}

bool CoreWaypointsLink(CoreWaypoints *w, int a, int b)
{
    if (!w || a < 0 || a >= w->count || b < 0 || b >= w->count || a == b)
        return false;
    int freeA = -1, freeB = -1;
    for (int k = 0; k < CORE_WAYPOINT_LINKS; k++)
    {
        if (w->links[a][k] == b)
            return true; // already linked, nothing to add
        if (freeA < 0 && w->links[a][k] < 0)
            freeA = k;
    }
    for (int k = 0; k < CORE_WAYPOINT_LINKS; k++)
        if (freeB < 0 && w->links[b][k] < 0)
            freeB = k;
    if (freeA < 0 || freeB < 0)
        return false;
    w->links[a][freeA] = b;
    w->links[b][freeB] = a;
    return true;
}

int CoreWaypointsNearest(const CoreWaypoints *w, Vector3 at)
{
    if (!w || w->count == 0)
        return -1;
    int best = -1;
    float bestD = 0.0f;
    for (int i = 0; i < w->count; i++)
    {
        float d = Vector3DistanceSqr(w->position[i], at);
        if (best < 0 || d < bestD)
        {
            best = i;
            bestD = d;
        }
    }
    return best;
}

/* A*, as Godot's AStar3D::_solve (references/godot/core/math/a_star.cpp:310-393): an open set
   scored by g (distance travelled from `from`) and f = g plus the straight-line distance to `to`
   (AStar3D::_estimate_cost, a_star.cpp:395-410); each edge costs its own Euclidean length
   (AStar3D::_compute_cost, a_star.cpp:412-427). The capacity here (CORE_WAYPOINTS_MAX) is small
   enough that picking the open point with the lowest f is a plain scan, not Godot's binary heap.
   Fills prev[] with each visited point's predecessor (-1 for `from`, -2 for never reached) and
   answers whether `to` was reached. Caller guarantees 0 <= from,to < w->count and from != to. */
static bool Solve(const CoreWaypoints *w, int from, int to, int prev[CORE_WAYPOINTS_MAX])
{
    float g[CORE_WAYPOINTS_MAX], f[CORE_WAYPOINTS_MAX];
    bool open[CORE_WAYPOINTS_MAX] = {0}, closed[CORE_WAYPOINTS_MAX] = {0};
    for (int i = 0; i < w->count; i++)
        prev[i] = -2;
    prev[from] = -1;
    g[from] = 0.0f;
    f[from] = Vector3Distance(w->position[from], w->position[to]);
    open[from] = true;
    for (;;)
    {
        int best = -1;
        for (int i = 0; i < w->count; i++)
            if (open[i] && (best < 0 || f[i] < f[best]))
                best = i;
        if (best < 0)
            return false; // open set exhausted before reaching `to` -- unreachable
        if (best == to)
            return true;
        open[best] = false;
        closed[best] = true;
        for (int k = 0; k < CORE_WAYPOINT_LINKS; k++)
        {
            int e = w->links[best][k];
            if (e < 0 || closed[e])
                continue;
            float tentative = g[best] + Vector3Distance(w->position[best], w->position[e]);
            if (open[e] && tentative >= g[e])
                continue; // no better than the path already known
            prev[e] = best;
            g[e] = tentative;
            f[e] = tentative + Vector3Distance(w->position[e], w->position[to]);
            open[e] = true;
        }
    }
}

int CoreWaypointsNextHop(const CoreWaypoints *w, int from, int to)
{
    if (!w || from < 0 || from >= w->count || to < 0 || to >= w->count)
        return -1;
    if (from == to)
        return from;
    int prev[CORE_WAYPOINTS_MAX];
    if (!Solve(w, from, to, prev))
        return -1;
    int c = to;
    while (prev[c] != from)
        c = prev[c];
    return c;
}

int CoreWaypointsPath(const CoreWaypoints *w, int from, int to, int *out, int capacity)
{
    if (!w || !out || capacity <= 0 || from < 0 || from >= w->count || to < 0 || to >= w->count)
        return 0;
    if (from == to)
    {
        out[0] = from;
        return 1;
    }
    int prev[CORE_WAYPOINTS_MAX];
    if (!Solve(w, from, to, prev))
        return 0;
    int count = 1;
    for (int c = to; c != from; c = prev[c])
        count++;
    if (count > capacity)
        return 0;
    int c = to;
    for (int i = count - 1; i >= 0; i--)
    {
        out[i] = c;
        if (c == from)
            break;
        c = prev[c];
    }
    return count;
}

// ---- as an engine type ---------------------------------------------------------------------------
static bool WaypointsAdd(EngineCall *call)
{
    call->result = EngineInt(CoreWaypointsAdd(call->data, call->arguments[0].as.vector3));
    return true;
}
static bool WaypointsLink(EngineCall *call)
{
    const EngineValue *a = call->arguments;
    call->result = EngineBool(CoreWaypointsLink(call->data, a[0].as.integer, a[1].as.integer));
    return true;
}
static bool WaypointsNearest(EngineCall *call)
{
    call->result = EngineInt(CoreWaypointsNearest(call->data, call->arguments[0].as.vector3));
    return true;
}
static bool WaypointsNextHop(EngineCall *call)
{
    const EngineValue *a = call->arguments;
    call->result = EngineInt(CoreWaypointsNextHop(call->data, a[0].as.integer, a[1].as.integer));
    return true;
}
static bool WaypointsPosition(EngineCall *call)
{
    const CoreWaypoints *w = call->data;
    int at = call->arguments[0].as.integer;
    if (at < 0 || at >= w->count)
    {
        call->error = "that index is not a waypoint";
        return false;
    }
    call->result = EngineVector3(w->position[at]);
    return true;
}
static const EngineProperty waypointsProperties[] = {
    ENGINE_FIELD("count", CoreWaypoints, count, ENGINE_INT, ENGINE_PROPERTY_READ_ONLY,
                 "points in the graph"),
};
static const EngineMethod waypointsMethods[] = {
    {"add!", ENGINE_INT, {ENGINE_VECTOR3}, 1, WaypointsAdd,
     "add a point; answers its index, or -1 when the graph is full"},
    {"link!", ENGINE_BOOL, {ENGINE_INT, ENGINE_INT}, 2, WaypointsLink,
     "link two points, undirected"},
    {"nearest", ENGINE_INT, {ENGINE_VECTOR3}, 1, WaypointsNearest,
     "the point nearest a position, or -1 when the graph is empty"},
    {"next-hop", ENGINE_INT, {ENGINE_INT, ENGINE_INT}, 2, WaypointsNextHop,
     "the next point on the shortest route to another; -1 when unreachable"},
    {"position", ENGINE_VECTOR3, {ENGINE_INT}, 1, WaypointsPosition, "a point's position"},
};
const EngineType CoreWaypointsType = {
    .name = "waypoints",
    .size = sizeof(CoreWaypoints),
    .properties = waypointsProperties,
    .propertyCount = sizeof waypointsProperties / sizeof waypointsProperties[0],
    .methods = waypointsMethods,
    .methodCount = sizeof waypointsMethods / sizeof waypointsMethods[0],
    .help = "a point graph with shortest-path queries, by summed straight-line edge distance",
};
