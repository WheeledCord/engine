/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The built-in 3D kinds (core/world3d.h), headless: collide-and-slide, areas, rays, line of sight,
// paths, tilemap chunks, cached transforms and determinism, each with what must be refused.
// docs/developer/store.md §3 and §8.
#include "checks.h"
#include "core/world3d.h"
#include "raymath.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: world3d: %s\n", what);
        failures++;
    }
}

static bool Same(StoreId a, StoreId b)
{
    return a.index == b.index && a.generation == b.generation;
}

static bool Near(float a, float b) { return fabsf(a - b) < 1e-3f; }

static bool NearVec(Vector3 a, Vector3 b)
{
    return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z);
}

static StoreValue Vec(float x, float y, float z)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_VEC3;
    v.as.v = (Vector3){x, y, z};
    return v;
}

static StoreValue Int(int i)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_INT;
    v.as.i = i;
    return v;
}

static StoreValue Float(float f)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_FLOAT;
    v.as.f = f;
    return v;
}

static void SetVec(Store *s, StoreId id, const char *field, float x, float y, float z)
{
    StoreValue v = Vec(x, y, z);
    StoreSet(s, id, StoreFieldIndex(s, StoreKindOf(s, id), field), &v);
}

static void SetNumber(Store *s, StoreId id, const char *field, StoreValue v)
{
    StoreSet(s, id, StoreFieldIndex(s, StoreKindOf(s, id), field), &v);
}

static Vector3 GetVec(Store *s, StoreId id, const char *field)
{
    StoreValue v;
    StoreGet(s, id, StoreFieldIndex(s, StoreKindOf(s, id), field), &v);
    return v.as.v;
}

static bool GetBool(Store *s, StoreId id, const char *field)
{
    StoreValue v;
    return StoreGet(s, id, StoreFieldIndex(s, StoreKindOf(s, id), field), &v) && v.as.b;
}

static void SetCell(Store *s, StoreId map, int x, int z, int value)
{
    StoreValue v = Int(value);
    StoreGridSet(s, map, StoreFieldIndex(s, StoreKindOf(s, map), "cells"), x, z, &v);
}

static StoreId Spawn(Store *s, const char *kind, StoreId parent)
{
    return StoreSpawn(s, StoreKindNamed(s, kind), 0, parent, STORE_NO_SYMBOL);
}

/* An 8 x 8 tilemap of 1 m cells, 3 m tall, with a wall along x = 4 for z = 0..6 (z = 7 is open),
   a floor solid under everything, and a character. */
typedef struct Scene
{
    Store s;
    World3D w;
    StoreId map, floor, hero;
} Scene;

static bool MakeScene(Scene *sc, uint64_t seed)
{
    if (!StoreInit(&sc->s, seed) || !World3DInit(&sc->w, &sc->s))
        return false;
    sc->map = Spawn(&sc->s, "tilemap", STORE_NULL);
    SetNumber(&sc->s, sc->map, "width", Int(8));
    SetNumber(&sc->s, sc->map, "depth", Int(8));
    SetNumber(&sc->s, sc->map, "cell-size", Float(1));
    for (int z = 0; z < 7; z++)
        SetCell(&sc->s, sc->map, 4, z, 1);
    sc->floor = Spawn(&sc->s, "solid", STORE_NULL);
    SetVec(&sc->s, sc->floor, "position", 4, -0.5f, 4);
    SetVec(&sc->s, sc->floor, "size", 20, 1, 20);
    sc->hero = Spawn(&sc->s, "character", STORE_NULL);
    return true;
}

static void FreeScene(Scene *sc)
{
    World3DFree(&sc->w);
    StoreFree(&sc->s);
}

// ---- kinds -------------------------------------------------------------------------------------
static void KindChecks(void)
{
    Scene sc;
    Expect(MakeScene(&sc, 1), "a store takes the built-in kinds and a scene spawns");
    Store *s = &sc.s;
    StoreKind node = StoreKindNamed(s, "node");
    const char *derived[] = {"model", "socket", "camera",  "light", "character",
                             "solid", "area",   "tilemap", "sound"};
    bool all = node >= 0;
    for (int i = 0; i < 9; i++)
        all = all && StoreKindBase(s, StoreKindNamed(s, derived[i])) == node;
    Expect(all, "every built-in kind extends node");
    StoreId cam = Spawn(s, "camera", STORE_NULL);
    StoreValue v;
    StoreKind camera = StoreKindNamed(s, "camera"), tilemap = StoreKindNamed(s, "tilemap");
    Expect(StoreGet(s, cam, StoreFieldIndex(s, camera, "fov"), &v) && v.as.f == 75 &&
               NearVec(GetVec(s, cam, "scale"), (Vector3){1, 1, 1}) && GetBool(s, cam, "visible"),
           "defaults: fov 75, scale 1 1 1, visible");
    const StoreFieldDecl *prev = StoreFieldAt(s, node, StoreFieldIndex(s, node, "%prev-position"));
    const StoreFieldDecl *cells = StoreFieldAt(s, tilemap, StoreFieldIndex(s, tilemap, "cells"));
    const StoreFieldDecl *floor =
        StoreFieldAt(s, StoreKindNamed(s, "character"),
                     StoreFieldIndex(s, StoreKindNamed(s, "character"), "on-floor"));
    Expect(prev && prev->flags == (STORE_LOCAL | STORE_HIDDEN | STORE_ENGINE) && cells &&
               cells->type == STORE_GRID && cells->max == 64 && cells->height == 64 && floor &&
               floor->flags == STORE_ENGINE,
           "flags and shapes: %prev-* local hidden engine, cells 64 x 64, on-floor engine");
    // Expected failure: the kinds can't be declared twice in one store.
    World3D again;
    Expect(!World3DInit(&again, s), "a second World3DInit on the same store is refused");
    World3DFree(&again);
    FreeScene(&sc);
}

