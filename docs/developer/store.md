# The store, kinds and the game runner (phases 0 and 1)

This is the implementation design for the proposal in `../../../trenchengine-design/proposal.md`
(Part B is the design; Part D.2 is the plan and the go/no-go). Section numbers there are cited as
"B3.1" and so on. This document is the contract between the tasks that build it: every module
below is written against the signatures here, and a task that needs to change a signature says so
in its report rather than changing it quietly.

Rules that hold for every task:

- Nothing here removes or changes an existing API. Games on the entity, object and net-sync paths
  (Trenchfoot, Skyrift) build and run unchanged. New code goes in new files.
- `core/` depends only on core, raylib and system headers; the build checks it. Nothing in `core/`
  includes `s7.h`.
- No new file opens a window or touches GL unless it says so here (`draw_path.c` and the
  presentation side of the runner). The store, the built-in kinds, saving, recording and replay run
  with no window and no GL, so a headless process links and runs them all.
- C99, `-Wall -Wextra -Wpedantic -Werror`, MPL header on every new file, doc comments (`/** */`)
  on public functions in the style of `core/object.h`. `make smoke` passes with zero warnings.
- Floats are `float` (32-bit) everywhere in the store: one representation for snapshot, compare,
  save and hash. Determinism is per build (B4), not across machines.
- Every check is a plain function in `tests/regression/<module>_checks.c` exposed through
  `tests/regression/checks.h` and called from `main.c`. Checks use the `Check(bool, const char *)`
  pattern already there (report `FAIL: ...` and count). Success and expected-failure cases both.

## 1. Layout

```
core/store.h  store.c           things, kinds, fields, tree, ownership, the tick, timers, messages,
                                 random streams, snapshot/restore, hash          (§2)
core/store_save.c                save and load of a store as text                (§2.9)
core/world3d.h  world3d.c        built-in 3D kinds and their systems: node transforms, character
                                 movement, areas, solids, tilemap geometry and paths, ray casts (§3)
core/draw_path.h  draw_path.c    draw items -> draw calls: cull, keys, radix sort, lean submit,
                                 static batching. The only new core file that touches GL       (§4)
core/replay.h  replay.c          per-tick input recording and replay files       (§6.3)
core/scheme/kinds.scm            the Scheme prelude: define-kind, states, vec math helpers (§5)
gameplay/script/game_s7.h game_s7.c   the Scheme side: things as c-objects, handlers, rules (§5)
gameplay/game.h game.c           the runner: loads a project, runs the loop windowed or headless,
                                 records, replays, hashes; the presentation glue (models, HUD,
                                 particles, sound, camera)                        (§6)
gameplay/game_main.c             EngineApplicationMain for a Scheme-only project (§6.1)
tools/trench/main.c              the `trench` command: `trench run <dir> [flags]` (§6.1)
tools/store_bench/main.c         go/no-go condition 3 bench (§8)
tools/draw_bench/main.c          go/no-go condition 6 bench, headless EGL (§8)
tests/regression/store_checks.c world3d_checks.c draw_path_checks.c game_checks.c replay_checks.c
```

`Makefile.core`: `core/scheme` ships with the SDK beside `core/shaders` and `core/fonts`; `trench`
is a `core_project` target copied into `$(SDK)/bin`; `draw_bench` links `-lEGL` in addition.

## 2. The store (`core/store.h`)

### 2.1 Ids, symbols, values

```c
typedef struct StoreId { uint32_t index, generation; } StoreId;   /* index UINT32_MAX: none */
#define STORE_NULL ((StoreId){UINT32_MAX, 0})
typedef int32_t StoreSymbol;            /* interned name; STORE_NO_SYMBOL is -1 */
typedef enum StoreType { STORE_NONE, STORE_INT, STORE_FLOAT, STORE_BOOL, STORE_SYMBOL,
                         STORE_STRING, STORE_VEC3, STORE_REF,
                         STORE_LIST, STORE_SET, STORE_MAP, STORE_GRID } StoreType;
#define STORE_STRING_MAX 63
typedef struct StoreValue {
    StoreType type;
    union { int32_t i; float f; bool b; StoreSymbol sym; char str[STORE_STRING_MAX + 1];
            Vector3 v; StoreId ref; } as;
} StoreValue;
```

Symbols are interned per store in first-seen order: `StoreSymbol StoreIntern(Store *, const char *)`
and `const char *StoreSymbolName(const Store *, StoreSymbol)`. A symbol is never freed. Saves write
names, not ids, so ids need not match across processes; snapshots are in-process and keep ids.

### 2.2 Kinds and fields

```c
#define STORE_LOCAL 1u       /* presentation only: not shared, saved, hashed or replayed (B1) */
#define STORE_ENGINE 2u      /* written by the engine, read-only from scripts (on-floor) */
#define STORE_HIDDEN 4u      /* not listed by inspect; used for %prev-* fields */
typedef struct StoreFieldDecl {
    const char *name;
    StoreType type;          /* the field's type */
    StoreType element;       /* LIST/SET/GRID element type or MAP value type; scalar types only,
                                never STRING, LIST, SET, MAP, GRID */
    StoreType key;           /* MAP key type: INT, SYMBOL or REF */
    int max;                 /* capacity of LIST/SET/MAP, width of GRID; 0 for scalars */
    int height;              /* GRID only */
    unsigned flags;
    StoreValue init;         /* scalar default; STORE_NONE means the type's zero */
} StoreFieldDecl;
typedef int StoreKind;       /* index; -1 is none */
StoreKind StoreDeclareKind(Store *, const char *name, StoreKind base,
                           const StoreFieldDecl *fields, int count, const char **error);
StoreKind StoreKindNamed(const Store *, const char *name);
const char *StoreKindName(const Store *, StoreKind);
StoreKind StoreKindBase(const Store *, StoreKind);
bool StoreKindIs(const Store *, StoreKind kind, StoreKind base);   /* kind is base or extends it */
int  StoreFieldIndex(const Store *, StoreKind, const char *name);   /* -1: none */
int  StoreFieldCount(const Store *, StoreKind);                    /* inherited fields included */
const StoreFieldDecl *StoreFieldAt(const Store *, StoreKind, int field);
bool StoreKindSetDefault(Store *, StoreKind, int field, const StoreValue *);   /* override a default */
bool StoreKindSetDefaultAt(Store *, StoreKind, int field, int index, const StoreValue *key,
                           const StoreValue *value);   /* collection defaults (:init) */
```

Layout: a kind's fields are its base's fields first, then its own, so a base field index is the
same in every derived kind (single inheritance, B1). A derived kind may redeclare a base field only
to change its default (same type; `error` otherwise). Field names are unique within the chain.

Each kind has two block layouts, **shared** (fields without `STORE_LOCAL`) and **local**, packed in
declaration order with 4-byte alignment: INT/FLOAT/BOOL/SYMBOL 4 bytes, STRING `max+1` rounded up
(always 64), VEC3 12, REF 8, LIST/SET `4 + max*element`, MAP `4 + max*(key+value)`, GRID
`max*height*element`. A kind keeps a template of each block holding the defaults; spawn copies
them. Elements of SET are kept sorted (ints by value, symbols by name, refs by index then
generation); MAP is kept sorted by key the same way (B1.1). The store owns per-kind pools: one
growable array of shared blocks and one of local blocks, each with a free list, so a snapshot of a
kind's shared state is one `memcpy` per pool (E3).

### 2.3 Things and the tree

```c
typedef struct Store Store;
bool StoreInit(Store *, uint64_t seed);
void StoreFree(Store *);
StoreId StoreSpawn(Store *, StoreKind, int owner, StoreId parent, StoreSymbol childName);
bool StoreRemove(Store *, StoreId);        /* takes effect: gone from queries now; freed at tick end */
bool StoreAlive(const Store *, StoreId);
StoreKind StoreKindOf(const Store *, StoreId);
int  StoreOwner(const Store *, StoreId);   /* cached root owner (B3.1) */
int  StoreSpawner(const Store *, StoreId); /* the player whose leaving removes it (B3.7) */
StoreId StoreParent(const Store *, StoreId);
StoreId StoreFirstChild(const Store *, StoreId);
StoreId StoreNextSibling(const Store *, StoreId);
StoreId StoreChildNamed(const Store *, StoreId, StoreSymbol name);   /* declared children */
StoreSymbol StoreChildName(const Store *, StoreId);                  /* STORE_NO_SYMBOL for guests */
bool StoreIsGuest(const Store *, StoreId);
bool StoreAttach(Store *, StoreId thing, StoreId parent);   /* becomes a guest; owner follows root */
bool StoreDetach(Store *, StoreId thing);                   /* becomes a root owned by the host (0) */
int  StoreThings(const Store *, StoreKind kindOrDerived, StoreId *out, int max); /* id order */
uint32_t StoreCount(const Store *);
```

- `owner` on `StoreSpawn` is used only when `parent` is `STORE_NULL` (a root); a child's owner is
  its root's. `StoreSpawner` of a root is its owner at spawn; of a declared child, its parent's.
- Sibling order is deterministic: declared children in declaration order (the caller spawns them in
  that order), then guests in attach order. Removal of a thing removes its declared children
  (recursively) and detaches its guests at their last world transform: the store cannot compute
  world transforms, so it calls the `orphan` hook (§2.6) before detaching, and the world3d module
  writes the world position into the guest's local fields there.
- A thing can be made local (`bool StoreMarkLocal(Store *, StoreId)`, `StoreIsLocal`) while its
  `start` is still pending: every field of it then counts as `STORE_LOCAL` for §2.7, and its
  declared children, present or spawned later, are local too. Local things are left out of
  `StoreHash`, `StoreSave` (so a load never sees them; the Scheme layer respawns missing `:local`
  children from the declarations) and nothing else: snapshots keep them and `StoreThings` lists them.
- Handles never point at freed memory or at a newer thing in the same slot (generation check on
  every call). Ids are 64 bits so a Scheme c-object can carry one in its value word (§5.1).

### 2.4 Fields

