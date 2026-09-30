/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The skinned test asset (tests/regression/assets/rig/test_rig.gltf, written by
// tools/make_test_rig.py) as raylib reads it: its three bones by name, its skinned mesh, and its two
// clips, and the runner's two skinning paths drawing one pose alike. Runs under the hidden window
// main() opens, since LoadModel uploads the meshes.
#include "checks.h"
#include "core/draw_path.h"
#include "core/shader.h"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RIG "tests/regression/assets/rig/test_rig.gltf"

static int failures;

static void Check(bool ok, const char *what)
{
    if (!ok)
        printf("FAIL: %s\n", what);
    failures += !ok;
}

static int BoneNamed(const Model *model, const char *name)
{
    for (int i = 0; i < model->boneCount; i++)
        if (!strcmp(model->bones[i].name, name))
            return i;
    return -1;
}

static const ModelAnimation *Clip(const ModelAnimation *clips, int count, const char *name)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(clips[i].name, name))
            return &clips[i];
    return NULL;
}

/* ---- the two skinning paths draw the same pose (docs/developer/store.md §3 Animation) ---------- */
typedef struct Silhouette
{
    long pixels, sumX;
    unsigned char *mask;
} Silhouette;

/* Draws one item into target from in front of the rig and reads back which pixels are lit. */
static Silhouette Shoot(DrawPath *path, RenderTexture2D target, const DrawItem *item)
{
    Camera3D camera = {{0, 1.2f, 6}, {0, 1.2f, 0}, {0, 1, 0}, 45.0f, CAMERA_PERSPECTIVE};
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode3D(camera);
    DrawPathBegin(path, camera, target.texture.width, target.texture.height);
    DrawPathAdd(path, item);
    DrawPathEnd(path);
    EndMode3D();
    EndTextureMode();
    Image image = LoadImageFromTexture(target.texture);
    Color *colors = LoadImageColors(image);
    int n = image.width * image.height;
    Silhouette s = {0, 0, calloc((size_t)n, 1)};
    for (int i = 0; colors && s.mask && i < n; i++)
        if (colors[i].r > 128)
        {
            s.mask[i] = 1;
            s.pixels++;
            s.sumX += i % image.width;
        }
    UnloadImageColors(colors);
    UnloadImage(image);
    return s;
}

static long Differ(const Silhouette *a, const Silhouette *b, int n)
{
    long d = 0;
    for (int i = 0; a->mask && b->mask && i < n; i++)
        d += a->mask[i] != b->mask[i];
    return d;
}

/* The test rig at the top of its wave, drawn the runner's two ways: bone matrices uploaded to the
   engine's skinning shader built for 24 bones (GPU), and raylib's UpdateModelAnimation skinning
   the vertices into a static mesh (CPU). Both must cover the same pixels, and not the rest pose's. */
