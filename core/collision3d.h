/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_COLLISION3D_H
#define CORE_COLLISION3D_H

#include "raylib.h"
#include <stdbool.h>

/* A ray query over a fixed set of Models, the 3D counterpart to collision2d.h's shape queries --
   modelled on Godot's PhysicsDirectSpaceState3D::intersect_ray (from, to, collision_mask ->
   position, normal, collider; servers/physics_3d/direct_states/physics_direct_space_state_3d.cpp).
   It stores each Model pointer rather than a copy, so a query always sees that model's current
   transform and meshes -- nothing here needs telling when a model moves. Not a Scheme engine type
   yet: scripts cannot add models to it. */
#define CORE_COLLISION3D_MAX 64

typedef struct CoreCollision3DEntry
{
    const Model *model; /* borrowed; its current transform and meshes are used at query time */
    unsigned layers;    /* bits a query's mask must intersect to test this entry */
    int tag;            /* caller's meaning, returned on a hit */
} CoreCollision3DEntry;

typedef struct CoreCollision3D
{
    CoreCollision3DEntry entries[CORE_COLLISION3D_MAX];
    int count;
} CoreCollision3D;

typedef struct CoreRayHit
{
    bool hit;
    float distance;
    Vector3 point;
    Vector3 normal; /* normalized, facing against the ray */
    int tag;        /* the hit entry's tag; meaningless when hit is false */
} CoreRayHit;

/** @brief Empties a collision set. Safe to call before first use.
 * @param collision Set to clear.
 * @return No value. */
void CoreCollision3DClear(CoreCollision3D *collision);
/** @brief Registers a model for ray queries.
 * @param collision Set to add to.
 * @param model Borrowed model, tested with its transform and meshes as they are at query time.
 * @param layers Bits a query's mask must intersect to test this entry.
 * @param tag Caller's meaning, returned on a hit.
 * @return True on success; false when collision or model is NULL, or the set is full. */
bool CoreCollision3DAdd(CoreCollision3D *collision, const Model *model, unsigned layers, int tag);
/** @brief Casts a ray against every mesh of every entry a mask selects, keeping the closest hit.
 * @param collision Set to query.
 * @param ray World-space ray.
 * @param maxDistance Farthest distance considered; also the starting "nothing closer than this".
 * @param mask Layers accepted by this query; an entry is tested when (entry.layers & mask) != 0.
 * @return The closest hit strictly nearer than maxDistance and beyond distance zero, in insertion
 * order among ties; hit false when nothing qualified. */
CoreRayHit CoreCollision3DRay(const CoreCollision3D *collision, Ray ray, float maxDistance, unsigned mask);

#endif