```c
bool StoreGet(const Store *, StoreId, int field, StoreValue *out);
bool StoreSet(Store *, StoreId, int field, const StoreValue *);     /* converts INT<->FLOAT; else type error */
int  StoreCountOf(const Store *, StoreId, int field);               /* elements in LIST/SET/MAP */
bool StoreGetAt(const Store *, StoreId, int field, int index, StoreValue *key, StoreValue *value);
bool StoreSetList(Store *, StoreId, int field, const StoreValue *items, int count); /* LIST or SET */
bool StoreMapGet(const Store *, StoreId, int field, const StoreValue *key, StoreValue *out);
bool StoreMapSet(Store *, StoreId, int field, const StoreValue *key, const StoreValue *value);
bool StoreMapRemove(Store *, StoreId, int field, const StoreValue *key);
bool StoreGridGet(const Store *, StoreId, int field, int x, int y, StoreValue *out);
bool StoreGridSet(Store *, StoreId, int field, int x, int y, const StoreValue *);
bool StoreGridFill(Store *, StoreId, int field, int x, int y, int w, int h, const StoreValue *);
void *StoreSharedBlock(Store *, StoreId);   /* raw column access for native systems (B7) */
void *StoreLocalBlock(Store *, StoreId);
const char *StoreLastError(const Store *);  /* why the last call answered false */
```

Every write goes through the rule check in §2.7. A capacity overflow, a type mismatch, or a
read of a removed thing answers false with `StoreLastError` set to a sentence a script error can
carry verbatim, for instance `carried holds 4 items at most`.

### 2.5 The tick and its handlers

```c
typedef enum StorePhase { STORE_PHASE_NONE, STORE_PHASE_GAMEPLAY, STORE_PHASE_PRESENTATION } StorePhase;
typedef bool (*StoreHandlerFn)(Store *, StoreId self, StoreSymbol event,
                               const StoreValue *args, int count, void *user);
void StoreKindSetHandler(Store *, StoreKind, StoreHandlerFn, void *user);
void StoreKindHandles(Store *, StoreKind, StoreSymbol event, bool handles);
bool StoreKindHandlesEvent(const Store *, StoreKind, StoreSymbol event);  /* walks the base chain */
typedef void (*StoreSystemFn)(Store *, float dt, void *user);
bool StoreAddSystem(Store *, StoreSystemFn, void *user);     /* step 6: produces events */
void StoreTick(Store *, float dt);
void StoreFrame(Store *, float dt);          /* frame handlers, then -changed, then draw-hud */
uint64_t StoreTickCount(const Store *);
float StoreTickTime(const Store *);          /* seconds since the world started, ticks * dt */
StorePhase StorePhaseNow(const Store *);
StoreId StoreCurrent(const Store *);         /* the thing whose handler is running, or STORE_NULL */
int  StoreCurrentOwner(const Store *);       /* the owner that handler runs for */
bool StoreSend(Store *, StoreId target, StoreSymbol event, const StoreValue *args, int count);
bool StoreAfter(Store *, StoreId self, float seconds, StoreSymbol event, const StoreValue *args, int count);
bool StoreCommand(Store *, StoreId target, StoreSymbol event, const StoreValue *args, int count);
int  StoreCommandsPending(const Store *, StoreId *targets, StoreSymbol *events,
                          StoreValue (*args)[STORE_MAX_ARGS], int *counts, int max);  /* copies the
                          commands queued since the last tick, for the recording (§6.3) */
void StoreSetLocalOwners(Store *, const int *owners, int count);    /* which owners run here */
```

`StoreTick` runs B2.2 in order:

1. (network: phase 2; nothing here.)
2. Player commands queued by `StoreCommand` since the last tick (from presentation code, §5.6) are
   moved into the message queue, in the order given. The runner records them first (§6.3).
3. Timers due this tick fire as messages, in due-tick then creation order.
4. `tick` handlers of things whose owner is local, in id order, with `args = {dt}`.
5. Messages are delivered first in, first out. Delivery may queue more; after 10,000 deliveries in
   one tick the rest are dropped with a `TraceLog` warning naming the target and event. A message to
   a removed thing is dropped silently (B2.2). A message whose kind has no handler for it is reported
   once per kind and event through `TraceLog(LOG_WARNING, ...)`.
6. Systems run (`StoreAddSystem`, in registration order) and the events they queue are delivered.
7. Removals apply: storage of things removed this tick is freed, declared children with them, guests
   detached (§2.3). A detached guest whose kind handles `orphaned` gets it as a timer due next tick
   (B1, B3.7), which is delivered like any message: its new owner is the host, so on the host it runs
   there, and on a client that removed its own holder it goes out through the `outgoing` hook to the
   host (B3.4, §9.3). A removal store_net applies from another machine's state
   (`StoreRemoveReplica`, `core/store_internal.h`) sends nothing, since the machine that made it did,
   so `orphaned` runs once, on the host (a leaver's guests get it from store_net, §9.4). Then the
   store records which shared fields changed since the last `StoreFrame`
   (compare the shared pools against a shadow copy per kind, only for kinds that registered any
   `-changed` event; fields are compared per declared field, so a collection is one change).
   `start` always reaches a thing before its first `tick`. A thing spawned during a tick (from a
   `tick` handler, a message handler or a system) has `start` queued at the tail of the message
   queue at spawn, so it is delivered later in that tick's step 5 or 6. A thing spawned outside a
   tick (level setup, the REPL, a frame, a load) keeps its `start` pending; pending starts are
   delivered at the very beginning of the next `StoreTick`, in spawn order, before step 2's commands
   and step 3's timers.

Handlers run with `StoreCurrent` and `StoreCurrentOwner` set, `StorePhaseNow` at GAMEPLAY. A kind's
`StoreHandlerFn` is called only for events `StoreKindHandlesEvent` answers true for (so a kind
without a `tick` costs nothing per tick, B9.5); the function is the language's dispatcher and may
walk the base chain itself.

`StoreFrame` sets PRESENTATION and runs, for every thing (owned anywhere): `frame` with `{dt}`;
then, for every field with a registered `<field>-changed` event whose value differs from the
shadow, `<field>-changed` with `{was, now}` (`was` is `STORE_NONE` when the thing appeared since the
last frame, B2.1); then `parent-changed` with `{was, now}` for a kind that handles it, when the
thing's parent differs from the one the last frame saw (`was` `STORE_NONE` when it appeared, a null
REF for a root; a detach and re-attach between two frames is invisible); then `draw-hud` with no
args; then it refreshes the shadow. A `-changed` event symbol is the field name with `-changed`
appended, interned by `StoreKindHandles`.

### 2.6 Hooks the runner and world3d install

```c
typedef struct StoreHooks {
    void *user;
    void (*error)(void *user, const char *message);         /* a rule or type error (§2.7) */
    void (*orphan)(void *user, StoreId guest);              /* just before a guest is detached */
    void (*spawned)(void *user, StoreId thing);             /* after a thing's blocks exist */
    void (*removed)(void *user, StoreId thing);             /* before its blocks are freed */
    void (*outgoing)(void *user, StoreId target, StoreSymbol event, const StoreValue *args, int count); /* owned elsewhere (§9.3) */
} StoreHooks;
void StoreSetHooks(Store *, const StoreHooks *);
```

### 2.7 Rules (A4)

`StoreSet` and friends refuse, and `StoreGet` refuses, in these cases, each with a fixed message
prefix the Scheme layer completes with names and positions:

| Rule | Check | `StoreLastError` prefix |
|---|---|---|
| 1 read | phase GAMEPLAY and the field has `STORE_LOCAL` | `local-read` |
| 1 write | phase PRESENTATION and the field lacks `STORE_LOCAL` | `shared-write` |
| 5 | phase GAMEPLAY and `StoreOwner(target) != StoreCurrentOwner()` and the field lacks `STORE_LOCAL` | `not-owner` |
| engine | field has `STORE_ENGINE` and the writer is not the engine (`StoreSetEngine` bypasses) | `engine-field` |
| 4 | value type not convertible to the field's | `type` |
| capacity | collection full | `capacity` |

Outside any handler (phase NONE: loading, the REPL, the runner setting up) every read and write is
allowed. `bool StoreSetEngine(Store *, StoreId, int field, const StoreValue *)` is the engine's own
write path (world3d writing `on-floor`, `velocity` after a slide, `%prev-position`) and skips the
rules. `StoreAttach`/`StoreDetach` apply rule 5 to the thing being moved (only its owner may attach
or detach it, B1); `StoreRemove` applies rule 5 too.

### 2.8 Randomness, snapshot, hash

```c
uint32_t StoreRandom(Store *, StoreId thing, uint32_t n);   /* [0, n) from that thing's stream */
uint32_t StoreRandomLocal(Store *, uint32_t n);             /* the presentation stream */
typedef struct StoreSnapshot StoreSnapshot;
StoreSnapshot *StoreSnapshotTake(const Store *);
bool StoreSnapshotRestore(Store *, const StoreSnapshot *);
void StoreSnapshotFree(StoreSnapshot *);
uint64_t StoreHash(const Store *);
```

Each thing carries a 64-bit `splitmix64` state seeded from its spawner's stream at spawn (the world
root from `StoreInit`'s seed), so one thing's draws never shift another's (B4). The stream is part
of the thing's header, hashed, snapshotted and saved. A snapshot copies the thing table, every
kind's shared pool and free list, the timers, the tick count and the pending `start`s; the message
queue is empty at a tick boundary and is not copied. `StoreHash` is FNV-1a over, in id order:
kind name, parent index, child name, owner, spawner, rng state and the shared block bytes; then
the timers in order and the tick count. Local blocks are never hashed.

### 2.9 Save and load (`core/store_save.c`)

```c
bool StoreSave(const Store *, const char *path);   /* atomic, through core/file.h */
bool StoreLoad(Store *, const char *path);         /* into a store whose kinds are declared */
```

Text, one thing per stanza:

```
store 1 tick 4200 seed 1234
thing 7 gen 3 kind soldier parent 2 name eye owner 1 spawner 1 rng 9182736455
  health 100
  carried (pistol shotgun)
  ammo ((pistol 36) (shotgun 0))
  position (1.5 0 3)
  prey #7:3
timer 12 after 4260 reload-done ()
```

Loading tolerates changed kinds: a missing field takes its default, an unknown one is skipped
with one `TraceLog` warning per kind and field (B4). Ids are preserved (index and generation), so
references stay valid. Local fields are not saved.

## 3. Built-in 3D kinds (`core/world3d.h`)

`bool World3DInit(World3D *, Store *)` declares the kinds below (names as scripts spell them) and
registers one system (areas). It holds no GL objects: geometry is CPU arrays; §6 uploads them.

| Kind | Base | Fields (type, flags) |
|---|---|---|
| `node` | - | `position` VEC3, `rotation` VEC3 (Euler radians X Y Z), `scale` VEC3 (1 1 1), `visible` BOOL #t, `static` BOOL, `cull-distance` FLOAT 0; `%prev-position` VEC3 LOCAL HIDDEN ENGINE, `%prev-rotation` VEC3 LOCAL HIDDEN ENGINE |
| `model` | node | `mesh` STRING, `animation` SYMBOL, `animation-speed` FLOAT 1, `spin` FLOAT (radians/s about Y, presentation only: the drawn rotation adds `spin * time`), `tint` VEC3 (1 1 1), `for-owner` BOOL, `hidden-for-owner` BOOL, `viewmodel` BOOL |
| `socket` | node | `bone` STRING, `of` LIST of SYMBOL max 4 |
| `camera` | node | `fov` FLOAT 75, `for-owner` BOOL, `viewmodel-fov` FLOAT 60 (§3.1) |
| `light` | node | `type` SYMBOL (`ambient` `directional` `point`), `energy` FLOAT 1, `color` VEC3 (1 1 1), `range` FLOAT 10 |
| `character` | node | `radius` FLOAT 0.4, `height` FLOAT 1.8, `velocity` VEC3, `on-floor` BOOL ENGINE |
| `solid` | node | `size` VEC3 (1 1 1): an axis-aligned box centred on the node |
| `area` | node | `radius` FLOAT 1 (a sphere's), `shape` SYMBOL `sphere` (or `box`: an axis-aligned box of `size` centred on the node, like a `solid`; any other symbol reads as a sphere), `size` VEC3 (1 1 1) (a box's); `%inside` LIST of REF max 16 HIDDEN ENGINE (who was overlapping last tick) |
| `tilemap` | node | `width` INT 16, `depth` INT 16, `cell-size` FLOAT 2, `height` FLOAT 3, `cells` GRID of INT (width x depth, 0 open, 1 solid; declared with max = width, height = depth at the kind level, so the grid is 64 x 64 at most and the tilemap's `width`/`depth` say how much is used), `floor-texture` STRING, `wall-texture` STRING, `ceiling-texture` STRING |
| `sound` | node | `stream` STRING, `volume` FLOAT 1, `playing` BOOL |

Kind settings in a `(child eye (camera :at v :fov 75 :for-owner #t))` form are just field writes
after spawn (`:at` is `position`); the Scheme layer does that (§5.3). Settings on `(is character
:radius 0.35)` are default overrides for the derived kind (`StoreKindSetDefault`). An area's
`:shape` (in `is`, spawn and child settings, and `set!`) takes `'sphere`, `'box` or `(box x y z)`,
which sets `shape` to `box` and `size` to (x y z); any other shape is refused with a message naming
it.

Functions (all deterministic; no GL; every one refuses a stale id):

```c
bool World3DWorldMatrix(World3D *, StoreId, Matrix *out);       /* cached, tree order (B9.1) */
bool World3DWorldPosition(World3D *, StoreId, Vector3 *out);
bool World3DDrawMatrix(World3D *, StoreId, Matrix *out);        /* drawing: sockets follow bones */
typedef enum World3DBone { WORLD3D_BONE_NOT_DRAWN, WORLD3D_BONE_NONE, WORLD3D_BONE_POSED } World3DBone;
void World3DSetBoneLookup(World3D *, World3DBone (*fn)(void *user, StoreId model, const char *bone,
                                                       Matrix *out), void *user);
void World3DUpdateTransforms(World3D *, float alpha);   /* once per frame: interpolate %prev->now by
                                                           alpha (a parent change is a jump, B3.5),
                                                           recompose dirty subtrees parent first */
void World3DBeginTick(World3D *);                       /* copies position/rotation into %prev-* */
bool World3DMoveAndSlide(World3D *, StoreId character, float dt);
bool World3DTeleport(World3D *, StoreId node, Vector3 world);   /* sets %prev too: no interpolation */
typedef struct World3DHit { bool hit; float distance; Vector3 point, normal; StoreId thing; } World3DHit;
World3DHit World3DRaycast(World3D *, Vector3 from, Vector3 direction, float maxDistance, StoreId ignore);
bool World3DLineOfSight(World3D *, Vector3 from, Vector3 to);   /* static geometry only */
int  World3DOverlapping(World3D *, StoreId area, StoreKind kind, StoreId *out, int max);
StoreId World3DNearest(World3D *, StoreKind kind, Vector3 point, float maxDistance,
                       bool (*accept)(StoreId, void *), void *user);
bool World3DCellToWorld(World3D *, StoreId tilemap, int x, int z, Vector3 *out); /* cell centre, y 0 */
bool World3DWorldToCell(World3D *, StoreId tilemap, Vector3 world, int *x, int *z);
bool World3DPathNext(World3D *, StoreId tilemap, Vector3 from, Vector3 to, Vector3 *out);
int  World3DTilemapChunks(World3D *, StoreId tilemap, World3DChunk *out, int max);
```

- **Collision world**: tilemap solid cells are boxes `[x*cs, (x+1)*cs] x [0, height] x [z*cs,
  (z+1)*cs]` in the tilemap's world space (assume the tilemap is unrotated and unscaled: refuse
  otherwise with a warning); `solid` boxes are `size` centred on the world position; characters
  are boxes `2r x height x 2r` with the position at the feet. `World3DMoveAndSlide` moves by
  `velocity * dt` in three axis-separated sweeps (X, then Z, then Y) against every box except its
  own, resolving each axis to the contact face; when Y is blocked from below, `on-floor` is #t and
  `velocity.y` is zeroed; the final position is written with `StoreSetEngine`. Deterministic:
  boxes are tested in id order.
- **Areas**: the system runs each tick: for every `area`, the set of `character`s whose box
  intersects its shape (the sphere, or the box), in id order; a newcomer gets `touched` sent to the area with `{other}`, a
  leaver `untouched`. `%inside` holds the current set (so a save restores it).
- **Rays**: 3D DDA over tilemap cells, slab tests on solids and character boxes; nearest hit wins;
  `ignore` and things under it are skipped. `line-of-sight?` tests tilemap and solids only.
- **Paths**: breadth-first over open cells from `to`'s cell, 4-connected, cached per tilemap and
  target cell for the current tick; `World3DPathNext` answers the centre of the next cell toward
  `to` from `from`'s cell, or `to` itself when adjacent or unreachable.
- **Chunks**: 8x8 cells; `World3DChunk` carries the chunk's vertex arrays (positions, normals, uvs,
  as `MB` from `core/mesh_builder.h`), a bounding box, a content hash and which texture: one chunk
  yields up to three (floor, wall, ceiling). A chunk is rebuilt when its 64 cells' hash changed;
  the caller (§6) sees `changed` set and re-uploads. Floors are quads at y 0 on open cells, ceilings
  at `height`, walls on each solid face next to an open cell, UVs in cell units.
- **Transforms**: `World3DUpdateTransforms` is the cached pass (B9.1): a dirty bit per thing set by
  the `spawned` hook and by any write to `position`/`rotation`/`scale`/parent (the store exposes a
  per-tick write bitmap per kind block; simplest: world3d compares the node fields it cached last
  frame, 3 vec3 compares per node, which E7 shows costs under the budget). Interpolation is between
  `%prev-*` and the current values by `alpha`, except when the thing's parent changed this tick or
  `World3DTeleport` was called (then `%prev` equals the current value).
- **Socket**: in drawing, its world matrix is the named bone of the model it follows (its `of`
  list: the first listed sibling model drawn on this machine; with no `of`, its parent if that is a
  model) times that model's world matrix, supplied by the runner through
  `World3DSetBoneLookup(world, fn, user)`; with no bone or no animation, its own local transform.
  **Gameplay never sees the bone**: `World3DWorldPosition` and `World3DWorldMatrix` for gameplay
  (and every thing under the socket) use the socket's local transform, the rest pose. Animation
  runs only where things are drawn, and headless runs draw nothing, so a gameplay read of a bone
  would differ between a replay and play (rule 1, proposal A4: what is drawn never feeds gameplay).
  Only the drawn matrices (`World3DDrawMatrix`) follow bones.
- **Animation** (B6 `model`: `animation` SYMBOL, shared; `animation-speed` FLOAT, default 1):
  presentation only. Each drawn model with clips keeps a local playback clock that restarts when
  `animation` changes (seen by comparing, like everything else); a missing clip name holds the rest
  pose with one warning per name. Skinning follows B9.3: on the GPU through the engine's
  `core/shaders/skinning.vs` when the model has at most 24 bones (GLSL 120's 512 vertex uniform
  components), else on the CPU with raylib's `UpdateModelAnimation`. The draw path takes the bone
  matrices per item (`DrawItem.bones`, `boneCount`) and uploads them only for skinned materials.

## 4. The draw path (`core/draw_path.h`)