// ---- collide and slide -------------------------------------------------------------------------
static void SlideChecks(void)
{
    Scene sc;
    MakeScene(&sc, 2);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    StoreId hero = sc.hero;
    // Into the wall at x = 4: X stops at the face, Z carries on.
    SetVec(s, hero, "position", 2, 0, 2.5f);
    SetVec(s, hero, "velocity", 30, 0, 2);
    Expect(World3DMoveAndSlide(w, hero, 0.1f), "a character moves");
    Vector3 p = GetVec(s, hero, "position"), v = GetVec(s, hero, "velocity");
    Expect(Near(p.x, 3.6f) && Near(p.z, 2.7f) && v.x == 0 && v.z == 2,
           "moving into a wall stops at its face, slides along it, and loses only the blocked "
           "velocity");
    // Along the wall across cell seams without snagging.
    SetVec(s, hero, "velocity", 1, 0, 3);
    World3DMoveAndSlide(w, hero, 0.2f);
    p = GetVec(s, hero, "position");
    Expect(Near(p.x, 3.6f) && Near(p.z, 3.3f),
           "pressed against a wall, it slides past the seams between cells");
    // Onto the floor.
    SetVec(s, hero, "position", 1, 2, 1);
    SetVec(s, hero, "velocity", 0, -50, 0);
    World3DMoveAndSlide(w, hero, 0.1f);
    p = GetVec(s, hero, "position");
    v = GetVec(s, hero, "velocity");
    Expect(Near(p.y, 0) && v.y == 0 && GetBool(s, hero, "on-floor"),
           "falling onto a floor stops at its top, sets on-floor and zeroes velocity y");
    SetVec(s, hero, "velocity", 2, -1, 0);
    World3DMoveAndSlide(w, hero, 0.1f);
    p = GetVec(s, hero, "position");
    Expect(Near(p.y, 0) && Near(p.x, 1.2f) && GetBool(s, hero, "on-floor"),
           "standing on the floor it walks along it and stays on it");
    SetVec(s, hero, "position", 1, 1, 1);
    SetVec(s, hero, "velocity", 0, 1, 0);
    World3DMoveAndSlide(w, hero, 0.1f);
    Expect(!GetBool(s, hero, "on-floor"), "in the air, on-floor is false");
    // Another character blocks.
    StoreId other = Spawn(s, "character", STORE_NULL);
    SetVec(s, other, "position", 2, 0, 6);
    SetVec(s, hero, "position", 0.5f, 0, 6);
    SetVec(s, hero, "velocity", 10, 0, 0);
    World3DMoveAndSlide(w, hero, 0.1f);
    Expect(Near(GetVec(s, hero, "position").x, 1.2f),
           "a character stops at another character's box");
    // Expected failures.
    Expect(!World3DMoveAndSlide(w, sc.floor, 0.1f) && !World3DMoveAndSlide(w, sc.map, 0.1f),
           "move-and-slide refuses a thing that is not a character");
    StoreRemove(s, other);
    Expect(!World3DMoveAndSlide(w, other, 0.1f), "move-and-slide refuses a removed character");
    FreeScene(&sc);
}

// ---- areas -------------------------------------------------------------------------------------
static char areaLog[256];
static bool AreaHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args,
                        int count, void *user)
{
    (void)self;
    (void)user;
    char line[64];
    snprintf(line, sizeof line, "%s:%u ", StoreSymbolName(s, event),
             count ? args[0].as.ref.index : 999u);
    if (strlen(areaLog) + strlen(line) < sizeof areaLog)
        strcat(areaLog, line);
    return true;
}

