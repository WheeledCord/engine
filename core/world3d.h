/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_WORLD3D_H
#define CORE_WORLD3D_H

#include "mesh_builder.h"
#include "raylib.h"
#include "store.h"
#include <stdbool.h>
#include <stdint.h>

/* The built-in 3D kinds of a store and the systems behind them: node transforms (a cached,
   interpolated world matrix per thing for drawing), character collide-and-slide against
   axis-aligned boxes, areas that send `touched`/`untouched`, solids, tilemaps turned into chunked
   CPU geometry, breadth-first paths over tilemap cells, and ray casts. The contract is
   docs/developer/store.md §3.

   Everything here is deterministic and headless: no window, no GL. Geometry is CPU vertex arrays
   (MB); the runner uploads them. Things are always visited in id order. A World3D belongs to one
   Store; once that store is freed, the only call left is World3DFree. */

#define WORLD3D_CHUNK_CELLS 8 /* a chunk is 8 x 8 tilemap cells */
#define WORLD3D_TILEMAP_MAX                                                                        \
    64 /* the `cells` grid is 64 x 64; width and depth say how much is used */

/* One 8 x 8 block of a tilemap's geometry, in the tilemap's local space (draw it with the
   tilemap's world matrix). Each MB holds triangles (count is a multiple of 3) as positions, normals
   and UVs in cell units; any of the three may be empty. */
typedef struct World3DChunk
{
    int chunkX, chunkZ; /* chunk coordinates: cells chunkX*8 .. chunkX*8+7 */
    MB floor, wall, ceiling;
    BoundingBox bounds; /* local space */
    uint64_t hash;      /* of the cells it was built from (its own 64 and their neighbours) */
    bool changed;       /* rebuilt since the last World3DTilemapChunks call: re-upload it */
} World3DChunk;

typedef struct World3DHit
{
    bool hit;
    float distance;
    Vector3 point, normal;
    StoreId thing; /* the solid, character or tilemap hit */
} World3DHit;

/* What a bone lookup answers for one model (World3DSetBoneLookup). */
typedef enum World3DBone
{
    WORLD3D_BONE_NOT_DRAWN, /* the model is not drawn on this machine: a socket tries its next `of` */
    WORLD3D_BONE_NONE,      /* drawn, but not posed (no animation) or no bone of that name */
    WORLD3D_BONE_POSED      /* drawn and posed: *out holds the bone's model-space matrix */
} World3DBone;

/* The runner's answer, for drawing only, to where a model's named bone is this frame. */
typedef World3DBone (*World3DBoneLookup)(void *user, StoreId model, const char *bone, Matrix *out);

/* Private to world3d.c; declared here only so a World3D can be embedded by value. */
typedef struct World3DEntry World3DEntry;
typedef struct World3DTilemapCache World3DTilemapCache;
typedef struct World3DPathCache World3DPathCache;

/* Caller-owned; prepare with World3DInit and release with World3DFree. Treat members as private. */
typedef struct World3D
{
    Store *store;
    StoreKind node, model, socket, camera, light, character, solid, area, tilemap, sound;
    int position, rotation, scale, prevPosition, prevRotation; /* node */
    int charRadius, charHeight, velocity, onFloor;             /* character */
    int size;                                                  /* solid */
    int areaRadius, areaShape, areaSize, inside;               /* area */
    int width, depth, cellSize, mapHeight, cells;              /* tilemap */
    int bone, of;                                              /* socket */
    StoreSymbol touched, untouched, box;
    World3DEntry *entries; /* per thing index */
    uint32_t entryCapacity;
    uint32_t frame;
    World3DTilemapCache *maps;
    int mapCount, mapCapacity;
    World3DPathCache *paths;
    int pathCount, pathCapacity;
    uint64_t pathTick;
    StoreHooks chained; /* the caller's hooks, run after world3d's own */
    World3DBoneLookup boneLookup; /* for drawing only; NULL until World3DSetBoneLookup */
    void *boneUser;
} World3D;

