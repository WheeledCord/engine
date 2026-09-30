/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_STORE_H
#define CORE_STORE_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The world store: every thing a game has, the kind each one is, its fields, where it hangs in the
   tree and who owns it, and the tick that runs their handlers. Fields live in per-kind pools of
   fixed-size blocks, so a snapshot is a memory copy, a change is found by comparison and a save is
   one walk over the table. docs/developer/store.md is the contract; this header states each call.

   Things are named by generational handles as EngineObjects are: a handle kept past its thing's
   removal is refused, and never reaches a newer thing in the same slot. The store opens no window,
   touches no GL and runs headless. It keeps no process-wide state: everything is in the caller's
   Store. */

typedef struct StoreId
{
    uint32_t index, generation;
} StoreId; /* index UINT32_MAX: none */

#define STORE_NULL ((StoreId){UINT32_MAX, 0})
typedef int32_t StoreSymbol; /* interned name; STORE_NO_SYMBOL is -1 */
#define STORE_NO_SYMBOL ((StoreSymbol)-1)

typedef enum StoreType
{
    STORE_NONE,
    STORE_INT,
    STORE_FLOAT,
    STORE_BOOL,
    STORE_SYMBOL,
    STORE_STRING,
    STORE_VEC3,
    STORE_REF,
    STORE_LIST,
    STORE_SET,
    STORE_MAP,
    STORE_GRID
} StoreType;

#define STORE_STRING_MAX 63
#define STORE_MAX_ARGS 8 /* arguments a message or timer carries at most */

typedef struct StoreValue
{
    StoreType type;
    union
    {
        int32_t i;
        float f;
        bool b;
        StoreSymbol sym;
        char str[STORE_STRING_MAX + 1];
        Vector3 v;
        StoreId ref;
    } as;
} StoreValue;

#define STORE_LOCAL 1u  /* presentation only: not shared, saved, hashed or replayed (B1) */
#define STORE_ENGINE 2u /* written by the engine, read-only from scripts (on-floor) */
#define STORE_HIDDEN 4u /* not listed by inspect; used for %prev-* fields */

typedef struct StoreFieldDecl
{
    const char *name;
    StoreType type;    /* the field's type */
    StoreType element; /* LIST/SET/GRID element type or MAP value type; scalar types only,
                          never STRING, LIST, SET, MAP, GRID */
    StoreType key;     /* MAP key type: INT, SYMBOL or REF */
    int max;           /* capacity of LIST/SET/MAP, width of GRID; 0 for scalars */
    int height;        /* GRID only */
    unsigned flags;
    StoreValue init;   /* scalar default; STORE_NONE means the type's zero */
} StoreFieldDecl;

typedef int StoreKind; /* index; -1 is none */

typedef enum StorePhase
{
    STORE_PHASE_NONE,
    STORE_PHASE_GAMEPLAY,
    STORE_PHASE_PRESENTATION
} StorePhase;

typedef struct Store Store;
typedef bool (*StoreHandlerFn)(Store *store, StoreId self, StoreSymbol event,
                               const StoreValue *args, int count, void *user);
typedef void (*StoreSystemFn)(Store *store, float dt, void *user);

typedef struct StoreHooks
{
    void *user;
    void (*error)(void *user, const char *message); /* a rule or type error (§2.7) */
    void (*orphan)(void *user, StoreId guest);      /* just before a guest is detached */
    void (*spawned)(void *user, StoreId thing);     /* after a thing's blocks exist */
    void (*removed)(void *user, StoreId thing);     /* before its blocks are freed */
    /* A message or command StoreTick would deliver to a live thing whose owner is not local: the
       network layer sends it to that owner's machine (docs/developer/store.md §9.3). NULL drops it.
       Called between handlers (phase NONE); args are only valid during the call. */
    void (*outgoing)(void *user, StoreId target, StoreSymbol event, const StoreValue *args,
                     int count);
} StoreHooks;

/* Private to store.c; declared here only so a Store can be embedded by value. */
typedef struct StoreKindData StoreKindData;
typedef struct StoreThing StoreThing;
typedef struct StoreMessage StoreMessage;
typedef struct StoreTimer StoreTimer;
typedef struct StoreSystem StoreSystem;
typedef struct StoreSnapshot StoreSnapshot;