static void AreaChecks(void)
{
    Scene sc;
    MakeScene(&sc, 3);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    StoreKind trigger = StoreDeclareKind(s, "trigger", StoreKindNamed(s, "area"), NULL, 0, NULL);
    StoreKindSetHandler(s, trigger, AreaHandler, NULL);
    StoreKindHandles(s, trigger, StoreIntern(s, "touched"), true);
    StoreKindHandles(s, trigger, StoreIntern(s, "untouched"), true);
    StoreId area = StoreSpawn(s, trigger, 0, STORE_NULL, STORE_NO_SYMBOL);
    SetVec(s, area, "position", 1, 1, 6);
    SetVec(s, sc.hero, "position", 6, 0, 6);
    StoreId plain = Spawn(s, "area", STORE_NULL); // no handlers: nothing is sent to it
    SetVec(s, plain, "position", 1, 1, 6);
    int inside = StoreFieldIndex(s, trigger, "%inside");
    char expect[64];
    areaLog[0] = 0;
    StoreTick(s, 1.0f / 60);
    Expect(!areaLog[0] && StoreCountOf(s, area, inside) == 0, "an area nobody is in sends nothing");
    World3DTeleport(w, sc.hero, (Vector3){1.5f, 0, 6});
    StoreTick(s, 1.0f / 60);
    snprintf(expect, sizeof expect, "touched:%u ", sc.hero.index);
    Expect(!strcmp(areaLog, expect) && StoreCountOf(s, area, inside) == 1,
           "a character entering an area sends touched with it, and %inside holds it");
    StoreValue v;
    Expect(StoreGetAt(s, area, inside, 0, NULL, &v) && Same(v.as.ref, sc.hero) &&
               StoreCountOf(s, plain, inside) == 1,
           "%inside names the character, also for an area with no handlers");
    areaLog[0] = 0;
    StoreTick(s, 1.0f / 60);
    Expect(!areaLog[0], "staying inside sends nothing more");
    StoreId ids[4];
    Expect(World3DOverlapping(w, area, StoreKindNamed(s, "character"), ids, 4) == 1 &&
               Same(ids[0], sc.hero),
           "overlapping lists the character inside");
    World3DTeleport(w, sc.hero, (Vector3){1, 0, 1});
    StoreTick(s, 1.0f / 60);
    snprintf(expect, sizeof expect, "untouched:%u ", sc.hero.index);
    Expect(!strcmp(areaLog, expect) && StoreCountOf(s, area, inside) == 0,
           "leaving sends untouched and empties %inside");
    Expect(World3DOverlapping(w, area, StoreKindNamed(s, "character"), NULL, 0) == 0,
           "overlapping is empty once it left");
    StoreId marker = Spawn(s, "node", STORE_NULL);
    SetVec(s, marker, "position", 1.2f, 1, 6.2f);
    Expect(World3DOverlapping(w, area, StoreKindNamed(s, "node"), ids, 4) >= 1,
           "a plain node inside the sphere overlaps by its position");
    Expect(World3DOverlapping(w, sc.hero, StoreKindNamed(s, "node"), ids, 4) == -1,
           "overlapping refuses a thing that is not an area");

    // A box area (B6): shape box, size 3 x 2 x 0.5 about (6, 1, 3), so x 4.5..7.5, y 0..2, z
    // 2.75..3.25. Its radius is 0.1, so each place used here is outside the sphere it would be.
    StoreId box = StoreSpawn(s, trigger, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue shape;
    memset(&shape, 0, sizeof shape);
    shape.type = STORE_SYMBOL;
    shape.as.sym = StoreIntern(s, "box");
    SetVec(s, box, "position", 6, 1, 3);
    StoreSet(s, box, StoreFieldIndex(s, trigger, "shape"), &shape);
    SetVec(s, box, "size", 3, 2, 0.5f);
    SetNumber(s, box, "radius", Float(0.1f));
    StoreTick(s, 1.0f / 60);
    areaLog[0] = 0;
    World3DTeleport(w, sc.hero, (Vector3){7.2f, 0, 3});
    StoreTick(s, 1.0f / 60);
    snprintf(expect, sizeof expect, "touched:%u ", sc.hero.index);
    Expect(!strcmp(areaLog, expect) && StoreCountOf(s, box, inside) == 1,
           "a character entering a box area's corner sends touched");
    Expect(World3DOverlapping(w, box, StoreKindNamed(s, "character"), ids, 4) == 1 && Same(ids[0], sc.hero),
           "overlapping a box area lists the character in its corner");
    StoreId inBox = Spawn(s, "node", STORE_NULL), outBox = Spawn(s, "node", STORE_NULL);
    SetVec(s, inBox, "position", 4.8f, 1.9f, 3.1f);
    SetVec(s, outBox, "position", 6, 1, 3.4f);
    int n = World3DOverlapping(w, box, StoreKindNamed(s, "node"), ids, 4);
    bool hasIn = false, hasOut = false;
    for (int i = 0; i < n; i++)
        hasIn = hasIn || Same(ids[i], inBox), hasOut = hasOut || Same(ids[i], outBox);
    Expect(hasIn && !hasOut, "a node inside the box overlaps it, one just past its face does not");
    areaLog[0] = 0;
    World3DTeleport(w, sc.hero, (Vector3){7.2f, 0, 4.5f});
    StoreTick(s, 1.0f / 60);
    snprintf(expect, sizeof expect, "untouched:%u ", sc.hero.index);
    Expect(!strcmp(areaLog, expect) && StoreCountOf(s, box, inside) == 0,
           "a character leaving a box area sends untouched");
    StoreId skip[1] = {sc.hero};
    World3DHit hit = World3DRaycastIgnoring(w, (Vector3){7.3f, 1, 5.5f}, (Vector3){0, 0, -1}, 10, skip, 1, true);
    Expect(hit.hit && Same(hit.thing, box) && Near(hit.distance, 2.25f) && NearVec(hit.normal, (Vector3){0, 0, 1}),
           "a ray that tests areas hits a box area's face, 1.3 m off its centre");
    hit = World3DRaycast(w, (Vector3){7.3f, 1, 5.5f}, (Vector3){0, 0, -1}, 10, sc.hero);
    Expect(!hit.hit || !Same(hit.thing, box), "a plain ray walks through a box area");
    FreeScene(&sc);
}

// ---- rays and line of sight --------------------------------------------------------------------
static void RayChecks(void)
{
    Scene sc;
    MakeScene(&sc, 4);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    SetVec(s, sc.hero, "position", 6, 0, 6);
    World3DHit hit = World3DRaycast(w, (Vector3){1, 1, 2.5f}, (Vector3){2, 0, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 3) &&
               NearVec(hit.normal, (Vector3){-1, 0, 0}) &&
               NearVec(hit.point, (Vector3){4, 1, 2.5f}),
           "a ray hits a solid cell's face, with the tilemap, distance, point and normal");
    hit = World3DRaycast(w, (Vector3){3.5f, 2, 3.5f}, (Vector3){0, 1, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 1) &&
               NearVec(hit.normal, (Vector3){0, -1, 0}),
           "a ray up hits the tilemap's ceiling at its height, facing down");
    hit = World3DRaycast(w, (Vector3){3.5f, 5, 3.5f}, (Vector3){0, -1, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 1) &&
               NearVec(hit.normal, (Vector3){0, 1, 0}),
           "a ray from above lands on the top of the ceiling slab");
    hit = World3DRaycast(w, (Vector3){2, 1, 2.5f}, (Vector3){1, 0, 0}, 1.5f, STORE_NULL);
    Expect(!hit.hit, "a hit beyond the maximum distance is not a hit");
    hit = World3DRaycast(w, (Vector3){7.5f, 1, 6}, (Vector3){-1, 0, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.hero) && Near(hit.distance, 1.1f) &&
               NearVec(hit.normal, (Vector3){1, 0, 0}),
           "a ray hits a character's box");
    hit = World3DRaycast(w, (Vector3){7.5f, 1, 6}, (Vector3){-1, 0, 0}, 100, sc.hero);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 2.5f),
           "the ignored character is passed through and the wall behind it is hit");
    StoreId eye = Spawn(s, "node", sc.hero);
    StoreId held = Spawn(s, "solid", eye);
    SetVec(s, held, "position", 1, 1, 0); // hangs at the hero's side, in the ray's way
    SetVec(s, held, "size", 0.2f, 0.2f, 0.2f);
    hit = World3DRaycast(w, (Vector3){7.5f, 1, 6}, (Vector3){-1, 0, 0}, 100, sc.hero);
    Expect(hit.hit && Same(hit.thing, sc.map), "things under the ignored one are skipped too");
    hit = World3DRaycast(w, (Vector3){7.5f, 1, 6}, (Vector3){-1, 0, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, held), "without ignore the nearer held solid is hit");
    hit = World3DRaycast(w, (Vector3){1, 1, 1}, (Vector3){0, -1, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 1) &&
               NearVec(hit.normal, (Vector3){0, 1, 0}),
           "a ray down meets the tilemap floor and the floor solid at y 0; the lower id wins");
    hit = World3DRaycast(w, (Vector3){-2, 1, 1}, (Vector3){0, -1, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.floor) && Near(hit.distance, 1),
           "outside the tilemap's footprint a ray down hits the floor solid");
    hit = World3DRaycast(w, (Vector3){1, -0.5f, 1}, (Vector3){0, -1, 0}, 100, STORE_NULL);
    Expect(!hit.hit, "a box the ray starts inside is not hit");
    Expect(World3DLineOfSight(w, (Vector3){1, 1, 7.5f}, (Vector3){7, 1, 7.5f}),
           "line of sight along the open row is clear");
    Expect(!World3DLineOfSight(w, (Vector3){1, 1, 2.5f}, (Vector3){7, 1, 2.5f}),
           "line of sight through the wall is blocked");
    StoreRemove(s, eye); // and the solid it holds
    SetVec(s, sc.hero, "position", 3, 0, 7.5f);
    Expect(World3DLineOfSight(w, (Vector3){1, 1, 7.5f}, (Vector3){7, 1, 7.5f}) &&
               World3DRaycast(w, (Vector3){1, 1, 7.5f}, (Vector3){1, 0, 0}, 6, STORE_NULL).hit,
           "a character in the way blocks a ray but not line of sight");
    // Expected failures.
    Expect(!World3DRaycast(w, (Vector3){1, 1, 1}, (Vector3){0, 0, 0}, 100, STORE_NULL).hit,
           "a zero direction hits nothing");
    StoreId gone = Spawn(s, "node", STORE_NULL);
    StoreRemove(s, gone);
    Expect(!World3DRaycast(w, (Vector3){1, 1, 2.5f}, (Vector3){1, 0, 0}, 100, gone).hit,
           "a stale ignore is refused");
    FreeScene(&sc);
}