/** @brief Declares the built-in 3D kinds in a store and registers the area system.
 *
 * Declares `node`, `model`, `socket`, `camera`, `light`, `character`, `solid`, `area`, `tilemap`
 * and `sound` with the fields of docs/developer/store.md §3, adds one system (areas, run at step 6
 * of every StoreTick) and installs world3d's store hooks (spawned, removed, orphan), replacing
 * any the store had, with no caller hooks chained. A caller that has hooks of its own passes them
 * through World3DHooks and installs the result with StoreSetHooks.
 * @param world Caller-owned; its previous contents are not released. The store keeps its address
 * for the system and hooks, so it must not move until World3DFree.
 * @param store Initialised store with none of those kind names declared yet; it must stay at the
 * same address while world is in use.
 * @return True when declared; false when a name is taken or out of memory, with world safe to
 * World3DFree (kinds declared before the failure stay declared). */
bool World3DInit(World3D *world, Store *store);

/** @brief Releases the caches world3d keeps (transforms, chunk geometry, paths) and zeroes it.
 *
 * The store's hooks go back to the caller's own (those passed through World3DHooks, or none). The
 * kinds stay declared, and the area system stays registered with world's address and does nothing
 * from then on, so do not tick the store once world's memory is gone (free both together).
 * @param world World; NULL is accepted. Its store may already have been freed with StoreFree.
 * @return No value. Chunk arrays handed out by World3DTilemapChunks become invalid. */
void World3DFree(World3D *world);

/** @brief Chains a caller's store hooks behind world3d's own.
 *
 * Copies the caller's `user`, `orphan`, `spawned` and `removed` into world, then points those
 * members of hooks at world3d's functions (with user = world), which do world3d's work and then
 * call the caller's; the caller's `error` is relayed the same way. Pass the result to
 * StoreSetHooks.
 *
 * On `orphan`, the guest's position, rotation and scale are rewritten as its world transform so it
 * stays put once it is a root. The store refuses reads of a thing already marked removed, so a
 * removed ancestor counts where world3d last read it: at the latest World3DBeginTick, or later if a
 * world query walked it since. A parent moved and removed in the same tick drops its guests where
 * it was when the tick began.
 * @param world Initialised world.
 * @param hooks In: the caller's hooks (any member may be NULL). Out: the hooks to install.
 * @return No value. */
void World3DHooks(World3D *world, StoreHooks *hooks);

/** @brief Answers a thing's world matrix as the last World3DUpdateTransforms drew it.
 *
 * This is the frame's interpolated, cached matrix (B9.1), for drawing. A node no pass has reached
 * yet is composed on demand from its current fields (alpha 1). Gameplay code wants
 * World3DWorldPosition, which reads the tick's state.
 * @param world World.
 * @param id A node or derived thing.
 * @param out Receives the matrix.
 * @return True when answered; false for a stale id or a thing that is not a node. */
bool World3DWorldMatrix(World3D *world, StoreId id, Matrix *out);

/** @brief Answers a thing's world matrix for drawing: World3DWorldMatrix, except under a socket
 * that follows a bone.
 *
 * A socket (and so everything under it) follows the bone named by its `bone` field in the model it
 * follows: the first model named in its `of` list (children of the kind that declared the socket)
 * that the bone lookup says is drawn on this machine, or with no `of` its parent if that is a
 * model. When that model is posed, the socket's matrix is the bone's model-space matrix times the
 * model's own drawn matrix (itself found this way, so a socket in a carried thing follows too), and
 * the things under it keep their local transforms relative to it. With no lookup, no bone, nothing
 * drawn to follow or no pose, this is exactly World3DWorldMatrix.
 *
 * Only drawing sees bones (rule 1): World3DWorldMatrix and World3DWorldPosition, which gameplay
 * reads, keep every socket at its own local transform, the rest pose.
 * @param world World.
 * @param id A node or derived thing.
 * @param out Receives the matrix.
 * @return True when answered; false for a stale id or a thing that is not a node. */
bool World3DDrawMatrix(World3D *world, StoreId id, Matrix *out);

/** @brief Installs the runner's bone lookup, which World3DDrawMatrix asks about sockets' models.
 *
 * The lookup answers, for a model thing and a bone name, whether the model is drawn on this machine
 * and, when it is posed and has that bone, the bone's current model-space matrix (the pose, not the
 * skinning matrix). It is asked only by World3DDrawMatrix, never by anything gameplay reads.
 * @param world World.
 * @param lookup The runner's function, or NULL to follow no bones.
 * @param user Passed to lookup.
 * @return No value. */
void World3DSetBoneLookup(World3D *world, World3DBoneLookup lookup, void *user);

