#include "skeleton.h"
#include "raymath.h"
#include <string.h>
static Matrix TransformMatrix(Transform t)
{
    return MatrixMultiply(
        MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z), QuaternionToMatrix(t.rotation)),
        MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}
static bool InvertibleScale(Vector3 s)
{
    return fabsf(s.x) > 0.000001f && fabsf(s.y) > 0.000001f && fabsf(s.z) > 0.000001f;
}
bool RigInit(CoreSkeleton *r, const BoneInfo *b, const Transform *bind, int n)
{
    *r = (CoreSkeleton){0};
    if (!b || !bind || n < 1 || n > CORE_BONE_CAPACITY)
    {
        TraceLog(LOG_ERROR, "Skeleton bone count %d, required range 1..%d", n, CORE_BONE_CAPACITY);
        return false;
    }
    for (int i = 0; i < n; i++)
        if (b[i].parent < -1 || b[i].parent >= n || !InvertibleScale(bind[i].scale))
            return false;
    for (int i = 0; i < n; i++)
    {
        int p = i, steps = 0;
        while (p >= 0 && steps++ <= n)
            p = b[p].parent;
        if (p >= 0)
        {
            TraceLog(LOG_ERROR, "Skeleton hierarchy cycle at bone %d", i);
            return false;
        }
    }
    r->bones = b;
    r->count = n;
    bool used[CORE_BONE_CAPACITY] = {0};
    int count = 0;
    while (count < n)
        for (int i = 0; i < n; i++)
            if (!used[i] && (b[i].parent < 0 || used[b[i].parent]))
            {
                used[i] = true;
                r->order[count++] = i;
            }
    for (int i = 0; i < n; i++)
        r->inverseBind[i] = MatrixInvert(TransformMatrix(bind[i]));
    return true;
}
bool RigIsDescendant(const CoreSkeleton *r, int bone, int ancestor)
{
    if (bone < 0 || bone >= r->count || ancestor < 0 || ancestor >= r->count)
        return false;
    for (int p = bone; p >= 0; p = r->bones[p].parent)
        if (p == ancestor)
            return true;
    return false;
}
bool RigToLocal(const CoreSkeleton *r, const Transform *g, Transform *l)
{
    for (int i = 0; i < r->count; i++)
    {
        int p = r->bones[i].parent;
        l[i] = g[i];
        if (p < 0)
            continue;
        if (!InvertibleScale(g[p].scale))
            return false;
        Quaternion inv = QuaternionInvert(g[p].rotation);
        l[i].translation = Vector3Divide(
            Vector3RotateByQuaternion(Vector3Subtract(g[i].translation, g[p].translation), inv), g[p].scale);
        l[i].rotation = QuaternionNormalize(QuaternionMultiply(inv, g[i].rotation));
        l[i].scale = Vector3Divide(g[i].scale, g[p].scale);
    }
    return true;
}
void RigToGlobal(const CoreSkeleton *r, const Transform *l, Transform *g)
{
    for (int k = 0; k < r->count; k++)
    {
        int i = r->order[k], p = r->bones[i].parent;
        g[i] = l[i];
        if (p < 0)
            continue;
        g[i].translation = Vector3Add(
            g[p].translation,
            Vector3RotateByQuaternion(Vector3Multiply(l[i].translation, g[p].scale), g[p].rotation));
        g[i].rotation = QuaternionNormalize(QuaternionMultiply(g[p].rotation, l[i].rotation));
        g[i].scale = Vector3Multiply(g[p].scale, l[i].scale);
    }
}
Transform RigTransformBlend(Transform a, Transform b, float u)
{
    u = Clamp(u, 0, 1);
    return (Transform){Vector3Lerp(a.translation, b.translation, u),
                       QuaternionSlerp(a.rotation, b.rotation, u), Vector3Lerp(a.scale, b.scale, u)};
}
void RigSkinMatrices(const CoreSkeleton *r, const Transform *g, Matrix *out)
{
    for (int i = 0; i < r->count; i++)
        out[i] = MatrixMultiply(r->inverseBind[i], TransformMatrix(g[i]));
}
void RigBoneFix(const CoreSkeleton *r, Transform *g, int bone, Quaternion correction)
{
    if (bone < 0 || bone >= r->count)
        return;
    Vector3 pivot = g[bone].translation;
    correction = QuaternionNormalize(correction);
    for (int i = 0; i < r->count; i++)
        if (RigIsDescendant(r, i, bone))
        {
            g[i].translation = Vector3Add(
                pivot, Vector3RotateByQuaternion(Vector3Subtract(g[i].translation, pivot), correction));
            g[i].rotation = QuaternionNormalize(QuaternionMultiply(correction, g[i].rotation));
        }
}
void RigBoneLook(const CoreSkeleton *r, Transform *g, int bone, float yaw, float pitch, Vector3 up,
                 Vector3 side)
{
    RigBoneFix(r, g, bone,
               QuaternionMultiply(QuaternionFromAxisAngle(up, yaw), QuaternionFromAxisAngle(side, pitch)));
}
void RigBoneHide(const CoreSkeleton *r, Transform *g, int bone)
{
    if (bone < 0 || bone >= r->count)
        return;
    Vector3 pivot = g[bone].translation;
    for (int i = 0; i < r->count; i++)
        if (RigIsDescendant(r, i, bone))
        {
            g[i].translation = pivot;
            g[i].scale = (Vector3){0};
        }
}
