/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "world3d.h"
#include "raymath.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The built-in 3D kinds (docs/developer/store.md §3). Per-thing state world3d keeps beside the
   store is indexed by thing index and checked by generation, so a reused slot starts fresh. */

#define CONTACT_EPSILON 1e-4f
#define MAX_DEPTH 256 /* ancestors walked at most; the store refuses cycles, this only bounds */

struct World3DEntry
{
    uint32_t generation;
    bool live, jump, warned;
    /* The tick's view, refreshed whenever the fields are read: a removed ancestor's last values. */
    Vector3 tickPosition, tickRotation, tickScale;
    StoreId tickParent;
    /* The frame's view: what the matrix was composed from, to detect change. */
    bool computed, interpolated;
    Vector3 position, rotation, scale;
    StoreId parent;
    Matrix world;
    uint32_t stamp, changed;
};

struct World3DTilemapCache
{
    StoreId map;
    int chunksX, chunksZ;
    World3DChunk *chunks;
    bool *built;
};

struct World3DPathCache
{
    StoreId map;
    int targetX, targetZ, width, depth;
    int16_t distance[WORLD3D_TILEMAP_MAX * WORLD3D_TILEMAP_MAX];
};

typedef struct Box
{
    Vector3 min, max;
} Box;

/* ---- small helpers --------------------------------------------------------------------------- */

static bool Same(StoreId a, StoreId b)
{
    return a.index == b.index && a.generation == b.generation;
}

static bool IsNull(StoreId id) { return id.index == UINT32_MAX; }

