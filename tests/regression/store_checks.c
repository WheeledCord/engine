/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The world store (core/store.h): kinds, fields, the tree, the tick, the rules, snapshots, hashes
// and saves, each with what must work and what must be refused. docs/developer/store.md §8.
#include "checks.h"
#include "core/store.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: store: %s\n", what);
        failures++;
    }
}

static bool Prefix(const Store *s, const char *prefix)
{
    return !strncmp(StoreLastError(s), prefix, strlen(prefix));
}

static StoreValue Int(int32_t i)
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

static StoreValue Bool(bool b)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_BOOL;
    v.as.b = b;
    return v;
}

static StoreValue Sym(Store *s, const char *name)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_SYMBOL;
    v.as.sym = StoreIntern(s, name);
    return v;
}

static StoreValue Str(const char *text)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_STRING;
    snprintf(v.as.str, sizeof v.as.str, "%s", text);
    return v;
}

static StoreValue Vec(float x, float y, float z)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_VEC3;
    v.as.v = (Vector3){x, y, z};
    return v;
}

static StoreValue Ref(StoreId id)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_REF;
    v.as.ref = id;
    return v;
}

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

static StoreFieldDecl Collection(const char *name, StoreType type, StoreType element, StoreType key,
                                 int max, int height)
{
    StoreFieldDecl d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    d.element = element;
    d.key = key;
    d.max = max;
    d.height = height;
    return d;
}

static StoreValue None(void)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    return v;
}

static bool Same(StoreId a, StoreId b) { return a.index == b.index && a.generation == b.generation; }

static int GetInt(Store *s, StoreId id, int field)
{
    StoreValue v;
    return StoreGet(s, id, field, &v) && v.type == STORE_INT ? v.as.i : -99999;
}

static const char *Scratch(const char *name)
{
    static char path[256];
    FILE *probe = fopen("build/core/.store_probe", "w");
    if (probe)
    {
        fclose(probe);
        remove("build/core/.store_probe");
    }
    snprintf(path, sizeof path, probe ? "build/core/%s" : "%s", name);
    return path;
}

// Warnings through TraceLog, counted by what they say.
static int warnUnhandled, warnDropped, warnLoading;

static void CountWarnings(int level, const char *text, va_list args)
{
    char line[512];
    vsnprintf(line, sizeof line, text, args);
    if (level != LOG_WARNING)
        return;
    warnUnhandled += strstr(line, "has no handler for") != NULL;
    warnDropped += strstr(line, "dropping the rest") != NULL;
    warnLoading += strstr(line, "STORE: loading") != NULL;
}