/** @brief Answers where a thing is in the world now, from its current fields and its ancestors'.
 *
 * Independent of frames and interpolation, so gameplay and headless runs agree.
 * @param world World.
 * @param id A node or derived thing.
 * @param out Receives the world position.
 * @return True when answered; false for a stale id or a thing that is not a node. */
bool World3DWorldPosition(World3D *world, StoreId id, Vector3 *out);

/** @brief Recomposes world matrices for the frame, parents before children.
 *
 * Per node: position is lerped and rotation slerped from `%prev-*` to the current values by alpha,
 * except after a teleport, a spawn or a parent change since the last World3DBeginTick (then the
 * current value is used). A node whose inputs, parent and parent's matrix are all unchanged keeps
 * last frame's matrix untouched.
 * @param world World.
 * @param alpha Fraction of a tick since the last one, 0 to 1 (clamped).
 * @return No value. */
void World3DUpdateTransforms(World3D *world, float alpha);

/** @brief Starts a tick: copies every node's position and rotation into `%prev-position` and
 * `%prev-rotation` and ends the jumps of the last tick. Call it just before StoreTick.
 * @param world World.
 * @return No value. */
void World3DBeginTick(World3D *world);

/** @brief Moves a character by velocity * dt, sliding along boxes it meets.
 *
 * The collision world is axis-aligned boxes: each tilemap's solid cells (`cell-size` square,
 * 0 to `height` tall), a floor slab under its used footprint (width x depth cells, y -1 to 0) and a
 * ceiling slab over it (`height` to `height` + 1), all in the tilemap's space, which must be
 * unrotated and unscaled; each solid's `size` centred on its world position; and each character's
 * box. Three axis-separated sweeps, X then Z then Y, of the character's box (2 radius wide and
 * deep, `height` tall, feet at position) against every one of those boxes but its own.
 * Each blocked axis stops at the contact face and has its velocity component zeroed; `on-floor`
 * becomes whether Y was blocked from below. Position, velocity and on-floor are written with
 * StoreSetEngine. Phase 1: position is taken as a world position (a root with no rotation).
 * @param world World.
 * @param character A character or derived thing.
 * @param dt Seconds to move for.
 * @return True when moved; false for a stale id or a thing that is not a character. */
bool World3DMoveAndSlide(World3D *world, StoreId character, float dt);

/** @brief Puts a node at a world position with no interpolation from where it was.
 *
 * Writes position (converted into its parent's space) and `%prev-position`, and marks the node
 * as jumping until the next World3DBeginTick.
 * @param world World.
 * @param node A node or derived thing.
 * @param worldPosition Where to put it.
 * @return True when moved; false for a stale id, a non-node or a parent with no inverse. */
bool World3DTeleport(World3D *world, StoreId node, Vector3 worldPosition);

/** @brief Casts a ray against tilemap cells (DDA) and tilemap floor and ceiling slabs, solids and
 * characters (the boxes of World3DMoveAndSlide); the nearest hit wins.
 *
 * Boxes the ray starts inside are not hit. Equal distances go to the lower id.
 * @param world World.
 * @param from Origin.
 * @param direction Direction; normalised here. Zero hits nothing.
 * @param maxDistance Longest distance to look.
 * @param ignore A thing skipped with everything under it, or STORE_NULL.
 * @return The hit; hit is false for none, a zero direction or a stale ignore. */
World3DHit World3DRaycast(World3D *world, Vector3 from, Vector3 direction, float maxDistance,
                          StoreId ignore);

/** @brief World3DRaycast with several things to skip, and optionally areas as targets.
 *
 * Areas are walked through, so World3DRaycast never hits one; with areas set, an area's shape
 * (a sphere of its radius, or with shape box an axis-aligned box of its size, about its world
 * position) is hit like a solid, which is what aimed-at needs to find a thing that is only an
 * area (a carried item, proposal C4). A shape the ray starts inside is not hit.
 * @param world World.
 * @param from Origin.
 * @param direction Direction; normalised here. Zero hits nothing.
 * @param maxDistance Longest distance to look.
 * @param ignore Things skipped with everything under them (STORE_NULL entries are no-ops).
 * @param count How many in ignore.
 * @param areas Whether areas are hit.
 * @return The hit; hit is false for none, a zero direction or a stale ignore. */
