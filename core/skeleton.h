/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_SKELETON_H
#define CORE_SKELETON_H
#include "config.h"
#include "raylib.h"
#include <stdbool.h>
typedef struct CoreSkeleton
{
    const BoneInfo *bones;
    int count;
    int order[CORE_BONE_CAPACITY];
    Matrix inverseBind[CORE_BONE_CAPACITY];
} CoreSkeleton;
/* raylib bindPose/framePoses are model-space TRS. Local poses are used for blending. */
bool RigInit(CoreSkeleton *r, const BoneInfo *bones, const Transform *bindPose, int count);
bool RigIsDescendant(const CoreSkeleton *r, int bone, int ancestor);
bool RigToLocal(const CoreSkeleton *r, const Transform *global, Transform *local);
void RigToGlobal(const CoreSkeleton *r, const Transform *local, Transform *global);
Transform RigTransformBlend(Transform a, Transform b, float alpha);
void RigSkinMatrices(const CoreSkeleton *r, const Transform *global, Matrix *out);
void RigBoneLook(const CoreSkeleton *r, Transform *global, int bone, float yaw, float pitch, Vector3 up,
                 Vector3 side);
void RigBoneFix(const CoreSkeleton *r, Transform *global, int bone, Quaternion correction);
/* Shared weights with surviving bones can leave stretched triangles: this is not mesh cutting. */
void RigBoneHide(const CoreSkeleton *r, Transform *global, int bone);
#endif