// ---- kinds and inheritance --------------------------------------------------------------------
static void KindChecks(void)
{
    Store s;
    Expect(StoreInit(&s, 1), "a store initialises");
    Expect(StoreIntern(&s, "start") == 0 && StoreIntern(&s, "tick") == 1 &&
               StoreIntern(&s, "draw-hud") == 3,
           "the engine's own events have fixed symbols");
    StoreSymbol a = StoreIntern(&s, "alpha");
    Expect(a == StoreIntern(&s, "alpha") && !strcmp(StoreSymbolName(&s, a), "alpha") &&
               StoreIntern(&s, "") == STORE_NO_SYMBOL && !StoreSymbolName(&s, 9999),
           "symbols intern once and name themselves");
    StoreFieldDecl baseFields[] = {Field("hp", STORE_INT, 0, Int(100)),
                                   Field("pos", STORE_VEC3, 0, None()),
                                   Field("hurt", STORE_FLOAT, STORE_LOCAL, None()),
                                   Field("tag", STORE_SYMBOL, 0, None())};
    const char *error = NULL;
    StoreKind base = StoreDeclareKind(&s, "actor", -1, baseFields, 4, &error);
    StoreFieldDecl soldierFields[] = {Field("hp", STORE_INT, 0, Int(150)),
                                      Field("ammo", STORE_INT, 0, Int(7))};
    StoreKind soldier = StoreDeclareKind(&s, "soldier", base, soldierFields, 2, &error);
    Expect(base >= 0 && soldier >= 0, "a kind and a derived kind declare");
    Expect(StoreFieldCount(&s, soldier) == 5 && StoreFieldIndex(&s, soldier, "hp") == 0 &&
               StoreFieldIndex(&s, soldier, "tag") == StoreFieldIndex(&s, base, "tag") &&
               StoreFieldIndex(&s, soldier, "ammo") == 4 && StoreFieldIndex(&s, base, "ammo") == -1,
           "a derived kind's fields are its base's first, at the same indices, then its own");
    Expect(StoreFieldAt(&s, soldier, 0)->init.as.i == 150 && StoreFieldAt(&s, base, 0)->init.as.i == 100,
           "redeclaring a base field changes only the derived kind's default");
    Expect(StoreKindIs(&s, soldier, base) && StoreKindIs(&s, soldier, soldier) &&
               !StoreKindIs(&s, base, soldier) && StoreKindBase(&s, soldier) == base &&
               StoreKindBase(&s, base) == -1 && StoreKindNamed(&s, "soldier") == soldier &&
               !strcmp(StoreKindName(&s, soldier), "soldier") && StoreKindNamed(&s, "nobody") == -1,
           "kinds answer their base chain and names");
    StoreId t = StoreSpawn(&s, soldier, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue v;
    Expect(GetInt(&s, t, 0) == 150 && GetInt(&s, t, 4) == 7 && StoreGet(&s, t, 3, &v) &&
               v.type == STORE_SYMBOL && v.as.sym == STORE_NO_SYMBOL,
           "a spawned thing holds its kind's defaults; a symbol's zero is no symbol");
    Expect(StoreKindSetDefault(&s, soldier, 0, &(StoreValue){STORE_INT, {.i = 175}}) &&
               GetInt(&s, StoreSpawn(&s, soldier, 0, STORE_NULL, STORE_NO_SYMBOL), 0) == 175 &&
               GetInt(&s, t, 0) == 150,
           "a changed default applies to things spawned afterwards");
    StoreValue symbol = Sym(&s, "x");
    Expect(!StoreKindSetDefault(&s, soldier, 0, &symbol) && Prefix(&s, "type"),
           "a default of the wrong type is refused");
    // Expected failures.
    Expect(StoreDeclareKind(&s, "soldier", -1, NULL, 0, &error) == -1 && error && *error,
           "a second kind of the same name is refused with a reason");
    StoreFieldDecl retyped[] = {Field("hp", STORE_FLOAT, 0, None())};
    Expect(StoreDeclareKind(&s, "medic", base, retyped, 1, &error) == -1 && strstr(error, "hp"),
           "redeclaring a base field with another type is refused");
    Expect(StoreDeclareKind(&s, "ghost", 42, NULL, 0, &error) == -1, "an unknown base is refused");
    StoreFieldDecl twice[] = {Field("x", STORE_INT, 0, None()), Field("x", STORE_INT, 0, None())};
    Expect(StoreDeclareKind(&s, "twice", -1, twice, 2, &error) == -1,
           "a field declared twice is refused");
    StoreFieldDecl strings[] = {Collection("names", STORE_LIST, STORE_STRING, STORE_NONE, 4, 0)};
    Expect(StoreDeclareKind(&s, "strings", -1, strings, 1, &error) == -1,
           "a list of strings is refused");
    StoreFieldDecl floatKeys[] = {Collection("m", STORE_MAP, STORE_INT, STORE_FLOAT, 4, 0)};
    Expect(StoreDeclareKind(&s, "floatkeys", -1, floatKeys, 1, &error) == -1,
           "a map keyed by floats is refused");
    StoreFieldDecl badInit[] = {Field("n", STORE_INT, 0, Str("seven"))};
    Expect(StoreDeclareKind(&s, "badinit", -1, badInit, 1, &error) == -1,
           "a default of the wrong type is refused at declaration");
    Expect(StoreSpawn(&s, 99, 0, STORE_NULL, STORE_NO_SYMBOL).index == UINT32_MAX,
           "spawning an unknown kind is refused");
    uint64_t kinds = StoreKindsHash(&s);
    StoreFieldDecl extra[] = {Field("extra", STORE_INT, 0, None())};
    Expect(kinds == StoreKindsHash(&s) && StoreDeclareKind(&s, "later", base, extra, 1, NULL) >= 0 &&
               StoreKindsHash(&s) != kinds,
           "the kinds hash is stable, and declaring another field changes it");
    StoreFree(&s);
}

// ---- every field type and collection ----------------------------------------------------------
static void FieldChecks(void)
{
    Store s;
    StoreInit(&s, 2);
    // Interned in reverse alphabetical order, so name order and id order disagree.
    StoreSymbol zeta = StoreIntern(&s, "zeta"), mid = StoreIntern(&s, "mid"), alpha = StoreIntern(&s, "alpha");
    StoreFieldDecl fields[] = {
        Field("i", STORE_INT, 0, None()),
        Field("f", STORE_FLOAT, 0, Float(1.5f)),
        Field("b", STORE_BOOL, 0, Bool(true)),
        Field("s", STORE_SYMBOL, 0, None()),
        Field("str", STORE_STRING, 0, Str("hello")),
        Field("v", STORE_VEC3, 0, Vec(1, 2, 3)),
        Field("r", STORE_REF, 0, None()),
        Collection("list", STORE_LIST, STORE_SYMBOL, STORE_NONE, 3, 0),
        Collection("set", STORE_SET, STORE_INT, STORE_NONE, 4, 0),
        Collection("symset", STORE_SET, STORE_SYMBOL, STORE_NONE, 4, 0),
        Collection("map", STORE_MAP, STORE_INT, STORE_SYMBOL, 2, 0),
        Collection("grid", STORE_GRID, STORE_INT, STORE_NONE, 4, 3),
        Collection("refset", STORE_SET, STORE_REF, STORE_NONE, 3, 0),
    };
    enum { I, F, B, S, STR, V, R, LIST, SET, SYMSET, MAP, GRID, REFSET };
    StoreKind k = StoreDeclareKind(&s, "all", -1, fields, 13, NULL);
    StoreValue item = Sym(&s, "pistol");
    Expect(StoreKindSetDefaultAt(&s, k, LIST, 0, NULL, &item), "a list default is set");
    StoreId t = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue v, key;
    Expect(StoreGet(&s, t, F, &v) && v.as.f == 1.5f && StoreGet(&s, t, B, &v) && v.as.b &&
               StoreGet(&s, t, STR, &v) && !strcmp(v.as.str, "hello") && StoreGet(&s, t, V, &v) &&
               v.as.v.y == 2 && StoreGet(&s, t, R, &v) && v.as.ref.index == UINT32_MAX,
           "scalar defaults read back, and a ref's zero is none");
    Expect(StoreCountOf(&s, t, LIST) == 1 && StoreGetAt(&s, t, LIST, 0, NULL, &v) &&
               v.as.sym == item.as.sym,
           "a collection default (:init) is in a new thing");
    // Scalars: each type, and conversions.
    StoreValue x = Float(3.0f);
    Expect(StoreSet(&s, t, I, &x) && GetInt(&s, t, I) == 3, "a whole float converts to an int field");
    x = Float(2.5f);
    Expect(!StoreSet(&s, t, I, &x) && Prefix(&s, "type") && GetInt(&s, t, I) == 3,
           "an int field refuses 2.5");
    x = Int(4);
    Expect(StoreSet(&s, t, F, &x) && StoreGet(&s, t, F, &v) && v.type == STORE_FLOAT && v.as.f == 4.0f,
           "an int converts to a float field");
    Expect(!StoreSet(&s, t, B, &x) && Prefix(&s, "type"), "a bool field refuses an int");
    x = Sym(&s, "idle");
    Expect(StoreSet(&s, t, S, &x) && StoreGet(&s, t, S, &v) && v.as.sym == x.as.sym, "symbols store");
    x = Str("a \"quoted\" \\ string");
    Expect(StoreSet(&s, t, STR, &x) && StoreGet(&s, t, STR, &v) && !strcmp(v.as.str, x.as.str),
           "strings store");
    memset(x.as.str, 'x', sizeof x.as.str);
    Expect(!StoreSet(&s, t, STR, &x), "a string with no end is refused");
    x = Vec(-1, 0.5f, 9);
    Expect(StoreSet(&s, t, V, &x) && StoreGet(&s, t, V, &v) && v.as.v.x == -1 && v.as.v.z == 9,
           "vectors store");
    x = Ref(t);
    Expect(StoreSet(&s, t, R, &x) && StoreGet(&s, t, R, &v) && Same(v.as.ref, t), "refs store");
    Expect(!StoreGet(&s, t, LIST, &v) && Prefix(&s, "type"), "a collection is not read as a scalar");
    Expect(!StoreSet(&s, t, LIST, &x) && Prefix(&s, "type"), "a collection is not written as a scalar");
    Expect(!StoreGet(&s, t, 99, &v), "a field out of range is refused");
    // LIST: order as given, capacity refused and the field left alone.
    StoreValue list[4] = {Sym(&s, "c"), Sym(&s, "a"), Sym(&s, "b"), Sym(&s, "d")};
    Expect(StoreSetList(&s, t, LIST, list, 3) && StoreCountOf(&s, t, LIST) == 3 &&
               StoreGetAt(&s, t, LIST, 0, NULL, &v) && v.as.sym == list[0].as.sym,
           "a list keeps the order it was given");
    Expect(!StoreSetList(&s, t, LIST, list, 4) && Prefix(&s, "capacity") &&
               strstr(StoreLastError(&s), "list holds 3 items at most") &&
               StoreCountOf(&s, t, LIST) == 3,
           "a list refuses its 4th item when it holds 3, unchanged");
    StoreValue wrong[1] = {Int(1)};
    Expect(!StoreSetList(&s, t, LIST, wrong, 1) && Prefix(&s, "type"), "a list refuses the wrong element type");
    // SET: sorted, deduplicated, capacity after deduplication.
    StoreValue ints[5] = {Int(5), Int(1), Int(3), Int(1), Int(-2)};
    int got[4] = {0};
    Expect(StoreSetList(&s, t, SET, ints, 5) && StoreCountOf(&s, t, SET) == 4, "a set drops duplicates");
    for (int i = 0; i < 4; i++)
        got[i] = StoreGetAt(&s, t, SET, i, NULL, &v) ? v.as.i : 99;
    Expect(got[0] == -2 && got[1] == 1 && got[2] == 3 && got[3] == 5, "a set of ints is sorted by value");
    StoreValue five[5] = {Int(1), Int(2), Int(3), Int(4), Int(5)};
    Expect(!StoreSetList(&s, t, SET, five, 5) && Prefix(&s, "capacity") && StoreCountOf(&s, t, SET) == 4,
           "a set refuses a 5th distinct element when it holds 4");
    StoreValue names[3] = {{STORE_SYMBOL, {.sym = zeta}}, {STORE_SYMBOL, {.sym = alpha}},
                           {STORE_SYMBOL, {.sym = mid}}};
    StoreSymbol order[3] = {0};
    StoreSetList(&s, t, SYMSET, names, 3);
    for (int i = 0; i < 3; i++)
        order[i] = StoreGetAt(&s, t, SYMSET, i, NULL, &v) ? v.as.sym : -5;
    Expect(order[0] == alpha && order[1] == mid && order[2] == zeta,
           "a set of symbols is sorted by name, not by intern order");
    StoreId u = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue refs[2] = {Ref(u), Ref(t)};
    Expect(StoreSetList(&s, t, REFSET, refs, 2) && StoreGetAt(&s, t, REFSET, 0, NULL, &v) &&
               Same(v.as.ref, t),
           "a set of refs is sorted by index");
    // MAP: sorted by key name, capacity, get, remove.
    StoreValue kz = {STORE_SYMBOL, {.sym = zeta}}, ka = {STORE_SYMBOL, {.sym = alpha}},
               km = {STORE_SYMBOL, {.sym = mid}};
    StoreValue v36 = Int(36), v0 = Int(0);
    Expect(StoreMapSet(&s, t, MAP, &kz, &v36) && StoreMapSet(&s, t, MAP, &ka, &v0) &&
               StoreCountOf(&s, t, MAP) == 2,
           "a map takes new keys");
    Expect(StoreGetAt(&s, t, MAP, 0, &key, &v) && key.as.sym == alpha && v.as.i == 0 &&
               StoreGetAt(&s, t, MAP, 1, &key, &v) && key.as.sym == zeta && v.as.i == 36,
           "a map is sorted by key name");
    Expect(!StoreMapSet(&s, t, MAP, &km, &v0) && Prefix(&s, "capacity") && StoreCountOf(&s, t, MAP) == 2,
           "a full map refuses a new key");
    StoreValue v12 = Int(12);
    Expect(StoreMapSet(&s, t, MAP, &kz, &v12) && StoreMapGet(&s, t, MAP, &kz, &v) && v.as.i == 12,
           "a full map still updates an existing key");
    Expect(StoreMapRemove(&s, t, MAP, &ka) && StoreCountOf(&s, t, MAP) == 1 &&
               !StoreMapGet(&s, t, MAP, &ka, &v) && !StoreMapRemove(&s, t, MAP, &ka),
           "a removed key is gone");
    StoreValue badKey = Int(1);
    Expect(!StoreMapSet(&s, t, MAP, &badKey, &v0) && Prefix(&s, "type"), "a map refuses a key of the wrong type");
    // GRID: cells, fill, range.
    StoreValue seven = Int(7), nine = Int(9);
    Expect(StoreCountOf(&s, t, GRID) == 12 && StoreGridSet(&s, t, GRID, 3, 2, &seven) &&
               StoreGridGet(&s, t, GRID, 3, 2, &v) && v.as.i == 7 &&
               StoreGetAt(&s, t, GRID, 2 * 4 + 3, NULL, &v) && v.as.i == 7,
           "a grid cell is written and read, row-major");
    Expect(StoreGridFill(&s, t, GRID, 0, 0, 2, 2, &nine) && StoreGridGet(&s, t, GRID, 1, 1, &v) &&
               v.as.i == 9 && StoreGridGet(&s, t, GRID, 2, 1, &v) && v.as.i == 0,
           "a grid rectangle fills only its cells");
    Expect(!StoreGridSet(&s, t, GRID, 4, 0, &seven) && Prefix(&s, "range") &&
               !StoreGridFill(&s, t, GRID, 3, 2, 2, 1, &seven) && !StoreGridGet(&s, t, GRID, -1, 0, &v),
           "a grid refuses cells outside it");
    Expect(StoreSharedBlock(&s, t) && !StoreLocalBlock(&s, t), "a kind without local fields has no local block");
    StoreFree(&s);
}

// ---- things and the tree ----------------------------------------------------------------------
static int orphans, removedHooks, spawnedHooks;
static StoreId lastOrphan;
static void OnOrphan(void *user, StoreId guest)
{
    Store *s = user;
    // The guest is still under its parent here, so a world module can read where it was.
    if (StoreParent(s, guest).index != UINT32_MAX)
        orphans++;
    lastOrphan = guest;
}
static void OnRemoved(void *user, StoreId thing)
{
    (void)user;
    (void)thing;
    removedHooks++;
}
static void OnSpawned(void *user, StoreId thing)
{
    (void)user;
    (void)thing;
    spawnedHooks++;
}

static void TreeChecks(void)
{
    Store s;
    StoreInit(&s, 3);
    StoreKind node = StoreDeclareKind(&s, "node", -1, NULL, 0, NULL);
    StoreHooks hooks = {&s, NULL, OnOrphan, OnSpawned, OnRemoved, NULL};
    StoreSetHooks(&s, &hooks);
    orphans = removedHooks = spawnedHooks = 0;
    StoreSymbol eyeName = StoreIntern(&s, "eye"), gunName = StoreIntern(&s, "gun"),
                handName = StoreIntern(&s, "hand");
    StoreId soldier = StoreSpawn(&s, node, 2, STORE_NULL, STORE_NO_SYMBOL);
    StoreId eye = StoreSpawn(&s, node, 0, soldier, eyeName);
    StoreId gun = StoreSpawn(&s, node, 0, eye, gunName);
    StoreId crate = StoreSpawn(&s, node, 0, STORE_NULL, STORE_NO_SYMBOL);
    Expect(spawnedHooks == 4, "the spawned hook runs for every spawn");
    Expect(StoreOwner(&s, gun) == 2 && StoreSpawner(&s, gun) == 2 && StoreOwner(&s, crate) == 0,
           "a child is owned by its root's owner and spawned by its parent's spawner");
    Expect(Same(StoreParent(&s, gun), eye) && Same(StoreChildNamed(&s, soldier, eyeName), eye) &&
               StoreChildName(&s, eye) == eyeName && !StoreIsGuest(&s, eye),
           "declared children know their parent and name");
    Expect(StoreAttach(&s, crate, soldier) && StoreIsGuest(&s, crate) && StoreOwner(&s, crate) == 2 &&
               StoreChildName(&s, crate) == STORE_NO_SYMBOL && StoreSpawner(&s, crate) == 0,
           "an attached thing is a guest owned by its new root's owner, spawner unchanged");
    StoreId hand = StoreSpawn(&s, node, 0, soldier, handName);
    StoreId first = StoreFirstChild(&s, soldier), second = StoreNextSibling(&s, first),
            third = StoreNextSibling(&s, second);
    Expect(Same(first, eye) && Same(second, hand) && Same(third, crate) &&
               StoreNextSibling(&s, third).index == UINT32_MAX,
           "siblings are declared children in order, then guests");
    Expect(!StoreAttach(&s, soldier, gun) && Prefix(&s, "cycle"), "a thing can't hang under its own child");
    Expect(StoreDetach(&s, crate) && StoreOwner(&s, crate) == 0 && StoreParent(&s, crate).index == UINT32_MAX &&
               !StoreIsGuest(&s, crate),
           "a detached thing is a root owned by the host");
    Expect(!StoreDetach(&s, crate), "a root can't be detached");
    StoreId held = StoreSpawn(&s, node, 0, crate, STORE_NO_SYMBOL);
    StoreAttach(&s, crate, gun);
    Expect(StoreOwner(&s, held) == 2, "ownership follows the root down a guest's own children");
    StoreId all[8];
    Expect(StoreThings(&s, node, all, 8) == 6 && Same(all[0], soldier) && StoreCount(&s) == 6 &&
               StoreThings(&s, -1, NULL, 0) == 6,
           "every thing is listed, in id order");
    // Removing the soldier removes what it declared and detaches its guests.
    Expect(StoreRemove(&s, soldier), "a thing is removed");
    Expect(!StoreAlive(&s, soldier) && !StoreAlive(&s, eye) && !StoreAlive(&s, gun) && !StoreAlive(&s, hand),
           "removal takes the declared children with it");
    Expect(StoreAlive(&s, crate) && StoreAlive(&s, held) && StoreParent(&s, crate).index == UINT32_MAX &&
               StoreOwner(&s, crate) == 0 && StoreOwner(&s, held) == 0,
           "a guest of a removed thing is detached to a root owned by the host");
    Expect(orphans == 1 && Same(lastOrphan, crate) && removedHooks == 4,
           "the orphan hook runs once, before the detach, and the removed hook once per thing");
    StoreValue v;
    Expect(!StoreRemove(&s, soldier) && Prefix(&s, "removed") && !StoreGet(&s, gun, 0, &v) &&
               StoreOwner(&s, gun) == -1 && StoreKindOf(&s, gun) == -1,
           "a removed thing's handle is refused everywhere");
    StoreId again = StoreSpawn(&s, node, 0, STORE_NULL, STORE_NO_SYMBOL);
    Expect(again.index == soldier.index && again.generation != soldier.generation &&
               !StoreAlive(&s, soldier),
           "a new thing in a freed slot has a new generation, and the old handle stays dead");
    Expect(StoreSpawn(&s, node, 0, gun, eyeName).index == UINT32_MAX,
           "spawning under a removed parent is refused");
    Expect(StoreSpawn(&s, node, 64, STORE_NULL, STORE_NO_SYMBOL).index == UINT32_MAX,
           "an owner outside 0-63 is refused");
    StoreFree(&s);
}

// ---- the tick ---------------------------------------------------------------------------------
static char tickLog[2048];
static StoreId logA, logB, logC, logD;
static StoreKind logKind;
static bool sawGameplay;

static void Log(const char *text)
{
    if (strlen(tickLog) + strlen(text) + 2 < sizeof tickLog)
    {
        strcat(tickLog, text);
        strcat(tickLog, " ");
    }
}

static const char *Letter(StoreId id)
{
    return Same(id, logA) ? "A" : Same(id, logB) ? "B" : Same(id, logC) ? "C" : Same(id, logD) ? "D" : "?";
}

static bool LogHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                       void *user)
{
    (void)args;
    (void)count;
    (void)user;
    char line[64];
    snprintf(line, sizeof line, "%s:%s", StoreSymbolName(s, event), Letter(self));
    Log(line);
    sawGameplay |= StorePhaseNow(s) == STORE_PHASE_GAMEPLAY && Same(StoreCurrent(s), self) &&
                   StoreCurrentOwner(s) == StoreOwner(s, self);
    const char *name = StoreSymbolName(s, event);
    if (!strcmp(name, "tick") && Same(self, logA) && StoreTickCount(s) == 0)
    {
        StoreSend(s, logB, StoreIntern(s, "ping"), NULL, 0);
        StoreSend(s, logC, StoreIntern(s, "hit"), NULL, 0);
        StoreRemove(s, logC); // the hit queued a moment ago must never arrive
        logD = StoreSpawn(s, logKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    }
    if (!strcmp(name, "ping"))
        StoreSend(s, logA, StoreIntern(s, "pong"), NULL, 0);
    return true;
}

static void LogSystem(Store *s, float dt, void *user)
{
    (void)dt;
    (void)user;
    Log("system");
    StoreSend(s, logA, StoreIntern(s, "sys"), NULL, 0);
}

static int spins;
static bool SpinHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                        void *user)
{
    (void)args;
    (void)count;
    (void)user;
    spins++;
    StoreSend(s, self, event, NULL, 0);
    return true;
}

