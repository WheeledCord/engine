/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_WAYPOINTS_H
#define CORE_WAYPOINTS_H

#include "object.h"
#include "raylib.h"
#include <stdbool.h>

/* A general point graph for a project's own navigation -- nodes plus the edges a project links
   between them, with the fewest-hops route unlikely to be the shortest, so a query is a distance
   search, not a hop count. Fixed capacity, no allocation, the way core's other pools work.

   Shortest path follows Godot's AStar3D (references/godot/core/math/a_star.h, a_star.cpp
   AStar3D::_solve): an open set scored by g (distance travelled) and f = g plus a straight-line
   heuristic to the goal (AStar3D::_estimate_cost, a_star.cpp:395-410), each edge costed by its own
   Euclidean length (AStar3D::_compute_cost, a_star.cpp:412-427). The capacity here is small enough
   that the open set is a plain scan for the lowest f, not Godot's binary heap. */
#define CORE_WAYPOINTS_MAX 256
#define CORE_WAYPOINT_LINKS 8

typedef struct CoreWaypoints
{
    Vector3 position[CORE_WAYPOINTS_MAX];
    int links[CORE_WAYPOINTS_MAX][CORE_WAYPOINT_LINKS]; /* -1 = no link in that slot */
    int count;
} CoreWaypoints;

/** @brief Empties a graph. Safe to call on one never added to.
 * @param waypoints Graph to clear.
 * @return No value. */
void CoreWaypointsClear(CoreWaypoints *waypoints);

/** @brief Adds a point.
 * @param waypoints Graph to add to.
 * @param position World position of the new point.
 * @return The new point's index, or -1 when the graph is already full. */
int CoreWaypointsAdd(CoreWaypoints *waypoints, Vector3 position);

/** @brief Links two points, undirected.
 * @param waypoints Graph holding both points.
 * @param a First point index.
 * @param b Second point index.
 * @return True when linked or already linked; false for a bad index, a == b, or either point's
 * link list already full. */
bool CoreWaypointsLink(CoreWaypoints *waypoints, int a, int b);

/** @brief Finds the point nearest a position.
 * @param waypoints Graph to search.
 * @param at Position to search from.
 * @return The nearest point's index by straight-line distance, or -1 when the graph is empty. */
int CoreWaypointsNearest(const CoreWaypoints *waypoints, Vector3 at);

/** @brief Finds the next point to step to on the shortest route between two points.
 * @param waypoints Graph to search.
 * @param from Starting point index.
 * @param to Destination point index.
 * @return The point after `from` on the route shortest by summed edge distance; `from` itself
 * when from == to; -1 for a bad index or when `to` cannot be reached from `from`. */
int CoreWaypointsNextHop(const CoreWaypoints *waypoints, int from, int to);

/** @brief Writes the shortest route between two points.
 * @param waypoints Graph to search.
 * @param from Starting point index.
 * @param to Destination point index.
 * @param out Caller-owned array to receive the route, from..to inclusive.
 * @param capacity Number of writable entries in out.
 * @return Points written, or 0 when unreachable or capacity is too small for the route. */
int CoreWaypointsPath(const CoreWaypoints *waypoints, int from, int to, int *out, int capacity);

/* As an engine type, "waypoints": count (read-only); add!(vector3) -> int, link!(int,int) -> bool,
   nearest(vector3) -> int, next-hop(int,int) -> int, position(int) -> vector3. */
extern const EngineType CoreWaypointsType;

#endif
