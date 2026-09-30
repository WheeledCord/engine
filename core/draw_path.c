/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* clock_gettime and CLOCK_MONOTONIC are POSIX, and -std=c99 alone hides them. This has to precede
   every include in the translation unit to take effect. */
#define _POSIX_C_SOURCE 199309L

#include "draw_path.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VERTICES 65536 /* indices are unsigned short, as raylib's Mesh has them */
/* Where rlgl binds vertexBoneIds and vertexBoneWeights in every shader it links (raylib's config.h;
   rlgl.h declares them only under RL_SUPPORT_MESH_GPU_SKINNING, which the library's own build sets). */
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEIDS
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEIDS 7
#endif
#ifndef RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS
#define RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS 8
#endif

static double NowMicros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

// ---- keys and sort ----------------------------------------------------------------------------
/* Everything but the depth: fixed for an item once it is added (Ericson's cached key, E9). */
static uint64_t KeyBase(uint32_t layer, uint32_t shader, uint32_t material, uint32_t mesh)
{
    uint64_t l = layer & 3u, s = shader & 255u, m = material & 4095u, e = mesh & 4095u;
    if (l == DRAW_LAYER_TRANSLUCENT)
        return (l << 62) | (s << 24) | (m << 12) | e;
    return (l << 62) | (s << 40) | (m << 28) | (e << 16);
}

static uint64_t KeyDepth(uint32_t layer, uint32_t depth16)
{
    if ((layer & 3u) == DRAW_LAYER_TRANSLUCENT)
        return (uint64_t)(65535u - depth16) << 32; /* back to front */
    return depth16;                                 /* front to back */
}

static uint32_t Quantise(float depth01)
{
    if (!(depth01 > 0.0f))
        return 0;
    if (depth01 >= 1.0f)
        return 65535;
    return (uint32_t)(depth01 * 65535.0f + 0.5f);
}

uint64_t DrawPathKey(const DrawItem *item, uint32_t shaderIndex, float depth01)
{
    if (!item)
        return 0;
    return KeyBase(item->layer, shaderIndex, item->material, item->mesh) |
           KeyDepth(item->layer, Quantise(depth01));
}

/* 8-bit LSD radix sort of (key, value) pairs, as in E9 and Quake 3's renderer. All eight byte
   histograms are counted in one sweep; a byte every key shares needs no pass. */
static void RadixSort(uint64_t *keys, uint32_t *values, uint64_t *keysTmp, uint32_t *valuesTmp,
                      int count)
{
    uint32_t counts[8][256];
    if (count < 2)
        return;
    memset(counts, 0, sizeof counts);
    for (int i = 0; i < count; i++)
    {
        uint64_t k = keys[i];
        for (int b = 0; b < 8; b++)
            counts[b][(k >> (b * 8)) & 255u]++;
    }
    uint64_t *srcK = keys, *dstK = keysTmp;
    uint32_t *srcV = values, *dstV = valuesTmp;
    for (int b = 0; b < 8; b++)
    {
        int shift = b * 8;
        uint32_t *c = counts[b];
        if (c[(srcK[0] >> shift) & 255u] == (uint32_t)count)
            continue;
        uint32_t sum = 0;
        for (int i = 0; i < 256; i++)
        {
            uint32_t n = c[i];
            c[i] = sum;
            sum += n;
        }
        for (int i = 0; i < count; i++)
        {
            uint32_t at = c[(srcK[i] >> shift) & 255u]++;
            dstK[at] = srcK[i];
            dstV[at] = srcV[i];
        }
        uint64_t *tk = srcK;
        srcK = dstK;
        dstK = tk;
        uint32_t *tv = srcV;
        srcV = dstV;
        dstV = tv;
    }
    if (srcK != keys)
    {
        memcpy(keys, srcK, (size_t)count * sizeof *keys);
        memcpy(values, srcV, (size_t)count * sizeof *values);
    }
}

void DrawPathSort(uint64_t *keys, uint32_t *order, int count)
{
    if (!keys || !order || count < 2)
        return;
    uint64_t *tk = malloc((size_t)count * sizeof *tk);
    uint32_t *tv = malloc((size_t)count * sizeof *tv);
    if (tk && tv)
        RadixSort(keys, order, tk, tv, count);
    else
        for (int i = 1; i < count; i++) /* stable, and needs no memory */
        {
            uint64_t k = keys[i];
            uint32_t v = order[i];
            int j = i;
            for (; j > 0 && keys[j - 1] > k; j--)
            {
                keys[j] = keys[j - 1];
                order[j] = order[j - 1];
            }
            keys[j] = k;
            order[j] = v;
        }
    free(tk);
    free(tv);
}

