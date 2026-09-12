/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_ENTITY_H
#define GAMEPLAY_ENTITY_H
#include "core/engine.h"
#include "fields.h"
#include <stddef.h>
#include <stdint.h>

typedef struct GameplayWorld GameplayWorld;
typedef struct EntityHandle { uint32_t index, generation; } EntityHandle;
#define ENTITY_NULL ((EntityHandle){UINT32_MAX, 0})

typedef struct EntityContext
{
    GameplayWorld *world;
    EntityHandle entity;
    void *data;                 // This instance's payload; valid for this callback until destruction.
    void *app;                  // Project context, borrowed from the world.
    const EngineInput *input;  // Current simulation input; empty outside StepInput.
    double now, dt, elapsed;   // World time, step duration, time since spawn/previous Think.
    float alpha;              // Interpolation fraction during Draw.
    void *draw;               // Optional caller-provided drawing context.
} EntityContext;

typedef bool (*EntitySpawnFn)(EntityContext *entity);
typedef bool (*EntityKeyValueFn)(EntityContext *entity, const char *key, const char *value);
typedef void (*EntityThinkFn)(EntityContext *entity);
typedef void (*EntityDrawFn)(EntityContext *entity);
typedef void (*EntityDestroyFn)(EntityContext *entity);

/* C99 has no alignof; this is the usual stand-in, and what a class declares about its payload. */
#define ENTITY_ALIGNMENT_OF(type) offsetof(struct { char first; type second; }, second)
// The strictest alignment an ordinary payload can need: scalars, pointers and vectors.
#define ENTITY_ALIGNMENT_MAX (sizeof(union { long double number; void *pointer; void (*call)(void); }))

typedef struct EntityClass
{
    const char *classname;
    size_t size;
    /* What the payload needs, from ENTITY_ALIGNMENT_OF. Zero asks for the strictest alignment the
       platform has. Anything else must be a power of two that size is a whole number of. */
    size_t alignment;
    const void *defaults;      // Optional payload-sized value copied before properties; borrowed.
    const EntityField *fields; // Borrowed immutable declarations, alive as long as the world.
    size_t fieldCount;
    EntitySpawnFn Spawn;       // Properties are already applied. False aborts and calls Destroy.
    EntityKeyValueFn KeyValue; // Only keys not found in fields. NULL rejects unknown keys.
    EntityThinkFn Think;
    EntityDrawFn Draw;
    EntityDestroyFn Destroy;   // Also runs for partially failed Spawn, exactly once.
} EntityClass;

typedef struct EntityProperty { const char *key, *value; } EntityProperty;
typedef struct EntitySceneKeyValue { char *key, *value; } EntitySceneKeyValue;
typedef struct GameplayEntity
{
    uint32_t generation, keyValueCount;
    bool alive, started, destroying;
    uint64_t scheduledThinkTick, lastThinkTick;
    const EntityClass *type;
    EntitySceneKeyValue *keyValues;
} GameplayEntity;

/* Each class keeps its own entities' payloads, so no class can reach another's. sparse maps an
   entity's slot to its place in dense; a freed place is kept for the next entity of that class
   rather than being closed up, because compacting would move a payload a running callback is
   holding a pointer to. */
#define ENTITY_NO_PLACE UINT32_MAX
typedef struct EntityStorage
{
    unsigned char *dense;
    uint32_t *sparse; // one per entity slot
    uint32_t *freed;  // places to hand out again before taking a new one
    size_t stride, used, freedCount, capacity;
} EntityStorage;

struct GameplayWorld
{
    GameplayEntity *entities;
    EntityClass *classes;
    EntityStorage *storages; // one for each class, in step with classes
    size_t maxEntities, classCount, classCapacity;
    uint64_t tickCount;
    double tickInterval;
    void *context;
    const EngineInput *input;
    bool registrySealed, clearing, stepping;
};

typedef struct GameplayWorldConfig
{
    // How many entities can exist at once. A class brings its own payload size when it registers.
    size_t maxEntities;
    double tickInterval;
} GameplayWorldConfig;

bool GameplayWorldInit(GameplayWorld *world, GameplayWorldConfig config);
void GameplayWorldFree(GameplayWorld *world);
void GameplayWorldClear(GameplayWorld *world);
bool EntityRegister(GameplayWorld *world, EntityClass type);
bool EntityRegisterAll(GameplayWorld *world, const EntityClass *types, size_t count);
const EntityClass *EntityClassFind(const GameplayWorld *world, const char *classname);

EntityHandle EntitySpawn(GameplayWorld *world, const char *classname);
EntityHandle EntitySpawnWith(GameplayWorld *world, const char *classname,
                             const EntityProperty *properties, size_t count);
bool EntityDestroy(GameplayWorld *world, EntityHandle entity);
bool EntityAlive(const GameplayWorld *world, EntityHandle entity);
void *EntityData(GameplayWorld *world, EntityHandle entity);
const void *EntityDataConst(const GameplayWorld *world, EntityHandle entity);
const char *EntityClassname(const GameplayWorld *world, EntityHandle entity);
bool EntityKeyValue(GameplayWorld *world, EntityHandle entity, const char *key, const char *value);
size_t EntityKeyValueCount(const GameplayWorld *world, EntityHandle entity);
const EntitySceneKeyValue *EntityKeyValueAt(const GameplayWorld *world, EntityHandle entity, size_t index);

void EntityScheduleThink(GameplayWorld *world, EntityHandle entity, double when);
void EntityThinkNext(EntityContext *entity);
void EntityThinkAfter(EntityContext *entity, double delay);
double EntityScheduledThinkTime(const GameplayWorld *world, EntityHandle entity);
double GameplayTime(const GameplayWorld *world);
void GameplayWorldStep(GameplayWorld *world);
void GameplayWorldStepInput(GameplayWorld *world, const EngineInput *input);
void GameplayWorldDraw(GameplayWorld *world, void *drawContext);
void GameplayWorldDrawInterpolated(GameplayWorld *world, float alpha, void *drawContext);
#endif