static bool VecSame(Vector3 a, Vector3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

static float Axis(Vector3 v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

static void SetAxis(Vector3 *v, int axis, float value)
{
    if (axis == 0)
        v->x = value;
    else if (axis == 1)
        v->y = value;
    else
        v->z = value;
}

static StoreValue VecValue(Vector3 v)
{
    StoreValue value;
    memset(&value, 0, sizeof value);
    value.type = STORE_VEC3;
    value.as.v = v;
    return value;
}

static StoreValue BoolValue(bool b)
{
    StoreValue value;
    memset(&value, 0, sizeof value);
    value.type = STORE_BOOL;
    value.as.b = b;
    return value;
}

static Vector3 GetVec(const World3D *w, StoreId id, int field)
{
    StoreValue v;
    return StoreGet(w->store, id, field, &v) && v.type == STORE_VEC3 ? v.as.v : (Vector3){0, 0, 0};
}

static float GetFloat(const World3D *w, StoreId id, int field)
{
    StoreValue v;
    if (!StoreGet(w->store, id, field, &v))
        return 0;
    return v.type == STORE_FLOAT ? v.as.f : v.type == STORE_INT ? (float)v.as.i : 0;
}

static int GetInt(const World3D *w, StoreId id, int field)
{
    StoreValue v;
    return StoreGet(w->store, id, field, &v) && v.type == STORE_INT ? v.as.i : 0;
}

static bool Is(const World3D *w, StoreId id, StoreKind kind)
{
    return StoreKindIs(w->store, StoreKindOf(w->store, id), kind);
}

/* Things of a kind in id order, in a fresh array the caller frees (so callers may nest). */
static StoreId *List(const World3D *w, StoreKind kind, int *count)
{
    int n = StoreThings(w->store, kind, NULL, 0);
    StoreId *ids = malloc(sizeof *ids * (size_t)(n > 0 ? n : 1));
    *count = ids ? StoreThings(w->store, kind, ids, n) : 0;
    return ids;
}

static Matrix Local(Vector3 position, Quaternion rotation, Vector3 scale)
{
    return MatrixMultiply(
        MatrixMultiply(MatrixScale(scale.x, scale.y, scale.z), QuaternionToMatrix(rotation)),
        MatrixTranslate(position.x, position.y, position.z));
}

static Quaternion Euler(Vector3 r) { return QuaternionFromEuler(r.x, r.y, r.z); }

/* ---- per-thing entries ----------------------------------------------------------------------- */

static bool Reserve(World3D *w, uint32_t index)
{
    if (index < w->entryCapacity)
        return true;
    uint32_t capacity = w->entryCapacity ? w->entryCapacity : 64;
    while (capacity <= index)
        capacity *= 2;
    World3DEntry *entries = realloc(w->entries, sizeof *entries * capacity);
    if (!entries)
        return false;
    memset(entries + w->entryCapacity, 0, sizeof *entries * (capacity - w->entryCapacity));
    w->entries = entries;
    w->entryCapacity = capacity;
    return true;
}

/* The entry of a live thing or of one removed this tick whose entry still stands, or NULL. */
static World3DEntry *Find(World3D *w, StoreId id)
{
    if (IsNull(id) || id.index >= w->entryCapacity)
        return NULL;
    World3DEntry *e = &w->entries[id.index];
    return e->live && e->generation == id.generation ? e : NULL;
}

/* The entry of a live node, made fresh (and jumping) for a new thing in the slot. */
static World3DEntry *Entry(World3D *w, StoreId id)
{
    World3DEntry *e = Find(w, id);
    if (e)
        return e;
    if (IsNull(id) || !Reserve(w, id.index))
        return NULL;
    e = &w->entries[id.index];
    memset(e, 0, sizeof *e);
    e->generation = id.generation;
    e->live = true;
    e->jump = true;
    e->tickScale = (Vector3){1, 1, 1};
    e->tickParent = STORE_NULL;
    e->parent = STORE_NULL;
    return e;
}

/* The parent whose transform a node's is relative to: its parent when that is a node (or a node
   removed this tick, whose entry still stands, while its guests are orphaned), else none. */
static StoreId TransformParent(World3D *w, StoreId id)
{
    StoreId parent = StoreParent(w->store, id);
    if (Is(w, parent, w->node) || (!StoreAlive(w->store, parent) && Find(w, parent)))
        return parent;
    return STORE_NULL;
}

/* A node's local fields now; a removed one's as last seen. False when neither is known. */
static bool TickLocal(World3D *w, StoreId id, Vector3 *p, Vector3 *r, Vector3 *s, StoreId *parent)
{
    if (StoreAlive(w->store, id))
    {
        if (!Is(w, id, w->node))
            return false;
        *p = GetVec(w, id, w->position);
        *r = GetVec(w, id, w->rotation);
        *s = GetVec(w, id, w->scale);
        *parent = TransformParent(w, id);
        World3DEntry *e = Entry(w, id);
        if (e)
        {
            e->tickPosition = *p;
            e->tickRotation = *r;
            e->tickScale = *s;
            e->tickParent = *parent;
        }
        return true;
    }
    World3DEntry *e = Find(w, id);
    if (!e)
        return false;
    *p = e->tickPosition;
    *r = e->tickRotation;
    *s = e->tickScale;
    *parent = e->tickParent;
    return true;
}

/* The world matrix of the tick's state: current fields, no interpolation, nothing cached. */
static Matrix TickWorld(World3D *w, StoreId id)
{
    Matrix m = MatrixIdentity();
    Vector3 p, r, s;
    StoreId parent;
    for (int depth = 0; !IsNull(id) && depth < MAX_DEPTH; depth++, id = parent)
    {
        if (!TickLocal(w, id, &p, &r, &s, &parent))
            break;
        m = MatrixMultiply(m, Local(p, Euler(r), s));
    }
    return m;
}

static Vector3 Translation(Matrix m) { return (Vector3){m.m12, m.m13, m.m14}; }

/* A tilemap's world origin, refusing (with one warning) a rotated or scaled one. */
static bool MapOrigin(World3D *w, StoreId map, Vector3 *origin)
{
    Matrix m = TickWorld(w, map);
    *origin = Translation(m);
    const float e = 1e-5f;
    bool plain = fabsf(m.m0 - 1) < e && fabsf(m.m5 - 1) < e && fabsf(m.m10 - 1) < e &&
                 fabsf(m.m1) < e && fabsf(m.m2) < e && fabsf(m.m4) < e && fabsf(m.m6) < e &&
                 fabsf(m.m8) < e && fabsf(m.m9) < e;
    if (plain)
        return true;
    World3DEntry *entry = Find(w, map);
    if (entry && !entry->warned)
    {
        entry->warned = true;
        TraceLog(LOG_WARNING, "WORLD3D: tilemap #%u is rotated or scaled; tilemaps must be neither",
                 (unsigned)map.index);
    }
    return false;
}

typedef struct MapInfo
{
    int width, depth;
    float cellSize, height;
    Vector3 origin;
} MapInfo;

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool ReadMap(World3D *w, StoreId map, MapInfo *info)
{
    info->width = ClampInt(GetInt(w, map, w->width), 0, WORLD3D_TILEMAP_MAX);
    info->depth = ClampInt(GetInt(w, map, w->depth), 0, WORLD3D_TILEMAP_MAX);
    info->cellSize = GetFloat(w, map, w->cellSize);
    info->height = GetFloat(w, map, w->mapHeight);
    return info->cellSize > 0 && MapOrigin(w, map, &info->origin);
}

/* The slabs under and over a tilemap's used footprint, 1 m thick: floor, then ceiling. */
static void MapSlabs(const MapInfo *m, Box slabs[2])
{
    Vector3 o = m->origin;
    float x = o.x + m->width * m->cellSize, z = o.z + m->depth * m->cellSize;
    slabs[0] = (Box){{o.x, o.y - 1, o.z}, {x, o.y, z}};
    slabs[1] = (Box){{o.x, o.y + m->height, o.z}, {x, o.y + m->height + 1, z}};
}

static bool CellSolid(World3D *w, StoreId map, const MapInfo *info, int x, int z)
{
    StoreValue v;
    return x >= 0 && z >= 0 && x < info->width && z < info->depth &&
           StoreGridGet(w->store, map, w->cells, x, z, &v) && v.as.i != 0;
}

static bool CellOpen(World3D *w, StoreId map, const MapInfo *info, int x, int z)
{
    StoreValue v;
    return x >= 0 && z >= 0 && x < info->width && z < info->depth &&
           StoreGridGet(w->store, map, w->cells, x, z, &v) && v.as.i == 0;
}

/* ---- kinds ----------------------------------------------------------------------------------- */

static StoreFieldDecl Field(const char *name, StoreType type, unsigned flags, StoreValue init)
{
    StoreFieldDecl d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.flags = flags;
    d.init = init;
    return d;
}

static StoreFieldDecl Collection(const char *name, StoreType type, StoreType element, int max,
                                 int height, unsigned flags)
{
    StoreFieldDecl d = Field(name, type, flags, (StoreValue){STORE_NONE, {0}});
    d.element = element;
    d.max = max;
    d.height = height;
    return d;
}

static StoreValue None(void) { return (StoreValue){STORE_NONE, {0}}; }

static StoreValue FloatValue(float f)
{
    StoreValue v = None();
    v.type = STORE_FLOAT;
    v.as.f = f;
    return v;
}

static StoreValue IntValue(int i)
{
    StoreValue v = None();
    v.type = STORE_INT;
    v.as.i = i;
    return v;
}

static void AreaSystem(Store *store, float dt, void *user);
static void HookSpawned(void *user, StoreId id);
static void HookRemoved(void *user, StoreId id);
static void HookOrphan(void *user, StoreId id);
static void HookError(void *user, const char *message);

static StoreKind Declare(World3D *w, const char *name, StoreKind base, const StoreFieldDecl *fields,
                         int count)
{
    const char *error = NULL;
    StoreKind kind = StoreDeclareKind(w->store, name, base, fields, count, &error);
    if (kind < 0)
        TraceLog(LOG_WARNING, "WORLD3D: can't declare %s: %s", name, error ? error : "?");
    return kind;
}

bool World3DInit(World3D *w, Store *store)
{
    memset(w, 0, sizeof *w);
    w->store = store;
    const char *names[] = {"node",      "model", "socket", "camera",  "light",
                           "character", "solid", "area",   "tilemap", "sound"};
    for (int i = 0; i < 10; i++)
        if (StoreKindNamed(store, names[i]) >= 0)
        {
            TraceLog(LOG_WARNING, "WORLD3D: the store already has a kind named %s", names[i]);
            return false;
        }
    StoreValue one = VecValue((Vector3){1, 1, 1}), yes = BoolValue(true),
               zero = VecValue((Vector3){0, 0, 0});
    unsigned prev = STORE_LOCAL | STORE_HIDDEN | STORE_ENGINE;
    StoreFieldDecl node[] = {Field("position", STORE_VEC3, 0, zero),
                             Field("rotation", STORE_VEC3, 0, zero),
                             Field("scale", STORE_VEC3, 0, one),
                             Field("visible", STORE_BOOL, 0, yes),
                             Field("static", STORE_BOOL, 0, None()),
                             Field("cull-distance", STORE_FLOAT, 0, FloatValue(0)),
                             Field("%prev-position", STORE_VEC3, prev, zero),
                             Field("%prev-rotation", STORE_VEC3, prev, zero)};
    StoreFieldDecl model[] = {
        Field("mesh", STORE_STRING, 0, None()),    Field("animation", STORE_SYMBOL, 0, None()),
        Field("spin", STORE_FLOAT, 0, None()),     Field("tint", STORE_VEC3, 0, one),
        Field("for-owner", STORE_BOOL, 0, None()), Field("hidden-for-owner", STORE_BOOL, 0, None()),
        Field("viewmodel", STORE_BOOL, 0, None())};
    StoreFieldDecl socket[] = {Field("bone", STORE_STRING, 0, None()),
                               Collection("of", STORE_LIST, STORE_SYMBOL, 4, 0, 0)};
    StoreFieldDecl camera[] = {Field("fov", STORE_FLOAT, 0, FloatValue(75)),
                               Field("for-owner", STORE_BOOL, 0, None())};
    StoreFieldDecl light[] = {
        Field("type", STORE_SYMBOL, 0, None()), Field("energy", STORE_FLOAT, 0, FloatValue(1)),
        Field("color", STORE_VEC3, 0, one), Field("range", STORE_FLOAT, 0, FloatValue(10))};
    StoreFieldDecl character[] = {Field("radius", STORE_FLOAT, 0, FloatValue(0.4f)),
                                  Field("height", STORE_FLOAT, 0, FloatValue(1.8f)),
                                  Field("velocity", STORE_VEC3, 0, zero),
                                  Field("on-floor", STORE_BOOL, STORE_ENGINE, None())};
    StoreFieldDecl solid[] = {Field("size", STORE_VEC3, 0, one)};
    StoreValue sphere = None();
    sphere.type = STORE_SYMBOL;
    sphere.as.sym = StoreIntern(store, "sphere");
    StoreFieldDecl area[] = {
        Field("radius", STORE_FLOAT, 0, FloatValue(1)), Field("shape", STORE_SYMBOL, 0, sphere),
        Field("size", STORE_VEC3, 0, one),
        Collection("%inside", STORE_LIST, STORE_REF, 16, 0, STORE_HIDDEN | STORE_ENGINE)};
    StoreFieldDecl tilemap[] = {
        Field("width", STORE_INT, 0, IntValue(16)),
        Field("depth", STORE_INT, 0, IntValue(16)),
        Field("cell-size", STORE_FLOAT, 0, FloatValue(2)),
        Field("height", STORE_FLOAT, 0, FloatValue(3)),
        Collection("cells", STORE_GRID, STORE_INT, WORLD3D_TILEMAP_MAX, WORLD3D_TILEMAP_MAX, 0),
        Field("floor-texture", STORE_STRING, 0, None()),
        Field("wall-texture", STORE_STRING, 0, None()),
        Field("ceiling-texture", STORE_STRING, 0, None())};
    StoreFieldDecl sound[] = {Field("stream", STORE_STRING, 0, None()),
                              Field("volume", STORE_FLOAT, 0, FloatValue(1)),
                              Field("playing", STORE_BOOL, 0, None())};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
    w->node = Declare(w, "node", -1, node, COUNT(node));
    if (w->node < 0)
        return false;
    w->model = Declare(w, "model", w->node, model, COUNT(model));
    w->socket = Declare(w, "socket", w->node, socket, COUNT(socket));
    w->camera = Declare(w, "camera", w->node, camera, COUNT(camera));
    w->light = Declare(w, "light", w->node, light, COUNT(light));
    w->character = Declare(w, "character", w->node, character, COUNT(character));
    w->solid = Declare(w, "solid", w->node, solid, COUNT(solid));
    w->area = Declare(w, "area", w->node, area, COUNT(area));
    w->tilemap = Declare(w, "tilemap", w->node, tilemap, COUNT(tilemap));
    w->sound = Declare(w, "sound", w->node, sound, COUNT(sound));
#undef COUNT
    if (w->model < 0 || w->socket < 0 || w->camera < 0 || w->light < 0 || w->character < 0 ||
        w->solid < 0 || w->area < 0 || w->tilemap < 0 || w->sound < 0)
        return false;
    w->position = StoreFieldIndex(store, w->node, "position");
    w->rotation = StoreFieldIndex(store, w->node, "rotation");
    w->scale = StoreFieldIndex(store, w->node, "scale");
    w->prevPosition = StoreFieldIndex(store, w->node, "%prev-position");
    w->prevRotation = StoreFieldIndex(store, w->node, "%prev-rotation");
    w->charRadius = StoreFieldIndex(store, w->character, "radius");
    w->charHeight = StoreFieldIndex(store, w->character, "height");
    w->velocity = StoreFieldIndex(store, w->character, "velocity");
    w->onFloor = StoreFieldIndex(store, w->character, "on-floor");
    w->size = StoreFieldIndex(store, w->solid, "size");
    w->areaRadius = StoreFieldIndex(store, w->area, "radius");
    w->areaShape = StoreFieldIndex(store, w->area, "shape");
    w->areaSize = StoreFieldIndex(store, w->area, "size");
    w->box = StoreIntern(store, "box");
    w->inside = StoreFieldIndex(store, w->area, "%inside");
    w->width = StoreFieldIndex(store, w->tilemap, "width");
    w->depth = StoreFieldIndex(store, w->tilemap, "depth");
    w->cellSize = StoreFieldIndex(store, w->tilemap, "cell-size");
    w->mapHeight = StoreFieldIndex(store, w->tilemap, "height");
    w->cells = StoreFieldIndex(store, w->tilemap, "cells");
    w->touched = StoreIntern(store, "touched");
    w->untouched = StoreIntern(store, "untouched");
    if (!StoreAddSystem(store, AreaSystem, w))
        return false;
    StoreHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    World3DHooks(w, &hooks);
    StoreSetHooks(store, &hooks);
    return true;
}

static void FreeChunks(World3DTilemapCache *c)
{
    for (int i = 0; c->chunks && i < c->chunksX * c->chunksZ; i++)
    {
        MBFree(&c->chunks[i].floor);
        MBFree(&c->chunks[i].wall);
        MBFree(&c->chunks[i].ceiling);
    }
    free(c->chunks);
    free(c->built);
    c->chunks = NULL;
    c->built = NULL;
    c->chunksX = c->chunksZ = 0;
}

void World3DFree(World3D *w)
{
    if (!w)
        return;
    if (w->store)
        StoreSetHooks(w->store, &w->chained); /* the caller's own hooks, without world3d's */
    for (int i = 0; i < w->mapCount; i++)
        FreeChunks(&w->maps[i]);
    free(w->maps);
    free(w->paths);
    free(w->entries);
    memset(w, 0, sizeof *w);
}

/* ---- hooks ----------------------------------------------------------------------------------- */

void World3DHooks(World3D *w, StoreHooks *hooks)
{
    w->chained = *hooks;
    hooks->user = w;
    hooks->spawned = HookSpawned;
    hooks->removed = HookRemoved;
    hooks->orphan = HookOrphan;
    hooks->error = w->chained.error ? HookError : NULL;
}

/* The store calls every hook with one user, now world, so the caller's error hook is relayed. */
static void HookError(void *user, const char *message)
{
    World3D *w = user;
    if (w->chained.error)
        w->chained.error(w->chained.user, message);
}

static void HookSpawned(void *user, StoreId id)
{
    World3D *w = user;
    if (Is(w, id, w->node))
    {
        World3DEntry *e = Find(w, id);
        if (e)
            e->live = false; /* a load or restore over a live slot: start afresh */
        Vector3 p, r, s;
        StoreId parent;
        TickLocal(w, id, &p, &r, &s, &parent); /* makes the entry, jumping */
    }
    if (w->chained.spawned)
        w->chained.spawned(w->chained.user, id);
}

static void HookRemoved(void *user, StoreId id)
{
    World3D *w = user;
    World3DEntry *e = Find(w, id);
    if (e)
        e->live = false;
    for (int i = 0; i < w->mapCount; i++)
        if (Same(w->maps[i].map, id))
        {
            FreeChunks(&w->maps[i]);
            w->maps[i] = w->maps[--w->mapCount];
            break;
        }
    if (w->chained.removed)
        w->chained.removed(w->chained.user, id);
}

/* A guest about to lose its removed parent keeps where it was in the world. */
static void HookOrphan(void *user, StoreId id)
{
    World3D *w = user;
    if (Is(w, id, w->node))
    {
        Vector3 t, s;
        Quaternion q;
        MatrixDecompose(TickWorld(w, id), &t, &q, &s);
        StoreValue position = VecValue(t), rotation = VecValue(QuaternionToEuler(q)),
                   scale = VecValue(s);
        StoreSetEngine(w->store, id, w->position, &position);
        StoreSetEngine(w->store, id, w->rotation, &rotation);
        StoreSetEngine(w->store, id, w->scale, &scale);
        World3DEntry *e = Entry(w, id);
        if (e)
            e->jump = true;
    }
    if (w->chained.orphan)
        w->chained.orphan(w->chained.user, id);
}

/* ---- transforms ------------------------------------------------------------------------------ */

bool World3DWorldMatrix(World3D *w, StoreId id, Matrix *out)
{
    if (!Is(w, id, w->node))
        return false;
    World3DEntry *e = Find(w, id);
    *out = e && e->computed ? e->world : TickWorld(w, id);
    return true;
}

bool World3DWorldPosition(World3D *w, StoreId id, Vector3 *out)
{
    if (!Is(w, id, w->node))
        return false;
    *out = Translation(TickWorld(w, id));
    return true;
}

static void Compose(World3D *w, StoreId id, float alpha, int depth)
{
    World3DEntry *e = Entry(w, id);
    if (!e || e->stamp == w->frame || depth > MAX_DEPTH)
        return;
    e->stamp = w->frame;
    StoreId parent = TransformParent(w, id);
    Matrix parentWorld = MatrixIdentity();
    bool parentMoved = false;
    if (!IsNull(parent))
    {
        Compose(w, parent, alpha, depth + 1);
        World3DEntry *p = Find(w, parent);
        if (p)
        {
            parentWorld = p->world;
            parentMoved = p->changed == w->frame;
        }
        e = Find(w, id); /* the entries may have moved */
        if (!e)
            return;
    }
    Vector3 position = GetVec(w, id, w->position), rotation = GetVec(w, id, w->rotation),
            scale = GetVec(w, id, w->scale);
    Vector3 prevPosition = GetVec(w, id, w->prevPosition),
            prevRotation = GetVec(w, id, w->prevRotation);
    bool reparented = e->computed && !Same(e->parent, parent);
    if (reparented)
        e->jump = true;
    if (e->jump)
    {
        prevPosition = position;
        prevRotation = rotation;
    }
    bool moving = !VecSame(prevPosition, position) || !VecSame(prevRotation, rotation);
    bool dirty = !e->computed || reparented || parentMoved || moving || e->interpolated ||
                 !VecSame(e->position, position) || !VecSame(e->rotation, rotation) ||
                 !VecSame(e->scale, scale);
    if (!dirty)
        return;
    Vector3 p = moving ? Vector3Lerp(prevPosition, position, alpha) : position;
    Quaternion q =
        moving ? QuaternionSlerp(Euler(prevRotation), Euler(rotation), alpha) : Euler(rotation);
    e->world = MatrixMultiply(Local(p, q, scale), parentWorld);
    e->position = position;
    e->rotation = rotation;
    e->scale = scale;
    e->parent = parent;
    e->interpolated = moving;
    e->computed = true;
    e->changed = w->frame;
}

void World3DUpdateTransforms(World3D *w, float alpha)
{
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    if (++w->frame == 0)
        w->frame = 1;
    int count;
    StoreId *nodes = List(w, w->node, &count);
    if (!nodes)
        return;
    if (count && Reserve(w, nodes[count - 1].index))
        for (int i = 0; i < count; i++)
            Compose(w, nodes[i], alpha, 0);
    free(nodes);
}

void World3DBeginTick(World3D *w)
{
    int count;
    StoreId *nodes = List(w, w->node, &count);
    for (int i = 0; nodes && i < count; i++)
    {
        Vector3 p, r, s;
        StoreId parent;
        TickLocal(w, nodes[i], &p, &r, &s, &parent);
        StoreValue pv = VecValue(p), rv = VecValue(r);
        StoreSetEngine(w->store, nodes[i], w->prevPosition, &pv);
        StoreSetEngine(w->store, nodes[i], w->prevRotation, &rv);
        World3DEntry *e = Entry(w, nodes[i]);
        if (e)
            e->jump = false;
    }
    free(nodes);
}

bool World3DTeleport(World3D *w, StoreId id, Vector3 world)
{
    if (!Is(w, id, w->node))
        return false;
    StoreId parent = TransformParent(w, id);
    Vector3 local = world;
    if (!IsNull(parent))
    {
        Matrix m = TickWorld(w, parent);
        if (fabsf(MatrixDeterminant(m)) < 1e-12f)
            return false;
        local = Vector3Transform(world, MatrixInvert(m));
    }
    StoreValue position = VecValue(local);
    if (!StoreSet(w->store, id, w->position, &position))
        return false;
    StoreValue rotation = VecValue(GetVec(w, id, w->rotation));
    StoreSetEngine(w->store, id, w->prevPosition, &position);
    StoreSetEngine(w->store, id, w->prevRotation, &rotation);
    World3DEntry *e = Entry(w, id);
    if (e)
        e->jump = true;
    return true;
}

/* ---- boxes ----------------------------------------------------------------------------------- */

static Box CharacterBox(World3D *w, StoreId id)
{
    Vector3 p = Translation(TickWorld(w, id));
    float r = fabsf(GetFloat(w, id, w->charRadius)), h = fabsf(GetFloat(w, id, w->charHeight));
    return (Box){{p.x - r, p.y, p.z - r}, {p.x + r, p.y + h, p.z + r}};
}

static Box SolidBox(World3D *w, StoreId id)
{
    Vector3 p = Translation(TickWorld(w, id)), s = GetVec(w, id, w->size);
    Vector3 half = {fabsf(s.x) * 0.5f, fabsf(s.y) * 0.5f, fabsf(s.z) * 0.5f};
    return (Box){Vector3Subtract(p, half), Vector3Add(p, half)};
}

static bool SphereMeetsBox(Vector3 c, float r, Box b)
{
    Vector3 q = {fmaxf(b.min.x, fminf(c.x, b.max.x)), fmaxf(b.min.y, fminf(c.y, b.max.y)),
                 fmaxf(b.min.z, fminf(c.z, b.max.z))};
    return Vector3DistanceSqr(c, q) <= r * r;
}

/* An area's volume where it is now: a sphere of `radius`, or with `shape` box an axis-aligned box
   of `size` centred on its world position, like a solid. Any other shape reads as a sphere. */
typedef struct AreaShape
{
    bool box;
    Vector3 centre;
    float radius;
    Box bounds;
} AreaShape;

static AreaShape AreaShapeOf(World3D *w, StoreId area)
{
    AreaShape a;
    memset(&a, 0, sizeof a);
    a.centre = Translation(TickWorld(w, area));
    StoreValue v;
    a.box = StoreGet(w->store, area, w->areaShape, &v) && v.type == STORE_SYMBOL && v.as.sym == w->box;
    if (a.box)
    {
        Vector3 s = GetVec(w, area, w->areaSize);
        Vector3 half = {fabsf(s.x) * 0.5f, fabsf(s.y) * 0.5f, fabsf(s.z) * 0.5f};
        a.bounds = (Box){Vector3Subtract(a.centre, half), Vector3Add(a.centre, half)};
    }
    else
        a.radius = fabsf(GetFloat(w, area, w->areaRadius));
    return a;
}

static bool AreaMeetsBox(const AreaShape *a, Box b)
{
    if (!a->box)
        return SphereMeetsBox(a->centre, a->radius, b);
    return a->bounds.min.x <= b.max.x && b.min.x <= a->bounds.max.x && a->bounds.min.y <= b.max.y &&
           b.min.y <= a->bounds.max.y && a->bounds.min.z <= b.max.z && b.min.z <= a->bounds.max.z;
}

static bool AreaHolds(const AreaShape *a, Vector3 p)
{
    return AreaMeetsBox(a, (Box){p, p});
}

typedef struct Boxes
{
    Box *items;
    int count, capacity;
} Boxes;

static bool AddBox(Boxes *b, Box box)
{
    if (b->count == b->capacity)
    {
        int capacity = b->capacity ? b->capacity * 2 : 32;
        Box *items = realloc(b->items, sizeof *items * (size_t)capacity);
        if (!items)
            return false;
        b->items = items;
        b->capacity = capacity;
    }
    b->items[b->count++] = box;
    return true;
}

/* Every box but self's that may meet the region, in id order (a tilemap's cells row-major). */
static bool GatherBoxes(World3D *w, StoreId self, Box region, Boxes *out)
{
    int count;
    StoreId *nodes = List(w, w->node, &count);
    if (!nodes)
        return false;
    bool ok = true;
    for (int i = 0; ok && i < count; i++)
    {
        StoreId id = nodes[i];
        if (Same(id, self))
            continue;
        if (Is(w, id, w->solid))
            ok = AddBox(out, SolidBox(w, id));
        else if (Is(w, id, w->character))
            ok = AddBox(out, CharacterBox(w, id));
        else if (Is(w, id, w->tilemap))
        {
            MapInfo m;
            if (!ReadMap(w, id, &m) || !m.width || !m.depth)
                continue;
            Box slabs[2];
            MapSlabs(&m, slabs);
            ok = AddBox(out, slabs[0]) && AddBox(out, slabs[1]);
            if (region.max.y < m.origin.y || region.min.y > m.origin.y + m.height)
                continue;
            int x0 =
                    ClampInt((int)floorf((region.min.x - m.origin.x) / m.cellSize), 0, m.width - 1),
                x1 =
                    ClampInt((int)floorf((region.max.x - m.origin.x) / m.cellSize), 0, m.width - 1),
                z0 =
                    ClampInt((int)floorf((region.min.z - m.origin.z) / m.cellSize), 0, m.depth - 1),
                z1 =
                    ClampInt((int)floorf((region.max.z - m.origin.z) / m.cellSize), 0, m.depth - 1);
            for (int z = z0; ok && z <= z1; z++)
                for (int x = x0; ok && x <= x1; x++)
                    if (CellSolid(w, id, &m, x, z))
                        ok = AddBox(out,
                                    (Box){{m.origin.x + x * m.cellSize, m.origin.y,
                                           m.origin.z + z * m.cellSize},
                                          {m.origin.x + (x + 1) * m.cellSize, m.origin.y + m.height,
                                           m.origin.z + (z + 1) * m.cellSize}});
        }
    }
    free(nodes);
    return ok;
}

/* How far box b can go along one axis, stopping at the first face in the way. */
static float Sweep(const Boxes *boxes, Box b, int axis, float delta, bool *blocked)
{
    int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
    for (int i = 0; i < boxes->count; i++)
    {
        Box o = boxes->items[i];
        if (!(Axis(b.min, a1) < Axis(o.max, a1) - CONTACT_EPSILON &&
              Axis(b.max, a1) > Axis(o.min, a1) + CONTACT_EPSILON &&
              Axis(b.min, a2) < Axis(o.max, a2) - CONTACT_EPSILON &&
              Axis(b.max, a2) > Axis(o.min, a2) + CONTACT_EPSILON))
            continue;
        if (delta > 0 && Axis(b.max, axis) <= Axis(o.min, axis) + CONTACT_EPSILON)
        {
            float room = fmaxf(Axis(o.min, axis) - Axis(b.max, axis), 0);
            if (room < delta)
            {
                delta = room;
                *blocked = true;
            }
        }
        else if (delta < 0 && Axis(b.min, axis) >= Axis(o.max, axis) - CONTACT_EPSILON)
        {
            float room = fminf(Axis(o.max, axis) - Axis(b.min, axis), 0);
            if (room > delta)
            {
                delta = room;
                *blocked = true;
            }
        }
    }
    return delta;
}

bool World3DMoveAndSlide(World3D *w, StoreId id, float dt)
{
    if (!Is(w, id, w->character))
        return false;
    Vector3 position = GetVec(w, id, w->position), velocity = GetVec(w, id, w->velocity);
    float r = fabsf(GetFloat(w, id, w->charRadius)), h = fabsf(GetFloat(w, id, w->charHeight));
    Vector3 motion = Vector3Scale(velocity, dt);
    Box box = {{position.x - r, position.y, position.z - r},
               {position.x + r, position.y + h, position.z + r}};
    Box region = {Vector3Min(box.min, Vector3Add(box.min, motion)),
                  Vector3Max(box.max, Vector3Add(box.max, motion))};
    Boxes boxes = {0};
    if (!GatherBoxes(w, id, region, &boxes))
    {
        free(boxes.items);
        return false;
    }
    bool onFloor = false;
    const int order[3] = {0, 2, 1};
    for (int i = 0; i < 3; i++)
    {
        int axis = order[i];
        float want = Axis(motion, axis);
        if (want == 0)
            continue;
        bool blocked = false;
        float got = Sweep(&boxes, box, axis, want, &blocked);
        SetAxis(&position, axis, Axis(position, axis) + got);
        SetAxis(&box.min, axis, Axis(box.min, axis) + got);
        SetAxis(&box.max, axis, Axis(box.max, axis) + got);
        if (blocked)
        {
            SetAxis(&velocity, axis, 0);
            onFloor |= axis == 1 && want < 0;
        }
    }
    free(boxes.items);
    StoreValue p = VecValue(position), v = VecValue(velocity), f = BoolValue(onFloor);
    return StoreSetEngine(w->store, id, w->position, &p) &&
           StoreSetEngine(w->store, id, w->velocity, &v) &&
           StoreSetEngine(w->store, id, w->onFloor, &f);
}

/* ---- areas ----------------------------------------------------------------------------------- */

static bool Contains(const StoreValue *list, int count, StoreId id)
{
    for (int i = 0; i < count; i++)
        if (Same(list[i].as.ref, id))
            return true;
    return false;
}

/* Step 6 of every tick: who is in which area, touched and untouched. */
static void AreaSystem(Store *store, float dt, void *user)
{
    (void)dt;
    World3D *w = user;
    int areaCount, charCount;
    StoreId *areas = List(w, w->area, &areaCount), *chars = List(w, w->character, &charCount);
    for (int a = 0; areas && chars && a < areaCount; a++)
    {
        StoreId area = areas[a];
        AreaShape shape = AreaShapeOf(w, area);
        StoreValue now[16], was[16];
        int nowCount = 0, wasCount = StoreCountOf(store, area, w->inside);
        wasCount = wasCount < 0 ? 0 : wasCount > 16 ? 16 : wasCount;
        for (int i = 0; i < wasCount; i++)
            if (!StoreGetAt(store, area, w->inside, i, NULL, &was[i]))
                was[i].as.ref = STORE_NULL;
        for (int c = 0; c < charCount && nowCount < 16; c++)
            if (!Same(chars[c], area) && AreaMeetsBox(&shape, CharacterBox(w, chars[c])))
            {
                memset(&now[nowCount], 0, sizeof now[nowCount]);
                now[nowCount].type = STORE_REF;
                now[nowCount++].as.ref = chars[c];
            }
        StoreKind kind = StoreKindOf(store, area);
        bool hearsTouched = StoreKindHandlesEvent(store, kind, w->touched),
             hearsUntouched = StoreKindHandlesEvent(store, kind, w->untouched);
        for (int i = 0; i < nowCount; i++)
            if (hearsTouched && !Contains(was, wasCount, now[i].as.ref))
                StoreSend(store, area, w->touched, &now[i], 1);
        for (int i = 0; i < wasCount; i++)
            if (hearsUntouched && !Contains(now, nowCount, was[i].as.ref))
                StoreSend(store, area, w->untouched, &was[i], 1);
        StoreSetList(store, area, w->inside, now, nowCount);
    }
    free(areas);
    free(chars);
}

int World3DOverlapping(World3D *w, StoreId area, StoreKind kind, StoreId *out, int max)
{
    if (!Is(w, area, w->area))
        return -1;
    if (!StoreKindName(w->store, kind))
        return 0;
    AreaShape shape = AreaShapeOf(w, area);
    int count, n = 0;
    StoreId *things = List(w, kind, &count);
    for (int i = 0; things && i < count; i++)
    {
        StoreId id = things[i];
        bool meets;
        if (Same(id, area) || !Is(w, id, w->node))
            continue;
        if (Is(w, id, w->character))
            meets = AreaMeetsBox(&shape, CharacterBox(w, id));
        else if (Is(w, id, w->solid))
            meets = AreaMeetsBox(&shape, SolidBox(w, id));
        else
            meets = AreaHolds(&shape, Translation(TickWorld(w, id)));
        if (!meets)
            continue;
        if (out)
        {
            if (n >= max)
                break;
            out[n] = id;
        }
        n++;
    }
    free(things);
    return n;
}

StoreId World3DNearest(World3D *w, StoreKind kind, Vector3 point, float maxDistance,
                       bool (*accept)(StoreId, void *), void *user)
{
    if (!StoreKindName(w->store, kind))
        return STORE_NULL;
    int count;
    StoreId *things = List(w, kind, &count), best = STORE_NULL;
    float bestSq = maxDistance < 0 ? FLT_MAX : maxDistance * maxDistance;
    bool found = false;
    for (int i = 0; things && i < count; i++)
    {
        if (!Is(w, things[i], w->node))
            continue;
        float d = Vector3DistanceSqr(point, Translation(TickWorld(w, things[i])));
        if ((found ? d < bestSq : d <= bestSq) && (!accept || accept(things[i], user)))
        {
            best = things[i];
            bestSq = d;
            found = true;
        }
    }
    free(things);
    return best;
}

/* ---- rays ------------------------------------------------------------------------------------ */

/* Slab test from outside a box: false when missed, beyond maxDistance, or started inside. */
static bool RayBox(Vector3 from, Vector3 dir, Box b, float maxDistance, float *t, Vector3 *normal)
{
    float enter = -FLT_MAX, exit = FLT_MAX;
    int enterAxis = -1;
    for (int a = 0; a < 3; a++)
    {
        float o = Axis(from, a), d = Axis(dir, a), lo = Axis(b.min, a), hi = Axis(b.max, a);
        if (d == 0)
        {
            if (o < lo || o > hi)
                return false;
            continue;
        }
        float t0 = (lo - o) / d, t1 = (hi - o) / d;
        if (t0 > t1)
        {
            float swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t0 > enter)
        {
            enter = t0;
            enterAxis = a;
        }
        exit = fminf(exit, t1);
    }
    int nearAxis = enterAxis;
    if (enterAxis < 0 || enter < 0 || enter > exit || enter > maxDistance)
        return false;
    *t = enter;
    *normal = (Vector3){0, 0, 0};
    SetAxis(normal, nearAxis, Axis(dir, nearAxis) > 0 ? -1.0f : 1.0f);
    return true;
}

/* 2D DDA over a tilemap's cells (each a full-height column), in the tilemap's space. */
static bool RayMap(World3D *w, StoreId map, Vector3 from, Vector3 dir, float maxDistance, float *t,
                   Vector3 *normal)
{
    MapInfo m;
    if (!ReadMap(w, map, &m) || !m.width || !m.depth)
        return false;
    Vector3 o = Vector3Subtract(from, m.origin);
    Box bounds = {{0, 0, 0}, {m.width * m.cellSize, m.height, m.depth * m.cellSize}};
    /* Clip the ray to the map's box, allowing a start inside it. */
    float enter = -FLT_MAX, exit = FLT_MAX;
    int enterAxis = -1;
    for (int a = 0; a < 3; a++)
    {
        float oa = Axis(o, a), d = Axis(dir, a), lo = Axis(bounds.min, a), hi = Axis(bounds.max, a);
        if (d == 0)
        {
            if (oa < lo || oa > hi)
                return false;
            continue;
        }
        float t0 = (lo - oa) / d, t1 = (hi - oa) / d;
        if (t0 > t1)
        {
            float swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t0 > enter)
        {
            enter = t0;
            enterAxis = a;
        }
        exit = fminf(exit, t1);
    }
    bool inside = enter < 0;
    float at = inside ? 0 : enter;
    if (exit < at || at > maxDistance)
        return false;
    Vector3 n = {0, 0, 0};
    if (!inside && enterAxis >= 0)
        SetAxis(&n, enterAxis, Axis(dir, enterAxis) > 0 ? -1.0f : 1.0f);
    Vector3 p = Vector3Add(o, Vector3Scale(dir, at));
    int cx = ClampInt((int)floorf(p.x / m.cellSize), 0, m.width - 1),
        cz = ClampInt((int)floorf(p.z / m.cellSize), 0, m.depth - 1);
    int stepX = dir.x > 0 ? 1 : dir.x < 0 ? -1 : 0, stepZ = dir.z > 0 ? 1 : dir.z < 0 ? -1 : 0;
    float nextX = stepX ? ((cx + (stepX > 0)) * m.cellSize - o.x) / dir.x : FLT_MAX,
          nextZ = stepZ ? ((cz + (stepZ > 0)) * m.cellSize - o.z) / dir.z : FLT_MAX,
          deltaX = stepX ? m.cellSize / fabsf(dir.x) : FLT_MAX,
          deltaZ = stepZ ? m.cellSize / fabsf(dir.z) : FLT_MAX;
    bool first = true;
    for (int guard = 0; guard < 4 * WORLD3D_TILEMAP_MAX; guard++)
    {
        /* A cell the ray starts inside is not hit, as with boxes. */
        if (!(first && inside) && CellSolid(w, map, &m, cx, cz))
        {
            *t = at;
            *normal = n;
            return true;
        }
        first = false;
        if (nextX < nextZ)
        {
            at = nextX;
            nextX += deltaX;
            cx += stepX;
            n = (Vector3){(float)-stepX, 0, 0};
        }
        else
        {
            at = nextZ;
            nextZ += deltaZ;
            cz += stepZ;
            n = (Vector3){0, 0, (float)-stepZ};
        }
        if (at > exit || at > maxDistance || cx < 0 || cz < 0 || cx >= m.width || cz >= m.depth)
            return false;
    }
    return false;
}

static bool Under(World3D *w, StoreId id, StoreId ancestor)
{
    for (int depth = 0; !IsNull(id) && depth < MAX_DEPTH; depth++, id = StoreParent(w->store, id))
        if (Same(id, ancestor))
            return true;
    return false;
}

/* The nearest entry of a ray into a sphere, at or beyond 0 and within max; not from inside. */
static bool RaySphere(Vector3 from, Vector3 dir, Vector3 centre, float radius, float max, float *t,
                      Vector3 *normal)
{
    Vector3 m = Vector3Subtract(from, centre);
    float b = Vector3DotProduct(m, dir), c = Vector3DotProduct(m, m) - radius * radius;
    if (c <= 0 || b > 0)
        return false;
    float disc = b * b - c;
    if (disc < 0)
        return false;
    float at = -b - sqrtf(disc);
    if (at < 0 || at > max)
        return false;
    *t = at;
    *normal = Vector3Normalize(Vector3Subtract(Vector3Add(from, Vector3Scale(dir, at)), centre));
    return true;
}

static World3DHit Cast(World3D *w, Vector3 from, Vector3 direction, float maxDistance,
                       const StoreId *ignore, int ignoreCount, bool characters, bool areas)
{
    World3DHit hit;
    memset(&hit, 0, sizeof hit);
    hit.thing = STORE_NULL;
    float length = Vector3Length(direction);
    if (length == 0 || !(maxDistance >= 0))
        return hit;
    for (int k = 0; k < ignoreCount; k++)
        if (!IsNull(ignore[k]) && !StoreAlive(w->store, ignore[k]))
            return hit;
    Vector3 dir = Vector3Scale(direction, 1.0f / length);
    int count;
    StoreId *nodes = List(w, w->node, &count);
    float best = maxDistance;
    for (int i = 0; nodes && i < count; i++)
    {
        StoreId id = nodes[i];
        float t;
        Vector3 normal;
        bool got = false, skip = false;
        for (int k = 0; k < ignoreCount && !skip; k++)
            skip = !IsNull(ignore[k]) && Under(w, id, ignore[k]);
        if (skip)
            continue;
        if (areas && Is(w, id, w->area))
        {
            AreaShape shape = AreaShapeOf(w, id);
            got = shape.box ? RayBox(from, dir, shape.bounds, best, &t, &normal)
                            : RaySphere(from, dir, shape.centre, shape.radius, best, &t, &normal);
        }
        else if (Is(w, id, w->tilemap))
        {
            got = RayMap(w, id, from, dir, best, &t, &normal);
            MapInfo m;
            Box slabs[2];
            float slabT;
            Vector3 slabNormal;
            if (ReadMap(w, id, &m) && m.width && m.depth)
            {
                MapSlabs(&m, slabs);
                for (int k = 0; k < 2; k++)
                    if (RayBox(from, dir, slabs[k], got ? t : best, &slabT, &slabNormal) &&
                        (!got || slabT < t))
                    {
                        got = true;
                        t = slabT;
                        normal = slabNormal;
                    }
            }
        }
        else if (Is(w, id, w->solid))
            got = RayBox(from, dir, SolidBox(w, id), best, &t, &normal);
        else if (characters && Is(w, id, w->character))
            got = RayBox(from, dir, CharacterBox(w, id), best, &t, &normal);
        if (got && (!hit.hit || t < best))
        {
            hit.hit = true;
            hit.distance = t;
            hit.point = Vector3Add(from, Vector3Scale(dir, t));
            hit.normal = normal;
            hit.thing = id;
            best = t;
        }
    }
    free(nodes);
    return hit;
}

World3DHit World3DRaycast(World3D *w, Vector3 from, Vector3 direction, float maxDistance,
                          StoreId ignore)
{
    return Cast(w, from, direction, maxDistance, &ignore, 1, true, false);
}

World3DHit World3DRaycastIgnoring(World3D *w, Vector3 from, Vector3 direction, float maxDistance,
                                  const StoreId *ignore, int count, bool areas)
{
    return Cast(w, from, direction, maxDistance, ignore, count < 0 ? 0 : count, true, areas);
}

bool World3DLineOfSight(World3D *w, Vector3 from, Vector3 to)
{
    float distance = Vector3Distance(from, to);
    if (distance == 0)
        return true;
    World3DHit hit = Cast(w, from, Vector3Subtract(to, from), distance, NULL, 0, false, false);
    return !hit.hit || hit.distance >= distance - CONTACT_EPSILON;
}

/* ---- cells and paths ------------------------------------------------------------------------- */

bool World3DCellToWorld(World3D *w, StoreId map, int x, int z, Vector3 *out)
{
    MapInfo m;
    if (!Is(w, map, w->tilemap) || !ReadMap(w, map, &m) || x < 0 || z < 0 || x >= m.width ||
        z >= m.depth)
        return false;
    *out = (Vector3){m.origin.x + (x + 0.5f) * m.cellSize, m.origin.y,
                     m.origin.z + (z + 0.5f) * m.cellSize};
    return true;
}

static bool CellOf(const MapInfo *m, Vector3 world, int *x, int *z)
{
    float fx = floorf((world.x - m->origin.x) / m->cellSize),
          fz = floorf((world.z - m->origin.z) / m->cellSize);
    if (!(fx >= 0 && fz >= 0 && fx < (float)m->width && fz < (float)m->depth))
        return false;
    *x = (int)fx;
    *z = (int)fz;
    return true;
}

bool World3DWorldToCell(World3D *w, StoreId map, Vector3 world, int *x, int *z)
{
    MapInfo m;
    return Is(w, map, w->tilemap) && ReadMap(w, map, &m) && CellOf(&m, world, x, z);
}

static const int stepsX[4] = {1, -1, 0, 0}, stepsZ[4] = {0, 0, 1, -1};

static World3DPathCache *Distances(World3D *w, StoreId map, const MapInfo *m, int tx, int tz)
{
    uint64_t tick = StoreTickCount(w->store);
    if (tick != w->pathTick)
    {
        w->pathTick = tick;
        w->pathCount = 0;
    }
    for (int i = 0; i < w->pathCount; i++)
    {
        World3DPathCache *c = &w->paths[i];
        if (Same(c->map, map) && c->targetX == tx && c->targetZ == tz && c->width == m->width &&
            c->depth == m->depth)
            return c;
    }
    if (w->pathCount == w->pathCapacity)
    {
        int capacity = w->pathCapacity ? w->pathCapacity * 2 : 4;
        World3DPathCache *paths = realloc(w->paths, sizeof *paths * (size_t)capacity);
        if (!paths)
            return NULL;
        w->paths = paths;
        w->pathCapacity = capacity;
    }
    World3DPathCache *c = &w->paths[w->pathCount++];
    c->map = map;
    c->targetX = tx;
    c->targetZ = tz;
    c->width = m->width;
    c->depth = m->depth;
    for (int i = 0; i < WORLD3D_TILEMAP_MAX * WORLD3D_TILEMAP_MAX; i++)
        c->distance[i] = -1;
    const int size = WORLD3D_TILEMAP_MAX * WORLD3D_TILEMAP_MAX;
    int queue[WORLD3D_TILEMAP_MAX * WORLD3D_TILEMAP_MAX], head = 0, tail = 0;
    c->distance[tz * WORLD3D_TILEMAP_MAX + tx] = 0;
    queue[tail++] = tz * WORLD3D_TILEMAP_MAX + tx;
    while (head < tail)
    {
        int cell = queue[head++], x = cell % WORLD3D_TILEMAP_MAX, z = cell / WORLD3D_TILEMAP_MAX;
        for (int s = 0; s < 4; s++)
        {
            int nx = x + stepsX[s], nz = z + stepsZ[s], next = nz * WORLD3D_TILEMAP_MAX + nx;
            if (!CellOpen(w, map, m, nx, nz) || c->distance[next] >= 0 || tail >= size)
                continue;
            c->distance[next] = (int16_t)(c->distance[cell] + 1);
            queue[tail++] = next;
        }
    }
    return c;
}

bool World3DPathNext(World3D *w, StoreId map, Vector3 from, Vector3 to, Vector3 *out)
{
    MapInfo m;
    if (!Is(w, map, w->tilemap) || !ReadMap(w, map, &m))
        return false;
    int fx, fz, tx, tz;
    *out = to;
    if (!CellOf(&m, from, &fx, &fz) || !CellOf(&m, to, &tx, &tz) || !CellOpen(w, map, &m, tx, tz) ||
        abs(fx - tx) + abs(fz - tz) <= 1)
        return true;
    World3DPathCache *c = Distances(w, map, &m, tx, tz);
    if (!c)
        return false;
    int here = c->distance[fz * WORLD3D_TILEMAP_MAX + fx];
    if (here < 0)
        return true;
    for (int s = 0; s < 4; s++)
    {
        int nx = fx + stepsX[s], nz = fz + stepsZ[s];
        if (nx >= 0 && nz >= 0 && nx < m.width && nz < m.depth &&
            c->distance[nz * WORLD3D_TILEMAP_MAX + nx] == here - 1)
            return World3DCellToWorld(w, map, nx, nz, out);
    }
    return true;
}

/* ---- chunks ---------------------------------------------------------------------------------- */

static uint64_t Mix(uint64_t h, const void *data, size_t size)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static bool Quad(MB *mb, Vector3 p0, Vector3 p1, Vector3 p2, Vector3 normal, Vector2 uv0,
                 Vector2 uv1, Vector2 uv2)
{
    Vector3 p3 = Vector3Subtract(Vector3Add(p1, p2), p0);
    Vector2 uv3 = {uv1.x + uv2.x - uv0.x, uv1.y + uv2.y - uv0.y};
    if (Vector3DotProduct(Vector3CrossProduct(Vector3Subtract(p1, p0), Vector3Subtract(p2, p0)),
                          normal) < 0)
    {
        Vector3 swap = p1;
        Vector2 swapUv = uv1;
        p1 = p2;
        p2 = swap;
        uv1 = uv2;
        uv2 = swapUv;
    }
    Vector3 positions[4] = {p0, p1, p2, p3};
    Vector2 uvs[4] = {uv0, uv1, uv2, uv3};
    return EmitQuadN(mb, positions, normal, uvs);
}

static bool BuildChunk(World3D *w, StoreId map, const MapInfo *m, World3DChunk *c)
{
    MBFree(&c->floor);
    MBFree(&c->wall);
    MBFree(&c->ceiling);
    if (!MBInit(&c->floor, 64) || !MBInit(&c->wall, 64) || !MBInit(&c->ceiling, 64))
        return false;
    float s = m->cellSize, h = m->height;
    int x0 = c->chunkX * WORLD3D_CHUNK_CELLS, z0 = c->chunkZ * WORLD3D_CHUNK_CELLS;
    int x1 = x0 + WORLD3D_CHUNK_CELLS < m->width ? x0 + WORLD3D_CHUNK_CELLS : m->width;
    int z1 = z0 + WORLD3D_CHUNK_CELLS < m->depth ? z0 + WORLD3D_CHUNK_CELLS : m->depth;
    c->bounds = (BoundingBox){{x0 * s, 0, z0 * s}, {x1 * s, h, z1 * s}};
    bool ok = true;
    for (int z = z0; ok && z < z1; z++)
        for (int x = x0; ok && x < x1; x++)
        {
            float fx = (float)x, fz = (float)z;
            if (CellOpen(w, map, m, x, z))
            {
                Vector3 a = {x * s, 0, z * s}, b = {(x + 1) * s, 0, z * s},
                        d = {x * s, 0, (z + 1) * s};
                ok = Quad(&c->floor, a, b, d, (Vector3){0, 1, 0}, (Vector2){fx, fz},
                          (Vector2){fx + 1, fz}, (Vector2){fx, fz + 1});
                a.y = b.y = d.y = h;
                ok = ok && Quad(&c->ceiling, a, b, d, (Vector3){0, -1, 0}, (Vector2){fx, fz},
                                (Vector2){fx + 1, fz}, (Vector2){fx, fz + 1});
                continue;
            }
            for (int side = 0; ok && side < 4; side++)
            {
                if (!CellOpen(w, map, m, x + stepsX[side], z + stepsZ[side]))
                    continue;
                Vector3 normal = {(float)stepsX[side], 0, (float)stepsZ[side]};
                /* The face's bottom edge runs from a to b; d is above a. */
                Vector3 a, b;
                if (stepsX[side])
                {
                    float px = (stepsX[side] > 0 ? x + 1 : x) * s;
                    a = (Vector3){px, 0, z * s};
                    b = (Vector3){px, 0, (z + 1) * s};
                }
                else
                {
                    float pz = (stepsZ[side] > 0 ? z + 1 : z) * s;
                    a = (Vector3){x * s, 0, pz};
                    b = (Vector3){(x + 1) * s, 0, pz};
                }
                float u = stepsX[side] ? fz : fx, top = h / s;
                Vector3 d = {a.x, h, a.z};
                ok = Quad(&c->wall, a, b, d, normal, (Vector2){u, top}, (Vector2){u + 1, top},
                          (Vector2){u, 0});
            }
        }
    return ok;
}

static uint64_t ChunkHash(World3D *w, StoreId map, const MapInfo *m, int cx, int cz)
{
    uint64_t h = 14695981039346656037ull;
    int header[4] = {m->width, m->depth, cx, cz};
    h = Mix(h, header, sizeof header);
    h = Mix(h, &m->cellSize, sizeof m->cellSize);
    h = Mix(h, &m->height, sizeof m->height);
    int x0 = cx * WORLD3D_CHUNK_CELLS, z0 = cz * WORLD3D_CHUNK_CELLS;
    /* Its own cells and the ring around them, whose openness decides its walls. */
    for (int z = z0 - 1; z <= z0 + WORLD3D_CHUNK_CELLS; z++)
        for (int x = x0 - 1; x <= x0 + WORLD3D_CHUNK_CELLS; x++)
        {
            int32_t v = 2; /* outside the used area */
            StoreValue cell;
            if (x >= 0 && z >= 0 && x < m->width && z < m->depth &&
                StoreGridGet(w->store, map, w->cells, x, z, &cell))
                v = cell.as.i;
            h = Mix(h, &v, sizeof v);
        }
    return h;
}

int World3DTilemapChunks(World3D *w, StoreId map, World3DChunk *out, int max)
{
    MapInfo m;
    if (!Is(w, map, w->tilemap) || !ReadMap(w, map, &m))
        return -1;
    World3DTilemapCache *cache = NULL;
    for (int i = 0; i < w->mapCount && !cache; i++)
        if (Same(w->maps[i].map, map))
            cache = &w->maps[i];
    if (!cache)
    {
        if (w->mapCount == w->mapCapacity)
        {
            int capacity = w->mapCapacity ? w->mapCapacity * 2 : 4;
            World3DTilemapCache *maps = realloc(w->maps, sizeof *maps * (size_t)capacity);
            if (!maps)
                return -1;
            w->maps = maps;
            w->mapCapacity = capacity;
        }
        cache = &w->maps[w->mapCount++];
        memset(cache, 0, sizeof *cache);
        cache->map = map;
    }
    int chunksX = (m.width + WORLD3D_CHUNK_CELLS - 1) / WORLD3D_CHUNK_CELLS,
        chunksZ = (m.depth + WORLD3D_CHUNK_CELLS - 1) / WORLD3D_CHUNK_CELLS;
    if (chunksX != cache->chunksX || chunksZ != cache->chunksZ)
    {
        FreeChunks(cache);
        int n = chunksX * chunksZ;
        cache->chunks = calloc((size_t)(n ? n : 1), sizeof *cache->chunks);
        cache->built = calloc((size_t)(n ? n : 1), sizeof *cache->built);
        if (!cache->chunks || !cache->built)
        {
            FreeChunks(cache);
            return -1;
        }
        cache->chunksX = chunksX;
        cache->chunksZ = chunksZ;
        for (int i = 0; i < n; i++)
        {
            cache->chunks[i].chunkX = i % chunksX;
            cache->chunks[i].chunkZ = i / chunksX;
        }
    }
    int n = 0;
    for (int i = 0; i < chunksX * chunksZ; i++)
    {
        World3DChunk *c = &cache->chunks[i];
        uint64_t hash = ChunkHash(w, map, &m, c->chunkX, c->chunkZ);
        if (!cache->built[i] || hash != c->hash)
        {
            if (!BuildChunk(w, map, &m, c))
            {
                cache->built[i] = false;
                return -1;
            }
            c->hash = hash;
            c->changed = true;
            cache->built[i] = true;
        }
        if (out)
        {
            if (n >= max)
                continue;
            out[n] = *c;
            c->changed = false;
        }
        n++;
    }
    return n;
}