// ---- lifetime ---------------------------------------------------------------------------------
bool DrawPathInit(DrawPath *path)
{
    if (!path)
        return false;
    memset(path, 0, sizeof *path);
    path->view = path->projection = path->viewProjection = MatrixIdentity();
    path->farPlane = 1000.0f;
    return true;
}

static void MeshRelease(DrawPathMeshData *m)
{
    if (m->vao)
        rlUnloadVertexArray(m->vao);
    for (int i = 0; i < 7; i++)
        if (m->vbo[i])
            rlUnloadVertexBuffer(m->vbo[i]);
    free(m->positions);
    free(m->texcoords);
    free(m->normals);
    free(m->colors);
    free(m->indices);
    free(m->boneIds);
    free(m->boneWeights);
    memset(m, 0, sizeof *m);
}

void DrawPathFree(DrawPath *path)
{
    if (!path)
        return;
    for (int i = 0; i < path->meshCount; i++)
        MeshRelease(&path->meshes[i]);
    free(path->meshes);
    free(path->materials);
    free(path->items);
    free(path->itemKeys);
    free(path->keys);
    free(path->keysScratch);
    free(path->order);
    free(path->orderScratch);
    DrawPathInit(path);
}

// ---- meshes -----------------------------------------------------------------------------------
static void MeshBounds(DrawPathMeshData *m)
{
    Vector3 lo = {m->positions[0], m->positions[1], m->positions[2]}, hi = lo;
    for (int i = 1; i < m->vertexCount; i++)
    {
        const float *p = m->positions + i * 3;
        lo = Vector3Min(lo, (Vector3){p[0], p[1], p[2]});
        hi = Vector3Max(hi, (Vector3){p[0], p[1], p[2]});
    }
    Vector3 c = Vector3Scale(Vector3Add(lo, hi), 0.5f);
    float r2 = 0.0f;
    for (int i = 0; i < m->vertexCount; i++)
    {
        const float *p = m->positions + i * 3;
        float d2 = Vector3DistanceSqr(c, (Vector3){p[0], p[1], p[2]});
        if (d2 > r2)
            r2 = d2;
    }
    m->center = c;
    m->radius = sqrtf(r2);
}

/* The same rlLoad* calls as UploadMesh's GL 2.1/3.3 branch, at raylib's default attribute
   locations. Without vertex array objects rlLoadVertexArray answers 0 and the attribute state set
   here is simply overwritten per draw by the fallback in BindMesh. */
static bool MeshUpload(DrawPathMeshData *m)
{
    int n = m->vertexCount;
    m->vao = rlLoadVertexArray();
    rlEnableVertexArray(m->vao);
    m->vbo[0] = rlLoadVertexBuffer(m->positions, n * 3 * (int)sizeof(float), false);
    rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, 3, RL_FLOAT, 0, 0, 0);
    rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION);
    m->vbo[1] = rlLoadVertexBuffer(m->texcoords, n * 2 * (int)sizeof(float), false);
    rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, 2, RL_FLOAT, 0, 0, 0);
    rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD);
    if (m->normals)
    {
        m->vbo[2] = rlLoadVertexBuffer(m->normals, n * 3 * (int)sizeof(float), false);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL, 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL);
    }
    else
        rlDisableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL);
    if (m->colors)
    {
        m->vbo[3] = rlLoadVertexBuffer(m->colors, n * 4, false);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR, 4, RL_UNSIGNED_BYTE, 1, 0, 0);
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR);
    }
    else
        rlDisableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR);
    if (m->boneIds && m->boneWeights) /* as UploadMesh does under SUPPORT_GPU_SKINNING */
    {
        m->vbo[5] = rlLoadVertexBuffer(m->boneIds, n * 4, false);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEIDS, 4, RL_UNSIGNED_BYTE, 0, 0, 0);
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEIDS);
        m->vbo[6] = rlLoadVertexBuffer(m->boneWeights, n * 4 * (int)sizeof(float), false);
        rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS, 4, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_BONEWEIGHTS);
    }
    m->vbo[4] = rlLoadVertexBufferElement(m->indices,
                                          m->triangleCount * 3 * (int)sizeof(unsigned short), false);
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();
    return m->vbo[0] && m->vbo[1] && m->vbo[4] && (!m->boneIds || (m->vbo[5] && m->vbo[6]));
}

