/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_DRAW_PATH_H
#define CORE_DRAW_PATH_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

/* Draw items to draw calls (docs/developer/store.md section 4, the design's B9.2). A caller adds
   flat draw items -- mesh, material, world matrix, world bounding sphere, layer -- each frame;
   DrawPathEnd culls them against the camera's six frustum planes, gives each visible item a 64-bit
   sort key, sorts the keys with an 8-bit LSD radix sort and submits one draw call per item,
   changing the shader and texture only when they change. It never calls DrawMesh.

   Sort key, most significant bits first:
     layers 0, 1, 3:  layer(2) | shader(8) | material(12) | mesh(12) | depth(16, front to back)
     layer 2:         layer(2) | depth(16, back to front) | shader(8) | material(12) | mesh(12)
   The layer, shader, material and mesh part is built once when an item is added; only the depth
   bits are filled in by DrawPathEnd. Depth is the item's view-space distance divided by the far
   plane (rlGetCullDistanceFar, 1000 units by default), quantised to 16 bits.

   Mesh and material ids are 1-based indices into the path's own arrays: at most 4095 of each, and
   at most 256 distinct shaders, so every id fits its field of the key. Everything the path uploads
   it also frees in DrawPathFree. Uploading and submitting need a current GL context (after
   InitWindow); DrawPathInit, DrawPathKey and DrawPathSort do not. */

#define DRAW_PATH_MAX_MESHES 4095
#define DRAW_PATH_MAX_MATERIALS 4095
#define DRAW_PATH_MAX_SHADERS 256

/* Layers, drawn in this order. */
#define DRAW_LAYER_OPAQUE 0
#define DRAW_LAYER_ALPHA_TESTED 1
#define DRAW_LAYER_TRANSLUCENT 2
#define DRAW_LAYER_VIEWMODEL 3

typedef struct DrawItem
{
    uint32_t mesh;     /* DrawPathMesh id */
    uint32_t material; /* DrawPathMaterial id: shader, texture, tint */
    Matrix world;
    Vector3 center; /* world-space bounding sphere */
    float radius;
    uint8_t layer; /* 0 opaque, 1 alpha-tested, 2 translucent, 3 viewmodel */
} DrawItem;

typedef struct DrawStats
{
    int items, visible, draws, shaderSwitches, textureSwitches;
    double cullMicros, sortMicros, submitMicros;
} DrawStats;

/* One uploaded mesh. The path owns the CPU copy and the GL buffers; callers may read, not write. */
typedef struct DrawPathMeshData
{
    unsigned int vao;     /* 0 where vertex array objects are unsupported */
    unsigned int vbo[5];  /* positions, texcoords, normals, colours (0 when absent), indices */
    int vertexCount;
    int triangleCount;
    float *positions;      /* vertexCount * 3 */
    float *texcoords;      /* vertexCount * 2, zeros when the source mesh had none */
    float *normals;        /* vertexCount * 3, or NULL */
    unsigned char *colors; /* vertexCount * 4, or NULL */
    unsigned short *indices; /* triangleCount * 3; generated 0..n-1 for an unindexed mesh */
    Vector3 center;        /* bounding sphere of the positions, in mesh space */
    float radius;
} DrawPathMeshData;

/* One material: what a submit needs, with every uniform location looked up once. */
typedef struct DrawPathMaterialData
{
    unsigned int shaderId;
    uint32_t shaderIndex; /* 0..255: the shader's field in the sort key */
    unsigned int textureId; /* raylib's default white texture when the material has none */
    float tint[4];
    int mvpLoc, modelLoc, viewLoc, projectionLoc, normalLoc, colorLoc, samplerLoc;
    int positionAttrib, texcoordAttrib, normalAttrib, colorAttrib;
} DrawPathMaterialData;

/* A draw path, embedded by its caller. Zero it with DrawPathInit; release it with DrawPathFree. */
typedef struct DrawPath
{
    DrawPathMeshData *meshes;
    int meshCount, meshCapacity;
    DrawPathMaterialData *materials;
    int materialCount, materialCapacity;
    unsigned int shaders[DRAW_PATH_MAX_SHADERS]; /* GL program ids, indexed by shaderIndex */
    int shaderCount;
    DrawItem *items;        /* this frame's items, in the order added */
    uint64_t *itemKeys;     /* per item: the key without its depth bits */
    uint64_t *keys, *keysScratch;     /* visible keys and radix scratch */
    uint32_t *order, *orderScratch;   /* item index per visible key, and scratch */
    int itemCount, itemCapacity;
    Matrix view, projection, viewProjection;
    float planes[6][4]; /* inward normals: a point p is inside when n.p + d >= 0 */
    float farPlane;
} DrawPath;

/** @brief Starts an empty draw path. Allocates nothing and touches no GL state.
 * @param path Path to initialise; NULL answers false.
 * @return True when the path is ready. */
bool DrawPathInit(DrawPath *path);

/** @brief Frees every mesh buffer, CPU copy and array the path owns, and leaves it empty.
 *
 * Shaders and textures named by materials are borrowed and are not unloaded.
 * @param path Path to free; NULL is ignored. Needs the GL context the meshes were uploaded in.
 * @return Nothing. */
void DrawPathFree(DrawPath *path);

/** @brief Copies a mesh's CPU arrays and uploads the copy as vertex buffers, as UploadMesh does.
 *
 * Positions are required; texcoords, normals, colours and indices are optional (an unindexed mesh
 * gets indices 0..n-1). The source mesh is only read and may be unloaded afterwards. The path keeps
 * the vertex and triangle counts and a bounding sphere computed from the positions.
 * @param path Path that will own the upload.
 * @param mesh Mesh to copy; its GPU buffers are not used.
 * @return The mesh id (>= 1), or 0 when path or mesh is NULL, the mesh has no positions or more
 *         than 65536 vertices, an index is out of range, DRAW_PATH_MAX_MESHES is reached or memory
 *         runs out. */
