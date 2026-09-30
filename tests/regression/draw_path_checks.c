/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The draw path (core/draw_path.h): sort keys, the radix sort, frustum culling, static batching and
// state changes in the submit. Keys and the sort need no GL; the rest runs under the hidden window
// main() opens.
#include "checks.h"
#include "core/draw_path.h"
#include "core/shader.h"
#include "raymath.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void Check(bool ok, const char *what)
{
    if (!ok)
        printf("FAIL: %s\n", what);
    failures += !ok;
}

// ---- keys -------------------------------------------------------------------------------------
typedef struct KeyCase
{
    char name;
    uint8_t layer;
    uint32_t shader, material, mesh;
    float depth;
} KeyCase;

/* Sorts the cases added in the order given by `add` and writes their names in drawn order. */
static void SortedNames(const KeyCase *cases, const int *add, int count, char *out)
{
    uint64_t keys[16];
    uint32_t order[16];
    for (int i = 0; i < count; i++)
    {
        const KeyCase *c = &cases[add[i]];
        DrawItem item = {c->mesh, c->material, MatrixIdentity(), {0, 0, 0}, 1.0f, c->layer, NULL, 0};
        keys[i] = DrawPathKey(&item, c->shader, c->depth);
        order[i] = (uint32_t)add[i];
    }
    DrawPathSort(keys, order, count);
    for (int i = 0; i < count; i++)
        out[i] = cases[order[i]].name;
    out[count] = 0;
}

static void KeyOrderChecks(void)
{
    /* Opaque: shader, then material, then mesh, then near to far. Alpha-tested after opaque,
       translucent far to near after that whatever their shader, viewmodel last. */
    static const KeyCase cases[] = {
        {'A', 0, 1, 1, 1, 0.5f}, {'B', 0, 0, 2, 1, 0.9f}, {'C', 0, 0, 1, 2, 0.1f},
        {'D', 0, 0, 1, 1, 0.8f}, {'E', 0, 0, 1, 1, 0.2f}, {'F', 1, 0, 1, 1, 0.0f},
        {'T', 2, 0, 1, 1, 0.3f}, {'U', 2, 1, 1, 1, 0.7f}, {'V', 2, 0, 3, 2, 0.5f},
        {'W', 3, 0, 1, 1, 0.0f},
    };
    const char *expected = "EDCBAFUVTW";
    int count = (int)(sizeof cases / sizeof cases[0]);
    int forward[16], backward[16], shuffled[16];
    for (int i = 0; i < count; i++)
    {
        forward[i] = i;
        backward[i] = count - 1 - i;
        shuffled[i] = (i * 7 + 3) % count; /* 7 is coprime with 10: a permutation */
    }
    char names[17];
    SortedNames(cases, forward, count, names);
    Check(!strcmp(names, expected), "draw keys: opaque by shader, material, mesh, depth; translucent back to front");
    SortedNames(cases, backward, count, names);
    Check(!strcmp(names, expected), "draw keys: the order does not depend on add order (reversed)");
    SortedNames(cases, shuffled, count, names);
    Check(!strcmp(names, expected), "draw keys: the order does not depend on add order (shuffled)");

    DrawItem far = {1, 1, MatrixIdentity(), {0, 0, 0}, 1.0f, 0, NULL, 0};
    Check(DrawPathKey(&far, 0, 7.0f) == DrawPathKey(&far, 0, 1.0f) &&
              DrawPathKey(&far, 0, -3.0f) == DrawPathKey(&far, 0, 0.0f),
          "draw keys: depth outside [0, 1] is clamped");
}