/* Fills out with an owned copy of mesh's arrays; false (with nothing allocated) when refused. */
static bool MeshCopy(DrawPathMeshData *out, const Mesh *mesh)
{
    memset(out, 0, sizeof *out);
    if (!mesh || !mesh->vertices || mesh->vertexCount <= 0 || mesh->vertexCount > MAX_VERTICES)
        return false;
    int n = mesh->vertexCount;
    int tris = mesh->indices ? mesh->triangleCount : n / 3;
    if (tris <= 0)
        return false;
    if (mesh->indices)
        for (int i = 0; i < tris * 3; i++)
            if (mesh->indices[i] >= n)
                return false;
    out->vertexCount = n;
    out->triangleCount = tris;
    out->positions = malloc((size_t)n * 3 * sizeof(float));
    out->texcoords = calloc((size_t)n * 2, sizeof(float));
    out->indices = malloc((size_t)tris * 3 * sizeof(unsigned short));
    if (mesh->normals)
        out->normals = malloc((size_t)n * 3 * sizeof(float));
    if (mesh->colors)
        out->colors = malloc((size_t)n * 4);
    bool skinned = mesh->boneIds && mesh->boneWeights;
    if (skinned)
    {
        out->boneIds = malloc((size_t)n * 4);
        out->boneWeights = malloc((size_t)n * 4 * sizeof(float));
    }
    if (!out->positions || !out->texcoords || !out->indices || (mesh->normals && !out->normals) ||
        (mesh->colors && !out->colors) || (skinned && (!out->boneIds || !out->boneWeights)))
    {
        MeshRelease(out);
        return false;
    }
    memcpy(out->positions, mesh->vertices, (size_t)n * 3 * sizeof(float));
    if (mesh->texcoords)
        memcpy(out->texcoords, mesh->texcoords, (size_t)n * 2 * sizeof(float));
    if (mesh->normals)
        memcpy(out->normals, mesh->normals, (size_t)n * 3 * sizeof(float));
    if (mesh->colors)
        memcpy(out->colors, mesh->colors, (size_t)n * 4);
    if (skinned)
    {
        memcpy(out->boneIds, mesh->boneIds, (size_t)n * 4);
        memcpy(out->boneWeights, mesh->boneWeights, (size_t)n * 4 * sizeof(float));
    }
    if (mesh->indices)
        memcpy(out->indices, mesh->indices, (size_t)tris * 3 * sizeof(unsigned short));
    else
        for (int i = 0; i < tris * 3; i++)
            out->indices[i] = (unsigned short)i;
    MeshBounds(out);
    return true;
}

/* A released slot has no positions (DrawPathMeshRelease); every live mesh has them. */
static bool MeshLive(const DrawPath *path, uint32_t id)
{
    return id >= 1 && id <= (uint32_t)path->meshCount && path->meshes[id - 1].positions;
}

/* Takes ownership of a filled record: uploads it into the first released slot, or appends it.
   Releases it on failure. */
static uint32_t MeshAdopt(DrawPath *path, DrawPathMeshData *m)
{
    for (int i = 0; i < path->meshCount; i++)
        if (!path->meshes[i].positions)
        {
            if (!MeshUpload(m))
            {
                MeshRelease(m);
                return 0;
            }
            path->meshes[i] = *m;
            return (uint32_t)i + 1;
        }
    if (path->meshCount >= DRAW_PATH_MAX_MESHES)
    {
        MeshRelease(m);
        return 0;
    }
    if (path->meshCount == path->meshCapacity)
    {
        int capacity = path->meshCapacity ? path->meshCapacity * 2 : 16;
        DrawPathMeshData *grown = realloc(path->meshes, (size_t)capacity * sizeof *grown);
        if (!grown)
        {
            MeshRelease(m);
            return 0;
        }
        path->meshes = grown;
        path->meshCapacity = capacity;
    }
    if (!MeshUpload(m))
    {
        MeshRelease(m);
        return 0;
    }
    path->meshes[path->meshCount++] = *m;
    return (uint32_t)path->meshCount;
}

uint32_t DrawPathMesh(DrawPath *path, const Mesh *mesh)
{
    DrawPathMeshData m;
    if (!path || !MeshCopy(&m, mesh))
        return 0;
    return MeshAdopt(path, &m);
}