/* Caller-owned; prepare with StoreInit and release with StoreFree. Treat the members as private. */
struct Store
{
    StoreKindData **kinds;
    int kindCount, kindCapacity;
    StoreThing *things;
    uint32_t thingCount, thingCapacity, firstFree, pendingRemovals;
    char **symbols;
    int32_t symbolCount, symbolCapacity;
    int32_t *symbolTable; /* open addressing: symbol + 1, 0 empty */
    uint32_t symbolTableSize;
    StoreMessage *queue;
    int queueHead, queueCount, queueCapacity;
    StoreMessage *commands;
    int commandCount, commandCapacity;
    StoreTimer *timers;
    int timerCount, timerCapacity;
    uint64_t timerSequence;
    StoreId *starts;
    int startCount, startCapacity;
    StoreSystem *systems;
    int systemCount, systemCapacity;
    StoreHooks hooks;
    uint64_t worldRandom, localRandom, tickCount, localOwners;
    float dt;
    StorePhase phase;
    StoreId current;
    int currentOwner;
    bool ticking, framing, queueOpen;
    int deliveries, dropped;
    char *error;
};

/* ---- symbols -------------------------------------------------------------------------------- */

/** @brief Interns a name, answering the same symbol for the same name for the store's lifetime.
 *
 * Symbols are numbered in first-seen order and never freed. The store interns `start`, `tick`,
 * `frame` and `draw-hud` first, so those four have the same ids in every store.
 * @param store Initialised store.
 * @param name NUL-terminated name, copied; NULL or empty answers STORE_NO_SYMBOL.
 * @return The symbol, or STORE_NO_SYMBOL when out of memory. */
StoreSymbol StoreIntern(Store *store, const char *name);

/** @brief Names a symbol.
 * @param store Store that interned it.
 * @param symbol Symbol.
 * @return The store's own copy of the name, valid until StoreFree, or NULL for an unknown symbol. */
const char *StoreSymbolName(const Store *store, StoreSymbol symbol);

/* ---- kinds and fields ----------------------------------------------------------------------- */

/** @brief Declares a kind: its base's fields first, then its own, each in declaration order.
 *
 * A field named like a base field redeclares it only to change its default; its type, element,
 * key, capacity, height and flags must match. Collection fields take their defaults from
 * StoreKindSetDefaultAt, and their init must be STORE_NONE.
 * @param store Store.
 * @param name Kind name, copied; must not name an existing kind.
 * @param base Kind to extend, or -1 for none.
 * @param fields Field declarations, copied (names included); may be NULL when count is 0.
 * @param count Number of declarations.
 * @param error Receives why on failure, borrowed until the next failing call; may be NULL.
 * @return The new kind, or -1 on failure. */
StoreKind StoreDeclareKind(Store *store, const char *name, StoreKind base,
                           const StoreFieldDecl *fields, int count, const char **error);

/** @brief Finds a kind by name.
 * @param store Store.
 * @param name Kind name.
 * @return The kind, or -1. */
StoreKind StoreKindNamed(const Store *store, const char *name);

/** @brief Names a kind.
 * @param store Store.
 * @param kind Kind.
 * @return The store's copy of the name, or NULL for an unknown kind. */
const char *StoreKindName(const Store *store, StoreKind kind);

/** @brief Returns the kind a kind extends.
 * @param store Store.
 * @param kind Kind.
 * @return The base kind, or -1 for a kind with none or an unknown kind. */
StoreKind StoreKindBase(const Store *store, StoreKind kind);

/** @brief Says whether a kind is another or extends it.
 * @param store Store.
 * @param kind Kind to test.
 * @param base Kind it may be or extend.
 * @return True when kind is base or derives from it; false for unknown kinds. */
bool StoreKindIs(const Store *store, StoreKind kind, StoreKind base);

/** @brief Finds a field of a kind, inherited fields included.
 * @param store Store.
 * @param kind Kind.
 * @param name Field name.
 * @return The field index, the same in every kind derived from the one declaring it, or -1. */
int StoreFieldIndex(const Store *store, StoreKind kind, const char *name);

/** @brief Returns where a field sits in its thing's block, so a native mirror struct can be checked
 * against the layout and copied whole (B7).
 * @param store Store.
 * @param kind Kind.
 * @param field Field index, as StoreFieldIndex returns.
 * @return Byte offset in the shared block (StoreSharedBlock), or in the local block
 * (StoreLocalBlock) for a :local field; -1 for an unknown kind or field. */
int StoreFieldOffset(const Store *store, StoreKind kind, int field);

/** @brief Counts a kind's fields, inherited fields included.
 * @param store Store.
 * @param kind Kind.
 * @return The number of fields, or 0 for an unknown kind. */
int StoreFieldCount(const Store *store, StoreKind kind);