// ---- sort -------------------------------------------------------------------------------------
static int CompareKeys(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t state = 88172645463325252ull;
static uint64_t Next(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static void SortChecks(void)
{
    enum { N = 1000 };
    static uint64_t keys[N], original[N], reference[N];
    static uint32_t order[N];
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < N; i++)
        {
            /* The second pass draws from 40 values, so equal keys are common. */
            keys[i] = pass == 0 ? Next() : (Next() % 40) << 44;
            original[i] = reference[i] = keys[i];
            order[i] = (uint32_t)i;
        }
        qsort(reference, N, sizeof reference[0], CompareKeys);
        DrawPathSort(keys, order, N);
        bool same = true, carried = true, stable = true;
        for (int i = 0; i < N; i++)
        {
            same &= keys[i] == reference[i];
            carried &= keys[i] == original[order[i]];
            if (i > 0 && keys[i] == keys[i - 1])
                stable &= order[i] > order[i - 1];
        }
        Check(same, pass == 0 ? "radix sort: 1,000 random keys come out as qsort orders them"
                              : "radix sort: 1,000 keys with repeats come out as qsort orders them");
        Check(carried, "radix sort: each value travels with its key");
        Check(stable, "radix sort: equal keys keep the order they were given in");
    }
    uint64_t one = 5;
    uint32_t oneOrder = 9;
    DrawPathSort(&one, &oneOrder, 1);
    DrawPathSort(NULL, NULL, 10);
    Check(one == 5 && oneOrder == 9, "radix sort: one key, or no arrays, is left alone");
}

// ---- GL ---------------------------------------------------------------------------------------
static DrawItem ItemAt(const DrawPath *path, uint32_t mesh, uint32_t material, Vector3 at, uint8_t layer)
{
    DrawItem item = {mesh, material, MatrixTranslate(at.x, at.y, at.z), at,
                     path->meshes[mesh - 1].radius, layer, NULL, 0};
    return item;
}

static bool IsIdentity(Matrix m)
{
    const float *a = &m.m0; /* raylib's Matrix is 16 consecutive floats */
    const Matrix identity = MatrixIdentity();
    const float *b = &identity.m0;
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

static Camera3D ViewCamera(void)
{
    Camera3D camera = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 60.0f, CAMERA_PERSPECTIVE};
    return camera;
}

static DrawStats Frame(DrawPath *path, RenderTexture2D target, const DrawItem *items, int count)
{
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode3D(ViewCamera());
    DrawPathBegin(path, ViewCamera(), target.texture.width, target.texture.height);
    for (int i = 0; i < count; i++)
        DrawPathAdd(path, &items[i]);
    DrawStats stats = DrawPathEnd(path);
    EndMode3D();
    EndTextureMode();
    return stats;
}