bool DrawPathMeshRelease(DrawPath *path, uint32_t id)
{
    if (!path || !MeshLive(path, id))
        return false;
    MeshRelease(&path->meshes[id - 1]);
    return true;
}

uint32_t DrawPathMeshUpdate(DrawPath *path, uint32_t id, const Mesh *mesh)
{
    DrawPathMeshData m;
    if (!path || !MeshLive(path, id) || !MeshCopy(&m, mesh))
        return 0;
    if (!MeshUpload(&m))
    {
        MeshRelease(&m);
        return 0;
    }
    MeshRelease(&path->meshes[id - 1]);
    path->meshes[id - 1] = m;
    return id;
}

bool DrawPathMeshPositions(DrawPath *path, uint32_t id, const float *positions, const float *normals)
{
    if (!path || !MeshLive(path, id) || !positions)
        return false;
    DrawPathMeshData *m = &path->meshes[id - 1];
    size_t bytes = (size_t)m->vertexCount * 3 * sizeof(float);
    memcpy(m->positions, positions, bytes);
    rlUpdateVertexBuffer(m->vbo[0], m->positions, (int)bytes, 0);
    if (normals && m->normals)
    {
        memcpy(m->normals, normals, bytes);
        rlUpdateVertexBuffer(m->vbo[2], m->normals, (int)bytes, 0);
    }
    return true;
}

// ---- materials --------------------------------------------------------------------------------
static int Location(Shader shader, int slot, const char *name, bool attribute)
{
    if (shader.locs && shader.locs[slot] >= 0)
        return shader.locs[slot];
    return attribute ? rlGetLocationAttrib(shader.id, name) : rlGetLocationUniform(shader.id, name);
}

uint32_t DrawPathMaterial(DrawPath *path, Shader shader, Texture2D texture, Color tint,
                          int normalMatrixLoc)
{
    if (!path || shader.id == 0 || path->materialCount >= DRAW_PATH_MAX_MATERIALS)
        return 0;
    int shaderIndex = 0;
    while (shaderIndex < path->shaderCount && path->shaders[shaderIndex] != shader.id)
        shaderIndex++;
    if (shaderIndex == DRAW_PATH_MAX_SHADERS)
        return 0;
    if (path->materialCount == path->materialCapacity)
    {
        int capacity = path->materialCapacity ? path->materialCapacity * 2 : 16;
        DrawPathMaterialData *grown = realloc(path->materials, (size_t)capacity * sizeof *grown);
        if (!grown)
            return 0;
        path->materials = grown;
        path->materialCapacity = capacity;
    }
    if (shaderIndex == path->shaderCount)
        path->shaders[path->shaderCount++] = shader.id;
    DrawPathMaterialData *m = &path->materials[path->materialCount];
    m->shaderId = shader.id;
    m->shaderIndex = (uint32_t)shaderIndex;
    m->textureId = texture.id ? texture.id : rlGetTextureIdDefault();
    m->tint[0] = tint.r / 255.0f;
    m->tint[1] = tint.g / 255.0f;
    m->tint[2] = tint.b / 255.0f;
    m->tint[3] = tint.a / 255.0f;
    m->mvpLoc = Location(shader, SHADER_LOC_MATRIX_MVP, "mvp", false);
    m->modelLoc = Location(shader, SHADER_LOC_MATRIX_MODEL, "matModel", false);
    m->viewLoc = Location(shader, SHADER_LOC_MATRIX_VIEW, "matView", false);
    m->projectionLoc = Location(shader, SHADER_LOC_MATRIX_PROJECTION, "matProjection", false);
    m->colorLoc = Location(shader, SHADER_LOC_COLOR_DIFFUSE, "colDiffuse", false);
    m->samplerLoc = Location(shader, SHADER_LOC_MAP_DIFFUSE, "texture0", false);
    m->bonesLoc = Location(shader, SHADER_LOC_BONE_MATRICES, "boneMatrices", false);
    m->normalLoc = normalMatrixLoc;
    m->positionAttrib = Location(shader, SHADER_LOC_VERTEX_POSITION, "vertexPosition", true);
    m->texcoordAttrib = Location(shader, SHADER_LOC_VERTEX_TEXCOORD01, "vertexTexCoord", true);
    m->normalAttrib = Location(shader, SHADER_LOC_VERTEX_NORMAL, "vertexNormal", true);
    m->colorAttrib = Location(shader, SHADER_LOC_VERTEX_COLOR, "vertexColor", true);
    m->boneIdsAttrib = Location(shader, SHADER_LOC_VERTEX_BONEIDS, "vertexBoneIds", true);
    m->boneWeightsAttrib = Location(shader, SHADER_LOC_VERTEX_BONEWEIGHTS, "vertexBoneWeights", true);
    return (uint32_t)++path->materialCount;
}

