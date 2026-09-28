/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* .rig binary format, little-endian:
     magic "RIGB" (4 bytes)
     fps            float32
     nbones         u16
     nverts         u16
     nanims         u16
     bones[nbones]:   name[32], parent i16, rest 12 x half (top three rows of a row-major 4x4)
     verts[nverts]:   n u8, then n x { bone u8, weight half }         (at most CORE_RIG_WEIGHTS kept)
     anims[nanims]:   name[64], f0 i16, f1 i16,
                      then (f1-f0+1) frames x nbones x 12 half (one pose matrix per bone)
   Every matrix is armature (model) space; its fourth row is (0,0,0,1). A row-major float[16] with
   translation in elements 3, 7, 11 has the same memory layout as raylib's Matrix struct, so once the
   fourth row is filled in a plain memcpy produces the right Matrix. */
#include "rig_file.h"
#include "config.h"
#include "file.h"
#include "raymath.h"
#include <stdlib.h>
#include <string.h>

typedef struct RigFileReader
{
    const unsigned char *p, *end;
    bool ok;
} RigFileReader;

static void RigFileRead(RigFileReader *rd, void *out, size_t n)
{
    if (!rd->ok || n > (size_t)(rd->end - rd->p))
    {
        rd->ok = false;
        memset(out, 0, n);
        return;
    }
    memcpy(out, rd->p, n);
    rd->p += n;
}

/* IEEE-754 binary16 -> binary32, matching the halves tools/export_rig.py (or equivalent) writes. */
static float RigFileHalfToFloat(unsigned short h)
{
    unsigned int sign = (unsigned int)(h >> 15) & 1u, exp = (unsigned int)(h >> 10) & 0x1Fu,
                 man = h & 0x3FFu, out;
    if (exp == 0)
    {
        if (man == 0)
            out = sign << 31;
        else
        {
            exp = 1;
            while (!(man & 0x400u))
            {
                man <<= 1;
                exp--;
            }
            man &= 0x3FFu;
            out = (sign << 31) | ((exp + (127 - 15)) << 23) | (man << 13);
        }
    }
    else if (exp == 0x1F)
        out = (sign << 31) | (0xFFu << 23) | (man << 13);
    else
        out = (sign << 31) | ((exp + (127 - 15)) << 23) | (man << 13);
    float f;
    memcpy(&f, &out, sizeof f);
    return f;
}

/* Reads 12 halves (top three rows) and synthesises the affine fourth row. */
static void RigFileReadMatrix(RigFileReader *rd, Matrix *out)
{
    unsigned short h[12];
    RigFileRead(rd, h, sizeof h);
    float a[16];
    for (int i = 0; i < 12; i++)
        a[i] = RigFileHalfToFloat(h[i]);
    a[12] = a[13] = a[14] = 0.0f;
    a[15] = 1.0f;
    memcpy(out, a, sizeof a);
}

static Transform RigFileDecompose(Matrix m)
{
    Transform t;
    MatrixDecompose(m, &t.translation, &t.rotation, &t.scale);
    return t;
}

void CoreRigFileFree(CoreRigFile *rig)
{
    if (!rig)
        return;
    for (int c = 0; c < rig->clipCount; c++)
    {
        ModelAnimation *anim = &rig->clips[c].animation;
        for (int f = 0; f < anim->frameCount; f++)
            free(anim->framePoses[f]);
        free(anim->framePoses);
        free(anim->bones);
    }
    free(rig->clips);
    free(rig->boneWeights);
    free(rig->boneIds);
    free(rig->bindPose);
    free(rig->bones);
    *rig = (CoreRigFile){0};
}

