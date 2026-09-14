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

/**
 * @brief Initializes an empty entity world.
 *
 * The function zeroes world before validating config, so GameplayWorldFree is safe after failure.
 * @param world Storage to initialize; must not already own a live world.
 * @param config Slot capacity and positive fixed tick interval.
 * @return True when world is ready for class registration; false for invalid configuration or allocation failure.
 */
bool GameplayWorldInit(GameplayWorld *world, GameplayWorldConfig config);
/**
 * @brief Releases a world and destroys its remaining entities.
 *
 * It is safe to call with NULL or a zeroed/failed-initialization world.
 * @param world World to clear and release.
 * @return No value; world is reset to zero when non-NULL.
 */
void GameplayWorldFree(GameplayWorld *world);
void GameplayWorldClear(GameplayWorld *world);
/**
 * @brief Registers one entity class before the first spawn.
 *
 * The world copies the class record but borrows defaults and field metadata for its lifetime.
 * @param world Initialized world whose class registry is not sealed.
 * @param type Class declaration with a valid payload description.
 * @return True when the class is registered; false for invalid, duplicate, late, or unallocatable classes.
 */
bool EntityRegister(GameplayWorld *world, EntityClass type);
bool EntityRegisterAll(GameplayWorld *world, const EntityClass *types, size_t count);
const EntityClass *EntityClassFind(const GameplayWorld *world, const char *classname);

EntityHandle EntitySpawn(GameplayWorld *world, const char *classname);
/**
 * @brief Spawns a configured entity from a registered class.
 *
 * Defaults and properties are applied before Spawn. Property data is borrowed only for this call.
 * @param world Initialized world containing classname.
 * @param classname Registered class name.
 * @param properties Borrowed key/value properties, or NULL when count is zero.
 * @param count Number of properties.
 * @return A live handle, or ENTITY_NULL for invalid input, capacity failure, or failed Spawn.
 */
EntityHandle EntitySpawnWith(GameplayWorld *world, const char *classname,
                             const EntityProperty *properties, size_t count);
/**
 * @brief Destroys one live entity exactly once.
 *
 * Destruction invalidates the handle generation and releases the entity payload slot.
 * @param world World containing entity.
 * @param entity Handle to destroy.
 * @return True when a live entity was destroyed; false for a stale or invalid handle.
 */
bool EntityDestroy(GameplayWorld *world, EntityHandle entity);
/**
 * @brief Reports whether an entity handle is currently live.
 * @param world World to query.
 * @param entity Handle to test.
 * @return True only when entity's index and generation name a live entity.
 */
bool EntityAlive(const GameplayWorld *world, EntityHandle entity);
/**
 * @brief Returns an entity's mutable payload.
 *
 * Do not retain the returned pointer across destruction, world clear, or callbacks that can invalidate entity.
 * @param world World containing entity.
 * @param entity Live handle to query.
 * @return Borrowed payload pointer, or NULL for a stale or invalid handle.
 */
void *EntityData(GameplayWorld *world, EntityHandle entity);
const void *EntityDataConst(const GameplayWorld *world, EntityHandle entity);
const char *EntityClassname(const GameplayWorld *world, EntityHandle entity);
bool EntityKeyValue(GameplayWorld *world, EntityHandle entity, const char *key, const char *value);
size_t EntityKeyValueCount(const GameplayWorld *world, EntityHandle entity);
const EntitySceneKeyValue *EntityKeyValueAt(const GameplayWorld *world, EntityHandle entity, size_t index);

/* Walking the world. Spawning and destroying are not enough on their own: choosing a target,
   sweeping an area, counting what is left, saving -- all of them start by asking what is actually
   in the world, and until now nothing could.

   Handles rather than pointers, so a walk that destroys as it goes cannot walk into freed memory: a
   handle to something already gone simply stops the walk. A NULL classname walks everything; naming
   one walks only that class. Entities spawned during a walk may or may not be reached by it, which
   is why a walk that spawns should collect first and spawn afterwards. */
/**
 * @brief Starts a walk of live entities.
 * @param world World to walk.
 * @param classname Class name to filter by, or NULL for every class.
 * @return The first matching live handle, or ENTITY_NULL when none exists.
 */
EntityHandle EntityFirst(const GameplayWorld *world, const char *classname);
/**
 * @brief Advances a walk started by EntityFirst.
 *
 * Destroying an entity during a walk is supported; entities spawned during it may or may not be reached.
 * @param world World being walked.
 * @param after Previous handle returned by the walk.
 * @param classname The same class filter supplied to EntityFirst, or NULL.
 * @return The next matching live handle, or ENTITY_NULL at the end.
 */
EntityHandle EntityNext(const GameplayWorld *world, EntityHandle after, const char *classname);

void EntityScheduleThink(GameplayWorld *world, EntityHandle entity, double when);
void EntityThinkNext(EntityContext *entity);
void EntityThinkAfter(EntityContext *entity, double delay);
double EntityScheduledThinkTime(const GameplayWorld *world, EntityHandle entity);
double GameplayTime(const GameplayWorld *world);
void GameplayWorldStep(GameplayWorld *world);
/**
 * @brief Advances scheduled entity Think callbacks by one fixed tick.
 *
 * The supplied input is borrowed only while callbacks are dispatched.
 * @param world World to step.
 * @param input Current simulation input, or NULL for an empty input snapshot.
 * @return No value; reentrant, clearing, and exhausted worlds are left unchanged.
 */
void GameplayWorldStepInput(GameplayWorld *world, const EngineInput *input);
void GameplayWorldDraw(GameplayWorld *world, void *drawContext);
/**
 * @brief Dispatches entity Draw callbacks with an interpolation fraction.
 *
 * This function does not advance simulation time. drawContext is passed through without ownership transfer.
 * @param world World to draw.
 * @param alpha Interpolation fraction supplied to each draw callback.
 * @param drawContext Borrowed caller-defined drawing context.
 * @return No value; a clearing or NULL world draws nothing.
 */
void GameplayWorldDrawInterpolated(GameplayWorld *world, float alpha, void *drawContext);
#endif
