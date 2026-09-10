#include "viewmodel.h"
#include "raymath.h"
#include "rlgl.h"
bool DrawViewmodel(ViewmodelProjection c, void (*draw)(void *), void *ctx)
{
    if (!draw || !(c.fov > 0 && c.fov < 180) || !(c.aspect > 0) || !(c.depth > 0 && c.depth <= 1))
        return false;
    rlDrawRenderBatchActive();
    Matrix saved = rlGetMatrixProjection();
    Matrix p = MatrixPerspective(c.fov * DEG2RAD, c.aspect, rlGetCullDistanceNear(), rlGetCullDistanceFar());
    p.m10 = c.depth * p.m10 + (1 - c.depth);
    p.m14 *= c.depth;
    rlSetMatrixProjection(p);
    draw(ctx);
    rlDrawRenderBatchActive();
    rlSetMatrixProjection(saved);
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
