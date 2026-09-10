#ifndef CORE_ANIMATION_H
#define CORE_ANIMATION_H
#include "skeleton.h"
typedef struct ActorClip
{
    const ModelAnimation *animation;
    float fps;
} ActorClip;
typedef struct ActorAimJoint
{
    int bone;
    float weight;
} ActorAimJoint;
typedef struct ActorAim
{
    const ActorAimJoint *joints;
    int count;
    Vector3 up, side;
    float maxYaw, maxPitch, response;
} ActorAim;
typedef struct Actor
{
    Model *model;
    const ActorClip *clips;
    int clipCount, clip;
    CoreSkeleton skeleton;
    double time;
    bool loop, blending, holding;
    int blendClip, blendFrame;
    float blendTime, blendDuration;
    Transform previous[CORE_BONE_CAPACITY], current[CORE_BONE_CAPACITY], blendFrom[CORE_BONE_CAPACITY];
    Transform renderLocal[CORE_BONE_CAPACITY], renderGlobal[CORE_BONE_CAPACITY];
    Matrix matrices[CORE_BONE_CAPACITY];
    bool hidden[CORE_BONE_CAPACITY], corrected[CORE_BONE_CAPACITY];
    Quaternion corrections[CORE_BONE_CAPACITY];
    ActorAim aim;
    float lookYaw, lookPitch, targetYaw, targetPitch, previousYaw, previousPitch;
} Actor;
/* Borrows model, clips and aim joint storage. Each Actor owns its playback/pose state. */
bool ActorInit(Actor *a, Model *model, const ActorClip *clips, int clipCount);
int ActorBone(const Actor *a, const char *name);
bool ActorPlay(Actor *a, int clip, bool loop);
bool ActorAnimDone(const Actor *a);
bool ActorBlendToIdle(Actor *a, int destinationClip, int destinationFrame, float duration);
bool ActorPoseFrames(Actor *a, int clip, int frameA, int frameB, float alpha);
void ActorUpdate(Actor *a, float dt);
void ActorLookAt(Actor *a, Vector3 from, Vector3 target, float bodyYaw);
/* Evaluate then upload immediately before drawing this instance of a shared Model. */
void ActorUploadPose(Actor *a, float interpolationAlpha);
/* Explicit componentwise matrix interpolation; NOT rotation-preserving pose blending. */
Matrix ActorMatLerp(Matrix a, Matrix b, float alpha);
#endif