// ---- tilemap floor and ceiling -----------------------------------------------------------------
static void FloorChecks(void)
{
    Scene sc;
    MakeScene(&sc, 9);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    StoreRemove(s, sc.floor); // only the tilemap holds anyone up now
    StoreId faller = Spawn(s, "character", STORE_NULL);
    SetVec(s, faller, "position", 1.5f, 2, 1.5f);
    SetVec(s, sc.hero, "position", 6.5f, 0.5f, 1.5f);
    for (int i = 0; i < 120; i++)
    {
        Vector3 v = GetVec(s, faller, "velocity");
        SetVec(s, faller, "velocity", v.x, v.y - 9.8f / 60, v.z);
        World3DMoveAndSlide(w, faller, 1.0f / 60);
    }
    Vector3 p = GetVec(s, faller, "position");
    Expect(Near(p.y, 0) && GetBool(s, faller, "on-floor") && GetVec(s, faller, "velocity").y == 0,
           "a character over an open cell falls onto the tilemap floor at y 0, on-floor");
    SetVec(s, sc.hero, "position", 6.5f, 1, 1.5f);
    SetVec(s, sc.hero, "velocity", 0, 20, 0);
    World3DMoveAndSlide(w, sc.hero, 0.1f);
    p = GetVec(s, sc.hero, "position");
    Expect(Near(p.y, 3 - 1.8f) && GetVec(s, sc.hero, "velocity").y == 0 &&
               !GetBool(s, sc.hero, "on-floor"),
           "a character rising stops with its head under the tilemap ceiling");
    World3DHit hit =
        World3DRaycast(w, (Vector3){2.5f, 2, 5.5f}, (Vector3){0, -3, 0}, 100, STORE_NULL);
    Expect(hit.hit && Same(hit.thing, sc.map) && Near(hit.distance, 2) && Near(hit.point.y, 0) &&
               NearVec(hit.normal, (Vector3){0, 1, 0}),
           "a ray aimed down hits the tilemap floor at y 0 with normal (0 1 0)");
    Expect(!World3DLineOfSight(w, (Vector3){2.5f, 2, 5.5f}, (Vector3){2.5f, -2, 5.5f}) &&
               !World3DLineOfSight(w, (Vector3){2.5f, 2, 5.5f}, (Vector3){2.5f, 5, 5.5f}),
           "line of sight is blocked by the tilemap floor and ceiling");
    SetVec(s, faller, "position", -3, 2, 1.5f);
    SetVec(s, faller, "velocity", 0, -10, 0);
    World3DMoveAndSlide(w, faller, 0.5f);
    Expect(Near(GetVec(s, faller, "position").y, -3) && !GetBool(s, faller, "on-floor"),
           "outside the tilemap's footprint there is no floor");
    FreeScene(&sc);
}

