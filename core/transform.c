/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "transform.h"

Transform TransformIdentity(void)
{
    return (Transform){.rotation = {0, 0, 0, 1}, .scale = {1, 1, 1}};
}

Vector3 TransformDirection(Transform t, Vector3 direction)
{
    return Vector3RotateByQuaternion(direction, t.rotation);
}

Vector3 TransformInverseDirection(Transform t, Vector3 direction)
{
    return Vector3RotateByQuaternion(direction, QuaternionInvert(t.rotation));
}

void TransformMoveLocal(Transform *t, Vector3 displacement)
{
    t->translation = Vector3Add(t->translation, TransformDirection(*t, displacement));
}

void TransformMoveWorld(Transform *t, Vector3 displacement)
{
    t->translation = Vector3Add(t->translation, displacement);
}

void TransformRotateLocal(Transform *t, Vector3 axis, float radians)
{
    if (Vector3LengthSqr(axis) == 0)
        return;
    Quaternion turn = QuaternionFromAxisAngle(axis, radians);
    t->rotation = QuaternionNormalize(QuaternionMultiply(t->rotation, turn));
}

void TransformRotateWorld(Transform *t, Vector3 axis, float radians)
{
    if (Vector3LengthSqr(axis) == 0)
        return;
    Quaternion turn = QuaternionFromAxisAngle(axis, radians);
    t->rotation = QuaternionNormalize(QuaternionMultiply(turn, t->rotation));
}

Vector3 TransformForward(Transform t) { return TransformDirection(t, (Vector3){0, 0, -1}); }
Vector3 TransformRight(Transform t) { return TransformDirection(t, (Vector3){1, 0, 0}); }
Vector3 TransformUp(Transform t) { return TransformDirection(t, (Vector3){0, 1, 0}); }

Vector3 TransformPoint(Transform t, Vector3 point)
{
    return Vector3Add(t.translation, TransformDirection(t, Vector3Multiply(point, t.scale)));
}

bool TransformInversePoint(Transform t, Vector3 point, Vector3 *localPoint)
{
    if (!localPoint || t.scale.x == 0 || t.scale.y == 0 || t.scale.z == 0)
        return false;
    *localPoint =
        Vector3Divide(TransformInverseDirection(t, Vector3Subtract(point, t.translation)), t.scale);
    return true;
}

Matrix TransformMatrix(Transform t)
{
    return MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z),
                                         QuaternionToMatrix(t.rotation)),
                          MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}

bool TransformLookAt(Transform *t, Vector3 target, Vector3 up)
{
    Vector3 direction = Vector3Subtract(target, t->translation);
    if (Vector3LengthSqr(direction) == 0 || Vector3LengthSqr(up) == 0)
        return false;
    direction = Vector3Normalize(direction);
    up = Vector3Normalize(up);
    if (Vector3LengthSqr(Vector3CrossProduct(direction, up)) < 1e-12f)
        return false;
    Matrix view = MatrixLookAt((Vector3){0}, direction, up);
    t->rotation = QuaternionNormalize(QuaternionInvert(QuaternionFromMatrix(view)));
    return true;
}

Transform2D Transform2DIdentity(void) { return (Transform2D){.scale = {1, 1}}; }

Vector2 Transform2DDirection(Transform2D t, Vector2 direction)
{
    return Vector2Rotate(direction, t.rotation);
}

Vector2 Transform2DInverseDirection(Transform2D t, Vector2 direction)
{
    return Vector2Rotate(direction, -t.rotation);
}

void Transform2DMoveLocal(Transform2D *t, Vector2 displacement)
{
    t->translation = Vector2Add(t->translation, Transform2DDirection(*t, displacement));
}

void Transform2DMoveWorld(Transform2D *t, Vector2 displacement)
{
    t->translation = Vector2Add(t->translation, displacement);
}

void Transform2DRotate(Transform2D *t, float radians)
{
    t->rotation = Wrap(t->rotation + radians, -PI, PI);
}

Vector2 Transform2DPoint(Transform2D t, Vector2 point)
{
    return Vector2Add(t.translation, Transform2DDirection(t, Vector2Multiply(point, t.scale)));
}

bool Transform2DInversePoint(Transform2D t, Vector2 point, Vector2 *localPoint)
{
    if (!localPoint || t.scale.x == 0 || t.scale.y == 0)
        return false;
    *localPoint = Vector2Divide(
        Transform2DInverseDirection(t, Vector2Subtract(point, t.translation)), t.scale);
    return true;
}

Matrix Transform2DMatrix(Transform2D t)
{
    return MatrixMultiply(
        MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, 1), MatrixRotateZ(t.rotation)),
        MatrixTranslate(t.translation.x, t.translation.y, 0));
}

bool Transform2DLookAt(Transform2D *t, Vector2 target)
{
    Vector2 direction = Vector2Subtract(target, t->translation);
    if (Vector2LengthSqr(direction) == 0)
        return false;
    t->rotation = atan2f(direction.y, direction.x);
    return true;
}

float MoveTowards(float current, float target, float maxDistance)
{
    if (maxDistance <= 0)
        return current;
    float delta = target - current;
    if (fabsf(delta) <= maxDistance)
        return target;
    return current + copysignf(maxDistance, delta);
}

float AngleDelta(float from, float to) { return Wrap(to - from, -PI, PI); }

float AngleMoveTowards(float current, float target, float maxRadians)
{
    if (maxRadians <= 0)
        return current;
    float delta = AngleDelta(current, target);
    return current + MoveTowards(0, delta, maxRadians);
}

Quaternion QuaternionRotateTowards(Quaternion current, Quaternion target, float maxRadians)
{
    if (maxRadians <= 0)
        return current;
    current = QuaternionNormalize(current);
    target = QuaternionNormalize(target);
    float dot = fabsf(current.x * target.x + current.y * target.y + current.z * target.z +
                      current.w * target.w);
    float angle = 2 * acosf(Clamp(dot, 0, 1));
    if (angle <= maxRadians)
        return target;
    return QuaternionNormalize(QuaternionSlerp(current, target, maxRadians / angle));
}