/** @brief Describes one field of a kind.
 * @param store Store.
 * @param kind Kind.
 * @param field Field index.
 * @return The store's copy of the declaration, valid until StoreFree (init holds the kind's
 * current default), or NULL when out of range. */
const StoreFieldDecl *StoreFieldAt(const Store *store, StoreKind kind, int field);

/** @brief Overrides the default of a scalar field for things of this kind spawned from now on.
 * @param store Store.
 * @param kind Kind whose default changes; kinds already derived from it keep theirs.
 * @param field Scalar field index.
 * @param value New default, converted INT<->FLOAT; other mismatches are refused.
 * @return True when set; false with StoreLastError set otherwise. */
bool StoreKindSetDefault(Store *store, StoreKind kind, int field, const StoreValue *value);

/** @brief Sets part of a collection field's default (the :init of a list, set, map or grid).
 *
 * LIST: value becomes element `index`, where index may equal the count to append. SET: value is
 * added in sorted order and index is ignored. MAP: key maps to value and index is ignored. GRID:
 * value fills cell `index` in row-major order (y * width + x).
 * @param store Store.
 * @param kind Kind whose default changes.
 * @param field Collection field index.
 * @param index Position, as above.
 * @param key MAP key; ignored otherwise and may be NULL.
 * @param value Element or value.
 * @return True when set; false on a type, capacity or range error, with StoreLastError set. */
bool StoreKindSetDefaultAt(Store *store, StoreKind kind, int field, int index,
                           const StoreValue *key, const StoreValue *value);

/* ---- things and the tree -------------------------------------------------------------------- */

/** @brief Prepares an empty store.
 *
 * Zeroes the struct, then seeds the world random stream with seed and the presentation stream
 * with seed XOR a constant. Every owner is local until StoreSetLocalOwners says otherwise.
 * @param store Caller-owned store; its previous contents are not released.
 * @param seed World seed.
 * @return True on success; false when out of memory, with the store safe to StoreFree. */
bool StoreInit(Store *store, uint64_t seed);

/** @brief Releases everything the store owns and zeroes it; no hook is called.
 * @param store Store; NULL is accepted.
 * @return No value. Snapshots taken from it stay valid and must be freed separately. */
void StoreFree(Store *store);

/** @brief Spawns a thing with every field at its kind's default.
 *
 * A root (parent STORE_NULL) is owned by owner and its spawner is that owner; a child is a
 * declared child of parent, placed after parent's other declared children and before its guests,
 * with its root's owner and its parent's spawner. Its random stream is seeded from the running
 * gameplay handler's thing, or from the world stream outside handlers (the presentation stream in
 * presentation handlers). Its `start` message always arrives before its first `tick`: spawned
 * during StoreTick (a tick or message handler, a system), start is queued at the tail of the
 * message queue and delivered later in that tick; spawned anywhere else (setup, the REPL, a frame,
 * a removal hook), start is kept pending and delivered first thing in the next StoreTick.
 * Pointers from StoreSharedBlock and StoreLocalBlock may move.
 * @param store Store.
 * @param kind Kind to spawn.
 * @param owner Owner of a root, 0 (the host) to 63; ignored for a child.
 * @param parent Parent, or STORE_NULL for a root.
 * @param childName Name the parent's kind gives this child, or STORE_NO_SYMBOL.
 * @return The new thing, or STORE_NULL with StoreLastError set. */
StoreId StoreSpawn(Store *store, StoreKind kind, int owner, StoreId parent, StoreSymbol childName);

/** @brief Removes a thing and, recursively, the children it declared.
 *
 * They leave every query at once and messages queued for them are dropped. Inside StoreTick or
 * StoreFrame their storage is freed at the end of that call; otherwise at once. Guests are not
 * removed: at that point each gets the orphan hook and is detached to a root owned by the host.
 * In a gameplay handler only the thing's owner may remove it (rule 5).
 * @param store Store.
 * @param id Thing.
 * @return True when removed; false for a stale id or a refused removal, with StoreLastError set. */
bool StoreRemove(Store *store, StoreId id);

/** @brief Makes a thing local (proposal B1's :local children): presentation only on this machine.
 *
 * Every field of a local thing counts as STORE_LOCAL for the rules (a presentation handler may
 * write it, a gameplay handler may not read it). Its declared children are local too, those
 * already there and those spawned under it later. StoreHash and StoreSave leave local things and
 * their timers out, so a load never sees them (the language recreates them from its declarations);
 * snapshots keep them as they are, and StoreThings still lists them. A local thing's random stream
 * was still seeded from its spawner's, as every spawn's is.
 * @param store Store.
 * @param id Thing whose `start` is still pending: marking is refused once it has started.
 * @return True when marked; false for a stale or started thing, with StoreLastError set. */