// ---- static batching --------------------------------------------------------------------------
static bool ItemValid(const DrawPath *path, const DrawItem *item)
{
    return MeshLive(path, item->mesh) && item->material >= 1 &&
           item->material <= (uint32_t)path->materialCount && item->layer <= 3;
}

/* Builds one world-space mesh from members[0..count) and uploads it. */
static uint32_t MergeMeshes(DrawPath *path, const DrawItem *items, const int *members, int count)
{
    DrawPathMeshData m;
    memset(&m, 0, sizeof m);
    bool normals = false, colors = false;
    for (int i = 0; i < count; i++)
    {
        const DrawPathMeshData *src = &path->meshes[items[members[i]].mesh - 1];
        m.vertexCount += src->vertexCount;
        m.triangleCount += src->triangleCount;
        normals |= src->normals != NULL;
        colors |= src->colors != NULL;
    }
    size_t n = (size_t)m.vertexCount;
    m.positions = malloc(n * 3 * sizeof(float));
    m.texcoords = malloc(n * 2 * sizeof(float));
    m.indices = malloc((size_t)m.triangleCount * 3 * sizeof(unsigned short));
    m.normals = normals ? malloc(n * 3 * sizeof(float)) : NULL;
    m.colors = colors ? malloc(n * 4) : NULL;
    if (!m.positions || !m.texcoords || !m.indices || (normals && !m.normals) ||
        (colors && !m.colors))
    {
        MeshRelease(&m);
        return 0;
    }
    int v = 0, t = 0;
    for (int i = 0; i < count; i++)
    {
        const DrawItem *item = &items[members[i]];
        const DrawPathMeshData *src = &path->meshes[item->mesh - 1];
        Matrix w = item->world, nm = MatrixTranspose(MatrixInvert(w));
        for (int k = 0; k < src->vertexCount; k++)
        {
            const float *p = src->positions + k * 3;
            Vector3 wp = Vector3Transform((Vector3){p[0], p[1], p[2]}, w);
            float *d = m.positions + (v + k) * 3;
            d[0] = wp.x;
            d[1] = wp.y;
            d[2] = wp.z;
            if (m.normals)
            {
                Vector3 wn = {0.0f, 1.0f, 0.0f};
                if (src->normals)
                {
                    const float *q = src->normals + k * 3;
                    wn = Vector3Normalize((Vector3){nm.m0 * q[0] + nm.m4 * q[1] + nm.m8 * q[2],
                                                    nm.m1 * q[0] + nm.m5 * q[1] + nm.m9 * q[2],
                                                    nm.m2 * q[0] + nm.m6 * q[1] + nm.m10 * q[2]});
                }
                m.normals[(v + k) * 3 + 0] = wn.x;
                m.normals[(v + k) * 3 + 1] = wn.y;
                m.normals[(v + k) * 3 + 2] = wn.z;
            }
            if (m.colors)
            {
                if (src->colors)
                    memcpy(m.colors + (v + k) * 4, src->colors + k * 4, 4);
                else
                    memset(m.colors + (v + k) * 4, 255, 4);
            }
        }
        memcpy(m.texcoords + v * 2, src->texcoords, (size_t)src->vertexCount * 2 * sizeof(float));
        for (int k = 0; k < src->triangleCount * 3; k++)
            m.indices[t * 3 + k] = (unsigned short)(src->indices[k] + v);
        v += src->vertexCount;
        t += src->triangleCount;
    }
    MeshBounds(&m);
    return MeshAdopt(path, &m);
}