```c
typedef struct DrawItem {
    uint32_t mesh;        /* DrawPathMesh id */
    uint32_t material;    /* DrawPathMaterial id: shader, texture, tint */
    Matrix world;
    Vector3 center; float radius;   /* world-space bounding sphere */
    uint8_t layer;        /* 0 opaque, 1 alpha-tested, 2 translucent, 3 viewmodel */
    const Matrix *bones; int boneCount;   /* skinned: read at submit; NULL/0 for static meshes */
} DrawItem;
typedef struct DrawStats { int items, visible, draws, shaderSwitches, textureSwitches, boneUploads;
                           double cullMicros, sortMicros, submitMicros; } DrawStats;
bool DrawPathInit(DrawPath *);
void DrawPathFree(DrawPath *);
uint32_t DrawPathMesh(DrawPath *, const Mesh *);        /* uploads a copy; 0 on failure */
uint32_t DrawPathMeshUpdate(DrawPath *, uint32_t id, const Mesh *);   /* re-upload (chunks) */
bool DrawPathMeshPositions(DrawPath *, uint32_t id, const float *positions, const float *normals);
                                                        /* in place, for CPU skinning */
uint32_t DrawPathMaterial(DrawPath *, Shader, Texture2D, Color tint, int normalMatrixLoc);
int DrawPathStaticBatch(DrawPath *, const DrawItem *items, int count, DrawItem *out, int max);
                                        /* one merged world-space mesh per material (and per 65,536
                                           vertices), as items written to out; returns how many */
bool DrawPathMeshRelease(DrawPath *, uint32_t id);  /* frees a mesh; a later upload reuses its id */
void DrawPathBegin(DrawPath *, Camera3D camera, int screenWidth, int screenHeight);
void DrawPathAdd(DrawPath *, const DrawItem *);
DrawStats DrawPathEnd(DrawPath *);   /* cull, key, sort, submit; inside BeginMode3D */
```

`DrawPathEnd` does B9.2 exactly: sphere-against-six-planes cull (a `cull-distance` is applied by the
caller by not adding the item); 64-bit key `layer(2) | shader(8) | material(12) | mesh(12) |
depth(16)` for layers 0 and 1, and `layer(2) | depth(16, back to front) | shader | material |
mesh` for layer 2, with the material/mesh part cached per item and only the depth refreshed
(E9); an 8-bit LSD radix sort on the visible keys; then a submit loop that enables the shader
only when the key's shader bits change, binds the texture only when it changes, computes MVP on
the CPU per item and uploads it, uploads the model matrix and the normal matrix only for a
material whose `normalMatrixLoc >= 0`, and calls `rlDrawVertexArrayElements`. It does not call
`DrawMesh`. Start from `experiments/e9_cull_sort/e9.c` and `experiments/e6_draw_cost/bench.c` in
the design repository for the sort and the submit body. Layer 3 (viewmodel) is drawn last with the
depth buffer cleared and its own projection; `DrawPathEnd` approximates that by drawing layer 3 last
with depth testing off, and the runner does it properly with a pass of its own (§3.1).

Static batching (B9.3): items with the same material are merged into one world-space mesh (apply
each `world` to positions and normals); the result is a mesh id the caller adds as one item with the
identity matrix and the merged bounding sphere.

Skinning: a mesh with bone ids and weights keeps them and uploads them at raylib's attribute
locations for `vertexBoneIds`/`vertexBoneWeights`; a material whose shader has `boneMatrices` has a
bone-matrix location, and the submit uploads an item's `bones` (`rlSetUniformMatrices`) only for such
a material and only when the item has them (`boneUploads` counts them). Skinned items are never
batched. `DrawPathMeshPositions` rewrites a mesh's positions and normals in place for the CPU path.

## 5. The Scheme layer (`gameplay/script/game_s7.c`, `core/scheme/kinds.scm`)

The existing `script_s7.c` is untouched. `game_s7.c` is a second frontend for the store, with its own
`s7_scheme`. Read `.claude/skills/s7-embedding/SKILL.md` in the design repository and `vendor/s7/s7.h`
before writing it.

### 5.1 Things as values

One c-type `thing` whose value word is the `StoreId` packed into a pointer-sized integer
(`(uintptr_t)index << 32 | generation`); no allocation per access, no GC free, equality by value.
`ref` makes `(thing 'field)` read a field (or a declared child by name) and `(thing 'method args...)`
call the methods below; `set` makes `(set! (thing 'field) v)` write. A removed thing prints as
`#<removed soldier>` and every access to it is an error naming the kind. A REF field that names a
removed thing reads as `#f` (B1).

Values cross as: INT integer, FLOAT real, BOOL boolean, SYMBOL symbol, STRING string, VEC3 a
float-vector of 3 (`(vec3 x y z)` makes one; `vx` `vy` `vz` read), REF a thing or `#f`, LIST/SET a
fresh list, MAP a `map-view` c-object (applicable: `(ammo 'pistol)`, `(set! (ammo 'pistol) 12)`,
`map-keys`, `map-values`, `map-remove!`, `map-for-each`; assigning an alist to the field replaces
the map), GRID a `grid-view` c-object (`grid-ref`, `grid-set!`, `grid-fill!`, `grid-fill-rect!`,
`grid-width`, `grid-height`). Every C function is registered with `s7_define_typed_function`
where it never calls back into Scheme, and with `s7_define_function` where it does.

### 5.2 `define-kind`

`core/scheme/kinds.scm` defines the macro. It accepts, in any order after the name:

```scheme
(is base :setting value ...)        ; one base kind; settings override that kind's field defaults
(field name default [:local #t] [:init v])
(field name (list-of T :max n) [:init '(...)])   ; also set-of, (map-of K V :max n), (grid-of T w h)
(field name (ref kind))
(child name (kind :setting value ...) child...)  ; :at is position; nested children allowed
(on (event arg ...) body ...)
(define (helper arg ...) body ...)
(states initial (name (on ...) ...) ...)
```

Type of a scalar field from its default: exact integer INT, real FLOAT, boolean BOOL, symbol
SYMBOL, string STRING, float-vector VEC3, `#f` with `(ref k)` REF. The macro expands to calls on
`%kind-declare` (name, base, field decls, then children), and for each handler and helper
`(%kind-handler kind 'event (lambda (self args...) walked-body))`. The **code walk** rewrites, in
handler and helper bodies, a free reference to a field or declared child name `n` into `(%field
self i)` / `(%child self i)`, `(set! n v)` into `(%set-field! self i v)`, and a call `(helper
args)` into `(%helper-name self args)`; it respects shadowing by `lambda`, `let`, `let*`,
`letrec`, `do`, named `let` and inner `define`, and it does not descend into `quote`. Inherited
fields and children are in scope too (the base kind's declaration is looked up at expansion).
`self` is bound in every handler and helper. About 150-200 lines (B5.2); it is the risk named in
D.3, so it is written first and checked by a Scheme test file `tests/regression/kinds_walk.scm`
evaluated by the checks.

`states`: adds a hidden SYMBOL field `state` with the initial value, defines `(go 'name)` (records
the pending state), registers each state's handlers as `state:event`; dispatch tries `state:event`
first; after a handler returns, a pending `go` runs `exit` for the old state and `enter` for the new
one (B2.5).

### 5.3 Spawning and children

`(spawn 'kind :at v :owner p :parent thing)` spawns the kind and, in declaration order, every
declared child recursively, applying each child's settings as field writes, then queues `start`.
It answers the thing at once. `(remove thing)`, `(attach! thing parent :at v :rotation r)`,
`(detach! thing :at world-position :up normal :yaw y)` and `(detach! thing :keep-world #t)` (a
thing that is already a root and is given a placement is only placed, under the rules, so an
`orphaned` handler can seat what the engine detached; a root with no placement is refused),
`(parent t)`, `(children t)`, `(first-child t)`, `(child t 'name)`, `(is? t 'kind)`, `(kind-of t)`,
`(things 'kind)`, `(game)` (the root thing of kind `game`), `(local-player)`, `(players)`.

### 5.4 Handlers, messages, timers

`(send thing 'event args...)` from gameplay code queues a message (`StoreSend`); from presentation
code it is a player command (`StoreCommand`). `(after seconds 'event args...)` sets a timer on
`self`. Arguments are data: an argument that is a procedure, a hash table, a pair or a port raises
the rule 4 error. Engine events reach handlers by the same dispatcher: the C `StoreHandlerFn` for
Scheme kinds looks up `event` in the kind's handler table (an s7 hash table keyed by symbol,
protected), walks to the base kind if absent, and calls the closure with `self` and the args
converted by §5.1. Errors raised inside are caught (`s7_call_with_catch`) and reported once as
`<kind> #<index> <event>: <message> (<file>:<line>)` from the owlet's `error-file`/`error-line`
when present; the handler is not retried that tick, and the game keeps running.

### 5.5 The five rules, as errors

Each is an s7 error whose message is the sentence in proposal A4's table, with names filled in:

1. `local-read` -> `hurt is a local field (this screen only). The tick handler of soldier can't
   read it, because other players and replays don't have it. If gameplay needs it, remove :local
   from its declaration.` and `shared-write` -> `mesh on gun is shared state; a presentation
   handler can't write it. Declare the field :local, or write it from a gameplay handler.`
2. Top-level definitions frozen after load (§5.7): s7's own `immutable` error is caught and
   reworded: `can't set! score: top-level definitions are frozen once the game has loaded, ...`
   (the full A4 text).
3. `real-time`, `current-time`, `open-input-file`, `open-output-file`, `load` in a handler ->
   `real-time is for presentation. Gameplay code runs again on replay and on other machines, where
   the clock differs. Use (tick-time), seconds since the world started.`
4. `type` on a procedure/table/port -> `can't store a procedure in on-hit: fields hold data so they
   can be saved and sent. Store a symbol and dispatch on it: (set! on-hit 'explode) ...`
5. `not-owner` -> `health on soldier #12 belongs to player 2, and this handler runs for the host,
   so it can't write it. Send a message instead: (send other 'collect 'medkit), with a matching
   (on (collect what) ...) in soldier.`

The regression check loads a script per rule headless and asserts the error text's first sentence.

### 5.6 The rest of the surface