static void GlChecks(void)
{
    DrawPath path;
    Check(DrawPathInit(&path), "draw path: init");
    RenderTexture2D target = LoadRenderTexture(64, 64);
    Shader shader = {rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
    unsigned char pixel[4] = {255, 255, 255, 255};
    Texture2D white = {rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D other = {rlLoadTexture(pixel, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1), 1, 1, 1,
                       PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

    Mesh source = GenMeshCube(1, 1, 1);
    uint32_t cube = DrawPathMesh(&path, &source);
    Check(cube == 1 && path.meshes[0].vertexCount == source.vertexCount &&
              path.meshes[0].triangleCount == source.triangleCount,
          "draw path: a mesh is copied with its counts and answers id 1");
    Check(cube && FloatEquals(path.meshes[0].radius, sqrtf(0.75f)),
          "draw path: a unit cube's bounding sphere has radius sqrt(3)/2");
    uint32_t red = DrawPathMaterial(&path, shader, white, (Color){255, 0, 0, 255}, -1);
    uint32_t blue = DrawPathMaterial(&path, shader, other, (Color){0, 0, 255, 255}, -1);
    Check(red == 1 && blue == 2 && path.shaderCount == 1,
          "draw path: two materials on one shader share one shader index");

    /* Expected failures. */
    Mesh empty = {0};
    Mesh broken = source;
    unsigned short badIndices[3] = {0, 1, 9999};
    broken.indices = badIndices;
    broken.triangleCount = 1;
    Check(!DrawPathMesh(&path, NULL) && !DrawPathMesh(&path, &empty) && !DrawPathMesh(&path, &broken),
          "draw path: a missing, empty or out-of-range mesh is refused");
    Shader none = {0, NULL};
    Check(!DrawPathMaterial(&path, none, white, RED, -1), "draw path: a material without a shader is refused");
    Check(!DrawPathMeshUpdate(&path, 42, &source) && !DrawPathMeshUpdate(&path, cube, &empty) &&
              path.meshes[0].vertexCount == source.vertexCount,
          "draw path: updating an unknown id or with an empty mesh is refused and changes nothing");

    /* (c) Culling: 100 in front, 100 behind. */
    static DrawItem items[256];
    for (int i = 0; i < 100; i++)
    {
        Vector3 front = {(float)(i % 10) - 4.5f, (float)(i / 10) - 4.5f, -20.0f};
        Vector3 behind = {front.x, front.y, 20.0f};
        items[i] = ItemAt(&path, cube, red, front, 0);
        items[100 + i] = ItemAt(&path, cube, red, behind, 0);
    }
    DrawStats stats = Frame(&path, target, items, 200);
    Check(stats.items == 200 && stats.visible == 100 && stats.draws == 100,
          "draw path: of 100 items in front and 100 behind, 100 are drawn");
    items[0] = ItemAt(&path, cube, red, (Vector3){0, 0, -1500.0f}, 0);
    items[1] = ItemAt(&path, cube, red, (Vector3){0, 0, -999.0f}, 0);
    items[2] = ItemAt(&path, cube, red, (Vector3){0, 0, 0.3f}, 0); /* sphere crosses the near plane */
    stats = Frame(&path, target, items, 3);
    Check(stats.visible == 2 && stats.draws == 2,
          "draw path: an item beyond the far plane is culled, one straddling a plane is not");
    DrawItem stray = items[1];
    stray.mesh = 42;
    DrawItem layered = items[1];
    layered.layer = 4;
    DrawPathBegin(&path, ViewCamera(), 64, 64);
    DrawPathAdd(&path, &stray);
    DrawPathAdd(&path, &layered);
    Check(path.itemCount == 0, "draw path: an item with an unknown mesh or a layer above 3 is ignored");
    DrawPathEnd(&path);

    /* The submit reaches the framebuffer: a red cube in the middle of the view. */
    items[0] = ItemAt(&path, cube, red, (Vector3){0, 0, -3.0f}, 0);
    Frame(&path, target, items, 1);
    Image image = LoadImageFromTexture(target.texture);
    Color centre = GetImageColor(image, 32, 32), corner = GetImageColor(image, 1, 1);
    UnloadImage(image);
    Check(centre.r > 200 && centre.g < 50 && centre.b < 50 && corner.r < 50,
          "draw path: a submitted cube draws its material's tint where it is, and nothing elsewhere");

    /* (e) Two materials on one shader: one shader switch, two texture binds. */
    for (int i = 0; i < 4; i++)
        items[i] = ItemAt(&path, cube, i % 2 ? blue : red, (Vector3){(float)i - 1.5f, 0, -10.0f}, 0);
    stats = Frame(&path, target, items, 4);
    Check(stats.draws == 4 && stats.shaderSwitches == 1 && stats.textureSwitches == 2,
          "draw path: two materials sharing a shader switch shader once and texture twice");

    /* (d) Static batching: 10 cubes of one material become one mesh of ten cubes' vertices. */
    for (int i = 0; i < 10; i++)
        items[i] = ItemAt(&path, cube, red, (Vector3){(float)i * 2.0f, 0, -10.0f}, 0);
    DrawItem merged[4];
    int meshesBefore = path.meshCount;
    int written = DrawPathStaticBatch(&path, items, 10, merged, 4);
    const DrawPathMeshData *batch = written == 1 ? &path.meshes[merged[0].mesh - 1] : NULL;
    Check(written == 1 && path.meshCount == meshesBefore + 1 && batch &&
              batch->vertexCount == 10 * source.vertexCount &&
              batch->triangleCount == 10 * source.triangleCount,
          "static batch: 10 cubes of one material merge into one mesh with 10 cubes' vertices");
    Check(batch && merged[0].material == red && FloatEquals(merged[0].center.x, 9.0f) &&
              FloatEquals(merged[0].center.z, -10.0f) && IsIdentity(merged[0].world),
          "static batch: the merged item is in world space with the identity matrix");
    stats = Frame(&path, target, merged, written);
    Check(stats.draws == 1, "static batch: the merged item is one draw");
    items[3].material = blue;
    items[5].material = 99;
    written = DrawPathStaticBatch(&path, items, 10, merged, 4);
    Check(written == 2 && path.meshes[merged[0].mesh - 1].vertexCount == 8 * source.vertexCount,
          "static batch: one merged item per material, and an unknown material is skipped");
    Check(!DrawPathStaticBatch(&path, items, 10, merged, 0) && !DrawPathStaticBatch(&path, NULL, 3, merged, 4),
          "static batch: no room or no items merges nothing");

    /* A rebuilt batch releases its old meshes, and the next upload takes the freed slot. */
    uint32_t old = merged[0].mesh;
    int count = path.meshCount;
    Check(DrawPathMeshRelease(&path, old) && !DrawPathMeshRelease(&path, old) && !DrawPathMeshRelease(&path, 0) &&
              !DrawPathMeshRelease(&path, 999),
          "static batch: a merged mesh is released once; an unknown or released id is refused");
    DrawItem stale = merged[0];
    stale.mesh = old;
    DrawPathBegin(&path, ViewCamera(), 64, 64);
    DrawPathAdd(&path, &stale);
    Check(path.itemCount == 0 && !DrawPathMeshPositions(&path, old, source.vertices, NULL) &&
              !DrawPathMeshUpdate(&path, old, &source),
          "static batch: a released mesh is not drawn, rewritten or updated");
    DrawPathEnd(&path);
    Check(DrawPathStaticBatch(&path, items, 3, merged, 4) == 1 && merged[0].mesh == old && path.meshCount == count,
          "static batch: the next merged mesh reuses the released slot instead of growing the path");

    /* Updating a mesh keeps its id and takes the new contents. */
    Mesh plane = GenMeshPlane(1, 1, 1, 1);
    Check(DrawPathMeshUpdate(&path, cube, &plane) == cube && path.meshes[cube - 1].vertexCount == plane.vertexCount,
          "draw path: updating a mesh keeps its id and replaces its contents");
    UnloadMesh(plane);

    DrawPathFree(&path);
    Check(path.meshCount == 0 && path.materialCount == 0 && path.itemCount == 0 && !path.meshes,
          "draw path: free leaves the path empty");
    UnloadMesh(source);
    rlUnloadTexture(other.id);
    UnloadRenderTexture(target);
}

static bool Red(Color c) { return c.r > 200 && c.g < 50 && c.b < 50; }

/* Skinning (docs/developer/store.md §4): a mesh with bone ids and weights keeps them, a material on
   the engine's skinning shader finds its bone-matrix array, and an item's bones move what it draws;
   a static material, or a skinned one given no bones, uploads none. */
static void SkinChecks(void)
{
    DrawPath path;
    DrawPathInit(&path);
    RenderTexture2D target = LoadRenderTexture(64, 64);
    const ShaderFile files = {"core/shaders/skinning.vs", "core/shaders/textured.fs"};
    Shader skin = {0}, plain = {rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
    Check(CoreLoadShaders(&files, 1, &skin), "draw path skinning: the engine's skinning shader builds");
    Texture2D white = {rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};

    Mesh cube = GenMeshCube(1, 1, 1);
    unsigned char *ids = calloc((size_t)cube.vertexCount * 4, 1); /* every vertex on bone 0 */
    float *weights = calloc((size_t)cube.vertexCount * 4, sizeof(float));
    for (int v = 0; weights && v < cube.vertexCount; v++)
        weights[v * 4] = 1.0f;
    Mesh skinned = cube;
    skinned.boneIds = ids;
    skinned.boneWeights = weights;
    uint32_t still = DrawPathMesh(&path, &cube), rigged = DrawPathMesh(&path, &skinned);
    const DrawPathMeshData *r = rigged ? &path.meshes[rigged - 1] : NULL;
    Check(still && r && r->boneIds && r->boneWeights && r->vbo[5] && r->vbo[6] && !path.meshes[still - 1].boneIds &&
              !path.meshes[still - 1].vbo[5],
          "draw path skinning: a mesh with bone ids and weights keeps and uploads them, a static one has none");
    uint32_t skinRed = skin.id ? DrawPathMaterial(&path, skin, white, (Color){255, 0, 0, 255}, -1) : 0;
    uint32_t plainRed = DrawPathMaterial(&path, plain, white, (Color){255, 0, 0, 255}, -1);
    Check(skinRed && plainRed && path.materials[skinRed - 1].bonesLoc >= 0 && path.materials[plainRed - 1].bonesLoc < 0,
          "draw path skinning: a material on a skinning shader has a bone-matrix location, a static one none");

    Matrix bones[2] = {MatrixIdentity(), MatrixIdentity()};
    DrawItem item = {rigged, skinRed, MatrixTranslate(0, 0, -3), {0, 0, -3}, 2.0f, DRAW_LAYER_OPAQUE, bones, 2};
    DrawStats stats = Frame(&path, target, &item, 1);
    Image image = LoadImageFromTexture(target.texture);
    bool centred = Red(GetImageColor(image, 32, 32)) && !Red(GetImageColor(image, 54, 32));
    UnloadImage(image);
    Check(stats.draws == 1 && stats.boneUploads == 1 && centred,
          "draw path skinning: at rest (identity bones) the skinned cube draws where its item is");
    bones[0] = MatrixTranslate(1.2f, 0, 0); /* 1.2 m right at 3 m: about 54 px of 64 across */
    stats = Frame(&path, target, &item, 1);
    image = LoadImageFromTexture(target.texture);
    bool moved = !Red(GetImageColor(image, 32, 32)) && Red(GetImageColor(image, 54, 32));
    UnloadImage(image);
    Check(stats.boneUploads == 1 && moved, "draw path skinning: the item's bone matrix moves what it draws");

    /* Expected failures: bones on a static material, and a skinned material with no bones, upload none. */
    DrawItem items[2] = {item, item};
    items[0].mesh = still;
    items[0].material = plainRed;
    items[1].bones = NULL;
    items[1].boneCount = 0;
    stats = Frame(&path, target, items, 1);
    image = LoadImageFromTexture(target.texture);
    bool ignored = Red(GetImageColor(image, 32, 32)) && !Red(GetImageColor(image, 54, 32));
    UnloadImage(image);
    Check(stats.draws == 1 && stats.boneUploads == 0 && ignored,
          "draw path skinning: a static material ignores an item's bones and uploads none");
    stats = Frame(&path, target, &items[1], 1);
    Check(stats.draws == 1 && stats.boneUploads == 0, "draw path skinning: a skinned item with no bones uploads none");

    DrawPathFree(&path);
    free(ids);
    free(weights);
    UnloadMesh(cube);
    CoreUnloadShaders(&skin, 1);
    UnloadRenderTexture(target);
}

int DrawPathChecks(void)
{
    failures = 0;
    KeyOrderChecks();
    SortChecks();
    GlChecks();
    SkinChecks();
    return failures;
}