int DrawPathStaticBatch(DrawPath *path, const DrawItem *items, int count, DrawItem *out, int max)
{
    if (!path || !items || count <= 0 || !out || max <= 0)
        return 0;
    bool *used = calloc((size_t)count, sizeof *used);
    int *members = malloc((size_t)count * sizeof *members);
    int written = 0;
    for (int i = 0; used && members && i < count && written < max; i++)
    {
        if (used[i] || !ItemValid(path, &items[i]))
            continue;
        /* Take every later item of the same material and layer that still fits under the vertex
           limit; the ones that do not start the next merged mesh of the same group. */
        int n = 0, vertices = 0;
        for (int j = i; j < count; j++)
        {
            if (used[j] || !ItemValid(path, &items[j]) || items[j].material != items[i].material ||
                items[j].layer != items[i].layer)
                continue;
            int add = path->meshes[items[j].mesh - 1].vertexCount;
            if (vertices + add > MAX_VERTICES)
                continue;
            vertices += add;
            members[n++] = j;
            used[j] = true;
        }
        uint32_t mesh = MergeMeshes(path, items, members, n);
        if (!mesh)
            break;
        const DrawPathMeshData *merged = &path->meshes[mesh - 1];
        out[written++] = (DrawItem){mesh, items[i].material, MatrixIdentity(), merged->center,
                                    merged->radius, items[i].layer, NULL, 0};
    }
    free(used);
    free(members);
    return written;
}

// ---- the frame --------------------------------------------------------------------------------
/* Gribb and Hartmann: the planes are sums and differences of the view-projection's rows. raylib
   applies a Matrix to a column vector with rows (m0 m4 m8 m12), (m1 m5 m9 m13), ... */
static void FrustumPlanes(DrawPath *path)
{
    const Matrix *m = &path->viewProjection;
    float r0[4] = {m->m0, m->m4, m->m8, m->m12}, r1[4] = {m->m1, m->m5, m->m9, m->m13};
    float r2[4] = {m->m2, m->m6, m->m10, m->m14}, r3[4] = {m->m3, m->m7, m->m11, m->m15};
    for (int k = 0; k < 4; k++)
    {
        path->planes[0][k] = r3[k] + r0[k]; /* left */
        path->planes[1][k] = r3[k] - r0[k]; /* right */
        path->planes[2][k] = r3[k] + r1[k]; /* bottom */
        path->planes[3][k] = r3[k] - r1[k]; /* top */
        path->planes[4][k] = r3[k] + r2[k]; /* near */
        path->planes[5][k] = r3[k] - r2[k]; /* far */
    }
    for (int p = 0; p < 6; p++)
    {
        float *q = path->planes[p];
        float len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (len > 0.0f)
            for (int k = 0; k < 4; k++)
                q[k] /= len;
    }
}

void DrawPathBegin(DrawPath *path, Camera3D camera, int screenWidth, int screenHeight)
{
    if (!path)
        return;
    path->itemCount = 0;
    double aspect = screenHeight > 0 ? (double)screenWidth / (double)screenHeight : 1.0;
    double nearPlane = rlGetCullDistanceNear(), farPlane = rlGetCullDistanceFar();
    if (camera.projection == CAMERA_ORTHOGRAPHIC)
    {
        double top = camera.fovy / 2.0, right = top * aspect;
        path->projection = MatrixOrtho(-right, right, -top, top, nearPlane, farPlane);
    }
    else
    {
        double top = nearPlane * tan(camera.fovy * 0.5 * DEG2RAD), right = top * aspect;
        path->projection = MatrixFrustum(-right, right, -top, top, nearPlane, farPlane);
    }
    path->view = MatrixLookAt(camera.position, camera.target, camera.up);
    path->viewProjection = MatrixMultiply(path->view, path->projection);
    path->farPlane = (float)farPlane;
    FrustumPlanes(path);
}

/* Grows the item arrays and the sort arrays together, so DrawPathEnd never allocates. */
static bool ItemsReserve(DrawPath *path, int need)
{
    if (need <= path->itemCapacity)
        return true;
    int capacity = path->itemCapacity ? path->itemCapacity : 64;
    while (capacity < need)
        capacity *= 2;
    size_t n = (size_t)capacity;
    DrawItem *items = realloc(path->items, n * sizeof *items);
    if (items)
        path->items = items;
    uint64_t *itemKeys = realloc(path->itemKeys, n * sizeof *itemKeys);
    if (itemKeys)
        path->itemKeys = itemKeys;
    uint64_t *keys = realloc(path->keys, n * sizeof *keys);
    if (keys)
        path->keys = keys;
    uint64_t *keysScratch = realloc(path->keysScratch, n * sizeof *keysScratch);
    if (keysScratch)
        path->keysScratch = keysScratch;
    uint32_t *order = realloc(path->order, n * sizeof *order);
    if (order)
        path->order = order;
    uint32_t *orderScratch = realloc(path->orderScratch, n * sizeof *orderScratch);
    if (orderScratch)
        path->orderScratch = orderScratch;
    if (!items || !itemKeys || !keys || !keysScratch || !order || !orderScratch)
        return false; /* whatever did grow is kept; the capacity stays what all of them hold */
    path->itemCapacity = capacity;
    return true;
}

