/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_RIG_FILE_H
#define CORE_RIG_FILE_H
#include "raylib.h"
#include <stdbool.h>

/* Reads a ".rig" binary skeletal animation file into raylib's own skeleton and animation types, so
   an Actor (core/animation.h) can play it. See the format comment above CoreRigFileLoad in
   rig_file.c for the exact byte layout. */

#define CORE_RIG_WEIGHTS 4

typedef struct CoreRigClip
{
    char name[64];
    ModelAnimation animation; /* framePoses[frame][bone] model-space; bones/boneCount set */
} CoreRigClip;

typedef struct CoreRigFile
{
    float fps;
    int boneCount;
    BoneInfo *bones;        /* name, parent */
    Transform *bindPose;    /* model-space rest pose, from the rest matrices */
    int vertexCount;
    unsigned char *boneIds; /* vertexCount * CORE_RIG_WEIGHTS */
    float *boneWeights;     /* vertexCount * CORE_RIG_WEIGHTS; a vertex with no weights gets bone 0 weight 1 */
    int clipCount;
    CoreRigClip *clips;
} CoreRigFile;

/** @brief Loads a binary .rig file: bones with rest matrices, per-vertex bone weights, and clips of
 * pose matrices, into storage this call allocates.
 *
 * Rest and pose matrices are converted to raylib Transforms with MatrixDecompose, so bindPose and
 * every clip's framePoses are ready for RigInit and Actor without further conversion.
 * @param rig Destination; overwritten. Zeroed when this returns false.
 * @param path Passed through CoreReadData: absolute, working-directory-relative, or data-root-relative.
 * @return True on success. False when the file is missing, truncated, not a RIGB file, or declares
 * more bones than CORE_BONE_CAPACITY. */
bool CoreRigFileLoad(CoreRigFile *rig, const char *path);

/** @brief Releases everything a CoreRigFileLoad call allocated.
 * @param rig Rig to release; NULL and an already-zeroed rig are both accepted.
 * @return Nothing; the struct is left zeroed. */
void CoreRigFileFree(CoreRigFile *rig);

#endif