bool StoreMarkLocal(Store *store, StoreId id);

/** @brief Says whether a thing is local (StoreMarkLocal, or a declared child of a local thing).
 * @param store Store.
 * @param id Thing.
 * @return True for a live local thing. */
bool StoreIsLocal(const Store *store, StoreId id);

/** @brief Says whether a handle names a thing that exists and has not been removed.
 * @param store Store.
 * @param id Handle.
 * @return True for a live thing. */
bool StoreAlive(const Store *store, StoreId id);

/** @brief Returns a thing's kind.
 * @param store Store.
 * @param id Thing.
 * @return The kind, or -1 for a stale id. */
StoreKind StoreKindOf(const Store *store, StoreId id);

/** @brief Returns a thing's owner: the owner of the root of its tree, cached (B3.1).
 * @param store Store.
 * @param id Thing.
 * @return The owner, or -1 for a stale id. */
int StoreOwner(const Store *store, StoreId id);

/** @brief Says whether a thing is run on this machine, for native systems outside handlers (B7).
 * @param store Store.
 * @param id Thing.
 * @return True when its root owner is one of this machine's local owners (StoreSetLocalOwners);
 * false for a stale id. */
bool StoreOwnedHere(const Store *store, StoreId id);

/** @brief Returns the player whose leaving removes a thing (B3.7).
 * @param store Store.
 * @param id Thing.
 * @return The owner a root had at spawn, or a declared child's parent's spawner; -1 for a stale id. */
int StoreSpawner(const Store *store, StoreId id);

/** @brief Returns a thing's parent.
 * @param store Store.
 * @param id Thing.
 * @return The parent, or STORE_NULL for a root or a stale id. */
StoreId StoreParent(const Store *store, StoreId id);

/** @brief Returns a thing's first child: declared children in declaration order, then guests in
 * attach order.
 * @param store Store.
 * @param id Thing.
 * @return The first live child, or STORE_NULL. */
StoreId StoreFirstChild(const Store *store, StoreId id);

/** @brief Returns the next child of the same parent, in the order StoreFirstChild starts.
 * @param store Store.
 * @param id Child.
 * @return The next live sibling, or STORE_NULL. */
StoreId StoreNextSibling(const Store *store, StoreId id);

/** @brief Finds a declared child by the name its parent's kind gave it.
 * @param store Store.
 * @param id Parent.
 * @param name Child name.
 * @return The first live declared child of that name, or STORE_NULL. */
StoreId StoreChildNamed(const Store *store, StoreId id, StoreSymbol name);

/** @brief Returns the name a declared child was spawned with.
 * @param store Store.
 * @param id Thing.
 * @return The name, or STORE_NO_SYMBOL for a root, a guest, an unnamed child or a stale id. */
StoreSymbol StoreChildName(const Store *store, StoreId id);

/** @brief Says whether a thing hangs under its parent as a guest (attached at run time).
 * @param store Store.
 * @param id Thing.
 * @return True for a live guest. */
bool StoreIsGuest(const Store *store, StoreId id);

/** @brief Attaches a thing under another as a guest, last among its children.
 *
 * The thing and everything under it take the owner of parent's root. Refused for stale ids, for a
 * parent that is the thing or under it, in a presentation handler (rule 1), and in a gameplay
 * handler that does not run for the thing's owner (rule 5).
 * @param store Store.
 * @param thing Thing to move; it leaves its current parent.
 * @param parent New parent.
 * @return True when attached; false with StoreLastError set. */
bool StoreAttach(Store *store, StoreId thing, StoreId parent);

/** @brief Takes a thing out from under its parent, making it a root owned by the host (0).
 *
 * The same rules as StoreAttach apply; a thing that is already a root is refused.
 * @param store Store.
 * @param thing Thing to detach.
 * @return True when detached; false with StoreLastError set. */
bool StoreDetach(Store *store, StoreId thing);

/** @brief Lists live things of a kind or any kind derived from it, in id order.
 * @param store Store.
 * @param kindOrDerived Kind, or -1 for every kind.
 * @param out Receives up to max ids; may be NULL to count.
 * @param max Capacity of out.
 * @return The number written, or with out NULL the number matching. */
int StoreThings(const Store *store, StoreKind kindOrDerived, StoreId *out, int max);

/** @brief Counts live things.
 * @param store Store.
 * @return The number of live things. */
