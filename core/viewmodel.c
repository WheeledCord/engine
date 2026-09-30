/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "viewmodel.h"
#include "raymath.h"
#include "rlgl.h"
#include <GL/gl.h>
static bool Valid(ViewmodelProjection c, void (*draw)(void *))
{
    return draw && c.fov > 0 && c.fov < 180 && c.aspect > 0 && c.depth > 0 && c.depth <= 1;
}
static void Pass(ViewmodelProjection c, void (*draw)(void *), void *ctx)
{
    rlDrawRenderBatchActive();
    Matrix saved = rlGetMatrixProjection();
    Matrix p = MatrixPerspective(c.fov * DEG2RAD, c.aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    p.m10 = c.depth * p.m10 + (1 - c.depth);
    p.m14 *= c.depth;
    rlSetMatrixProjection(p);
    draw(ctx);
    rlDrawRenderBatchActive();
    rlSetMatrixProjection(saved);
}
bool DrawViewmodel(ViewmodelProjection c, void (*draw)(void *), void *ctx)
{
    if (!Valid(c, draw))
        return false;
    Pass(c, draw, ctx);
    return true;
}
bool DrawViewmodelCleared(ViewmodelProjection c, float nearPlane, float farPlane, void (*draw)(void *), void *ctx)
{
    if (!Valid(c, draw) || !(nearPlane > 0 && farPlane > nearPlane))
        return false;
    rlDrawRenderBatchActive();
    glClear(GL_DEPTH_BUFFER_BIT); /* rlgl clears depth only together with colour */
    double savedNear = rlGetCullDistanceNear(), savedFar = rlGetCullDistanceFar();
    rlSetClipPlanes(nearPlane, farPlane);
    Pass(c, draw, ctx);
    rlSetClipPlanes(savedNear, savedFar);
    return true;
}
Matrix ViewmodelTransform(Vector3 pos, Vector3 f, Vector3 up, Vector3 offset)
{
    f = Vector3Normalize(f);
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, up)), u = Vector3CrossProduct(r, f);
    pos = Vector3Add(pos, Vector3Add(Vector3Scale(r, offset.x),
                                     Vector3Add(Vector3Scale(u, offset.y), Vector3Scale(f, offset.z))));
    return (Matrix){r.x, u.x, -f.x, pos.x, r.y, u.y, -f.y, pos.y, r.z, u.z, -f.z, pos.z, 0, 0, 0, 1};
}
