/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_NODE_H
#define CORE_NODE_H

#include "object.h"
#include <stdbool.h>

/* A place in a parent/child hierarchy, the way Godot's Node3D holds a local transform and a parent
   and composes world = parent's world times its own local (scene/3d/node_3d.cpp:656). A "node3d"
   engine type is created with no arguments, and starts at the local identity transform -- position
   zero, rotation identity, scale (1,1,1) -- with no parent.

   Rotation is stored as a quaternion; scripts see and set it as a vector3 of Euler radians about X,
   then Y, then Z (QuaternionFromEuler/QuaternionToEuler -- raylib names the three parameters/results
   "pitch, yaw, roll" but they are independently the X, Y and Z axis angles). Local movement and
   rotation turn with the node's own local rotation, as core/transform.h's TransformMoveLocal and
   TransformRotateLocal already do for a single transform. */
typedef struct CoreNode
{
    Vector3 position;       /* local, relative to the parent */
    Quaternion rotation;    /* local */
    Vector3 scale;          /* local; starts at (1,1,1) */
    EngineObjectId parent;  /* ENGINE_OBJECT_NULL for none */
    EngineObjects *objects; /* the pool it lives in, set at create */
    /* Its own handle, set at create. A computed property's set/get only gets the object's storage
       (core/object.h's EngineProperty), not the pool or the handle that reached it, and destroy
       only gets the storage too -- so a node keeps both of its own, the way it must to change its
       own parent or find which live nodes are its children when it goes. */
    EngineObjectId self;
} CoreNode;

extern const EngineType CoreNodeType;

/* Every call below takes the pool a node lives in and its handle. A stale handle, or one that does
   not name a live node3d, answers false (or leaves *out untouched) rather than reaching whatever
   took its place. A parent handle that no longer names a live node counts as no parent, so a
   destroyed ancestor never breaks a world computation; chain walks stop after 256 steps either way. */

/** @brief The node's world transform: its local transform times every ancestor's, out to the first
 * ancestor with no live parent.
 * @param objects Pool the node lives in.
 * @param node Node handle.
 * @param out Receives the world transform as a matrix.
 * @return True when node names a live node3d. */
bool CoreNodeWorld(const EngineObjects *objects, EngineObjectId node, Matrix *out);

/** @brief Changes a node's parent.
 *
 * Refused, leaving the node unchanged, when parent is node itself or one of node's own descendants
 * (a cycle, self-parenting included); a parent argument that does not currently name a live node3d
 * is otherwise treated the same as ENGINE_OBJECT_NULL. With keepWorld, the node's local position,
 * rotation and scale are adjusted so CoreNodeWorld is unchanged; without it, the local transform is
 * kept as it was and the node's world transform moves with the new parent.
 * @param objects Pool the node lives in.
 * @param node Node to reparent.
 * @param parent New parent, or ENGINE_OBJECT_NULL to detach.
 * @param keepWorld Adjust the local transform to hold the world transform steady.
 * @return True when node names a live node3d and the change was not a cycle. */
bool CoreNodeSetParent(EngineObjects *objects, EngineObjectId node, EngineObjectId parent, bool keepWorld);

/** @brief The node's world position: CoreNodeWorld's translation.
 * @param objects Pool the node lives in.
 * @param node Node handle.
 * @param out Receives the world position.
 * @return True when node names a live node3d. */
bool CoreNodeWorldPosition(const EngineObjects *objects, EngineObjectId node, Vector3 *out);

/** @brief Sets the node's local position so its world position becomes the one given, leaving its
 * local rotation and scale as they were.
 * @param objects Pool the node lives in.
 * @param node Node to move.
 * @param world Wanted world position.
 * @return True when node names a live node3d and its parent chain's scale is invertible. */
bool CoreNodeSetWorldPosition(EngineObjects *objects, EngineObjectId node, Vector3 world);

#endif