uint32_t StoreCount(const Store *store);

/* ---- fields --------------------------------------------------------------------------------- */

/** @brief Reads a scalar field (every type but LIST, SET, MAP and GRID).
 *
 * Refused, with StoreLastError set, for a stale id, a bad field, a collection field, and in a
 * gameplay handler for a STORE_LOCAL field (`local-read`, rule 1). A REF naming a removed thing is
 * answered as stored; StoreAlive tells.
 * @param store Store.
 * @param id Thing.
 * @param field Field index.
 * @param out Receives the value, fully initialised.
 * @return True when read. */
bool StoreGet(const Store *store, StoreId id, int field, StoreValue *out);

/** @brief Writes a scalar field, subject to the rules of docs/developer/store.md §2.7.
 *
 * INT converts to FLOAT, and FLOAT to INT when it is a whole number in range; every other
 * mismatch is a `type` error. Errors also go to the error hook.
 * @param store Store.
 * @param id Thing.
 * @param field Field index.
 * @param value Value to write.
 * @return True when written; false with StoreLastError set. */
bool StoreSet(Store *store, StoreId id, int field, const StoreValue *value);

/** @brief Writes a field through the engine's own path, skipping the rules (type still checked).
 * @param store Store.
 * @param id Thing.
 * @param field Scalar field index.
 * @param value Value to write.
 * @return True when written; false for a stale id, a bad field or a type error. */
bool StoreSetEngine(Store *store, StoreId id, int field, const StoreValue *value);

/** @brief Counts the elements of a collection field.
 * @param store Store.
 * @param id Thing.
 * @param field LIST, SET, MAP or GRID field (a grid counts width * height cells).
 * @return The count, or -1 with StoreLastError set. */
int StoreCountOf(const Store *store, StoreId id, int field);

/** @brief Reads one element of a collection field.
 * @param store Store.
 * @param id Thing.
 * @param field LIST, SET, MAP or GRID field.
 * @param index Element position (sorted order for SET and MAP; row-major cell for GRID).
 * @param key Receives a MAP key, or STORE_NONE otherwise; may be NULL.
 * @param value Receives the element or MAP value; may be NULL.
 * @return True when read; false out of range or refused, with StoreLastError set. */
bool StoreGetAt(const Store *store, StoreId id, int field, int index, StoreValue *key,
                StoreValue *value);

/** @brief Replaces the contents of a LIST or SET field.
 *
 * A SET drops duplicates and keeps its elements sorted: ints, floats and bools by value, symbols
 * by name, refs by index then generation, vectors by x, y, z. More elements than the capacity is a
 * `capacity` error and leaves the field unchanged.
 * @param store Store.
 * @param id Thing.
 * @param field LIST or SET field.
 * @param items Elements, converted to the element type; may be NULL when count is 0.
 * @param count Number of elements.
 * @return True when written; false with StoreLastError set. */
bool StoreSetList(Store *store, StoreId id, int field, const StoreValue *items, int count);

/** @brief Replaces a LIST or SET field through the engine's own path, skipping the rules (type and
 * capacity still checked), as StoreSetEngine does for scalars: a spawn's settings on a thing it
 * gives to another owner.
 * @param store Store.
 * @param id Thing.
 * @param field LIST or SET field index.
 * @param items The new elements.
 * @param count How many.
 * @return True when written; false with StoreLastError set. */
bool StoreSetListEngine(Store *store, StoreId id, int field, const StoreValue *items, int count);

/** @brief Reads a MAP field's value for a key.
 * @param store Store.
 * @param id Thing.
 * @param field MAP field.
 * @param key Key, converted to the key type.
 * @param out Receives the value.
 * @return True when the key is present; false otherwise, with StoreLastError set. */
bool StoreMapGet(const Store *store, StoreId id, int field, const StoreValue *key,
                 StoreValue *out);

/** @brief Sets a MAP field's value for a key, inserting the key in sorted order when new.
 * @param store Store.
 * @param id Thing.
 * @param field MAP field.
 * @param key Key.
 * @param value Value.
 * @return True when written; false on a full map (`capacity`) or a refusal, with StoreLastError. */
bool StoreMapSet(Store *store, StoreId id, int field, const StoreValue *key,
                 const StoreValue *value);

/** @brief Removes a key from a MAP field.
 * @param store Store.
 * @param id Thing.
 * @param field MAP field.
 * @param key Key.
 * @return True when the key was present and removed. */
bool StoreMapRemove(Store *store, StoreId id, int field, const StoreValue *key);