// ---- paths -------------------------------------------------------------------------------------
static void PathChecks(void)
{
    Scene sc;
    MakeScene(&sc, 5);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    Vector3 to, from, next;
    int x, z;
    Expect(World3DCellToWorld(w, sc.map, 6, 2, &to) && NearVec(to, (Vector3){6.5f, 0, 2.5f}) &&
               World3DWorldToCell(w, sc.map, (Vector3){6.9f, 5, 2.1f}, &x, &z) && x == 6 && z == 2,
           "cell->world is the cell's centre at y 0, and world->cell inverts it");
    World3DCellToWorld(w, sc.map, 2, 2, &from);
    Vector3 at = from;
    int steps = 0;
    bool adjacent = true, open = true, rounded = false;
    int px = 2, pz = 2;
    while (steps < 40 && World3DPathNext(w, sc.map, at, to, &next) && !Vector3Equals(next, to))
    {
        World3DWorldToCell(w, sc.map, next, &x, &z);
        StoreValue cell;
        StoreGridGet(s, sc.map, StoreFieldIndex(s, StoreKindOf(s, sc.map), "cells"), x, z, &cell);
        adjacent = adjacent && abs(x - px) + abs(z - pz) == 1;
        open = open && cell.as.i == 0;
        rounded = rounded || z == 7;
        px = x;
        pz = z;
        at = next;
        steps++;
    }
    Expect(steps == 13 && adjacent && open && rounded && abs(px - 6) + abs(pz - 2) == 1,
           "path-next walks cell by cell around the wall's end, the shortest way, to the target");
    if (steps != 13)
        printf("      steps: %d\n", steps);
    Vector3 beside;
    World3DCellToWorld(w, sc.map, 3, 2, &beside);
    Expect(
        World3DPathNext(w, sc.map, beside, (Vector3){3.2f, 0, 3.7f}, &next) &&
            Vector3Equals(next, (Vector3){3.2f, 0, 3.7f}) &&
            World3DPathNext(w, sc.map, (Vector3){3.1f, 0, 2.1f}, (Vector3){3.7f, 0, 2.9f}, &next) &&
            Vector3Equals(next, (Vector3){3.7f, 0, 2.9f}),
        "an adjacent or the same cell answers the target itself");
    SetCell(s, sc.map, 4, 7, 1); // close the gap
    StoreTick(s, 1.0f / 60);     // paths are cached for a tick
    Expect(World3DPathNext(w, sc.map, from, to, &next) && Vector3Equals(next, to),
           "with no way through, path-next answers the target itself");
    // Expected failures.
    Expect(!World3DPathNext(w, sc.hero, from, to, &next) &&
               !World3DCellToWorld(w, sc.hero, 0, 0, &next),
           "path-next and cell->world refuse a thing that is not a tilemap");
    Expect(!World3DCellToWorld(w, sc.map, 8, 0, &next) &&
               !World3DWorldToCell(w, sc.map, (Vector3){-1, 0, 1}, &x, &z),
           "cells outside the used area are refused");
    StoreId gone = Spawn(s, "tilemap", STORE_NULL);
    StoreRemove(s, gone);
    Expect(!World3DPathNext(w, gone, from, to, &next) &&
               World3DTilemapChunks(w, gone, NULL, 0) == -1,
           "a removed tilemap is refused");
    FreeScene(&sc);
}

// ---- chunks ------------------------------------------------------------------------------------
static bool FacesOut(const MB *m)
{
    for (size_t t = 0; t + 2 < m->count; t += 3)
    {
        const float *v = m->vertices + t * 3, *n = m->normals + t * 3;
        Vector3 a = {v[0], v[1], v[2]}, b = {v[3], v[4], v[5]}, c = {v[6], v[7], v[8]};
        Vector3 cross = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        if (Vector3DotProduct(cross, (Vector3){n[0], n[1], n[2]}) <= 0)
            return false;
    }
    return true;
}

static void ChunkChecks(void)
{
    Scene sc;
    MakeScene(&sc, 6);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    World3DChunk chunks[8];
    int n = World3DTilemapChunks(w, sc.map, chunks, 8);
    // 57 open cells; walls: 7 faces each side of the column, plus its open end.
    Expect(
        n == 1 && chunks[0].changed && chunks[0].floor.count == 57 * 6 &&
            chunks[0].ceiling.count == 57 * 6 && chunks[0].wall.count == 15 * 6,
        "an 8 x 8 map is one chunk: a floor and a ceiling per open cell, a wall per exposed face");
    Expect(FacesOut(&chunks[0].floor) && FacesOut(&chunks[0].wall) && FacesOut(&chunks[0].ceiling),
           "every triangle winds toward its normal");
    Expect(Near(chunks[0].bounds.max.x, 8) && Near(chunks[0].bounds.max.y, 3) &&
               chunks[0].wall.uvs[1] >= 0,
           "a chunk's bounds cover its cells and the tilemap's height");
    uint64_t hash = chunks[0].hash;
    n = World3DTilemapChunks(w, sc.map, chunks, 8);
    Expect(n == 1 && !chunks[0].changed && chunks[0].hash == hash, "unchanged cells: no rebuild");
    SetCell(s, sc.map, 1, 1, 1);
    n = World3DTilemapChunks(w, sc.map, chunks, 8);
    Expect(n == 1 && chunks[0].changed && chunks[0].hash != hash &&
               chunks[0].floor.count == 56 * 6 && chunks[0].wall.count == 19 * 6,
           "a changed cell rebuilds its chunk");
    // A 16 x 16 map far away: four chunks; a change inside one rebuilds only that one.
    StoreId big = Spawn(s, "tilemap", STORE_NULL);
    SetVec(s, big, "position", 100, 0, 0);
    Expect(World3DTilemapChunks(w, big, NULL, 0) == 4, "a 16 x 16 map is four chunks");
    n = World3DTilemapChunks(w, big, chunks, 8);
    Expect(n == 4 && chunks[1].chunkX == 1 && chunks[1].chunkZ == 0 &&
               Near(chunks[1].bounds.min.x, 16) && Near(chunks[3].bounds.max.z, 32),
           "chunks come in row-major order with their own bounds, in the tilemap's space");
    SetCell(s, big, 3, 3, 1);
    n = World3DTilemapChunks(w, big, chunks, 8);
    Expect(n == 4 && chunks[0].changed && !chunks[1].changed && !chunks[2].changed &&
               !chunks[3].changed,
           "only the chunk holding the changed cell is rebuilt");
    SetCell(s, big, 7, 3, 1); // on the border: the neighbour's wall-facing ring changes too
    n = World3DTilemapChunks(w, big, chunks, 2);
    Expect(n == 2 && chunks[0].changed && chunks[1].changed,
           "a border cell rebuilds both chunks; max caps the count");
    SetNumber(s, big, "width", Int(4));
    Expect(World3DTilemapChunks(w, big, NULL, 0) == 2, "only the used area makes chunks");
    StoreRemove(s, big); // its geometry is released
    Expect(World3DTilemapChunks(w, sc.hero, chunks, 8) == -1,
           "chunks refuse a thing that is not a tilemap");
    FreeScene(&sc);
}

