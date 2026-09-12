/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_TRANSFORM_H
#define CORE_TRANSFORM_H
#include "raylib.h"
#include "raymath.h"
#include <stdbool.h>

/* Plain transforms, no entities, hierarchy, ownership or physics. All angles in radians.
   3D uses raylib Transform: +Z forward, +Y up, -X right, as FpsCamera, ActorLookAt and glTF
   models already do. Rotation must be a unit quaternion.
   Initialise with TransformIdentity (a zero scale/quaternion is NOT an identity).
   Local movement/directions use rotation only, so object scale cannot change movement speed. */
Transform TransformIdentity(void);
void TransformMoveLocal(Transform *transform, Vector3 displacement);
void TransformMoveWorld(Transform *transform, Vector3 displacement);
void TransformRotateLocal(Transform *transform, Vector3 axis, float radians);
void TransformRotateWorld(Transform *transform, Vector3 axis, float radians);
Vector3 TransformForward(Transform transform);
Vector3 TransformRight(Transform transform);
Vector3 TransformUp(Transform transform);
Vector3 TransformDirection(Transform transform, Vector3 direction);
Vector3 TransformInverseDirection(Transform transform, Vector3 direction);
Vector3 TransformPoint(Transform transform, Vector3 point);
/* Zero scale is not invertible: false, with localPoint left untouched. */
bool TransformInversePoint(Transform transform, Vector3 point, Vector3 *localPoint);
Matrix TransformMatrix(Transform transform);
/* Faces local +Z toward target. False leaves rotation unchanged for coincident target,
   zero up, or up parallel to the viewing direction. Does not move or scale the object. */
bool TransformLookAt(Transform *transform, Vector3 target, Vector3 up);

/* 2D uses raylib screen coordinates: +X right, +Y down; positive angles turn clockwise.
   Local facing is +X. This is optional world/sprite data, unrelated to integer UiRect layout. */
typedef struct Transform2D
{
    Vector2 translation;
    float rotation;
    Vector2 scale;
} Transform2D;
Transform2D Transform2DIdentity(void);
void Transform2DMoveLocal(Transform2D *transform, Vector2 displacement);
void Transform2DMoveWorld(Transform2D *transform, Vector2 displacement);
void Transform2DRotate(Transform2D *transform, float radians);
Vector2 Transform2DDirection(Transform2D transform, Vector2 direction);
Vector2 Transform2DInverseDirection(Transform2D transform, Vector2 direction);
Vector2 Transform2DPoint(Transform2D transform, Vector2 point);
bool Transform2DInversePoint(Transform2D transform, Vector2 point, Vector2 *localPoint);
Matrix Transform2DMatrix(Transform2D transform);
bool Transform2DLookAt(Transform2D *transform, Vector2 target);

/* Nonnegative maximum steps (normally speed*dt); negative steps leave the current value unchanged.
   Unlike Lerp, these are bounded speed steps and cannot overshoot the target. */
float MoveTowards(float current, float target, float maxDistance);
float AngleDelta(float from,
                 float to); // Shortest signed difference in [-PI, PI); half-turn chooses -PI.
float AngleMoveTowards(float current, float target, float maxRadians);
Quaternion QuaternionRotateTowards(Quaternion current, Quaternion target, float maxRadians);
/* Use raymath's existing Vector2MoveTowards/Vector3MoveTowards, Lerp, QuaternionSlerp,
   Vector2/3ClampValue, Vector2/3Distance, etc. directly. */
#endif