void DrawPathAdd(DrawPath *path, const DrawItem *item)
{
    if (!path || !item || !ItemValid(path, item) || !ItemsReserve(path, path->itemCount + 1))
        return;
    const DrawPathMaterialData *m = &path->materials[item->material - 1];
    path->items[path->itemCount] = *item;
    path->itemKeys[path->itemCount] = KeyBase(item->layer, m->shaderIndex, item->material, item->mesh);
    path->itemCount++;
}

/* Binds a mesh's buffers for a material's shader. With vertex array objects that is one bind; on
   GL 2.1 without them each buffer is bound and each attribute set per draw, as DrawMesh does. */
static void BindMesh(const DrawPathMeshData *mesh, const DrawPathMaterialData *m)
{
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float normalDefault[3] = {1.0f, 1.0f, 1.0f}; /* as UploadMesh sets it */
    if (mesh->vao && rlEnableVertexArray(mesh->vao))
    {
        /* A generic attribute's current value is undefined after a draw that sourced it from an
           array (GL 2.1, 2.8), and rlgl's batch does, so the default colour is set again here. */
        if (!mesh->vbo[3] && m->colorAttrib >= 0)
            rlSetVertexAttributeDefault(m->colorAttrib, white, RL_SHADER_ATTRIB_VEC4, 4);
        return;
    }
    if (m->positionAttrib >= 0)
    {
        rlEnableVertexBuffer(mesh->vbo[0]);
        rlSetVertexAttribute((unsigned int)m->positionAttrib, 3, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute((unsigned int)m->positionAttrib);
    }
    if (m->texcoordAttrib >= 0)
    {
        rlEnableVertexBuffer(mesh->vbo[1]);
        rlSetVertexAttribute((unsigned int)m->texcoordAttrib, 2, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute((unsigned int)m->texcoordAttrib);
    }
    if (m->normalAttrib >= 0)
    {
        if (mesh->vbo[2])
        {
            rlEnableVertexBuffer(mesh->vbo[2]);
            rlSetVertexAttribute((unsigned int)m->normalAttrib, 3, RL_FLOAT, 0, 0, 0);
            rlEnableVertexAttribute((unsigned int)m->normalAttrib);
        }
        else
        {
            rlSetVertexAttributeDefault(m->normalAttrib, normalDefault, RL_SHADER_ATTRIB_VEC3, 3);
            rlDisableVertexAttribute((unsigned int)m->normalAttrib);
        }
    }
    if (m->colorAttrib >= 0)
    {
        if (mesh->vbo[3])
        {
            rlEnableVertexBuffer(mesh->vbo[3]);
            rlSetVertexAttribute((unsigned int)m->colorAttrib, 4, RL_UNSIGNED_BYTE, 1, 0, 0);
            rlEnableVertexAttribute((unsigned int)m->colorAttrib);
        }
        else
        {
            rlSetVertexAttributeDefault(m->colorAttrib, white, RL_SHADER_ATTRIB_VEC4, 4);
            rlDisableVertexAttribute((unsigned int)m->colorAttrib);
        }
    }
    if (m->boneIdsAttrib >= 0 && m->boneWeightsAttrib >= 0 && mesh->vbo[5])
    {
        rlEnableVertexBuffer(mesh->vbo[5]);
        rlSetVertexAttribute((unsigned int)m->boneIdsAttrib, 4, RL_UNSIGNED_BYTE, 0, 0, 0);
        rlEnableVertexAttribute((unsigned int)m->boneIdsAttrib);
        rlEnableVertexBuffer(mesh->vbo[6]);
        rlSetVertexAttribute((unsigned int)m->boneWeightsAttrib, 4, RL_FLOAT, 0, 0, 0);
        rlEnableVertexAttribute((unsigned int)m->boneWeightsAttrib);
    }
    rlEnableVertexBufferElement(mesh->vbo[4]);
}

static void Submit(DrawPath *path, int visible, DrawStats *stats)
{
    uint32_t shader = UINT32_MAX, material = 0, mesh = 0;
    unsigned int texture = 0;
    bool viewmodel = false;
    rlDrawRenderBatchActive(); /* whatever was batched before this frame's items goes first */
    rlActiveTextureSlot(0);
    for (int i = 0; i < visible; i++)
    {
        const DrawItem *item = &path->items[path->order[i]];
        const DrawPathMaterialData *m = &path->materials[item->material - 1];
        const DrawPathMeshData *e = &path->meshes[item->mesh - 1];
        if (item->layer == DRAW_LAYER_VIEWMODEL && !viewmodel)
        {
            /* Phase 1's approximation of "depth cleared, own projection": the viewmodel is drawn
               with the scene's projection and no depth test. rlgl has no depth-only clear, and
               rlClearScreenBuffers would clear the colour too. */
            rlDisableDepthTest();
            viewmodel = true;
        }
        if (m->shaderIndex != shader) /* the key's shader bits are the material's shader index */
        {
            shader = m->shaderIndex;
            rlEnableShader(m->shaderId);
            if (m->viewLoc >= 0)
                rlSetUniformMatrix(m->viewLoc, path->view);
            if (m->projectionLoc >= 0)
                rlSetUniformMatrix(m->projectionLoc, path->projection);
            if (m->samplerLoc >= 0)
            {
                int unit = 0;
                rlSetUniform(m->samplerLoc, &unit, RL_SHADER_UNIFORM_SAMPLER2D, 1);
            }
            stats->shaderSwitches++;
            material = 0;
            mesh = 0; /* the fallback binds attributes at this shader's locations */
        }
        if (m->textureId != texture)
        {
            texture = m->textureId;
            rlEnableTexture(texture);
            stats->textureSwitches++;
        }
        if (item->material != material)
        {
            material = item->material;
            if (m->colorLoc >= 0)
                rlSetUniform(m->colorLoc, m->tint, RL_SHADER_UNIFORM_VEC4, 1);
        }
        if (item->mesh != mesh)
        {
            mesh = item->mesh;
            BindMesh(e, m);
        }
        rlSetUniformMatrix(m->mvpLoc, MatrixMultiply(item->world, path->viewProjection));
        if (m->modelLoc >= 0)
            rlSetUniformMatrix(m->modelLoc, item->world);
        if (m->normalLoc >= 0)
            rlSetUniformMatrix(m->normalLoc, MatrixTranspose(MatrixInvert(item->world)));
        if (m->bonesLoc >= 0 && item->bones && item->boneCount > 0) /* only skinned materials */
        {
            rlSetUniformMatrices(m->bonesLoc, item->bones, item->boneCount);
            stats->boneUploads++;
        }
        rlDrawVertexArrayElements(0, e->triangleCount * 3, 0);
        stats->draws++;
    }
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();
    rlDisableTexture();
    rlDisableShader();
    if (viewmodel)
        rlEnableDepthTest();
}

DrawStats DrawPathEnd(DrawPath *path)
{
    DrawStats stats;
    memset(&stats, 0, sizeof stats);
    if (!path)
        return stats;
    stats.items = path->itemCount;
    double t0 = NowMicros();
    float(*pl)[4] = path->planes;
    const Matrix *v = &path->view;
    float depthScale = path->farPlane > 0.0f ? 1.0f / path->farPlane : 0.0f;
    int visible = 0;
    for (int i = 0; i < path->itemCount; i++)
    {
        const DrawItem *item = &path->items[i];
        float x = item->center.x, y = item->center.y, z = item->center.z, r = -item->radius;
        bool inside = true;
        for (int p = 0; p < 6 && inside; p++)
            inside = pl[p][0] * x + pl[p][1] * y + pl[p][2] * z + pl[p][3] >= r;
        if (!inside)
            continue;
        float depth = -(v->m2 * x + v->m6 * y + v->m10 * z + v->m14); /* view looks down -z */
        path->keys[visible] = path->itemKeys[i] | KeyDepth(item->layer, Quantise(depth * depthScale));
        path->order[visible] = (uint32_t)i;
        visible++;
    }
    double t1 = NowMicros();
    RadixSort(path->keys, path->order, path->keysScratch, path->orderScratch, visible);
    double t2 = NowMicros();
    Submit(path, visible, &stats);
    double t3 = NowMicros();
    stats.visible = visible;
    stats.cullMicros = t1 - t0;
    stats.sortMicros = t2 - t1;
    stats.submitMicros = t3 - t2;
    return stats;
}
