/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_COLLISION2D_H
#define CORE_COLLISION2D_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

/* Optional query-only collision. It never changes positions, applies forces, or owns entities.
   Shapes use the Transform2D world convention: +X right and +Y down. */
typedef struct Collision2DAabb { Vector2 min, max; } Collision2DAabb;
typedef enum Collision2DShapeKind { COLLISION2D_CIRCLE, COLLISION2D_AABB } Collision2DShapeKind;
typedef struct Collision2DShape
{
    Collision2DShapeKind kind;
    Vector2 center;
    Vector2 half; /* AABB only */
    float radius; /* circle only */
} Collision2DShape;
typedef struct Collision2DFilter { uint32_t layer, mask; } Collision2DFilter;
typedef struct Collision2DHandle { uint32_t index, generation; } Collision2DHandle;
#define COLLISION2D_NULL ((Collision2DHandle){UINT32_MAX, 0})
typedef struct Collision2DHit { Collision2DHandle handle; void *user; } Collision2DHit;
typedef struct Collision2DSweep { Collision2DHit hit; Vector2 point, normal; float fraction; } Collision2DSweep;
typedef struct Collision2DProxy Collision2DProxy;
typedef struct Collision2DLink Collision2DLink;
typedef struct Collision2DWorld
{
    Collision2DProxy *proxies;
    Collision2DLink *links;
    int *buckets;
    int capacity, bucketCount, linkCount, linkCapacity;
    float cellSize;
    uint32_t queryStamp;
    bool dirty;
} Collision2DWorld;

/** @brief Creates an AABB from a centre and nonnegative half extents.
 * @param center Box centre.
 * @param half Half width and height; negative axes are treated as zero.
 * @return Axis-aligned world-space bounds. */
Collision2DAabb Collision2DAabbMake(Vector2 center, Vector2 half);
/** @brief Returns whether two axis-aligned bounds overlap, including their edges.
 * @param a First bounds.
 * @param b Second bounds.
 * @return True when the bounds intersect. */
bool Collision2DAabbOverlaps(Collision2DAabb a, Collision2DAabb b);
/** @brief Tests exact overlap between circles and/or AABBs.
 * @param a First shape.
 * @param b Second shape.
 * @return True when the two finite shapes touch or overlap. */
bool Collision2DOverlaps(Collision2DShape a, Collision2DShape b);
/** @brief Applies Box2D-style symmetric layer/mask filtering to two shapes.
 * @param a First shape filter.
 * @param b Second shape filter.
 * @return True when each layer is accepted by the other's mask. */
bool Collision2DShouldCollide(Collision2DFilter a, Collision2DFilter b);
/** @brief Allocates an optional spatial-hash query world.
 * @param world Storage to initialize.
 * @param capacity Maximum registered shapes.
 * @param cellSize Positive hash-cell width and height in world units.
 * @return True on success; false leaves world safe to free. */
bool Collision2DWorldInit(Collision2DWorld *world, int capacity, float cellSize);
/** @brief Releases all spatial-hash and shape storage.
 * @param world World to release; NULL is accepted.
 * @return No value. */
void Collision2DWorldFree(Collision2DWorld *world);
/** @brief Registers a shape for broad-phase queries.
 * @param world Initialized query world.
 * @param shape Finite circle or AABB shape.
 * @param filter Layer/mask pair; zero mask or layer simply matches nothing.
 * @param user Borrowed project pointer returned in query hits.
 * @return A generational handle, or COLLISION2D_NULL when full or invalid. */
Collision2DHandle Collision2DWorldAdd(Collision2DWorld *world, Collision2DShape shape,
                                      Collision2DFilter filter, void *user);
/** @brief Removes a registered shape and invalidates its handle.
 * @param world Query world.
 * @param handle Registered shape handle.
 * @return True when a live shape was removed. */
bool Collision2DWorldRemove(Collision2DWorld *world, Collision2DHandle handle);
/** @brief Replaces a registered shape's geometry.
 * @param world Query world.
 * @param handle Registered shape handle.
 * @param shape New finite circle or AABB.
 * @return True when the shape was updated. */
bool Collision2DWorldSetShape(Collision2DWorld *world, Collision2DHandle handle, Collision2DShape shape);
/** @brief Finds registered shapes exactly overlapping an AABB.
 * @param world Query world.
 * @param bounds World-space query bounds.
 * @param mask Layers accepted by this query.
 * @param out Caller-owned hit array, or NULL to count only.
 * @param capacity Number of writable out entries.
 * @return Number of hits written, capped at capacity when out is non-NULL. */
int Collision2DQueryAabb(Collision2DWorld *world, Collision2DAabb bounds, uint32_t mask,
                         Collision2DHit *out, int capacity);
/** @brief Finds registered shapes exactly overlapping a circle.
 * @param world Query world.
 * @param center Circle centre.
 * @param radius Nonnegative circle radius.
 * @param mask Layers accepted by this query.
 * @param out Caller-owned hit array, or NULL to count only.
 * @param capacity Number of writable out entries.
 * @return Number of hits written, capped at capacity when out is non-NULL. */
int Collision2DQueryCircle(Collision2DWorld *world, Vector2 center, float radius, uint32_t mask,
                           Collision2DHit *out, int capacity);
/** @brief Sweeps a circle to the nearest registered shape without moving anything.
 * @param world Query world.
 * @param center Circle start centre.
 * @param radius Nonnegative circle radius.
 * @param delta Proposed displacement.
 * @param mask Layers accepted by this query.
 * @param out Nearest hit result when true; may be NULL.
 * @return True when the sweep hits during the closed fraction [0, 1]. */
bool Collision2DSweepCircle(Collision2DWorld *world, Vector2 center, float radius, Vector2 delta,
                            uint32_t mask, Collision2DSweep *out);
#endif
