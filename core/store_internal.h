/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_STORE_INTERNAL_H
#define CORE_STORE_INTERNAL_H

/* What store.c and store_save.c share and nothing else may use. */
#include "store.h"

#define STORE_NO_INDEX UINT32_MAX
#define STORE_THING_LIVE 1u
#define STORE_THING_REMOVED 2u /* out of every query; freed at the end of the tick or frame */
#define STORE_THING_GUEST 4u
#define STORE_THING_LOCAL 8u /* presentation only: StoreMarkLocal, or a declared child of one */
#define STORE_THING_REPLICA 16u /* removed by StoreRemoveReplica: its guests are not sent orphaned */

/* One slot of the thing table. Only integers, so a snapshot copies it and a hash reads it field by
   field with no padding in play. */
struct StoreThing
{
    uint32_t generation;
    int32_t kind; /* -1 while the slot is free */
    uint32_t row; /* the thing's block in its kind's pools */
    uint32_t parent, firstChild, nextSibling;
    StoreSymbol childName;
    int32_t owner, spawner;
    uint32_t flags;
    uint64_t random;
};

struct StoreKindData
{
    char *name;
    StoreKind base;
    int fieldCount;
    StoreFieldDecl *fields; /* names owned; init holds the current default */
    int *offsets;           /* into the shared or the local block, by the field's flags */
    int *sizes;
    int sharedSize, localSize;
    unsigned char *sharedTemplate, *localTemplate;
    unsigned char *shared, *local; /* rowCapacity blocks each */
    unsigned char *shadow;         /* shared blocks as the last StoreFrame saw them */
    StoreId *shadowThing;          /* which thing each shadow row belongs to */
    StoreId *shadowParent;         /* each shadow row's parent as the last StoreFrame saw it */
    uint32_t rows, rowCapacity, shadowCapacity;
    uint32_t *freeRows; /* capacity is always rowCapacity, so freeing never allocates */
    uint32_t freeCount;
    StoreHandlerFn handler;
    void *user;
    StoreSymbol *events; /* handled by this kind itself */
    int eventCount, eventCapacity;
    StoreSymbol *warned; /* events reported as unhandled */
    int warnedCount, warnedCapacity;
    /* Worked out from the base chain whenever a kind, handler or handled event changes. */
    StoreHandlerFn callHandler;
    void *callUser;
    bool handlesTick, handlesFrame, handlesDrawHud, watches;
    StoreSymbol parentChanged; /* parent-changed when the kind handles it, else STORE_NO_SYMBOL */
    StoreSymbol *changed; /* per field: its <field>-changed event, or STORE_NO_SYMBOL */
};

struct StoreTimer
{
    uint64_t due, sequence;
    StoreId target;
    StoreSymbol event;
    int count;
    StoreValue args[STORE_MAX_ARGS];
};

/* Records an error in StoreLastError and, for a rule or type error, tells the error hook.
   Always answers false. */
bool StoreFail(const Store *store, bool rule, const char *format, ...);
/* Empties the world, keeping kinds, symbols, systems and hooks. No hook is called. */
void StoreClearWorld(Store *store);
/* Makes the thing table hold count slots; new slots are free with the given generation. */
bool StoreReserveSlots(Store *store, uint32_t count, uint32_t generation);
/* Puts a thing of a kind, at its defaults, into the free slot id.index with id.generation. It is a
   root owned by owner until StoreLinkChild. */
bool StorePlaceThing(Store *store, StoreId id, StoreKind kind, int owner, int spawner,
                     uint64_t random);
/* Hangs child last under parent (after the declared children when not a guest). */
void StoreLinkChild(Store *store, uint32_t child, uint32_t parent, StoreSymbol name, bool guest);
bool StoreInsertTimer(Store *store, const StoreTimer *timer);
/* StoreRemove for a removal another machine made, as its state says (store_net): no owner rule, and
   guests are detached without `orphaned`, which the machine that removed the parent sent. */
bool StoreRemoveReplica(Store *store, StoreId id);
/* Whether a timer belongs to a local thing: not hashed, not saved. */
bool StoreLocalTimer(const Store *store, const StoreTimer *timer);
void StoreSetClock(Store *store, uint64_t tickCount, uint64_t worldRandom);
const char *StoreTypeName(StoreType type);

#endif