// ---- transforms --------------------------------------------------------------------------------
static int callerSpawned, callerErrors;
static void CallerSpawned(void *user, StoreId id)
{
    (void)id;
    callerSpawned += user == &callerSpawned;
}
static void CallerError(void *user, const char *message)
{
    (void)message;
    callerErrors += user == &callerSpawned;
}

static Vector3 At(World3D *w, StoreId id)
{
    Matrix m;
    return World3DWorldMatrix(w, id, &m) ? (Vector3){m.m12, m.m13, m.m14}
                                         : (Vector3){-999, -999, -999};
}

static void TransformChecks(void)
{
    Store store;
    World3D world;
    Store *s = &store;
    World3D *w = &world;
    StoreInit(s, 7);
    World3DInit(w, s);
    StoreHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.user = &callerSpawned;
    hooks.spawned = CallerSpawned;
    hooks.error = CallerError;
    World3DHooks(w, &hooks);
    StoreSetHooks(s, &hooks);
    callerSpawned = callerErrors = 0;
    StoreId parent = Spawn(s, "node", STORE_NULL), child = Spawn(s, "node", parent);
    StoreId still = Spawn(s, "node", STORE_NULL), under = Spawn(s, "model", still);
    SetVec(s, parent, "position", 10, 0, 0);
    SetVec(s, child, "position", 1, 0, 0);
    SetVec(s, still, "position", 0, 0, 5);
    SetVec(s, under, "position", 0, 1, 0);
    Expect(callerSpawned == 4, "the caller's own hooks still run behind world3d's");
    StoreValue bad = Int(1);
    StoreSet(s, parent, 0, &bad);
    Expect(callerErrors == 1, "the caller's error hook is relayed with its own user");
    World3DBeginTick(w);
    World3DUpdateTransforms(w, 1);
    Expect(NearVec(At(w, child), (Vector3){11, 0, 0}) && NearVec(At(w, under), (Vector3){0, 1, 5}),
           "a child's world matrix is its parent's times its own");
    Matrix underBefore;
    World3DWorldMatrix(w, under, &underBefore);
    SetVec(s, parent, "position", 20, 0, 0);
    Vector3 now;
    Expect(World3DWorldPosition(w, child, &now) && NearVec(now, (Vector3){21, 0, 0}) &&
               NearVec(At(w, child), (Vector3){11, 0, 0}),
           "world-position reads the tick's state at once; the drawn matrix waits for the frame");
    World3DUpdateTransforms(w, 1);
    Matrix underAfter;
    World3DWorldMatrix(w, under, &underAfter);
    Expect(NearVec(At(w, child), (Vector3){21, 0, 0}), "a child under a moved parent moves");
    Expect(!memcmp(&underBefore, &underAfter, sizeof underBefore),
           "a child under an unmoved parent keeps last frame's matrix bit for bit");
    // Interpolation between ticks.
    World3DBeginTick(w);
    SetVec(s, parent, "position", 30, 0, 0);
    World3DUpdateTransforms(w, 0.5f);
    Expect(NearVec(At(w, parent), (Vector3){25, 0, 0}) &&
               NearVec(At(w, child), (Vector3){26, 0, 0}),
           "the frame interpolates %prev -> now by alpha, and children follow");
    World3DUpdateTransforms(w, 0.75f);
    Expect(NearVec(At(w, parent), (Vector3){27.5f, 0, 0}),
           "a later frame of the same tick moves on");
    Expect(World3DTeleport(w, parent, (Vector3){40, 0, 0}), "a node teleports");
    World3DUpdateTransforms(w, 0.5f);
    Expect(NearVec(At(w, parent), (Vector3){40, 0, 0}) &&
               NearVec(At(w, child), (Vector3){41, 0, 0}),
           "a teleport is not interpolated");
    World3DBeginTick(w);
    SetVec(s, parent, "rotation", 0, PI / 2, 0);
    World3DUpdateTransforms(w, 1);
    Expect(NearVec(At(w, child), (Vector3){40, 0, -1}),
           "rotation about Y turns a child around its parent");
    World3DUpdateTransforms(w, 0.5f);
    Vector3 half = At(w, child);
    Expect(Near(Vector3Distance(half, (Vector3){40, 0, 0}), 1) && half.x > 40.5f && half.z < -0.5f,
           "rotation is slerped: half way is 45 degrees");
    // Teleporting a child converts the world position into its parent's space.
    World3DTeleport(w, child, (Vector3){40, 0, 3});
    Expect(World3DWorldPosition(w, child, &now) && NearVec(now, (Vector3){40, 0, 3}),
           "teleporting a child puts it at the world position asked for");
    // A parent change is a jump.
    World3DBeginTick(w);
    SetVec(s, child, "position", 2, 0, 0);
    StoreAttach(s, child, still);
    World3DUpdateTransforms(w, 0.5f);
    Expect(NearVec(At(w, child), (Vector3){2, 0, 5}),
           "a thing that changed parent is not interpolated");
    // Orphans keep their world transform.
    StoreId guest = Spawn(s, "node", STORE_NULL);
    SetVec(s, guest, "position", 0, 0, 2);
    StoreAttach(s, guest, parent);
    Vector3 was;
    World3DWorldPosition(w, guest, &was);
    StoreRemove(s, parent);
    Expect(StoreAlive(s, guest) && StoreParent(s, guest).index == UINT32_MAX &&
               NearVec(GetVec(s, guest, "position"), was) && NearVec(was, (Vector3){42, 0, 0}),
           "a guest of a removed node is left where it was in the world");
    // Expected failures on stale ids and non-nodes.
    Matrix m;
    Expect(!World3DWorldMatrix(w, parent, &m) && !World3DWorldPosition(w, parent, &now) &&
               !World3DTeleport(w, parent, now),
           "transforms refuse a removed thing");
    StoreKind plainKind = StoreDeclareKind(s, "bag", -1, NULL, 0, NULL);
    StoreId bag = StoreSpawn(s, plainKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    Expect(!World3DWorldPosition(w, bag, &now), "a thing that is not a node has no world position");
    World3DFree(w);
    StoreFree(s);
}

// ---- sockets that follow bones in drawing only (§3 Socket) --------------------------------------
/* A fake bone lookup: `arms` answers as armsAnswer says, `body` has hand.R posed at (-1, 1, 0) in
   model space (the test rig's wave at its top) unless bodyDrawn is false; every call is counted. */
static World3DBone armsAnswer;
static bool bodyDrawn;
static int lookups;
static StoreId armsThing, bodyThing;

static World3DBone FakeBones(void *user, StoreId model, const char *bone, Matrix *out)
{
    (void)user;
    lookups++;
    if (Same(model, armsThing))
    {
        if (armsAnswer == WORLD3D_BONE_POSED)
            *out = MatrixTranslate(0, 5, 0);
        return armsAnswer;
    }
    if (!Same(model, bodyThing) || !bodyDrawn)
        return WORLD3D_BONE_NOT_DRAWN;
    if (strcmp(bone, "hand.R"))
        return WORLD3D_BONE_NONE;
    *out = MatrixTranslate(-1, 1, 0);
    return WORLD3D_BONE_POSED;
}

static Vector3 Drawn(World3D *w, StoreId id)
{
    Matrix m;
    return World3DDrawMatrix(w, id, &m) ? (Vector3){m.m12, m.m13, m.m14} : (Vector3){-999, -999, -999};
}

static void SocketChecks(void)
{
    Store store;
    World3D world;
    Store *s = &store;
    World3D *w = &world;
    StoreInit(s, 3);
    World3DInit(w, s);
    StoreKind node = StoreKindNamed(s, "node"), model = StoreKindNamed(s, "model"), socketKind = StoreKindNamed(s, "socket");
    /* A soldier at (5, 0, 0): eye > arms, body, and a hand socket at (0.3, 1, 0) following
       (arms body) with a box guest 0.1 in front of it. */
    StoreId soldier = StoreSpawn(s, node, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreId eye = StoreSpawn(s, node, 0, soldier, StoreIntern(s, "eye"));
    armsThing = StoreSpawn(s, model, 0, eye, StoreIntern(s, "arms"));
    bodyThing = StoreSpawn(s, model, 0, soldier, StoreIntern(s, "body"));
    StoreId hand = StoreSpawn(s, socketKind, 0, soldier, StoreIntern(s, "hand"));
    StoreId box = Spawn(s, "model", STORE_NULL);
    SetVec(s, soldier, "position", 5, 0, 0);
    SetVec(s, eye, "position", 0, 1.6f, 0);
    SetVec(s, hand, "position", 0.3f, 1, 0);
    SetVec(s, box, "position", 0, 0, 0.1f);
    StoreValue bone;
    memset(&bone, 0, sizeof bone);
    bone.type = STORE_STRING;
    snprintf(bone.as.str, sizeof bone.as.str, "hand.R");
    StoreSet(s, hand, StoreFieldIndex(s, socketKind, "bone"), &bone);
    StoreValue of[2];
    memset(of, 0, sizeof of);
    of[0].type = of[1].type = STORE_SYMBOL;
    of[0].as.sym = StoreIntern(s, "arms");
    of[1].as.sym = StoreIntern(s, "body");
    Expect(StoreSetList(s, hand, StoreFieldIndex(s, socketKind, "of"), of, 2) && StoreAttach(s, box, hand),
           "sockets: a hand following (arms body) holds a box");
    World3DBeginTick(w);
    World3DUpdateTransforms(w, 1);
    const Vector3 rest = {5.3f, 1, 0.1f}, onBone = {4, 1, 0.1f};
    Vector3 gameplay;

    /* With no lookup, drawing and gameplay agree on the rest pose. */
    Expect(NearVec(Drawn(w, box), rest) && NearVec(At(w, box), rest) && World3DWorldPosition(w, box, &gameplay) &&
               NearVec(gameplay, rest),
           "sockets: with no bone lookup the guest draws at the socket's rest pose, as gameplay reads it");

    /* The arms are not drawn here, the body is and is posed: the box draws on the body's hand. */
    World3DSetBoneLookup(w, FakeBones, NULL);
    armsAnswer = WORLD3D_BONE_NOT_DRAWN;
    bodyDrawn = true;
    Vector3 drawn = Drawn(w, box), socketDrawn = Drawn(w, hand);
    printf("world3d sockets: box drawn at (%.2f %.2f %.2f), gameplay world position (%.2f %.2f %.2f)\n", drawn.x,
           drawn.y, drawn.z, gameplay.x, gameplay.y, gameplay.z);
    Expect(NearVec(drawn, onBone) && NearVec(socketDrawn, (Vector3){4, 1, 0}),
           "sockets: a guest under a socket draws at the bone of the first drawn model of its :of");
    Expect(World3DWorldPosition(w, box, &gameplay) && NearVec(gameplay, rest) && NearVec(At(w, box), rest),
           "sockets: gameplay's world position and world matrix stay the rest pose while drawing follows the bone");
    Expect(NearVec(Drawn(w, bodyThing), (Vector3){5, 0, 0}) && NearVec(Drawn(w, eye), (Vector3){5, 1.6f, 0}),
           "sockets: things not under a socket draw at their world matrix");
    armsAnswer = WORLD3D_BONE_POSED;
    Expect(NearVec(Drawn(w, box), (Vector3){5, 6.6f, 0.1f}),
           "sockets: the first listed model drawn here wins (the arms' bone, times the arms' matrix)");
    armsAnswer = WORLD3D_BONE_NONE;
    Expect(NearVec(Drawn(w, box), rest), "sockets: the first drawn model unposed leaves the socket at rest");

    /* No :of: a socket straight under a model follows it. */
    StoreId grip = StoreSpawn(s, socketKind, 0, bodyThing, StoreIntern(s, "grip"));
    StoreSet(s, grip, StoreFieldIndex(s, socketKind, "bone"), &bone);
    StoreId held = Spawn(s, "node", grip);
    World3DBeginTick(w);
    World3DUpdateTransforms(w, 1);
    Expect(NearVec(Drawn(w, held), (Vector3){4, 1, 0}), "sockets: with no :of, a socket follows its parent model");

    /* Expected failures. */
    bodyDrawn = false;
    armsAnswer = WORLD3D_BONE_NOT_DRAWN;
    Expect(NearVec(Drawn(w, box), rest) && NearVec(Drawn(w, held), (Vector3){5, 0, 0}),
           "sockets: a model not drawn here leaves its socket at rest");
    bodyDrawn = true;
    snprintf(bone.as.str, sizeof bone.as.str, "hand.L");
    StoreSet(s, hand, StoreFieldIndex(s, socketKind, "bone"), &bone);
    Expect(NearVec(Drawn(w, box), rest), "sockets: a bone the model does not have leaves the socket at rest");
    bone.as.str[0] = 0;
    StoreSet(s, hand, StoreFieldIndex(s, socketKind, "bone"), &bone);
    lookups = 0;
    Expect(NearVec(Drawn(w, box), rest) && lookups == 0, "sockets: a socket with no bone asks nothing and stays at rest");
    StoreId bag = StoreSpawn(s, StoreDeclareKind(s, "bagged", -1, NULL, 0, NULL), 0, STORE_NULL, STORE_NO_SYMBOL);
    Matrix m;
    StoreRemove(s, box);
    Expect(!World3DDrawMatrix(w, box, &m) && !World3DDrawMatrix(w, bag, &m),
           "sockets: the draw matrix refuses a removed thing and a thing that is not a node");
    World3DFree(w);
    StoreFree(s);
}

// ---- nearest -----------------------------------------------------------------------------------
static bool NotSecond(StoreId id, void *user) { return !Same(id, *(StoreId *)user); }

static void NearestChecks(void)
{
    Scene sc;
    MakeScene(&sc, 8);
    Store *s = &sc.s;
    World3D *w = &sc.w;
    StoreKind camera = StoreKindNamed(s, "camera");
    StoreId a = Spawn(s, "camera", STORE_NULL), b = Spawn(s, "camera", STORE_NULL),
            c = Spawn(s, "camera", STORE_NULL);
    SetVec(s, a, "position", 3, 0, 0);
    SetVec(s, b, "position", -3, 0, 0);
    SetVec(s, c, "position", 1, 0, 1);
    Expect(Same(World3DNearest(w, camera, (Vector3){0, 0, 0}, -1, NULL, NULL), c),
           "nearest picks the closest");
    Expect(Same(World3DNearest(w, camera, (Vector3){0, 0, 0}, -1, NotSecond, &c), a),
           "a tie goes to the lower id, and accept filters");
    Expect(World3DNearest(w, camera, (Vector3){0, 0, 0}, 1, NULL, NULL).index == UINT32_MAX,
           "nothing within the maximum distance answers none");
    Expect(World3DNearest(w, -1, (Vector3){0, 0, 0}, -1, NULL, NULL).index == UINT32_MAX &&
               World3DNearest(w, 999, (Vector3){0, 0, 0}, -1, NULL, NULL).index == UINT32_MAX,
           "nearest of an unknown kind answers none");
    FreeScene(&sc);
}

// ---- determinism -------------------------------------------------------------------------------
static bool WalkHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args,
                        int count, void *user)
{
    (void)event;
    (void)count;
    World3D *w = user;
    int velocity = StoreFieldIndex(s, StoreKindOf(s, self), "velocity");
    StoreValue v;
    StoreGet(s, self, velocity, &v);
    v.as.v.x = (float)StoreRandom(s, self, 9) - 4.0f;
    v.as.v.z = (float)StoreRandom(s, self, 9) - 4.0f;
    v.as.v.y -= 9.8f * args[0].as.f;
    StoreSet(s, self, velocity, &v);
    World3DMoveAndSlide(w, self, args[0].as.f);
    return true;
}

static uint64_t Walk(uint64_t seed, Vector3 *where)
{
    Scene sc;
    MakeScene(&sc, seed);
    StoreKind walker =
        StoreDeclareKind(&sc.s, "walker", StoreKindNamed(&sc.s, "character"), NULL, 0, NULL);
    StoreKindSetHandler(&sc.s, walker, WalkHandler, &sc.w);
    StoreKindHandles(&sc.s, walker, StoreIntern(&sc.s, "tick"), true);
    StoreId a = StoreSpawn(&sc.s, walker, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreId b = StoreSpawn(&sc.s, walker, 0, STORE_NULL, STORE_NO_SYMBOL);
    SetVec(&sc.s, a, "position", 2, 1, 3);
    SetVec(&sc.s, b, "position", 3, 1, 3);
    for (int i = 0; i < 60; i++)
    {
        World3DBeginTick(&sc.w);
        StoreTick(&sc.s, 1.0f / 60);
    }
    *where = GetVec(&sc.s, a, "position");
    uint64_t hash = StoreHash(&sc.s);
    FreeScene(&sc);
    return hash;
}

static void DeterminismChecks(void)
{
    Vector3 p1, p2;
    uint64_t one = Walk(42, &p1), two = Walk(42, &p2);
    Expect(one == two && Vector3Equals(p1, p2),
           "two fresh stores walking the same 60 ticks hash the same");
    Expect(!NearVec(p1, (Vector3){2, 1, 3}) && Near(p1.y, 0) && (p1.x < 3.61f || p1.z > 6.59f),
           "the walkers moved, landed on the floor and stayed left of the wall");
}

int World3DChecks(void)
{
    failures = 0;
    KindChecks();
    SlideChecks();
    AreaChecks();
    RayChecks();
    FloorChecks();
    PathChecks();
    ChunkChecks();
    TransformChecks();
    SocketChecks();
    NearestChecks();
    DeterminismChecks();
    return failures;
}