bool CoreRigFileLoad(CoreRigFile *rig, const char *path)
{
    *rig = (CoreRigFile){0};
    size_t dataSize = 0;
    unsigned char *data = CoreReadData(path, &dataSize);
    if (!data)
        return false;
    RigFileReader rd = {data, data + dataSize, true};

    char magic[4];
    RigFileRead(&rd, magic, sizeof magic);
    if (!rd.ok || memcmp(magic, "RIGB", 4))
    {
        CoreFreeData(data);
        return false;
    }
    float fps = 0;
    unsigned short nb = 0, nv = 0, na = 0;
    RigFileRead(&rd, &fps, sizeof fps);
    RigFileRead(&rd, &nb, sizeof nb);
    RigFileRead(&rd, &nv, sizeof nv);
    RigFileRead(&rd, &na, sizeof na);
    if (!rd.ok || nb < 1 || nb > CORE_BONE_CAPACITY)
    {
        CoreFreeData(data);
        return false;
    }
    rig->fps = fps;
    rig->boneCount = nb;
    rig->bones = calloc((size_t)nb, sizeof(BoneInfo));
    rig->bindPose = calloc((size_t)nb, sizeof(Transform));
    for (int i = 0; i < nb; i++)
    {
        char name[32];
        short parent = 0;
        Matrix rest;
        RigFileRead(&rd, name, sizeof name);
        RigFileRead(&rd, &parent, sizeof parent);
        RigFileReadMatrix(&rd, &rest);
        memcpy(rig->bones[i].name, name, sizeof name);
        rig->bones[i].name[sizeof rig->bones[i].name - 1] = 0;
        rig->bones[i].parent = parent;
        rig->bindPose[i] = RigFileDecompose(rest);
    }

    rig->vertexCount = nv;
    rig->boneIds = calloc((size_t)(nv ? nv : 1) * CORE_RIG_WEIGHTS, sizeof(unsigned char));
    rig->boneWeights = calloc((size_t)(nv ? nv : 1) * CORE_RIG_WEIGHTS, sizeof(float));
    for (int v = 0; v < nv; v++)
    {
        unsigned char n = 0;
        RigFileRead(&rd, &n, sizeof n);
        if (n == 0)
        {
            rig->boneIds[v * CORE_RIG_WEIGHTS] = 0;
            rig->boneWeights[v * CORE_RIG_WEIGHTS] = 1.0f;
        }
        for (int k = 0; k < n; k++)
        {
            unsigned char bone = 0;
            unsigned short w = 0;
            RigFileRead(&rd, &bone, sizeof bone);
            RigFileRead(&rd, &w, sizeof w);
            if (k < CORE_RIG_WEIGHTS)
            {
                rig->boneIds[v * CORE_RIG_WEIGHTS + k] = bone;
                rig->boneWeights[v * CORE_RIG_WEIGHTS + k] = RigFileHalfToFloat(w);
            }
        }
    }

    rig->clipCount = na;
    rig->clips = calloc((size_t)(na ? na : 1), sizeof(CoreRigClip));
    for (int c = 0; c < na; c++)
    {
        char name[64];
        short f0 = 0, f1 = 0;
        RigFileRead(&rd, name, sizeof name);
        RigFileRead(&rd, &f0, sizeof f0);
        RigFileRead(&rd, &f1, sizeof f1);
        int frames = f1 - f0 + 1;
        if (frames < 0)
        {
            rd.ok = false;
            frames = 0;
        }
        CoreRigClip *clip = &rig->clips[c];
        memcpy(clip->name, name, sizeof name);
        clip->name[sizeof clip->name - 1] = 0;
        clip->animation.boneCount = nb;
        clip->animation.frameCount = frames;
        clip->animation.bones = calloc((size_t)nb, sizeof(BoneInfo));
        memcpy(clip->animation.bones, rig->bones, (size_t)nb * sizeof(BoneInfo));
        clip->animation.framePoses = calloc((size_t)frames, sizeof(Transform *));
        for (int f = 0; f < frames; f++)
        {
            clip->animation.framePoses[f] = calloc((size_t)nb, sizeof(Transform));
            for (int b = 0; b < nb; b++)
            {
                Matrix pose;
                RigFileReadMatrix(&rd, &pose);
                clip->animation.framePoses[f][b] = RigFileDecompose(pose);
            }
        }
    }

    CoreFreeData(data);
    if (!rd.ok)
    {
        CoreRigFileFree(rig);
        return false;
    }
    return true;
}