/** @brief Reads a GRID cell.
 * @param store Store.
 * @param id Thing.
 * @param field GRID field.
 * @param x Column, 0 to max - 1.
 * @param y Row, 0 to height - 1.
 * @param out Receives the cell.
 * @return True when read; false out of range or refused, with StoreLastError set. */
bool StoreGridGet(const Store *store, StoreId id, int field, int x, int y, StoreValue *out);

/** @brief Writes a GRID cell.
 * @param store Store.
 * @param id Thing.
 * @param field GRID field.
 * @param x Column.
 * @param y Row.
 * @param value Value.
 * @return True when written; false out of range or refused, with StoreLastError set. */
bool StoreGridSet(Store *store, StoreId id, int field, int x, int y, const StoreValue *value);

/** @brief Fills a rectangle of GRID cells with one value.
 * @param store Store.
 * @param id Thing.
 * @param field GRID field.
 * @param x Left column.
 * @param y Top row.
 * @param w Width in cells; the rectangle must lie inside the grid.
 * @param h Height in cells.
 * @param value Value.
 * @return True when written; false out of range or refused, with StoreLastError set. */
bool StoreGridFill(Store *store, StoreId id, int field, int x, int y, int w, int h,
                   const StoreValue *value);

/** @brief Gives a native system raw access to a thing's shared block (B7).
 *
 * Fields sit at the kind's layout offsets (docs/developer/store.md §2.2). No rule is checked.
 * @param store Store.
 * @param id Thing.
 * @return The block, valid until the next spawn, restore or load, or NULL for a stale id or a kind
 * with no shared fields. */
void *StoreSharedBlock(Store *store, StoreId id);

/** @brief Gives raw access to a thing's local (presentation) block.
 * @param store Store.
 * @param id Thing.
 * @return The block, valid until the next spawn, restore or load, or NULL for a stale id or a kind
 * with no local fields. */
void *StoreLocalBlock(Store *store, StoreId id);

/** @brief Says why the last refused call answered false.
 * @param store Store.
 * @return A sentence starting with the rule prefix (`local-read`, `shared-write`, `not-owner`,
 * `engine-field`, `type`, `capacity`, ...), owned by the store and overwritten by the next error;
 * empty when nothing has failed. */
const char *StoreLastError(const Store *store);

/* ---- the tick and its handlers -------------------------------------------------------------- */

/** @brief Sets the function that runs a kind's handlers; derived kinds without one use their
 * base's.
 * @param store Store.
 * @param kind Kind.
 * @param handler Dispatcher, called only for events the kind handles; NULL removes it.
 * @param user Passed back to handler.
 * @return No value. */
void StoreKindSetHandler(Store *store, StoreKind kind, StoreHandlerFn handler, void *user);

/** @brief Declares whether a kind handles an event. An event named `<field>-changed` for one of
 * the kind's shared fields makes StoreFrame compare that field.
 * @param store Store.
 * @param kind Kind.
 * @param event Event symbol.
 * @param handles True to handle it, false to stop.
 * @return No value. */
void StoreKindHandles(Store *store, StoreKind kind, StoreSymbol event, bool handles);

/** @brief Says whether a kind or any kind it extends handles an event.
 * @param store Store.
 * @param kind Kind.
 * @param event Event symbol.
 * @return True when handled. */
bool StoreKindHandlesEvent(const Store *store, StoreKind kind, StoreSymbol event);

/** @brief Adds a system run at step 6 of every tick, after the ones already added.
 * @param store Store.
 * @param system Function; it runs with phase NONE and may queue events with StoreSend.
 * @param user Passed back to system.
 * @return True when added; false for NULL or out of memory. */
bool StoreAddSystem(Store *store, StoreSystemFn system, void *user);

/** @brief Runs one tick: pending starts, commands, due timers, `tick` handlers, messages, systems,
 * removals.
 *
 * First, the `start` of every thing spawned outside a tick since the last one is delivered, in spawn
 * order (with whatever those handlers send), before commands and timers are queued.
 * Messages are delivered first in, first out, at most 10,000 per tick (the rest are dropped with
 * a warning); a message to a removed thing is dropped silently, and one to a thing no local owner
 * runs goes to the outgoing hook (dropped silently when there is none); a message the target's
 * kind does not handle is reported once per kind and event. A thing spawned
 * during the tick gets `start` at the tail of the queue, so it is delivered in this tick's step 5
 * or 6, before the thing's first `tick` next tick.
 * @param store Store; must not already be inside StoreTick or StoreFrame.
 * @param dt Seconds per tick; also the unit StoreAfter converts seconds with.
 * @return No value. */