static void SkinningPathChecks(void)
{
    enum { SIZE = 128 };
    Model model = LoadModel(TEST_RIG);
    int count = 0;
    ModelAnimation *clips = LoadModelAnimations(TEST_RIG, &count);
    const ModelAnimation *wave = Clip(clips, count, "wave");
    const ShaderFile files = {"core/shaders/skinning.vs", "core/shaders/textured.fs"};
    Shader skin = {0}, plain = {rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
    bool built = CoreLoadShaderDefined(&files, "#define SKINNING_BONES 24", &skin);
    Check(built && GetShaderLocation(skin, "boneMatrices[23]") >= 0 && GetShaderLocation(skin, "boneMatrices[24]") < 0,
          "skinning paths: skinning.vs builds with SKINNING_BONES 24 and a 24-matrix bone array");
    if (!built || !wave || model.meshCount != 1)
    {
        Check(false, "skinning paths: the rig, its wave and the skinning shader are there");
        UnloadModelAnimations(clips, count);
        UnloadModel(model);
        return;
    }
    DrawPath path;
    DrawPathInit(&path);
    RenderTexture2D target = LoadRenderTexture(SIZE, SIZE);
    Texture2D white = {rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Mesh rest = model.meshes[0];
    Mesh unskinned = rest;
    unskinned.boneIds = NULL;
    unskinned.boneWeights = NULL;
    uint32_t shared = DrawPathMesh(&path, &rest), own = DrawPathMesh(&path, &unskinned);
    uint32_t gpuRed = DrawPathMaterial(&path, skin, white, RED, -1), cpuRed = DrawPathMaterial(&path, plain, white, RED, -1);
    Matrix bones[3];
    DrawItem item = {own, cpuRed, MatrixIdentity(), path.meshes[own - 1].center, 2 * path.meshes[own - 1].radius,
                     DRAW_LAYER_OPAQUE, NULL, 0};
    Silhouette atRest = Shoot(&path, target, &item);

    UpdateModelAnimationBones(model, *wave, 59);
    memcpy(bones, model.meshes[0].boneMatrices, sizeof bones);
    item = (DrawItem){shared, gpuRed, MatrixIdentity(), item.center, item.radius, DRAW_LAYER_OPAQUE, bones, 3};
    Silhouette gpu = Shoot(&path, target, &item);

    UpdateModelAnimation(model, *wave, 59);
    Check(DrawPathMeshPositions(&path, own, model.meshes[0].animVertices, model.meshes[0].animNormals) &&
              !DrawPathMeshPositions(&path, 999, model.meshes[0].animVertices, NULL) &&
              !DrawPathMeshPositions(&path, own, NULL, NULL),
          "skinning paths: a mesh's positions are rewritten in place; an unknown id or no positions is refused");
    item = (DrawItem){own, cpuRed, MatrixIdentity(), item.center, item.radius, DRAW_LAYER_OPAQUE, NULL, 0};
    Silhouette cpu = Shoot(&path, target, &item);

    long both = Differ(&gpu, &cpu, SIZE * SIZE), moved = Differ(&gpu, &atRest, SIZE * SIZE);
    double restX = atRest.pixels ? (double)atRest.sumX / (double)atRest.pixels : 0;
    double gpuX = gpu.pixels ? (double)gpu.sumX / (double)gpu.pixels : 0;
    printf("skinning paths: lit pixels rest %ld gpu %ld cpu %ld; gpu vs cpu differ in %ld, gpu vs rest in %ld; "
           "centroid x rest %.1f gpu %.1f\n",
           atRest.pixels, gpu.pixels, cpu.pixels, both, moved, restX, gpuX);
    Check(gpu.pixels > 200 && cpu.pixels > 200 && both * 100 <= gpu.pixels,
          "skinning paths: GPU and CPU skinning of one pose cover the same pixels (within 1%)");
    Check(moved * 10 > gpu.pixels && gpuX < restX - 2,
          "skinning paths: the waved pose is not the rest pose (the arm swings to -x)");
    free(atRest.mask);
    free(gpu.mask);
    free(cpu.mask);
    DrawPathFree(&path);
    UnloadRenderTexture(target);
    CoreUnloadShaders(&skin, 1);
    UnloadModelAnimations(clips, count);
    UnloadModel(model);
}

int AnimationAssetChecks(void)
{
    failures = 0;
    Model model = LoadModel(TEST_RIG);
    int root = BoneNamed(&model, "root"), arm = BoneNamed(&model, "arm"), hand = BoneNamed(&model, "hand.R");
    Check(model.meshCount == 1 && model.boneCount == 3 && root >= 0 && arm >= 0 && hand >= 0,
          "test rig: raylib loads one mesh and the bones root, arm and hand.R by name");
    Check(hand >= 0 && arm >= 0 && root >= 0 && model.bones[hand].parent == arm && model.bones[arm].parent == root &&
              model.bones[root].parent == -1,
          "test rig: the bones are a chain root > arm > hand.R");
    Check(model.meshCount == 1 && model.meshes[0].boneIds && model.meshes[0].boneWeights &&
              model.meshes[0].boneCount == 3 && model.meshes[0].vertexCount == 72,
          "test rig: its mesh carries bone ids and weights for 72 vertices");
    Check(hand >= 0 && Vector3Distance(model.bindPose[hand].translation, (Vector3){0, 2, 0}) < 1e-4f,
          "test rig: hand.R rests at (0, 2, 0) in model space");

    int count = 0;
    ModelAnimation *clips = LoadModelAnimations(TEST_RIG, &count);
    const ModelAnimation *idle = Clip(clips, count, "idle"), *wave = Clip(clips, count, "wave");
    Check(count == 2 && idle && wave && IsModelAnimationValid(model, *idle) && IsModelAnimationValid(model, *wave),
          "test rig: LoadModelAnimations reads idle and wave, both valid for the model");
    if (wave && hand >= 0 && wave->frameCount > 60)
    {
        /* raylib bakes glTF clips at 17 ms a frame (rmodels.c GLTF_ANIMDELAY): frame 59 is 1.003 s,
           the top of the wave, where the hand has swung a quarter turn about +Z round the arm. */
        Vector3 top = wave->framePoses[59][hand].translation, start = wave->framePoses[0][hand].translation;
        printf("test rig: wave has %d frames; hand.R at frame 0 (%.3f %.3f %.3f), frame 59 (%.3f %.3f %.3f)\n",
               wave->frameCount, start.x, start.y, start.z, top.x, top.y, top.z);
        Check(Vector3Distance(start, (Vector3){0, 2, 0}) < 1e-3f && Vector3Distance(top, (Vector3){-1, 1, 0}) < 0.02f,
              "test rig: wave swings hand.R from (0, 2, 0) to (-1, 1, 0) in the first second");
    }
    else
        Check(false, "test rig: wave has more than 60 frames and a hand.R bone");
    if (idle && hand >= 0)
        Check(Vector3Distance(idle->framePoses[idle->frameCount - 1][hand].translation, (Vector3){0, 2, 0}) < 1e-4f,
              "test rig: idle holds hand.R at rest");
    UnloadModelAnimations(clips, count);
    UnloadModel(model);

    /* Expected failures: a missing file has no clips, and a model without bones matches none. */
    int none = -1;
    ModelAnimation *missing = LoadModelAnimations("tests/regression/assets/rig/no_such_rig.gltf", &none);
    Check(!missing && none == 0, "test rig: a missing file loads no animations");
    Model cube = LoadModelFromMesh(GenMeshCube(1, 1, 1));
    clips = LoadModelAnimations(TEST_RIG, &count);
    Check(count == 2 && cube.boneCount == 0 && !IsModelAnimationValid(cube, clips[0]),
          "test rig: a clip is not valid for a model without its skeleton");
    UnloadModelAnimations(clips, count);
    UnloadModel(cube);
    SkinningPathChecks();
    return failures;
}