Input (read from the tick's `GameInput`, §6.2): `(define-actions (name "Key") ...)`, `(held?
'action)`, `(pressed? 'action)`, `(input-vector 'left 'right 'forward 'back)` -> vec3 (x, 0, z)
normalised, `(mouse-motion)` -> vec3 (dx, dy, 0). Vectors: `vec3 vx vy vz v+ v- v* vscale vlength
vdistance vnormalize vdot vcross rotate-y heading aim spread clamp`. World: `world-position`,
`raycast` (from dir max :ignore thing-or-list, up to 16) -> hit or #f, `hit-thing hit-point hit-normal hit-distance`,
`line-of-sight?`, `nearest` (kind point :max :where), `box` (x y z: an area's `:shape`), `overlapping` (area kind), `aimed-at` (camera
kind distance, or kind distance from a handler: the first camera under its thing; areas are
targets by their shape), `path-next` (tilemap from to), `cell->world`, `world->cell`, `move-and-slide!`,
`teleport!`, `random` (n; from `self`'s stream, or the presentation stream in presentation phase),
`tick-time`. Presentation: `draw-text` (text x y :size :color :align), `draw-rect` (x y w h :color),
`draw-ring` (x y r [fill 0-1, clockwise from the top] :color), `draw-image` (name x y), `screen-width`, `screen-height`, `rgba`,
colour symbols `white black red ...`, `play-sound` (name :at), `burst` (preset :at), `profile-ref`,
`profile-set!` (a per-player key/value file beside the project, presentation only), `format` (s7's).
Networking: `(host-game port)`, `(join-game address port)` (presentation or REPL only; §9.6),
`local-player`, `players`, answered through `GameS7SetNetwork` by the runner. REPL:
`(things 'kind)`, `(inspect thing)` prints every field, `(reload)`, `(save-game path)`,
`(load-game path)`, `(snapshot)`, `(restore s)`.

Every one is a row-like C function in `game_s7.c`; there is no `script_api.def` row for them
because they take keyword arguments and things, which that table has no types for. State that in
`gameplay/script/README.md`.

### 5.7 Environment, freeze, reload

Game files load into `(sublet (rootlet))`. After load, the environment and every binding in it,
and the funclets of every closure reachable from it (E5's `freeze-let!`, in
`experiments/e5_s7_determinism/traps.c` of the design repository), are made immutable. The clock
and file functions listed in §5.5 are shadowed in that environment by procedures that raise the
rule 3 error. `(reload)` loads the game files into a fresh environment, re-registers every kind's
handlers (kinds keep their ids; a kind whose fields changed is migrated, as "Reload migrates changed
kinds" below says), and re-resolves cached closures. The stdin REPL evaluates in that
environment between frames, as `script_s7.c`'s does, and records each line as a developer command
in the recording (§6.3).

### 3.1 What the first version draws (B6, B9)

- **Static batching (B9.3).** At the first frame after a `:static #t` thing (and every model under
  it) exists, the runner merges static models by material into world-space meshes with
  `DrawPathStaticBatch`, one batch per material per 8x8-cell region of the tilemap they stand on
  (or per material when there is no tilemap), and draws the batches instead of the things. A
  batched static thing that moves (its own position, rotation or scale, or an ancestor's) is, on
  the machine that owns it, an error naming the flag (`position on lamp #12 changed, but lamp is
  :static; remove :static if it moves`); on another machine it is no error, and it leaves its batch
  and is batched again where it now stands. A removed static thing rebuilds its region's batch at
  the next frame.
- **Lights (B6).** The world shader takes the ambient light, one directional light, and up to four
  point lights (`type 'point`, `color`, `energy`, `range`): the four nearest the camera among the
  visible ones, lit per pixel with a fixed loop of four (unused slots have zero energy), attenuation
  `energy * max(0, 1 - d / range)^2`. Four is the budget question 10 names ("a few dynamic lights");
  more would cost fill on the X61's GMA 965, where pixels, not calls, are the limit (B9.6).
- **Viewmodel (B6, B9.2 layer 3).** Models with `viewmodel #t` draw after the world, for their
  owner only, through `DrawViewmodel` (`core/viewmodel.h`, the same pass Trenchfoot uses): the
  local camera's position and orientation, the camera's `viewmodel-fov` field (new on `camera`,
  default 60 degrees) and a depth range of 0.01-10 m, with the depth buffer cleared first, so a gun
  never clips into a wall. Other machines draw them like any model (or not at all with
  `hidden-for-owner` on the third-person copy).
- **Sound (B6).** A `sound` thing with `playing #t` loops its `stream` through `core/audio.h` as a
  positional emitter: volume from `volume` and distance to the local camera (linear falloff to
  silence at 30 m), pan from the camera's right vector. Headless plays nothing.

As built (`gameplay/game.c`; the checks are `tests/regression/present_checks.c`), where the above
left room:

- *Static batching.* A model is batched when it or an ancestor has `static` set, it is drawn here,
  and it has no clips, no `spin`, no `cull-distance` and no `viewmodel` (all of which need it drawn
  as itself); none under a `socket`. Its region is the 8x8-cell chunk of the first tilemap whose
  cells hold its position (`World3DWorldToCell`), or one region for everything on no tilemap. A
  region is re-merged (`DrawPathStaticBatch`, the old merged meshes freed with
  `DrawPathMeshRelease`) at the end of a frame in which a member joined, was removed, stopped being
  drawn here, or changed `mesh`, `tint`, `spin`, `viewmodel`, `cull-distance` or its static thing.
  The transform error is **the runner's check at the next frame**, not a store rule at the write:
  it compares every batched model's gameplay world matrix (`World3DWorldMatrix`, so a moved
  ancestor anywhere above it counts) with the one it was batched at. A store rule could name the
  handler's line, but batching happens only where a window draws, so refusing the write there would
  make gameplay differ between a headless replay and play (rule 1). Only the machine that owns the
  model (`StoreOwnedHere`) reports it, since only there did its own game code make the write: the
  write stands, the error is reported once per static thing per frame (the bottom-of-screen line
  and `ERROR: RUN: ...`), and the moved models are drawn as themselves from then on. On any other
  machine the move came from its owner: the model leaves its batch without an error and is batched
  again at its new place in the same frame. The error names the model whose matrix changed: when
  its own fields changed, `position on bulb #13 changed, but it hangs from lamp #12, which is
  :static; ...` (or `... but lamp is :static; ...` for the static thing itself); when they did not,
  `an ancestor of bulb #13 moved it, but ...`. `--no-static-batch` draws everything as itself.
- *Lights.* "Visible" is the light's `visible` field and its range sphere reaching into the view
  frustum; the nearest four by distance from the camera to the light are uploaded as
  `pointPosition[4]`, `pointColor[4]` (colour times energy) and `pointRange[4]`, and unused slots
  have zero colour. With no directional light the default sun of §6.2 still shines, so a dark scene
  places a directional light of energy 0. The skinning vertex shader shares `world.fs`, so skinned
  models are lit the same way.
- *Viewmodel.* The owner's viewmodel items are collected apart from the world's and drawn after the
  world and the particles by `DrawViewmodelCleared` (`core/viewmodel.h`: `DrawViewmodel`'s pass
  with the depth buffer cleared and its own clip planes, which a draw path begun inside it uses
  too), as a second `DrawPathBegin/End` on a camera with the local camera's place and
  `viewmodel-fov`; its draws add to the frame's stats. The runner no longer uses layer 3.
- *Sound.* Each `sound` thing gets a held voice (`CoreAudioVoiceCreate` on the `game` bus, looping,
  `CoreAudioVoicePlayAt` with range 30, then moved and re-gained each frame, stopped when `playing`
  turns off, freed when the thing goes); the stream is loaded whole, as `play-sound`'s sounds are.
  `GameSoundHeard` (`gameplay/game.h`) is the gain and pan it is heard at, for checks.


**Reload migrates changed kinds (B5.1, B4).** `(reload)` re-declares a kind whose fields changed
with `StoreRedeclareKind(store, kind, fields, count)`: every existing thing of that kind (and of
kinds derived from it) keeps each field whose name and type are unchanged, a new field gets its
default, a removed field is dropped, and a field whose type changed gets its default; each dropped
or reset field is reported once per kind and field, as loading a save does. Kind ids do not change.
In a networked session a reload that changes any kind's fields is refused with a message saying to
restart the session: every machine must agree on the kinds (the B3.7 kinds check), and nothing can
change them on the other machines mid-session. A reload that changes only handlers and helpers is
allowed there.

**The REPL in recordings (B5.5).** Each REPL line is recorded as a developer command with the tick
it ran before; a replay evaluates it at the same point the live line ran (between frames, before the next
tick's input reaches the game), so replay and play agree, and its printed answer is discarded. REPL code runs outside handlers, so the store's
rules do not apply to it, except in a networked session, where a write to a thing this machine does
not own is refused with the rule 5 message: the owner's next state would overwrite it silently,
which is the failure rule 5 exists to make loud.
## 6. The runner (`gameplay/game.c`)

### 6.1 Projects and entry points

A Scheme-only project has an `engine.project` with `game <file.scm>` and no `source` line. The
engine-side `EngineApplicationMain` in `gameplay/game_main.c` reads the manifest beside the
executable (engine-build copies it) or the directory given as the first argument, and runs it.
`tools/trench/main.c` is the same entry for developers: `trench run <dir> [flags]`. Flags:

```
--headless           no window, no GL, no audio; ticks as fast as the CPU allows
--ticks N            stop after N ticks (headless), or after N ticks in a window
--seed S             world seed (default: from the clock; always written to the recording)
--record FILE        record this session (§6.3)
--replay FILE        replay FILE's inputs; with --headless nothing is drawn
--hash-every N       print "tick T hash H" every N ticks (and at the end)
--bot                headless only: synthesize input from the seed each tick (a random walk over
                     the actions and mouse), for a recording nobody had to play
--bench              print per-tick and per-frame timing at exit (p50/p99 in microseconds, and the
                     draw stats), as tools/store_bench does
--save FILE / --load FILE
--present            with --headless only: each tick also runs World3DUpdateTransforms(1) and
                     StoreFrame(dt) without GL, so frame, -changed and draw-hud run; the HUD list is
                     counted and cleared each tick, play-sound and burst do nothing, screen-width and
                     screen-height answer 1280x720; --bench prints "bench hud calls per frame p50 N"
--shot-every N       windowed only: after drawing, every N ticks, save DIR/shot_<tick>.png and print
                     "shot T PATH"
--shot-dir DIR       where --shot-every writes (default: the current directory)
--no-time-limit      GameS7SetHandlerLimit(0): no handler is stopped (default 0.05 s)
--host PORT / --join ADDRESS:PORT, --bot-until N, --print-field KIND FIELD, --print-count KIND: §9.6
--no-static-batch    windowed only: draw every :static model as itself (§3.1; for comparing shots)
```

A handler that runs past the limit (proposal B2.6, 50 ms) is stopped: `Run` in game_s7.c arms the
limit around the dispatcher's `s7_call`; s7's begin hook, and `GameS7LimitCheck` at the entry of the
engine's field, thing, number and vec3 argument helpers, read the clock every 256th call, then set
`all_done` or raise `handler-time-limit`. The report is `the <event> handler of <kind> #<n> ran for
over 50 ms and was stopped; the loop at <file>:<line> may never end`, with the handler's `(on ...)`
line, throttled like other handler errors. Its effects so far stay. A loop that enters no body and
calls no engine function is not seen. SIGINT (Ctrl+C) sets a flag (sigaction, no SA_RESTART): the
same checks stop the running handler, `Update` returns false, `run: interrupted (press Ctrl+C again
to kill)` is printed once, and `Shutdown` runs as usual; a second SIGINT restores the default
disposition and re-raises. With no display, `EngineRunApplication` starts GLFW once to ask before
`InitWindow` (which in raylib 5.5 carries on into OpenGL after a failed start and crashes) and
returns 1 with `Engine: no display; run with --headless or under a display`.

### 6.2 The loop

Per tick (`Update`): build the tick's `GameInput` (action bit set, pressed edges, mouse delta) from
`EngineInput` and the actions declared by `define-actions`, or from the replay file; append it to
the recording; `World3DBeginTick`; `StoreTick(dt)`. Per frame (`Draw`): `World3DUpdateTransforms(alpha)`;
`StoreFrame(dt)`; upload changed tilemap chunks and new models; build draw items from every visible
`model` (one per mesh of its file; `for-owner`/`hidden-for-owner` decide per machine; layer from
the material) and every tilemap chunk; `DrawPathBegin/Add/End` with the local player's `for-owner`
camera (or a default camera when none); particles through `CoreParticlesDraw`; then the HUD calls
queued by `draw-hud` are drawn in order; REPL poll. Headless skips the frame entirely (B2.2).

Models: `mesh` names resolve through the project's data root; `.glb`, `.obj` and `.iqm` load
through raylib once per name; a missing file gets a 0.5 m cube and one warning naming the file, so a
game runs before its art exists. `animation` selects a clip by name from the file's animations
(§3 Animation): each drawn model thing with clips has a clock that restarts when `animation`
changes and runs at `animation-speed`, choosing one of raylib's 17 ms glTF frames; the pose comes
from raylib's `UpdateModelAnimationBones` (one evaluator for the GPU path, the CPU path's
`UpdateModelAnimation` and the socket lookup, for any bone count), not the engine's `Actor`, whose
interpolation, aim and blending nothing here asks for and which stops at `CORE_BONE_CAPACITY`. Up
to 24 bones: GPU skinning, `core/shaders/skinning.vs` built with `SKINNING_BONES 24` over `world.fs`
(`CoreLoadShaderDefined`), lit and fogged like everything else; more: CPU skinning into per-thing
draw path meshes. A clip name the file lacks holds the rest pose with one warning per model and
name; a hitch advances a clock by at most 0.1 s. Every model is drawn at `World3DDrawMatrix`, and
the runner's bone lookup answers for drawn, posed models, so sockets follow bones in drawing only.
Headless does none of this. Sounds: `play-sound` loads a `.wav`/`.ogg` once per name; missing is one warning.
`burst` presets: `muzzle-flash`, `blood`, `dust`, `sparks`, as `CoreParticles` emits with fixed
parameters. Lighting: one GLSL 120 pair `core/shaders/world.vs` / `world.fs`: ambient plus one
directional light from the first `light` of type `directional` (default from above) plus up to four
point lights (§3.1), distance fog, a diffuse texture, `tint`. Static models are drawn in batches,
viewmodels in their own pass and `sound` things play as emitters (§3.1).

The window path is built and checked for build and for draw statistics only. It is not verified in
play by this work; the user does that on their hardware.

### 6.3 Recording and replay (`core/replay.h`)

```c
typedef struct ReplayTick { uint32_t actions; uint32_t pressed; float mouseDx, mouseDy;
                            uint16_t commandCount, packetCount; /* commands, then packets */ } ReplayTick;
bool ReplayOpenWrite(Replay *, const char *path, uint64_t seed, const char *engineRevision, uint64_t kindsHash);
bool ReplaySetRole(Replay *, ReplayRole role, int player);           /* §9.6 */
bool ReplayWriteTick(Replay *, const ReplayTick *, const ReplayCommand *commands, const ReplayPacket *packets);
bool ReplayOpenRead(Replay *, const char *path, uint64_t *seed, uint64_t *kindsHash); /* role in Replay */
bool ReplayReadTick(Replay *, ReplayTick *, ReplayCommand *commands, int max);
bool ReplayReadPacket(Replay *, ReplayPacket *);                     /* the tick's packets, in order */
void ReplayClose(Replay *);
```

A command is `{StoreId target; char event[32]; StoreValue args[4]; int count}`, written as text
inside a binary stream is fine; simplest is a small binary format with a magic and a version.
Version 3 gives each command a kind: a player command as above, or a REPL line (`ReplayCommand.kind`
`REPLAY_COMMAND_REPL`, its `text`), in the order they reached the store; the commands a REPL line
queued are not recorded, since replaying the line queues them again. Versions 1 and 2 still read.
`--repl-file FILE` feeds the REPL lines `TICK TEXT` just before each tick TICK (§5.7).
Replay refuses a file whose kinds hash differs from the loaded game's (`StoreKindsHash(store)`: FNV
over every kind's name and field declarations) with a message naming the mismatch. A replay in a
fresh process with the same build, seed and file reaches the same `StoreHash` at every tick; the
`--hash-every` output is what the go/no-go compares across three processes.

## 7. Phase 0 (in `core/engine.h`, `core/diagnostics.h`, `core/net_session.h`)

- `EngineConfig.headless` (bool) and `EngineConfig.maxTicks` (uint64, 0 = unlimited). Headless:
  no `InitWindow`, no capability check, no audio, no `Draw`, no `BuildUi`; the loop runs `Update`
  with `fixed_dt` and a zero `EngineInput` until it returns false or `maxTicks` is reached, with
  no sleeping; `CoreSetDataRoot` still runs. `EngineRunningConfig` works.
- `CoreDiagnostics` gains `double tickMicrosLast, tickMicrosMax, tickMicrosTotal; uint64_t ticks;
  double frameMicrosLast, frameMicrosMax, frameMicrosTotal;` filled in both modes with
  `clock_gettime(CLOCK_MONOTONIC)`.
- `CoreNetSession` gains `uint64_t bytesSent, bytesReceived;` (cumulative, from ENet's
  `totalSentData`/`totalReceivedData` at each `CoreNetSessionStep`) and `double sendRate,
  receiveRate;` (bytes per second over the last whole second).
- Regression checks: a headless run of 100 ticks through `EngineRunApplication` (no window is
  created; the check asserts `ticks == 100` and that `tickMicrosTotal > 0`); a loopback session
  step showing the counters rise.

## 8. Checks and benches

- `tests/regression/store_checks.c`: kinds and inheritance; every field type and collection incl.
  capacity refusal and sorted order; spawn/remove/children/guests/attach/detach and owner
  following the root; the tick order (a script-free C kind whose handler logs events; asserts the
  B2.2 order and that `remove` drops queued messages); timers; rules 1, 5, engine-field, type;
  snapshot/restore/hash equality; save/load round trip and a changed-kind load.
- `tests/regression/world3d_checks.c`: collide-and-slide into a wall and onto a floor; area
  touched/untouched, for a sphere and a box (with overlapping and a ray that tests areas); raycast against a cell and a character; line of sight; path-next around a
  wall; chunk rebuild on a cell change; transform caching (a child under a moved parent moves, a
  child under an unmoved parent's matrix is bit-identical to last frame's); sockets with a fake bone
  lookup (a guest draws on the bone of the first drawn model of `:of`, or of its parent model, while
  gameplay's world position stays the rest pose; with no lookup, no bone, an undrawn or unposed
  model, both are the rest pose).
- `tests/regression/anim_checks.c`: the test rig (`tools/make_test_rig.py`) loads with its bones by
  name and its clips `idle` and `wave`; GPU and CPU skinning of one pose cover the same pixels.
  `runner_checks.c` runs `tests/regression/anim/` in a window (with and without `--skin-on-cpu`):
  the box in the rig's hand socket is drawn over 0.2 m apart at ticks 30 and 60 while
  `(world-position box)` is the rest pose at both, and the arm's pixels move.
- `tests/regression/draw_path_checks.c`: key order for opaque and translucent items; radix sort
  equals qsort order; cull counts; static batch vertex count; skinning (bones move the drawn mesh,
  uploads only for skinned materials with bones). GL-dependent parts run under the hidden window the
  suite already opens.
- `tests/regression/present_checks.c` (§3.1; `regression_test --present` runs only these): the
  gain and pan `GameSoundHeard` gives at 0, 15, 30 and 45 m and on either side; then, in a window,
  `tests/regression/statics/` (200 static crates: draws against regions x materials + non-static
  items, the same pixels as `--no-static-batch` before and after a move and a removal, the moved
  crate's error, the error naming a crate whose parent node moved, and a crate handed to player 2
  by `attach!` that moves with no error here and is drawn at its new place), `lights/` (the floor under a point light against twice its range away, lit
  and at energy 0, a skinned rig lit, a fifth light left out), `viewmodel/` (a box behind a wall
  hidden as a model and drawn as a viewmodel; another player's `:for-owner` viewmodel not drawn)
  and `emitters/` (a run with sound things and no audio device ends cleanly).
- `tests/regression/game_checks.c`: `define-kind` walk (from `kinds_walk.scm`); the five rule
  errors; a headless bot session of 600 ticks recorded then replayed to the same hash.
- `tools/store_bench`: 1,000 things of a 12-field kind all changing every tick, 3,600 ticks: prints
  µs per tick p50/p99, snapshot, hash. Condition 3.
- `tools/draw_bench`: E6's method (EGL surfaceless, geometry clipped, `glFinish` per frame): 400
  items / 120 draws through the draw path, 600 frames, prints CPU µs per frame p50/p99. Condition 6.
  Then 16 skinned test rigs on the GPU path, with and without the bone upload, and unskinned: CPU µs
  per frame and per skinned draw, and the upload's share (B9.3's measurement).
- `tools/net_bench` (`make -f Makefile.core build/core/net_bench`, not built by `all`): three stores
  in one process over an in-memory transport (150 ms each way, no loss), a host with 30 things of a
  10-field kind all changing every tick and two clients each moving a soldier, 3,600 ticks: prints
  the host's µs per tick p50/p99/mean for receiving, `StoreNetBeforeTick`, `StoreNetAfterTick` and
  their total, the bytes the host sent and the state hashes (so a cost change can be shown to leave
  the wire alone).
- `tools/bench_x61.sh LABEL`: builds and runs both benches pinned to the last core and writes
  `bench_LABEL.txt`, for the user's ThinkPad X61.

## 9. Phase 2: networking on the store (`core/store_net.{h,c}`)

This implements proposal B3 (owners, what moves on the wire, change detection by comparison,
consistency, interpolation, joining and leaving) for things in a store. It knows nothing about
sockets: it produces and consumes byte packets through two callbacks, so the same code runs over
ENet in the runner and over an in-memory queue in the checks. It may include `store_internal.h`, as
`store_save.c` does, to create things with a given kind, parent, child name, owner and spawner.

### 9.1 Machines, owners, network ids

- The host is machine 0 and owns things with owner 0; it also plays as player 1. Clients are players
  2-16, one machine each; a client's local owners are `{p}`, the host's `{0, 1}` (B3.1).
- Every replicated thing has a **network id**: `u32 = creator machine << 27 | counter`, the counter
  per creator starting at 1 and never reused in a session. Each machine keeps both maps (local
  `StoreId` <-> network id). A thing gets its network id when this machine first sees it (it spawned
  it, or it arrived). Local things (`StoreMarkLocal`) are never replicated and have none.
- On the wire, a REF value is its network id (0 for none or a thing the sender has no id for), and a
  SYMBOL value is its name (u8 length + bytes). Everything else is little-endian raw: INT/FLOAT/BOOL
  4 bytes, VEC3 12, STRING u8 length + bytes. Collections are the count then each element (map: key
  then value) translated the same way; a grid is its cells.

### 9.2 What each machine sends (B3.2, B3.3)

- **A capture** is taken at every send tick (every 3rd tick, 20 Hz): for every replicated thing this
  machine holds, its network id, kind, parent network id, child name symbol, guest flag, root owner,
  spawner, and a copy of its shared block. Captures go in a ring of 32 keyed by tick.
- **To each peer**, a state packet holds a delta of the latest capture against the capture at the
  tick that peer last acknowledged (none: a full state). Per thing: absent in the baseline -> a
  *spawn* entry (header and every field); present in both -> an *update* entry listing only fields
  whose bytes differ (a field index count, then index and value per field) and the header when parent
  or owner changed; present only in the baseline -> a *remove* entry; still here but no longer sent
  to `p` (it went to `p`'s ownership, or the sender let go of it) -> a *left* entry, until `p` has
  acknowledged it. Which things go to peer `p`:
  - a client sends the things it owns;
  - the host sends every thing not owned by `p`, including other clients' things as it last had them
    (the host relays; the session is a star, as `core/net_session.c` is);
  - both also send a thing whose ownership moved to `p` until `p` has acknowledged a capture in which
    it did (the transfer's last write, B3.1).
- State packets go unreliable-sequenced on channel 1. Header: sender tick (u32), the latest sender
  tick received from this peer (the ack), the baseline tick used (0 = full).
- **Receiving** follows Quake 3 [N16]: the receiver keeps a ring of 32 reconstructed states per
  sender keyed by the sender's tick. A delta is applied to a copy of the named baseline to make the
  new state; a delta whose baseline is gone is dropped (the ack will fall back to a full state).
  States older than the newest applied are stored but not applied. A *left* entry keeps the thing in
  the rebuilt state marked as no longer the sender's word: nothing reads it (applying, taking what
  becomes ours, interpolating, the host's relay), and a thing that comes back is sent whole. So a
  sender's rebuilt states never hold, as its word, a thing it no longer sends: a client that dropped
  a thing and later is given it again is not read as handing it straight back. A *left* entry is not
  a removal: the thing stays; a later *remove* entry for it still removes it.
- **Applying a state** (B2.2 step 1, at the start of the next `StoreTick`): for each thing in it, in
  network id order, the receiver accepts it only if the thing is new, or its current owner in the
  receiver's store is **not** a local owner (a client), or **is** the sending peer (the host).
  Spawns create the thing raw (no declared children, no `start`); updates write the differing fields
  with `StoreSetEngine`; header changes reparent raw; removes remove it. After a new thing appears the
  runner is told (`onArrived`), so the Scheme layer can spawn its declared `:local` children.
- **Interpolation (B3.5), one tick per remote thing.** States are not applied on arrival. Each
  tick, per sender, `renderTick = newest sender tick received + ticks since it arrived - 6` (100 ms);
  every remote thing from that sender is written (with `StoreSetEngine`) as it stood at `renderTick`:
  fields registered with `StoreNetInterpolate(net, kind, field)` (any FLOAT or VEC3, not only
  transforms) are blended between the two received states either side of `renderTick` (equal values
  are copied, so a -0.0 at rest stays -0.0 and a still game hashes the same everywhere); every other
  field, the header (parent, owner) and spawns and removes come from the earlier of the two. A parent
  change between the two states is a jump (the later value). So a remote thing never shows two
  ticks at once. A thing whose new owner is this machine stops being remote on arrival: that state's
  header and fields apply at once and the thing is never interpolated again while it is ours
  (research N26 and N27 in the design repository: Quake 3 runs commands and events at the
  render-time snapshot; this is its rule, plus the arrival of one's own things). (Replaces a first cut that applied non-registered fields on arrival:
  `resolutions.md` §20 in the design repository.)
- **Releasing ownership.** A machine that stops owning a thing by its own write (a detach, or an
  attach under another owner's thing) at its tick D ignores the header of that thing in states
  whose sender has not yet acknowledged a capture of this machine at or after D, so a grab and a
  drop inside one round trip cannot re-attach it. Giving a thing away (the host attaching it under a
  client's soldier) is such a write, so the same rule keeps the receiver of a grant from reading the
  new owner's states made before it knew of the grant; the host's relay of a client's things obeys
  it too.
- **Large states.** A state packet over 1,200 bytes goes reliable on channel 0 (B3.2).

### 9.3 Messages, commands, effects (B2.4, B3.4)

- The store gains one hook, `StoreHooks.outgoing(user, target, event, args, count)`, called for a
  message or command whose target is owned by a machine that is not local, instead of dropping it.
- The net layer sends it reliable on channel 0 **in the same packet as a state delta for that peer**
  (it leaves at the end of the tick that queued it, with that tick's capture). The receiver queues
  the message with the sender tick it came with and delivers it with `StoreSend` (arguments
  translated, §9.1) in the first tick whose `renderTick` for that sender reaches it, after that
  tick's state is written: a message never overtakes its state and never arrives ahead of it.
- The host, receiving a message for a thing it does not own, forwards it to that thing's owner with
  its own delta. A client receiving a message for a thing it no longer owns sends it back to the
  host once (a flag in the packet); the host delivers it to the current owner, or drops it with a
  warning naming the thing and event after a second miss.
- **Effects** (`play-sound`, `burst` called from gameplay code) go to every other machine unreliable
  on channel 2 as `{name, preset/sound, position}`; the host relays clients' effects to the others.
  Presentation-phase effects stay local.

### 9.4 Joining, leaving, the kinds check (B3.7)

- Hello (client -> host, reliable): engine protocol, game name, `StoreKindsHash`, and each kind's
  name with a hash of its own declaration. The host refuses a different protocol or game, or a kinds
  mismatch, with a message listing the kinds that differ (for example `kinds differ: soldier (fields),
  medkit (missing here)`). Accepted: welcome `{player id, host tick}`; the first state to that peer is
  full. The client's world is emptied before the first state applies; a joining runner does not
  spawn `game` itself.
- On acceptance the host sends `player-joined p` to its `game` root thing (B3.7). On a peer's
  disconnect the host, between ticks: detaches every guest under things whose spawner is `p` at its
  world transform and sends each `orphaned`; removes every root whose spawner is `p` (declared
  children go with it); then sends `player-left p` to `game`. Clients learn all of it from states.
- The host leaving ends the session on every client with a message; host migration waits (A5).

### 9.5 API

```c
typedef struct StoreNetConfig {
    void *user;
    bool (*send)(void *user, int peer, int channel, bool reliable, const void *data, size_t size);
    void (*onArrived)(void *user, StoreId thing);            /* a thing appeared from the network */
    void (*onEffect)(void *user, const char *name, const char *what, Vector3 at);
    void (*onJoined)(void *user, int player);                 /* client: welcome received */
    void (*onEnded)(void *user, const char *why);             /* refused, dropped, host left */
    const char *game;
    bool dedicated;          /* host only: plays no one; local owners {0}; clients are players 2+ */
} StoreNetConfig;
bool StoreNetHost(StoreNet *, Store *, const StoreNetConfig *);
bool StoreNetJoin(StoreNet *, Store *, const StoreNetConfig *);   /* sends hello to peer 0 */
void StoreNetPeerConnected(StoreNet *, int peer);                 /* host: a transport peer came */
void StoreNetPeerLeft(StoreNet *, int peer);
void StoreNetReceive(StoreNet *, int peer, int channel, const void *data, size_t size);
void StoreNetBeforeTick(StoreNet *);   /* applies received states and messages; interpolates */
void StoreNetAfterTick(StoreNet *);    /* captures and sends every 3rd tick */
bool StoreNetEffect(StoreNet *, const char *name, const char *what, Vector3 at);
bool StoreNetInterpolate(StoreNet *, StoreKind kind, const char *field);
int  StoreNetPlayer(const StoreNet *);                             /* this machine's player id */
int  StoreNetPlayers(const StoreNet *, int *out, int max);
uint64_t StoreNetStateHash(const StoreNet *);   /* shared fields of replicated things, by network id,
                                                   REFs as network ids and SYMBOLs as names: equal on
                                                   every machine once the game has been still */
void StoreNetFree(StoreNet *);
```

And in the store (`core/store.h`), for native code (B7): `bool StoreOwnedHere(const Store *, StoreId)`
(its root owner is one of this machine's local owners) and `int StoreFieldOffset(const Store *,
StoreKind, int field)` (byte offset in the shared or local block, by the field's flags; -1 for none).

### 9.5b The shared transport (`core/store_net_enet.{h,c}`)

The ENet glue lives in core, so the runner and a C game use the same code:

```c
typedef void (*StoreNetLinkTap)(void *user, int peer, int channel, const void *data, size_t size);
typedef struct StoreNetLink { StoreNet net; uint64_t wireSent, wireReceived; /* ENet's totals */ ... } StoreNetLink;
bool StoreNetLinkHost(StoreNetLink *, Store *, const StoreNetConfig *, uint16_t port);
bool StoreNetLinkJoin(StoreNetLink *, Store *, const StoreNetConfig *, const char *address, uint16_t port);
bool StoreNetLinkInterpolate(StoreNetLink *, StoreKind, const char *field);
void StoreNetLinkPoll(StoreNetLink *, uint32_t timeoutMs);
void StoreNetLinkFlush(StoreNetLink *);
const char *StoreNetLinkEnded(const StoreNetLink *);   /* NULL while live or joining, else why */
void StoreNetLinkSetTap(StoreNetLink *, StoreNetLinkTap, void *user);   /* sees every received packet */
void StoreNetLinkClose(StoreNetLink *, double lingerSeconds);
```

The link supplies `config.send`, maps transport peers as §9.5 says, retries a join for 5 s, and
counts ENet's own wire totals (headers and acknowledgements included), which is what a byte gate
compares.

Transport peers are numbered by the caller: on a client the host is peer 0; on the host a client's
peer number is its player id.

### 9.6 The runner (`gameplay/game.c`)

`--host PORT` and `--join ADDRESS:PORT` (both work headless); `(host-game port)` and
`(join-game address port)` from Scheme do the same at run time. ENet through the shared link
(`core/store_net_enet.h`, §9.5b), three channels; recording and replay stay in the runner and read
the link's tap. `local-player` answers `StoreNetPlayer`, `players` the list. Gameplay-phase `play-sound`
and `burst` also call `StoreNetEffect`. Recording (B4) also records every received packet with its
arrival tick, channel and peer; replaying feeds them back at the same ticks instead of a socket, so
a client's or the host's session replays exactly. `--bench` prints bytes per second sent and
received on the wire (the link's ENet totals). The kinds check refusal is printed and the process exits nonzero.

How it is built:

- **Calls.** `host-game`/`join-game` are refused in a gameplay handler (`host-game is for
  presentation: ...`); from a `frame` handler or the REPL they start the session at once. game_s7.c
  answers them and `local-player`/`players` through `GameS7SetNetwork` (runner-provided hooks).
- **Setup.** Hosting: `StoreNetHost` after world3d's hooks and the game are in (so store_net chains
  in front), local owners {0, 1}; `game` is spawned and told `player-joined 1` as in phase 1. Joining:
  no `game`, no `player-joined`; ENet connects (the link retries for up to 5 s), `StoreNetJoin` says
  hello on connect, and until the welcome the runner ticks nothing but the network; the client's tick count
  (`--ticks`, `--bot-until`) starts at the welcome. store_net sets the local owners to {p} at the
  welcome (`onJoined`), which the runner repeats.
- **Transport.** On a client the host's ENet peer is peer 0. On the host a connecting ENet peer takes
  the lowest free player id from 2 and that is its store_net peer number (`StoreNetPeerConnected`);
  a disconnect is `StoreNetPeerLeft`. Per tick: poll ENet (each event to store_net) ->
  `StoreNetBeforeTick` -> `GameS7RestoreLocalChildren` once if anything arrived (their `:local`
  children) -> input -> `World3DBeginTick` -> `StoreTick` -> `StoreNetAfterTick` -> flush. Things
  store_net creates go through the store's `spawned` hook, so world3d caches their transforms.
  Headless, a live session keeps to 60 ticks a second (it sleeps; `--bench` leaves the sleep out).
- **Effects.** `onEffect` plays the sound (at its place; a sound given no `:at` travels with a NaN
  x) or bursts the preset in a window, and does nothing headless.
- **Ending.** A refusal, a join with no welcome in 5 s, or the host leaving prints `run: the
  session ended: <why>`; the run ends and exits 1. At exit every networked run prints `net state
  hash H` (`StoreNetStateHash`) after its `tick T hash H`. Ctrl+C disconnects cleanly. A headless
  host that reached its `--ticks` waits up to 5 s for its clients to leave before it closes, so
  that clients finishing a moment later are not told the host left.
- **Flags added for the checks.** `--bot-until N`: bot input only up to tick N, zero after.
  `--print-field KIND FIELD` prints `field KIND FIELD VALUE` for the first thing of KIND at exit;
  `--print-count KIND` prints `count KIND N`.
- **Replay format (version 2).** The header gains the role (`REPLAY_ROLE_NONE/HOST/CLIENT`, u32)
  and the player id (u32; a client's is written at the welcome); each tick record gains a packet
  count (u16) and, after its commands, that many packets: store_net peer (u32), channel (u8), size
  (u32), bytes. Channels 252-255 are transport events with no bytes: a session hosted or joined at
  that tick from `host-game`/`join-game`, a peer connected, a peer left. A client records from its
  welcome tick. The commands recorded are those queued before the tick's poll, so messages that
  arrived are not recorded twice. Replaying a networked recording opens no socket: the role comes
  from the header (`--host`/`--join` are ignored with a note), packets go to `StoreNetReceive` at
  their ticks and outgoing ones are dropped. Version 1 files still read (no role, no packets).

### 9.7 Checks

- `tests/regression/net_checks.c`: three stores in one process joined by an in-memory transport
  with a fixed 150 ms one-way delay and 5% loss on channel 1 (deterministic from a seed), running a
  small C kind set (a root kind with position, an int and a symbol; a child kind; a ref field).
  Assert: spawns, updates and removes reach both clients; `StoreNetStateHash` equal on all three
  after 60 still ticks; a client's message reaches a host-owned thing and a host message reaches a
  client-owned thing after its carried state; an attach by the host moves ownership to the client
  and a detach back; a kinds mismatch is refused with the kind named; a client removing its own
  holder, and the host removing one, each run the held thing's `orphaned` once, on the host, and on
  no client, with the thing the host's and unparented everywhere; a leaver's roots are removed and
  guests orphaned; bytes per state packet for 300 things with one field changing is under 1 KB.
  For the §9.2-§9.5 rules: a `look` message's handler reads the sender's position as written in the
  sending tick; a shell kind (`alive` BOOL, `age` FLOAT registered) whose owner clears `alive` in the
  tick `age` reaches 1 is never seen spent without `age >= 1` on a receiver; a thing the host
  attaches under a client's soldier is the client's in the tick the granting state arrives, and
  dropped within a tick it ends the host's with no parent everywhere; a thing granted to a client,
  dropped, and granted again three times ends each grant the client's under its soldier and each
  drop the host's with no parent; a join to 300 things with 50%
  of each 1,200-byte fragment on channel 1 lost converges within 120 ticks; a dedicated host is
  player 0 and its two clients converge with players {2, 3} everywhere. `store_checks.c` checks
  `StoreFieldOffset` against `StoreSharedBlock` and `StoreOwnedHere` against local owners and attach.
- The runner (`tests/regression/runner_checks.c`, built): three processes of
  `tests/regression/net_game` (a host and two clients, 1,500 ticks, bots until tick 600) print the
  same `net state hash`, no `ERROR`, and the host counts 3 hellos; the host's and a client's
  recordings replay to their live `tick 1500 hash`. Three processes of SWAT Tower with bots
  throughout for 1,200 ticks: no `ERROR`, 3 soldiers on every machine, the host's bytes per second
  per client printed (SWAT Tower is never still, so its state hashes are not compared). Also: things
  store_net creates reach world3d's spawned hook; `(host-game port)` from a frame handler hosts and
  its recording replays; a join nobody answers, a kinds refusal and a host leaving exit 1; Ctrl+C on
  a client leaves cleanly (`regression_test --net-interrupt` runs only this one: it waits for the
  host's `net: hosting`, the client's `net: joined` and the host's `net: player 2 left` lines rather
  than for times, and sends the client one SIGINT through `timeout --foreground`, since plain
  `timeout` also signals the child's process group and a client that runs between the two sees a
  second Ctrl+C and is killed). Three processes of `tests/regression/net_carry` (the carried-item
  example's carryable and a test-only carrier per player that grabs the nearest free item and drops
  it 3 m away after 2 s; 2,400 ticks, the last 120 still) agree on every item's holder and every
  player's counts, answer every grab, reach at least 5 grants and the same net state hash; a second
  session whose carriers keep what they get, with one client run for 1,200 ticks, shows the host
  running that client's item's `orphaned` once and the item on the floor when `player-left` runs. The checks are skipped with a note when ENet cannot bind a loopback
  port.