void StoreTick(Store *store, float dt);

/** @brief Runs the presentation handlers once: `frame` {dt} for every thing, then
 * `<field>-changed` {was, now} for every watched shared field that differs from the last frame
 * (was is STORE_NONE for a thing that appeared since), then `draw-hud`, then refreshes the shadow.
 *
 * For a collection field, was and now carry only the collection's type; read it from the thing.
 * @param store Store; must not be inside StoreTick or StoreFrame.
 * @param dt Seconds since the last frame.
 * @return No value. */
void StoreFrame(Store *store, float dt);

/** @brief Counts completed ticks.
 * @param store Store.
 * @return Ticks completed; during a tick, that tick's zero-based number. */
uint64_t StoreTickCount(const Store *store);

/** @brief Returns seconds since the world started: ticks completed times the last tick's dt.
 * @param store Store.
 * @return Seconds; 1/60 per tick is assumed before the first StoreTick. */
float StoreTickTime(const Store *store);

/** @brief Says which kind of handler is running.
 * @param store Store.
 * @return STORE_PHASE_GAMEPLAY, STORE_PHASE_PRESENTATION, or STORE_PHASE_NONE outside handlers. */
StorePhase StorePhaseNow(const Store *store);

/** @brief Returns the thing whose handler is running.
 * @param store Store.
 * @return The thing, or STORE_NULL outside handlers. */
StoreId StoreCurrent(const Store *store);

/** @brief Returns the owner the running handler runs for: its thing's owner when it started.
 * @param store Store.
 * @return The owner, or -1 outside handlers. */
int StoreCurrentOwner(const Store *store);

/** @brief Queues a gameplay message, delivered later in the same tick (step 5 or 6).
 *
 * Outside StoreTick it is queued as a command for the next tick (step 2). Refused in a
 * presentation handler (`shared-write`: use StoreCommand), for a stale target, and for more than
 * STORE_MAX_ARGS arguments or an argument that is a collection type.
 * @param store Store.
 * @param target Thing to deliver to.
 * @param event Event symbol.
 * @param args Arguments, copied; may be NULL when count is 0.
 * @param count Number of arguments.
 * @return True when queued; false with StoreLastError set. */
bool StoreSend(Store *store, StoreId target, StoreSymbol event, const StoreValue *args,
               int count);

/** @brief Sets a timer: after the given game time, event is sent to self.
 *
 * Seconds become whole ticks with the last tick's dt (rounded, at least 1). Timers are part of the
 * world: snapshotted, saved and hashed. Refused in a presentation handler (rule 1) and in a gameplay
 * handler that does not run for self's owner (rule 5).
 * @param store Store.
 * @param self Thing the timer belongs to and is sent to.
 * @param seconds Game seconds from now.
 * @param event Event symbol.
 * @param args Arguments, copied; may be NULL when count is 0.
 * @param count Number of arguments, at most STORE_MAX_ARGS.
 * @return True when set; false with StoreLastError set. */
bool StoreAfter(Store *store, StoreId self, float seconds, StoreSymbol event,
                const StoreValue *args, int count);

/** @brief Queues a player command for step 2 of the next tick, in the order given.
 * @param store Store.
 * @param target Thing to deliver to.
 * @param event Event symbol.
 * @param args Arguments, copied; may be NULL when count is 0.
 * @param count Number of arguments, at most STORE_MAX_ARGS.
 * @return True when queued; false with StoreLastError set. */
bool StoreCommand(Store *store, StoreId target, StoreSymbol event, const StoreValue *args,
                  int count);

/** @brief Copies the player commands queued since the last tick, in queue order, for a recording.
 *
 * These are the commands StoreCommand queued (and StoreSend outside a tick) that the next
 * StoreTick will deliver at its step 2. The queue is left as it is.
 * @param store Store.
 * @param targets Receives each command's target; may be NULL.
 * @param events Receives each command's event; may be NULL.
 * @param args Receives each command's arguments, STORE_MAX_ARGS per command; may be NULL.
 * @param counts Receives each command's argument count; may be NULL.
 * @param max Room in each non-NULL array; at most that many commands are copied.
 * @return The number of commands queued, which may exceed max; 0 for a NULL store. */
int StoreCommandsPending(const Store *store, StoreId *targets, StoreSymbol *events,
                         StoreValue (*args)[STORE_MAX_ARGS], int *counts, int max);