static void TickChecks(void)
{
    Store s;
    StoreInit(&s, 4);
    logKind = StoreDeclareKind(&s, "logger", -1, NULL, 0, NULL);
    StoreKindSetHandler(&s, logKind, LogHandler, NULL);
    const char *events[] = {"start", "tick", "ping", "pong", "hit", "cmd", "timer", "sys"};
    for (int i = 0; i < 8; i++)
        StoreKindHandles(&s, logKind, StoreIntern(&s, events[i]), true);
    StoreAddSystem(&s, LogSystem, NULL);
    logA = StoreSpawn(&s, logKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    logB = StoreSpawn(&s, logKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    logC = StoreSpawn(&s, logKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    logD = STORE_NULL;
    StoreCommand(&s, logA, StoreIntern(&s, "cmd"), NULL, 0);
    // One tick at the default 1/60 (no tick has run yet): due in tick 1.
    StoreAfter(&s, logA, 1.0f / 60.0f, StoreIntern(&s, "timer"), NULL, 0);
    tickLog[0] = 0;
    sawGameplay = false;
    StoreTick(&s, 0.1f);
    const char *tick0 = "start:A start:B start:C tick:A tick:B cmd:A ping:B start:D pong:A system sys:A ";
    Expect(!strcmp(tickLog, tick0),
           "tick 0 delivers pending starts, then runs tick handlers, then commands and messages "
           "FIFO, then systems and their events; a removed thing's queued messages are dropped");
    if (strcmp(tickLog, tick0))
        printf("      got: %s\n", tickLog);
    const char *startA = strstr(tickLog, "start:A"), *tickA = strstr(tickLog, "tick:A");
    Expect(startA && tickA && startA < tickA,
           "a thing spawned outside a tick sees start before its first tick");
    const char *startD = strstr(tickLog, "start:D");
    Expect(startD && tickA && startD > tickA && !strstr(tickLog, "tick:D"),
           "a thing spawned in a tick handler sees start later in that tick, after the handler");
    Expect(sawGameplay, "handlers run with phase gameplay, themselves current, for their owner");
    Expect(!StoreAlive(&s, logC) && StoreAlive(&s, logD) && StoreTickCount(&s) == 1,
           "a removal and a spawn during a tick take effect");
    tickLog[0] = 0;
    StoreTick(&s, 0.1f);
    Expect(!strcmp(tickLog, "tick:A tick:B tick:D timer:A system sys:A "),
           "a thing spawned in tick N ticks first in tick N+1, with no second start; due timers "
           "are delivered after the tick handlers");
    if (strcmp(tickLog, "tick:A tick:B tick:D timer:A system sys:A "))
        printf("      got: %s\n", tickLog);
    Expect(StoreTickTime(&s) > 0.19f && StoreTickTime(&s) < 0.21f, "tick time is ticks times dt");
    // Only local owners tick.
    StoreId remote = StoreSpawn(&s, logKind, 1, STORE_NULL, STORE_NO_SYMBOL);
    int local[1] = {0};
    StoreSetLocalOwners(&s, local, 1);
    StoreTick(&s, 0.1f); // remote's start is dropped too: it is not run here
    tickLog[0] = 0;
    StoreTick(&s, 0.1f);
    Expect(!strstr(tickLog, "?"), "a thing whose owner is not local runs no handlers here");
    StoreSetLocalOwners(&s, (int[]){0, 1}, 2);
    tickLog[0] = 0;
    StoreTick(&s, 0.1f);
    Expect(strstr(tickLog, "tick:?") != NULL, "it runs once its owner is local");
    (void)remote;
    // A message the kind has no handler for is reported once per kind and event.
    SetTraceLogCallback(CountWarnings);
    warnUnhandled = 0;
    StoreSend(&s, logA, StoreIntern(&s, "nobody-handles"), NULL, 0);
    StoreSend(&s, logB, StoreIntern(&s, "nobody-handles"), NULL, 0);
    StoreTick(&s, 0.1f);
    StoreSend(&s, logA, StoreIntern(&s, "nobody-handles"), NULL, 0);
    StoreTick(&s, 0.1f);
    Expect(warnUnhandled == 1, "an unhandled message is reported once per kind and event");
    // A message loop is cut at 10,000 deliveries in a tick.
    StoreKind spin = StoreDeclareKind(&s, "spinner", -1, NULL, 0, NULL);
    StoreKindSetHandler(&s, spin, SpinHandler, NULL);
    StoreKindHandles(&s, spin, StoreIntern(&s, "spin"), true);
    StoreId spinner = StoreSpawn(&s, spin, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreTick(&s, 0.1f); // its start
    StoreCommand(&s, spinner, StoreIntern(&s, "spin"), NULL, 0);
    spins = 0;
    warnDropped = 0;
    StoreTick(&s, 0.1f);
    Expect(spins == 10000 && warnDropped == 1, "a message loop stops at 10,000 deliveries with one warning");
    spins = 0;
    StoreTick(&s, 0.1f);
    Expect(spins == 0, "the dropped messages do not come back next tick");
    SetTraceLogCallback(NULL);
    StoreValue tooMany[STORE_MAX_ARGS + 1];
    memset(tooMany, 0, sizeof tooMany);
    Expect(!StoreSend(&s, logA, StoreIntern(&s, "sys"), tooMany, STORE_MAX_ARGS + 1),
           "a message with too many arguments is refused");
    Expect(!StoreSend(&s, logC, StoreIntern(&s, "sys"), NULL, 0) && Prefix(&s, "removed"),
           "a message to a removed thing is refused");
    StoreFree(&s);
}

// ---- timers -----------------------------------------------------------------------------------
static char timerLog[256];
static bool TimerHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                         void *user)
{
    (void)self;
    (void)user;
    char line[64];
    snprintf(line, sizeof line, "%s@%d(%d) ", StoreSymbolName(s, event), (int)StoreTickCount(s),
             count ? args[0].as.i : -1);
    if (strlen(timerLog) + strlen(line) < sizeof timerLog)
        strcat(timerLog, line);
    return true;
}

static void TimerChecks(void)
{
    Store s;
    StoreInit(&s, 5);
    StoreKind k = StoreDeclareKind(&s, "timed", -1, NULL, 0, NULL);
    StoreKindSetHandler(&s, k, TimerHandler, NULL);
    StoreSymbol late = StoreIntern(&s, "late"), early = StoreIntern(&s, "early"), second = StoreIntern(&s, "second");
    StoreKindHandles(&s, k, late, true);
    StoreKindHandles(&s, k, early, true);
    StoreKindHandles(&s, k, second, true);
    StoreId t = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreId doomed = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreTick(&s, 0.1f); // tick 0; dt is now 0.1
    StoreValue arg = Int(42);
    Expect(StoreAfter(&s, t, 0.5f, late, &arg, 1) && StoreAfter(&s, t, 0.2f, early, NULL, 0) &&
               StoreAfter(&s, t, 0.2f, second, NULL, 0) && StoreAfter(&s, doomed, 0.1f, late, NULL, 0),
           "timers are set");
    StoreRemove(&s, doomed);
    timerLog[0] = 0;
    for (int i = 0; i < 7; i++)
        StoreTick(&s, 0.1f);
    Expect(!strcmp(timerLog, "early@3(-1) second@3(-1) late@6(42) "),
           "timers fire after their ticks, due order then creation order, with their arguments, "
           "and never for a removed thing");
    if (strcmp(timerLog, "early@3(-1) second@3(-1) late@6(42) "))
        printf("      got: %s\n", timerLog);
    Expect(!StoreAfter(&s, t, -1.0f, late, NULL, 0) && !StoreAfter(&s, doomed, 1.0f, late, NULL, 0),
           "a negative time or a removed thing is refused");
    StoreFree(&s);
}

// ---- the rules --------------------------------------------------------------------------------
enum { RULE_HP, RULE_HURT, RULE_FLOOR };
static StoreId ruleMine, ruleTheirs;
static int ruleErrors, ruleHooked;
static bool ruleResults[16];

static void OnError(void *user, const char *message)
{
    (void)user;
    (void)message;
    ruleHooked++;
}

static bool RuleHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                        void *user)
{
    (void)args;
    (void)count;
    (void)user;
    StoreValue v, n = Int(5), wrong = Str("five"), yes = Bool(true), f = Float(0.5f);
    const char *name = StoreSymbolName(s, event);
    if (!strcmp(name, "tick") && Same(self, ruleMine))
    {
        ruleResults[0] = !StoreGet(s, self, RULE_HURT, &v) && Prefix(s, "local-read");
        ruleResults[1] = !StoreSet(s, ruleTheirs, RULE_HP, &n) && Prefix(s, "not-owner");
        ruleResults[2] = !StoreSet(s, self, RULE_FLOOR, &yes) && Prefix(s, "engine-field");
        ruleResults[3] = StoreSetEngine(s, self, RULE_FLOOR, &yes);
        ruleResults[4] = !StoreSet(s, self, RULE_HP, &wrong) && Prefix(s, "type");
        ruleResults[5] = StoreSet(s, self, RULE_HP, &n) && StoreSet(s, self, RULE_HURT, &f) &&
                         StoreGet(s, ruleTheirs, RULE_HP, &v);
        ruleResults[6] = !StoreRemove(s, ruleTheirs) && Prefix(s, "not-owner");
        ruleResults[7] = !StoreAttach(s, ruleTheirs, self) && Prefix(s, "not-owner");
        ruleResults[8] = StoreRandom(s, self, 10) < 10 && !StoreAfter(s, ruleTheirs, 1, event, NULL, 0) &&
                         Prefix(s, "not-owner");
    }
    if (!strcmp(name, "frame") && Same(self, ruleMine))
    {
        ruleResults[9] = !StoreSet(s, self, RULE_HP, &n) && Prefix(s, "shared-write");
        ruleResults[10] = StoreSet(s, self, RULE_HURT, &f) && StoreGet(s, self, RULE_HURT, &v) &&
                          StoreGet(s, self, RULE_HP, &v);
        ruleResults[11] = !StoreSend(s, self, event, NULL, 0) && Prefix(s, "shared-write") &&
                          StoreCommand(s, self, StoreIntern(s, "noop"), NULL, 0);
        ruleResults[12] = StoreRandom(s, self, 10) == 0 && Prefix(s, "shared-write");
        ruleResults[13] = !StoreDetach(s, self) && Prefix(s, "shared-write");
    }
    ruleErrors++;
    return true;
}

static void RuleChecks(void)
{
    Store s;
    StoreInit(&s, 6);
    StoreFieldDecl fields[] = {Field("hp", STORE_INT, 0, Int(10)),
                               Field("hurt", STORE_FLOAT, STORE_LOCAL, None()),
                               Field("on-floor", STORE_BOOL, STORE_ENGINE, None())};
    StoreKind k = StoreDeclareKind(&s, "soldier", -1, fields, 3, NULL);
    StoreKindSetHandler(&s, k, RuleHandler, NULL);
    StoreKindHandles(&s, k, StoreIntern(&s, "tick"), true);
    StoreKindHandles(&s, k, StoreIntern(&s, "frame"), true);
    StoreHooks hooks = {NULL, OnError, NULL, NULL, NULL, NULL};
    StoreSetHooks(&s, &hooks);
    ruleMine = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    ruleTheirs = StoreSpawn(&s, k, 2, STORE_NULL, STORE_NO_SYMBOL);
    memset(ruleResults, 0, sizeof ruleResults);
    ruleHooked = 0;
    StoreTick(&s, 1.0f / 60.0f);
    StoreFrame(&s, 1.0f / 60.0f);
    const char *what[14] = {
        "rule 1: a gameplay handler can't read a local field (local-read)",
        "rule 5: a gameplay handler can't write another owner's shared field (not-owner)",
        "an engine field is refused to a handler (engine-field)",
        "StoreSetEngine writes an engine field",
        "rule 4: a value of the wrong type is refused (type)",
        "a gameplay handler writes its own shared and local fields and reads another's",
        "rule 5: a gameplay handler can't remove another owner's thing",
        "rule 5: a gameplay handler can't attach another owner's thing",
        "rule 5: a gameplay handler can't set another owner's timer",
        "rule 1: a presentation handler can't write a shared field (shared-write)",
        "a presentation handler reads anything and writes local fields",
        "a presentation handler can't send a message, but can queue a command",
        "a presentation handler can't draw from a thing's stream",
        "a presentation handler can't detach a thing",
    };
    for (int i = 0; i < 14; i++)
        Expect(ruleResults[i], what[i]);
    Expect(ruleHooked >= 10, "rule and type errors reach the error hook");
    StoreValue v, n = Int(3), yes = Bool(false);
    Expect(StorePhaseNow(&s) == STORE_PHASE_NONE && StoreGet(&s, ruleMine, RULE_HURT, &v) &&
               StoreSet(&s, ruleTheirs, RULE_HP, &n) && StoreSet(&s, ruleMine, RULE_FLOOR, &yes) &&
               StoreCurrentOwner(&s) == -1 && StoreCurrent(&s).index == UINT32_MAX,
           "outside handlers every read and write is allowed");
    StoreFree(&s);
}

// ---- presentation: frame, -changed, draw-hud --------------------------------------------------
static char frameLog[512];
static bool FrameHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                         void *user)
{
    (void)self;
    (void)user;
    char line[64];
    const char *name = StoreSymbolName(s, event);
    if (count == 2 && args[1].type == STORE_INT && args[0].type == STORE_NONE)
        snprintf(line, sizeof line, "%s(none,%d) ", name, args[1].as.i);
    else if (count == 2 && args[1].type == STORE_INT)
        snprintf(line, sizeof line, "%s(%d,%d) ", name, args[0].as.i, args[1].as.i);
    else
        snprintf(line, sizeof line, "%s ", name);
    if (strlen(frameLog) + strlen(line) < sizeof frameLog)
        strcat(frameLog, line);
    return true;
}

static void FrameChecks(void)
{
    Store s;
    StoreInit(&s, 7);
    StoreFieldDecl fields[] = {Field("hp", STORE_INT, 0, Int(100)), Field("quiet", STORE_INT, 0, None()),
                               Collection("items", STORE_LIST, STORE_INT, STORE_NONE, 4, 0)};
    StoreKind k = StoreDeclareKind(&s, "watched", -1, fields, 3, NULL);
    StoreKind plain = StoreDeclareKind(&s, "plain", -1, fields, 3, NULL);
    StoreKindSetHandler(&s, k, FrameHandler, NULL);
    StoreKindSetHandler(&s, plain, FrameHandler, NULL);
    const char *events[] = {"frame", "hp-changed", "items-changed", "draw-hud"};
    for (int i = 0; i < 4; i++)
        StoreKindHandles(&s, k, StoreIntern(&s, events[i]), true);
    StoreKind derived = StoreDeclareKind(&s, "derived", k, NULL, 0, NULL);
    StoreId t = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreSpawn(&s, plain, 0, STORE_NULL, STORE_NO_SYMBOL);
    frameLog[0] = 0;
    StoreFrame(&s, 0.016f);
    Expect(!strcmp(frameLog, "frame hp-changed(none,100) items-changed draw-hud "),
           "a new thing gets frame, then -changed with was none for each watched field, then draw-hud");
    if (strcmp(frameLog, "frame hp-changed(none,100) items-changed draw-hud "))
        printf("      got: %s\n", frameLog);
    frameLog[0] = 0;
    StoreFrame(&s, 0.016f);
    Expect(!strcmp(frameLog, "frame draw-hud "), "nothing changed: no -changed");
    StoreValue v = Int(90), w = Int(50), q = Int(1);
    StoreValue items[2] = {Int(1), Int(2)};
    StoreSet(&s, t, 0, &v);
    StoreSet(&s, t, 1, &q); // not watched
    StoreSetList(&s, t, 2, items, 2);
    frameLog[0] = 0;
    StoreFrame(&s, 0.016f);
    Expect(!strcmp(frameLog, "frame hp-changed(100,90) items-changed draw-hud "),
           "a changed field reports was and now; a collection is one change; unwatched fields none");
    if (strcmp(frameLog, "frame hp-changed(100,90) items-changed draw-hud "))
        printf("      got: %s\n", frameLog);
    StoreSet(&s, t, 0, &w);
    StoreSet(&s, t, 0, &v);
    frameLog[0] = 0;
    StoreFrame(&s, 0.016f);
    Expect(!strcmp(frameLog, "frame draw-hud "), "a change that ends where it started is invisible");
    StoreId d = StoreSpawn(&s, derived, 0, STORE_NULL, STORE_NO_SYMBOL);
    Expect(StoreKindHandlesEvent(&s, derived, StoreIntern(&s, "hp-changed")) && StoreAlive(&s, d),
           "a derived kind handles its base's events");
    frameLog[0] = 0;
    StoreFrame(&s, 0.016f);
    // Each pass runs over every thing: both frames, the new one's changes, both draw-huds.
    Expect(!strcmp(frameLog, "frame frame hp-changed(none,100) items-changed draw-hud draw-hud "),
           "a derived kind is watched like its base");
    if (strcmp(frameLog, "frame frame hp-changed(none,100) items-changed draw-hud draw-hud "))
        printf("      got: %s\n", frameLog);
    StoreFree(&s);
}

// ---- randomness, snapshot, restore, hash ------------------------------------------------------
static StoreKind BuildWorld(Store *s, uint64_t seed, StoreId *out)
{
    StoreInit(s, seed);
    StoreFieldDecl fields[] = {Field("hp", STORE_INT, 0, Int(100)), Field("pos", STORE_VEC3, 0, None()),
                               Field("mood", STORE_SYMBOL, 0, None()),
                               Field("hurt", STORE_FLOAT, STORE_LOCAL, None()),
                               Collection("bag", STORE_SET, STORE_SYMBOL, STORE_NONE, 4, 0)};
    StoreKind k = StoreDeclareKind(s, "unit", -1, fields, 5, NULL);
    for (int i = 0; i < 4; i++)
    {
        out[i] = StoreSpawn(s, k, i % 2, STORE_NULL, STORE_NO_SYMBOL);
        StoreValue hp = Int(10 * i), mood = Sym(s, i % 2 ? "calm" : "angry");
        StoreSet(s, out[i], 0, &hp);
        StoreSet(s, out[i], 2, &mood);
    }
    StoreAfter(s, out[0], 1.0f, StoreIntern(s, "wake"), NULL, 0);
    return k;
}

static void SnapshotChecks(void)
{
    Store a, b;
    StoreId ta[4], tb[4];
    StoreKind k = BuildWorld(&a, 99, ta);
    BuildWorld(&b, 99, tb);
    Expect(StoreHash(&a) == StoreHash(&b), "the same operations give the same hash");
    // Streams: one thing's draws never shift another's.
    Expect(StoreRandom(&a, ta[0], 0) == 0, "a random range of 0 answers 0");
    uint32_t r = 0;
    for (int i = 0; i < 5; i++)
        r |= StoreRandom(&a, ta[0], 7) >= 7;
    Expect(!r, "draws stay in range");
    Expect(StoreRandom(&a, ta[1], 1000000) == StoreRandom(&b, tb[1], 1000000),
           "drawing from one thing leaves another's stream alone");
    Expect(StoreHash(&a) != StoreHash(&b), "a draw changes the hash");
    StoreRandom(&b, tb[0], 7);
    StoreRandom(&b, tb[0], 7);
    StoreRandom(&b, tb[0], 7);
    StoreRandom(&b, tb[0], 7);
    StoreRandom(&b, tb[0], 7);
    Expect(StoreHash(&a) == StoreHash(&b), "the same draws give the same hash");
    uint32_t localBefore = StoreRandomLocal(&a, 1000);
    Expect(localBefore < 1000 && StoreHash(&a) == StoreHash(&b), "the presentation stream is not hashed");
    StoreValue hurt = Float(0.75f);
    StoreSet(&a, ta[2], 3, &hurt);
    Expect(StoreHash(&a) == StoreHash(&b), "local fields are not hashed");
    // Snapshot, mutate, restore.
    uint64_t before = StoreHash(&a);
    StoreSnapshot *snap = StoreSnapshotTake(&a);
    Expect(snap != NULL, "a snapshot is taken");
    StoreValue hp = Int(-5);
    StoreSet(&a, ta[1], 0, &hp);
    StoreRemove(&a, ta[3]);
    StoreId extra = StoreSpawn(&a, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue extraHurt = Float(0.5f);
    StoreSet(&a, extra, 3, &extraHurt); // extra reuses ta[3]'s slot and row
    StoreTick(&a, 0.1f);
    StoreRandom(&a, ta[2], 5);
    uint64_t after = StoreHash(&a);
    Expect(after != before, "mutation changes the hash");
    Expect(StoreSnapshotRestore(&a, snap) && StoreHash(&a) == before && StoreAlive(&a, ta[3]) &&
               GetInt(&a, ta[1], 0) == 10 && StoreTickCount(&a) == 0,
           "restoring a snapshot gives back the same world and hash");
    StoreValue v;
    Expect(StoreGet(&a, ta[2], 3, &v) && v.as.f == 0.75f && StoreGet(&a, ta[3], 3, &v) && v.as.f == 0.0f,
           "a thing kept across a restore keeps its local block; a resurrected one starts fresh");
    // Redoing the same mutation after the restore reaches the same hash.
    StoreSet(&a, ta[1], 0, &hp);
    StoreRemove(&a, ta[3]);
    StoreId extra2 = StoreSpawn(&a, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreTick(&a, 0.1f);
    StoreRandom(&a, ta[2], 5);
    Expect(StoreHash(&a) == after && Same(extra, extra2), "re-running after a restore is deterministic");
    Expect(StoreSnapshotRestore(&a, snap) && StoreHash(&a) == before, "a snapshot restores more than once");
    StoreSnapshotFree(snap);
    // Restoring into a store laid out differently is refused.
    Store c;
    StoreInit(&c, 1);
    StoreFieldDecl other[] = {Field("x", STORE_INT, 0, None())};
    StoreDeclareKind(&c, "unit", -1, other, 1, NULL);
    snap = StoreSnapshotTake(&a);
    Expect(!StoreSnapshotRestore(&c, snap) && StoreCount(&c) == 0,
           "a snapshot is refused by a store whose kinds differ");
    StoreSnapshotFree(snap);
    StoreFree(&c);
    StoreFree(&a);
    StoreFree(&b);
}

// ---- save and load ----------------------------------------------------------------------------
static StoreKind DeclareSaved(Store *s, bool changed)
{
    StoreFieldDecl fields[] = {
        Field("hp", STORE_INT, 0, Int(100)),
        Field("name", STORE_STRING, 0, None()),
        Field("pos", STORE_VEC3, 0, None()),
        Field("speed", STORE_FLOAT, 0, None()),
        Field("up", STORE_BOOL, 0, Bool(true)),
        Field("weapon", STORE_SYMBOL, 0, None()),
        Field("prey", STORE_REF, 0, None()),
        Field("hurt", STORE_FLOAT, STORE_LOCAL, None()),
        Collection("carried", STORE_LIST, STORE_SYMBOL, STORE_NONE, 4, 0),
        Collection("ammo", STORE_MAP, STORE_INT, STORE_SYMBOL, 4, 0),
        Collection("floor", STORE_GRID, STORE_INT, STORE_NONE, 3, 2),
        Collection("seen", STORE_SET, STORE_REF, STORE_NONE, 4, 0),
        Field(changed ? "armor" : "doomed", STORE_INT, 0, Int(changed ? 3 : 0)),
    };
    StoreKind k = StoreDeclareKind(s, "soldier", -1, fields, 13, NULL);
    StoreDeclareKind(s, "eye", -1, NULL, 0, NULL);
    return k;
}

static bool Noop(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count, void *user)
{
    (void)s;
    (void)self;
    (void)event;
    (void)args;
    (void)count;
    (void)user;
    return true;
}

static void SaveChecks(void)
{
    const char *path = Scratch("regression_store.sav");
    char pathCopy[256];
    snprintf(pathCopy, sizeof pathCopy, "%s", path);
    Store a;
    StoreInit(&a, 1234);
    StoreKind soldier = DeclareSaved(&a, false);
    StoreKind eye = StoreKindNamed(&a, "eye");
    StoreKindSetHandler(&a, soldier, Noop, NULL);
    StoreId s1 = StoreSpawn(&a, soldier, 1, STORE_NULL, STORE_NO_SYMBOL);
    StoreId gap = StoreSpawn(&a, eye, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreId s2 = StoreSpawn(&a, soldier, 2, STORE_NULL, STORE_NO_SYMBOL);
    StoreId e1 = StoreSpawn(&a, eye, 0, s1, StoreIntern(&a, "eye"));
    StoreId guest = StoreSpawn(&a, eye, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreAttach(&a, guest, s1);
    StoreId e2 = StoreSpawn(&a, eye, 0, s1, StoreIntern(&a, "hand"));
    StoreRemove(&a, gap); // a free slot in the middle
    StoreValue v = Int(-42), name = Str("say \"hi\"\\ now\nplease"), pos = Vec(1.5f, -0.1f, 3e9f),
               speed = Float(0.1f), up = Bool(false), weapon = Sym(&a, "shotgun"), prey = Ref(s2);
    StoreSet(&a, s1, 0, &v);
    StoreSet(&a, s1, 1, &name);
    StoreSet(&a, s1, 2, &pos);
    StoreSet(&a, s1, 3, &speed);
    StoreSet(&a, s1, 4, &up);
    StoreSet(&a, s1, 5, &weapon);
    StoreSet(&a, s1, 6, &prey);
    StoreValue hurt = Float(9);
    StoreSet(&a, s1, 7, &hurt);
    StoreValue carried[2] = {Sym(&a, "pistol"), Sym(&a, "shotgun")};
    StoreSetList(&a, s1, 8, carried, 2);
    StoreValue pistol = Sym(&a, "pistol"), shells = Sym(&a, "shells"), n36 = Int(36), n0 = Int(0);
    StoreMapSet(&a, s1, 9, &pistol, &n36);
    StoreMapSet(&a, s1, 9, &shells, &n0);
    StoreValue five = Int(5);
    StoreGridSet(&a, s1, 10, 2, 1, &five);
    StoreValue seen[2] = {Ref(s2), Ref(e1)};
    StoreSetList(&a, s1, 11, seen, 2);
    StoreValue args[3] = {Int(7), Str("x y"), Vec(1, 2, 3)};
    StoreAfter(&a, s1, 2.0f, StoreIntern(&a, "reload-done"), args, 3);
    StoreAfter(&a, s2, 1.0f, StoreIntern(&a, "wake"), NULL, 0);
    StoreTick(&a, 0.1f);
    StoreRandom(&a, s2, 100);
    uint64_t hash = StoreHash(&a);
    Expect(StoreSave(&a, pathCopy), "a world saves");
    // Load into a fresh store whose symbols were interned in another order.
    Store b;
    StoreInit(&b, 1);
    StoreIntern(&b, "zzz");
    StoreIntern(&b, "shotgun");
    DeclareSaved(&b, false);
    Expect(StoreLoad(&b, pathCopy), "a save loads into a store with the kinds declared");
    if (strlen(StoreLastError(&b)))
        printf("      last error: %s\n", StoreLastError(&b));
    Expect(StoreHash(&b) == hash, "a loaded world hashes as the one that saved it");
    Expect(StoreTickCount(&b) == 1 && StoreCount(&b) == 5 && !StoreAlive(&b, gap) &&
               StoreAlive(&b, e2) && StoreIsGuest(&b, guest) && StoreOwner(&b, guest) == 1,
           "things keep their ids, owners and guest flag; the free slot stays free");
    StoreId first = StoreFirstChild(&b, s1), second = StoreNextSibling(&b, first),
            third = StoreNextSibling(&b, second);
    Expect(Same(first, e1) && Same(second, e2) && Same(third, guest) &&
               StoreChildName(&b, e2) == StoreIntern(&b, "hand"),
           "sibling order and child names survive a save");
    StoreValue got;
    Expect(StoreGet(&b, s1, 1, &got) && !strcmp(got.as.str, name.as.str) && StoreGet(&b, s1, 2, &got) &&
               got.as.v.z == 3e9f && StoreGet(&b, s1, 3, &got) && got.as.f == 0.1f &&
               StoreGet(&b, s1, 6, &got) && Same(got.as.ref, s2) && StoreGet(&b, s1, 7, &got) &&
               got.as.f == 0.0f,
           "strings with escapes, floats exactly and refs load; local fields are not saved");
    StoreId fresh = StoreSpawn(&b, eye, 0, STORE_NULL, STORE_NO_SYMBOL);
    Expect(fresh.index == gap.index && fresh.generation != gap.generation,
           "a spawn after loading takes the free slot with a generation no saved handle has");
    // The timers came back: they fire with their arguments at their ticks.
    Store c;
    StoreInit(&c, 5);
    DeclareSaved(&c, false);
    Expect(StoreLoad(&c, pathCopy) && StoreSave(&c, Scratch("regression_store2.sav")),
           "a loaded world saves again");
    char *one = LoadFileText(pathCopy), *two = LoadFileText(Scratch("regression_store2.sav"));
    Expect(one && two && !strcmp(one, two), "saving a loaded world writes the same text");
    UnloadFileText(one);
    UnloadFileText(two);
    // A changed kind: a field added takes its default, a field removed is skipped with a warning.
    Store d;
    StoreInit(&d, 5);
    StoreKind changed = DeclareSaved(&d, true);
    SetTraceLogCallback(CountWarnings);
    warnLoading = 0;
    Expect(StoreLoad(&d, pathCopy), "a save loads into a kind that gained and lost a field");
    Expect(warnLoading == 1, "a removed field is skipped with one warning per kind and field");
    SetTraceLogCallback(NULL);
    int armor = StoreFieldIndex(&d, changed, "armor");
    Expect(GetInt(&d, s1, armor) == 3 && GetInt(&d, s2, armor) == 3 && GetInt(&d, s1, 0) == -42,
           "an added field takes its default and the others load");
    // Refusals.
    FILE *f = fopen(Scratch("regression_store_bad.sav"), "w");
    if (f)
    {
        fputs("not a save\n", f);
        fclose(f);
    }
    uint32_t count = StoreCount(&d);
    Expect(!StoreLoad(&d, Scratch("regression_store_bad.sav")) && StoreCount(&d) == count,
           "a file that is not a save is refused and the world is untouched");
    f = fopen(Scratch("regression_store_bad.sav"), "w");
    if (f)
    {
        fputs("store 1 tick 0 seed 1\nthing 0 gen 1 kind dragon parent - name - owner 0 spawner 0 rng 1\n", f);
        fclose(f);
    }
    Expect(!StoreLoad(&d, Scratch("regression_store_bad.sav")) && strstr(StoreLastError(&d), "dragon") &&
               StoreCount(&d) == count,
           "a save with an undeclared kind is refused before the world is touched");
    Expect(!StoreLoad(&d, Scratch("regression_store_missing.sav")), "a missing save is refused");
    StoreFree(&a);
    StoreFree(&b);
    StoreFree(&c);
    StoreFree(&d);
}

// ---- local things: StoreMarkLocal ---------------------------------------------------------------
static StoreId localShared, localThing;
static bool localReadRefused, localWritten, sharedWriteRefused;

static bool LocalHandler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                         void *user)
{
    (void)args;
    (void)count;
    (void)user;
    StoreValue v, glow = Int(9);
    if (!Same(self, localShared))
        return true;
    if (!strcmp(StoreSymbolName(s, event), "tick"))
        localReadRefused = !StoreGet(s, localThing, 0, &v) && Prefix(s, "local-read");
    else
    {
        localWritten = StoreSet(s, localThing, 0, &glow);
        sharedWriteRefused = !StoreSet(s, localShared, 0, &glow) && Prefix(s, "shared-write");
    }
    return true;
}

// Two worlds that differ only by a local thing: withLocal also marks one and hangs a child under it.
static uint64_t LocalWorld(Store *s, bool withLocal, StoreId *local, StoreId *child)
{
    StoreInit(s, 11);
    StoreFieldDecl fields[] = {Field("glow", STORE_INT, 0, Int(1))};
    StoreKind k = StoreDeclareKind(s, "lamp", -1, fields, 1, NULL);
    StoreKindSetHandler(s, k, LocalHandler, NULL);
    StoreKindHandles(s, k, StoreIntern(s, "tick"), true);
    StoreKindHandles(s, k, StoreIntern(s, "frame"), true);
    localShared = StoreSpawn(s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    if (withLocal)
    {
        *local = localThing = StoreSpawn(s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
        Expect(StoreMarkLocal(s, *local) && StoreIsLocal(s, *local), "a thing is marked local before its first tick");
        *child = StoreSpawn(s, k, 0, *local, StoreIntern(s, "bulb"));
        Expect(StoreIsLocal(s, *child), "a declared child of a local thing is local");
    }
    StoreTick(s, 1.0f / 60.0f);
    StoreFrame(s, 1.0f / 60.0f);
    return StoreHash(s);
}

static void LocalChecks(void)
{
    Store a, b, c;
    StoreId local, child, none;
    localReadRefused = localWritten = sharedWriteRefused = false;
    uint64_t with = LocalWorld(&a, true, &local, &child);
    Expect(localReadRefused, "a gameplay handler can't read a field of a local thing (local-read)");
    Expect(localWritten && sharedWriteRefused,
           "a presentation handler writes a field of a local thing, and still not a shared one");
    Expect(!StoreMarkLocal(&a, localShared) && !StoreIsLocal(&a, localShared),
           "a thing that has started can't be made local");
    Expect(StoreThings(&a, -1, NULL, 0) == 3, "StoreThings lists local things");
    uint64_t without = LocalWorld(&b, false, &none, &none);
    Expect(with == without, "a local thing leaves the hash as it was");
    StoreSnapshot *snapshot = StoreSnapshotTake(&a);
    Expect(snapshot && StoreSnapshotRestore(&a, snapshot) && StoreIsLocal(&a, local) && StoreIsLocal(&a, child),
           "a snapshot keeps local things local");
    StoreSnapshotFree(snapshot);
    const char *path = Scratch("regression_store_local.sav");
    char pathCopy[256];
    snprintf(pathCopy, sizeof pathCopy, "%s", path);
    StoreInit(&c, 3);
    StoreFieldDecl fields[] = {Field("glow", STORE_INT, 0, Int(1))};
    StoreDeclareKind(&c, "lamp", -1, fields, 1, NULL);
    Expect(StoreSave(&a, pathCopy) && StoreLoad(&c, pathCopy) && StoreCount(&c) == 1 &&
               StoreAlive(&c, localShared) && !StoreAlive(&c, local),
           "a save leaves local things out");
    StoreFree(&a);
    StoreFree(&b);
    StoreFree(&c);
}

// ---- for native systems (B7) --------------------------------------------------------------------
// StoreFieldOffset says where StoreSet writes; StoreOwnedHere follows the local owners and attach.
static void NativeChecks(void)
{
    Store s;
    StoreInit(&s, 5);
    StoreFieldDecl fields[] = {Field("hp", STORE_INT, 0, Int(0)), Field("glow", STORE_FLOAT, STORE_LOCAL, Float(0)),
                               Field("at", STORE_VEC3, 0, Vec(0, 0, 0)), Field("age", STORE_FLOAT, 0, Float(0))};
    StoreKind k = StoreDeclareKind(&s, "shell", -1, fields, 4, NULL);
    StoreId shell = StoreSpawn(&s, k, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue at = Vec(1.5f, 2.5f, 3.5f), age = Float(0.75f), hp = Int(42);
    StoreSet(&s, shell, 2, &at);
    StoreSet(&s, shell, 3, &age);
    StoreSet(&s, shell, 0, &hp);
    const unsigned char *block = StoreSharedBlock(&s, shell);
    int atOffset = StoreFieldOffset(&s, k, 2), ageOffset = StoreFieldOffset(&s, k, 3),
        hpOffset = StoreFieldOffset(&s, k, 0);
    float v[3], a = 0;
    int32_t h = 0;
    if (block && atOffset >= 0 && ageOffset >= 0 && hpOffset >= 0)
    {
        memcpy(v, block + atOffset, sizeof v);
        memcpy(&a, block + ageOffset, sizeof a);
        memcpy(&h, block + hpOffset, sizeof h);
    }
    printf("store: field offsets hp %d at %d age %d (glow, local, %d)\n", hpOffset, atOffset, ageOffset,
           StoreFieldOffset(&s, k, 1));
    Expect(block && v[0] == 1.5f && v[1] == 2.5f && v[2] == 3.5f && a == 0.75f && h == 42,
           "StoreFieldOffset says where StoreSet writes a field in StoreSharedBlock");
    Expect(StoreFieldOffset(&s, k, 1) == 0 && StoreFieldOffset(&s, k, 4) == -1 &&
               StoreFieldOffset(&s, k, -1) == -1 && StoreFieldOffset(&s, 99, 0) == -1,
           "a local field's offset is in the local block; an unknown field or kind answers -1");
    StoreId soldier = StoreSpawn(&s, k, 2, STORE_NULL, STORE_NO_SYMBOL);
    StoreSetLocalOwners(&s, (int[]){0, 1}, 2);
    bool before = StoreOwnedHere(&s, shell) && !StoreOwnedHere(&s, soldier);
    StoreAttach(&s, shell, soldier);
    bool attached = !StoreOwnedHere(&s, shell);
    StoreSetLocalOwners(&s, (int[]){2}, 1);
    bool asClient = StoreOwnedHere(&s, shell) && StoreOwnedHere(&s, soldier);
    StoreSetLocalOwners(&s, (int[]){0, 1}, 2);
    StoreDetach(&s, shell);
    bool detached = StoreOwnedHere(&s, shell);
    StoreRemove(&s, shell);
    Expect(before && attached && asClient && detached && !StoreOwnedHere(&s, shell),
           "StoreOwnedHere follows the local owners and attach, and is false for a removed thing");
    StoreFree(&s);
}

int StoreChecks(void)
{
    failures = 0;
    KindChecks();
    FieldChecks();
    TreeChecks();
    TickChecks();
    TimerChecks();
    RuleChecks();
    FrameChecks();
    SnapshotChecks();
    SaveChecks();
    LocalChecks();
    NativeChecks();
    return failures;
}
