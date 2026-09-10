#include "animation.h"
#include "raymath.h"
#include <math.h>
#include <string.h>
static bool ClipValid(const Actor *a, int i)
{
    return i >= 0 && i < a->clipCount;
}
static bool FrameLocal(Actor *a, int clip, int frame, Transform *out)
{
    const ModelAnimation *anim = a->clips[clip].animation;
    return frame >= 0 && frame < anim->frameCount && RigToLocal(&a->skeleton, anim->framePoses[frame], out);
}
static void Sample(Actor *a, Transform *out)
{
    const ModelAnimation *anim = a->clips[a->clip].animation;
    double f = a->time * a->clips[a->clip].fps;
    if (a->loop)
        f = fmod(f, anim->frameCount);
    else if (f > anim->frameCount - 1)
        f = anim->frameCount - 1;
    int first = (int)f, second = first + 1;
    if (second >= anim->frameCount)
        second = a->loop ? 0 : first;
    Transform x[CORE_BONE_CAPACITY], y[CORE_BONE_CAPACITY];
    FrameLocal(a, a->clip, first, x);
    FrameLocal(a, a->clip, second, y);
    for (int b = 0; b < a->skeleton.count; b++)
        out[b] = RigTransformBlend(x[b], y[b], (float)(f - first));
}
bool ActorInit(Actor *a, Model *model, const ActorClip *clips, int count)
{
    *a = (Actor){0};
    if (!model || !clips || count < 1 ||
        !RigInit(&a->skeleton, model->bones, model->bindPose, model->boneCount))
        return false;
    a->model = model;
    a->clips = clips;
    a->clipCount = count;
    for (int i = 0; i < count; i++)
    {
        const ModelAnimation *anim = clips[i].animation;
        if (!anim || !isfinite(clips[i].fps) || clips[i].fps <= 0 || anim->frameCount < 1 ||
            !anim->framePoses || !IsModelAnimationValid(*model, *anim))
        {
            TraceLog(LOG_ERROR, "Animation %d requires a matching skeleton and explicit positive FPS", i);
            return false;
        }
        Transform local[CORE_BONE_CAPACITY];
        for (int f = 0; f < anim->frameCount; f++)
            if (!anim->framePoses[f] || !RigToLocal(&a->skeleton, anim->framePoses[f], local))
            {
                TraceLog(LOG_ERROR, "Animation %d frame %d has a singular parent scale", i, f);
                return false;
            }
    }
    for (int i = 0; i < model->meshCount; i++)
    {
        Mesh m = model->meshes[i];
        if (m.boneCount == 0)
            continue;
        if (m.boneCount != model->boneCount || !m.boneMatrices || !m.boneIds || !m.boneWeights || !m.vboId ||
            !m.vboId[7] || !m.vboId[8])
        {
            TraceLog(LOG_ERROR, "Mesh %d lacks required GPU skinning buffers", i);
            return false;
        }
        for (int v = 0; v < m.vertexCount; v++)
            for (int k = 0; k < 4; k++)
                if (m.boneIds[v * 4 + k] >= model->boneCount)
                {
                    TraceLog(LOG_ERROR, "Mesh %d has an out-of-range bone ID", i);
                    return false;
                }
    }
    a->aim = (ActorAim){NULL, 0, {0, 1, 0}, {1, 0, 0}, 75 * DEG2RAD, 45 * DEG2RAD, 7};
    return ActorPlay(a, 0, true);
}
int ActorBone(const Actor *a, const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < a->skeleton.count; i++)
        if (!strncmp(a->skeleton.bones[i].name, name, sizeof a->skeleton.bones[i].name))
            return i;
    return -1;
}
bool ActorPlay(Actor *a, int clip, bool loop)
{
    if (!ClipValid(a, clip))
        return false;
    a->clip = clip;
    a->loop = loop;
    a->time = 0;
    a->blending = false;
    a->holding = false;
    Sample(a, a->current);
    memcpy(a->previous, a->current, sizeof a->current);
    return true;
}
bool ActorAnimDone(const Actor *a)
{
    return a->clipCount == 0 ||
           (!a->loop && !a->blending &&
            (a->holding || a->time * a->clips[a->clip].fps >= a->clips[a->clip].animation->frameCount - 1));
}
bool ActorBlendToIdle(Actor *a, int clip, int frame, float duration)
{
    if (!ClipValid(a, clip) || frame < 0 || frame >= a->clips[clip].animation->frameCount ||
        !isfinite(duration) || duration < 0)
        return false;
    memcpy(a->blendFrom, a->current, sizeof a->current);
    a->blendClip = clip;
    a->blendFrame = frame;
    a->blendTime = 0;
    a->blendDuration = duration;
    a->blending = true;
    a->holding = false;
    return true;
}
bool ActorPoseFrames(Actor *a, int clip, int fA, int fB, float u)
{
    Transform x[CORE_BONE_CAPACITY], y[CORE_BONE_CAPACITY];
    if (!ClipValid(a, clip) || !FrameLocal(a, clip, fA, x) || !FrameLocal(a, clip, fB, y))
        return false;
    memcpy(a->previous, a->current, sizeof a->current);
    for (int b = 0; b < a->skeleton.count; b++)
        a->current[b] = RigTransformBlend(x[b], y[b], u);
    return true;
}
void ActorUpdate(Actor *a, float dt)
{
    if (dt < 0 || !isfinite(dt))
        return;
    memcpy(a->previous, a->current, sizeof a->current);
    a->previousYaw = a->lookYaw;
    a->previousPitch = a->lookPitch;
    float response = a->aim.response > 0 ? 1 - expf(-a->aim.response * dt) : 1;
    a->lookYaw += (a->targetYaw - a->lookYaw) * response;
    a->lookPitch += (a->targetPitch - a->lookPitch) * response;
    if (a->blending)
    {
        a->blendTime += dt;
        float u = a->blendDuration > 0 ? Clamp(a->blendTime / a->blendDuration, 0, 1) : 1;
        Transform target[CORE_BONE_CAPACITY];
        FrameLocal(a, a->blendClip, a->blendFrame, target);
        for (int b = 0; b < a->skeleton.count; b++)
            a->current[b] = RigTransformBlend(a->blendFrom[b], target[b], u);
        if (u >= 1)
        {
            a->blending = false;
            a->clip = a->blendClip;
            a->time = (double)a->blendFrame / a->clips[a->clip].fps;
            a->loop = false;
            a->holding = true;
        }
    }
    else if (!a->holding)
    {
        a->time += dt;
        Sample(a, a->current);
    }
}
void ActorLookAt(Actor *a, Vector3 from, Vector3 target, float yaw)
{
    Vector3 d = Vector3Subtract(target, from);
    float wanted = atan2f(d.x, d.z) - yaw;
    wanted = atan2f(sinf(wanted), cosf(wanted));
    a->targetYaw = Clamp(wanted, -a->aim.maxYaw, a->aim.maxYaw);
    a->targetPitch = Clamp(-atan2f(d.y, sqrtf(d.x * d.x + d.z * d.z)), -a->aim.maxPitch, a->aim.maxPitch);
}
void ActorUploadPose(Actor *a, float alpha)
{
    alpha = Clamp(alpha, 0, 1);
    for (int b = 0; b < a->skeleton.count; b++)
        a->renderLocal[b] = RigTransformBlend(a->previous[b], a->current[b], alpha);
    RigToGlobal(&a->skeleton, a->renderLocal, a->renderGlobal);
    for (int b = 0; b < a->skeleton.count; b++)
        if (a->corrected[b])
            RigBoneFix(&a->skeleton, a->renderGlobal, b, a->corrections[b]);
    float yaw = Lerp(a->previousYaw, a->lookYaw, alpha), pitch = Lerp(a->previousPitch, a->lookPitch, alpha);
    for (int j = 0; j < a->aim.count; j++)
    {
        ActorAimJoint joint = a->aim.joints[j];
        RigBoneLook(&a->skeleton, a->renderGlobal, joint.bone, yaw * joint.weight, pitch * joint.weight,
                    a->aim.up, a->aim.side);
    }
    /* Collapse after rotations, so later aim layers cannot separate a collapsed chain. */
    for (int b = 0; b < a->skeleton.count; b++)
        if (a->hidden[b])
            RigBoneHide(&a->skeleton, a->renderGlobal, b);
    RigSkinMatrices(&a->skeleton, a->renderGlobal, a->matrices);
    for (int i = 0; i < a->model->meshCount; i++)
        if (a->model->meshes[i].boneMatrices)
            memcpy(a->model->meshes[i].boneMatrices, a->matrices, sizeof(Matrix) * (size_t)a->skeleton.count);
}
Matrix ActorMatLerp(Matrix a, Matrix b, float u)
{
    Matrix r;
#define COMPONENT(n) r.m##n = a.m##n + (b.m##n - a.m##n) * u
    COMPONENT(0);
    COMPONENT(1);
    COMPONENT(2);
    COMPONENT(3);
    COMPONENT(4);
    COMPONENT(5);
    COMPONENT(6);
    COMPONENT(7);
    COMPONENT(8);
    COMPONENT(9);
    COMPONENT(10);
    COMPONENT(11);
    COMPONENT(12);
    COMPONENT(13);
    COMPONENT(14);
    COMPONENT(15);
#undef COMPONENT
    return r;
}