/** @brief Says which owners run on this machine: their `tick` handlers and messages run here.
 * @param store Store.
 * @param owners Owners 0 to 63; others are ignored. May be NULL when count is 0 (none local).
 * @param count Number of owners.
 * @return No value. Until called, every owner is local. */
void StoreSetLocalOwners(Store *store, const int *owners, int count);

/** @brief Installs the runner's and world modules' hooks.
 * @param store Store.
 * @param hooks Hooks, copied; NULL clears them. Any member may be NULL.
 * @return No value. */
void StoreSetHooks(Store *store, const StoreHooks *hooks);

/* ---- randomness, snapshot, hash ------------------------------------------------------------- */

/** @brief Draws from a thing's own random stream (splitmix64), which only it advances.
 *
 * Changing the stream is a shared write: refused in a presentation handler, and in a gameplay
 * handler not running for the thing's owner, answering 0 with StoreLastError set.
 * @param store Store.
 * @param thing Thing whose stream to draw from; STORE_NULL draws from the world stream.
 * @param n Range.
 * @return A uniform value in [0, n); 0 when n is 0, for a stale thing or a refusal. */
uint32_t StoreRandom(Store *store, StoreId thing, uint32_t n);

/** @brief Draws from the presentation stream, which never touches world state.
 * @param store Store.
 * @param n Range.
 * @return A uniform value in [0, n); 0 when n is 0. */
uint32_t StoreRandomLocal(Store *store, uint32_t n);

/** @brief Copies the world: the thing table, every kind's shared pool and free list, the timers,
 * the tick count, the world random stream and the pending `start`s. Local blocks are not copied.
 * @param store Store, between ticks.
 * @return A snapshot owned by the caller (release with StoreSnapshotFree), or NULL when called
 * inside StoreTick or StoreFrame or out of memory. */
StoreSnapshot *StoreSnapshotTake(const Store *store);

/** @brief Puts the world back as a snapshot found it.
 *
 * Things that are the same (index, generation and kind) before and after keep their local
 * blocks; every other restored thing's local block is reset to its kind's defaults. Pending
 * commands are left alone. No hook is called. Restore into the store the snapshot came from, or
 * one with the same kinds and symbols declared in the same order.
 * @param store Store, between ticks.
 * @param snapshot Snapshot; still owned by the caller.
 * @return True when restored; false when the kinds do not match, inside a tick or frame, or out
 * of memory, leaving the store unchanged. */
bool StoreSnapshotRestore(Store *store, const StoreSnapshot *snapshot);

/** @brief Releases a snapshot.
 * @param snapshot Snapshot; NULL is accepted.
 * @return No value. */
void StoreSnapshotFree(StoreSnapshot *snapshot);

/** @brief Hashes the world with 64-bit FNV-1a.
 *
 * Over, for each live thing in id order: its index and generation, kind name, parent index, child
 * name, owner, spawner, random state and shared block (symbols by name); then the timers in order
 * and the tick count. Local blocks are never hashed. Equal worlds hash equal across processes.
 * @param store Store.
 * @return The hash. */
uint64_t StoreHash(const Store *store);

/** @brief Hashes the kinds a game declared, so a replay can refuse a file recorded against others.
 *
 * 64-bit FNV-1a over every kind in declaration order: its name, its base's name, and for each field
 * it declares itself (not inherited ones, and not a redeclared base field) the name, type, element,
 * key, max, height and flags. Defaults, handlers and handled events are not included.
 * @param store Store.
 * @return The hash; the same declarations give the same hash in every process. */
uint64_t StoreKindsHash(const Store *store);

/* ---- save and load (core/store_save.c) ------------------------------------------------------ */

/** @brief Writes the world as text, atomically (core/file.h): every thing with its shared fields,
 * the timers, the tick count and the world random state. Local fields are not saved.
 * @param store Store, between ticks.
 * @param path Destination; the previous file survives a failed save.
 * @return True when the file was replaced; false with StoreLastError set. */
bool StoreSave(const Store *store, const char *path);

/** @brief Replaces the world with a save, keeping every thing's index and generation.
 *
 * The store's kinds must be declared. A field the save lacks keeps its default; a field the kind
 * lacks is skipped with one warning per kind and field. After the header and the kinds are
 * checked the store is cleared (with no removed hook), so a later failure leaves it partly loaded.
 * The spawned hook runs for every loaded thing, in id order; no `start` is queued, and every loaded
 * thing counts as new to the next StoreFrame.
 * @param store Store, between ticks.
 * @param path Save file.
 * @return True when loaded; false with StoreLastError set. */
bool StoreLoad(Store *store, const char *path);

#endif