uint32_t DrawPathMesh(DrawPath *path, const Mesh *mesh);

/** @brief Replaces an uploaded mesh's contents in place, keeping its id (for tilemap chunks).
 *
 * The new copy is built and uploaded before the old one is released, so on failure the mesh keeps
 * its previous contents.
 * @param path Path that owns the mesh.
 * @param id Id from DrawPathMesh.
 * @param mesh New contents, under the same rules as DrawPathMesh.
 * @return id on success, or 0 when the id is unknown or the new contents are refused. */
uint32_t DrawPathMeshUpdate(DrawPath *path, uint32_t id, const Mesh *mesh);

/** @brief Records a material: a shader, a texture, a tint and where the shader takes its matrices.
 *
 * The shader and texture are borrowed and must outlive the path. The MVP, model, view, projection,
 * colDiffuse and texture0 locations are taken from shader.locs when set there and otherwise looked
 * up once by raylib's names ("mvp", "matModel", "matView", "matProjection", "colDiffuse",
 * "texture0"); vertex attributes likewise ("vertexPosition", "vertexTexCoord", "vertexNormal",
 * "vertexColor"). A texture id of 0 draws with raylib's default white texture.
 * @param path Path that will hold the material.
 * @param shader Shader to draw with; id 0 is refused.
 * @param texture Diffuse texture, bound to slot 0.
 * @param tint Uploaded to colDiffuse when the shader has it.
 * @param normalMatrixLoc Location of the shader's normal matrix, or -1 for none; when >= 0 the
 *        transposed inverse of each item's world matrix is uploaded there.
 * @return The material id (>= 1), or 0 when path is NULL, the shader id is 0,
 *         DRAW_PATH_MAX_MATERIALS or DRAW_PATH_MAX_SHADERS is reached, or memory runs out. */
uint32_t DrawPathMaterial(DrawPath *path, Shader shader, Texture2D texture, Color tint,
                          int normalMatrixLoc);

/** @brief Merges items that share a material and layer into world-space meshes, one per material.
 *
 * Each item's mesh positions and normals are transformed by its world matrix and appended to the
 * group's mesh with its indices offset; a group that would pass 65536 vertices continues in a
 * further merged mesh. Each merged mesh is uploaded as a new path mesh, and out receives one item
 * per merged mesh with the identity matrix and the merged bounding sphere, for the caller to add in
 * place of the originals every frame. Items naming an unknown mesh or material are skipped.
 * @param path Path owning the meshes and materials the items name.
 * @param items Items to merge; only read.
 * @param count Number of items.
 * @param out Receives the merged items.
 * @param max Capacity of out; merging stops when it is full.
 * @return The number of merged items written to out, 0 when nothing could be merged (bad
 *         arguments, no valid items, or an upload or allocation failure on the first group). */
int DrawPathStaticBatch(DrawPath *path, const DrawItem *items, int count, DrawItem *out, int max);

/** @brief Starts a frame: forgets the last frame's items and takes the camera to cull and draw with.
 *
 * View and projection are built as BeginMode3D builds them (MatrixLookAt; a perspective or
 * orthographic frustum with rlGetCullDistanceNear/Far), with the aspect of the given size.
 * @param path Path to draw with; NULL is ignored.
 * @param camera Camera for this frame.
 * @param screenWidth Width of the target in pixels.
 * @param screenHeight Height of the target in pixels; 0 is treated as a square target.
 * @return Nothing. */
void DrawPathBegin(DrawPath *path, Camera3D camera, int screenWidth, int screenHeight);

/** @brief Adds one item to this frame, copying it and building the non-depth part of its key.
 *
 * An item naming an unknown mesh or material, or a layer above 3, is ignored, as is an item that
 * cannot be stored because memory ran out.
 * @param path Path between DrawPathBegin and DrawPathEnd.
 * @param item Item to copy.
 * @return Nothing. */
void DrawPathAdd(DrawPath *path, const DrawItem *item);

/** @brief Culls, keys, sorts and submits this frame's items.
 *
 * Draws to whatever framebuffer is bound, after flushing rlgl's batch; call it inside BeginMode3D
 * (or with depth testing enabled). It leaves no shader, texture or vertex array bound. Layer 3
 * (viewmodel) is drawn last with depth testing disabled, then re-enabled -- phase 1's
 * approximation of a cleared depth buffer and its own projection.
 * @param path Path with this frame's items; NULL answers empty stats.
 * @return Counts and times for this frame (CLOCK_MONOTONIC microseconds). */
DrawStats DrawPathEnd(DrawPath *path);

/** @brief Builds the full 64-bit sort key DrawPathEnd gives an item. Needs no GL.
 * @param item Item whose layer, material and mesh fill the key; only the low 12 bits of the ids
 *        and 2 bits of the layer are used.
 * @param shaderIndex The material's shader index (0..255; higher bits are dropped).
 * @param depth01 View-space distance over the far plane; clamped to [0, 1].
 * @return The key; smaller keys are drawn first. */
uint64_t DrawPathKey(const DrawItem *item, uint32_t shaderIndex, float depth01);

/** @brief Sorts keys ascending with the path's 8-bit LSD radix sort, carrying order along.
 *
 * Stable: equal keys keep their relative order. Byte positions every key shares are skipped.
 * Allocates scratch for the call; if that fails it falls back to an insertion sort.
 * @param keys Keys to sort in place.
 * @param order Values permuted with the keys (typically item indices).
 * @param count Number of entries; 0 or less does nothing.
 * @return Nothing. */
void DrawPathSort(uint64_t *keys, uint32_t *order, int count);

#endif