World3DHit World3DRaycastIgnoring(World3D *world, Vector3 from, Vector3 direction, float maxDistance,
                                  const StoreId *ignore, int count, bool areas);

/** @brief Says whether the segment between two points misses static geometry: tilemap solid
 * cells, floors and ceilings, and solids (characters are not tested).
 * @param world World.
 * @param from One end.
 * @param to Other end.
 * @return True when nothing static lies between them. */
bool World3DLineOfSight(World3D *world, Vector3 from, Vector3 to);

/** @brief Lists things of a kind whose shape meets an area's shape (sphere or box), in id order.
 *
 * Characters and solids are tested as boxes, other nodes as their world position.
 * @param world World.
 * @param area An area or derived thing.
 * @param kind Kind to look for (derived kinds included); an unknown kind matches nothing.
 * @param out Receives up to max ids; may be NULL to count.
 * @param max Capacity of out.
 * @return The number written (or matching, with out NULL); -1 for a stale id or a non-area. */
int World3DOverlapping(World3D *world, StoreId area, StoreKind kind, StoreId *out, int max);

/** @brief Finds the thing of a kind nearest a point, by world position.
 * @param world World.
 * @param kind Kind to look for (derived kinds included); non-nodes are skipped, and an unknown
 * kind matches nothing.
 * @param point Where to measure from.
 * @param maxDistance Farthest to accept; negative for no limit.
 * @param accept Called with each candidate in id order and user; false skips it. May be NULL.
 * @param user Passed to accept.
 * @return The nearest accepted thing, the lower id on a tie, or STORE_NULL. */
StoreId World3DNearest(World3D *world, StoreKind kind, Vector3 point, float maxDistance,
                       bool (*accept)(StoreId, void *), void *user);

/** @brief Answers the world position of a tilemap cell's centre at the tilemap's floor (y 0).
 * @param world World.
 * @param tilemap A tilemap or derived thing.
 * @param x Column, 0 to width - 1.
 * @param z Row, 0 to depth - 1.
 * @param out Receives the position.
 * @return True when answered; false for a stale id, a non-tilemap or a cell outside the map. */
bool World3DCellToWorld(World3D *world, StoreId tilemap, int x, int z, Vector3 *out);

/** @brief Answers which tilemap cell a world position lies over.
 * @param world World.
 * @param tilemap A tilemap or derived thing.
 * @param worldPosition Position.
 * @param x Receives the column.
 * @param z Receives the row.
 * @return True when inside the used area; false for a stale id, a non-tilemap, a rotated or
 * scaled tilemap, or a position outside it. */
bool World3DWorldToCell(World3D *world, StoreId tilemap, Vector3 worldPosition, int *x, int *z);

/** @brief Answers where to walk next on the way from one point to another over a tilemap.
 *
 * Breadth-first over open cells from to's cell, 4-connected; the distance field is cached per
 * tilemap and target cell for the current tick (a cell changed after a query is seen from the next
 * tick). The answer is the centre of the next cell toward to from from's cell, or to itself when
 * the two cells are the same or adjacent, or no path joins them.
 * @param world World.
 * @param tilemap A tilemap or derived thing.
 * @param from Where the walker is.
 * @param to Where it is going.
 * @param out Receives the point to head for.
 * @return True when answered; false for a stale id, a non-tilemap or out of memory. */
bool World3DPathNext(World3D *world, StoreId tilemap, Vector3 from, Vector3 to, Vector3 *out);

/** @brief Answers a tilemap's geometry in 8 x 8 chunks, rebuilding those whose cells changed.
 *
 * Floors are quads at y 0 on open cells, ceilings at `height` facing down, walls on each face of
 * a solid cell next to an open one; UVs in cell units. Only chunks of the used area are answered,
 * in row-major chunk order. A chunk rebuilt since it was last handed out has changed set; handing
 * it out clears it.
 * @param world World.
 * @param tilemap A tilemap or derived thing.
 * @param out Receives up to max chunks; their MB arrays are borrowed from world, valid until the
 * next call for this tilemap, its removal or World3DFree. May be NULL to count.
 * @param max Capacity of out.
 * @return The number written (or the number of chunks, with out NULL); -1 for a stale id, a
 * non-tilemap or out of memory. */
int World3DTilemapChunks(World3D *world, StoreId tilemap, World3DChunk *out, int max);

#endif
