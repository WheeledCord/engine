/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "entity.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *Duplicate(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

static GameplayEntity *Lookup(GameplayWorld *world, EntityHandle entity)
{
    if (!world || entity.index >= world->maxEntities)
        return NULL;
    GameplayEntity *slot = &world->entities[entity.index];
    return slot->alive && slot->generation == entity.generation ? slot : NULL;
}

static const GameplayEntity *LookupConst(const GameplayWorld *world, EntityHandle entity)
{
    return Lookup((GameplayWorld *)world, entity);
}

static const EngineInput emptyInput;

static EntityContext Context(GameplayWorld *world, EntityHandle entity)
{
    GameplayEntity *slot = Lookup(world, entity);
    return (EntityContext){.world = world, .entity = entity,
        .data = EntityData(world, entity), .app = world->context,
        .input = world->input ? world->input : &emptyInput,
        .now = GameplayTime(world), .dt = world->tickInterval,
        .elapsed = slot ? (world->tickCount - slot->lastThinkTick) * world->tickInterval : 0,
        .alpha = 1};
}

// ---- a class's own storage ---------------------------------------------------------------------
static void StorageFree(EntityStorage *storage)
{
    free(storage->dense);
    free(storage->sparse);
    free(storage->freed);
    *storage = (EntityStorage){0};
}

static bool StorageInit(EntityStorage *storage, size_t size, size_t alignment, size_t capacity)
{
    *storage = (EntityStorage){0};
    // Whole multiples of the alignment, so every payload in the array starts where it must.
    storage->stride = (size + alignment - 1) / alignment * alignment;
    if (!storage->stride || capacity > SIZE_MAX / storage->stride)
        return false;
    storage->dense = calloc(capacity, storage->stride);
    storage->sparse = malloc(capacity * sizeof(*storage->sparse));
    storage->freed = malloc(capacity * sizeof(*storage->freed));
    if (!storage->dense || !storage->sparse || !storage->freed)
    {
        StorageFree(storage);
        return false;
    }
    for (size_t i = 0; i < capacity; i++)
        storage->sparse[i] = ENTITY_NO_PLACE;
    storage->capacity = capacity;
    return true;
}

static void *StorageAcquire(EntityStorage *storage, uint32_t slot)
{
    if (storage->sparse[slot] != ENTITY_NO_PLACE)
        return NULL;
    uint32_t place;
    if (storage->freedCount)
        place = storage->freed[--storage->freedCount];
    else if (storage->used < storage->capacity)
        place = (uint32_t)storage->used++;
    else
        return NULL;
    storage->sparse[slot] = place;
    unsigned char *data = storage->dense + (size_t)place * storage->stride;
    memset(data, 0, storage->stride);
    return data;
}

static void StorageRelease(EntityStorage *storage, uint32_t slot)
{
    uint32_t place = storage->sparse[slot];
    if (place == ENTITY_NO_PLACE)
        return;
    memset(storage->dense + (size_t)place * storage->stride, 0, storage->stride);
    storage->sparse[slot] = ENTITY_NO_PLACE;
    storage->freed[storage->freedCount++] = place;
}

// Which storage belongs to an entity's class: the classes never move once one has been spawned.
static EntityStorage *StorageOf(const GameplayWorld *world, const GameplayEntity *slot)
{
    if (!slot || !slot->type)
        return NULL;
    size_t index = (size_t)(slot->type - world->classes);
    return index < world->classCount ? &world->storages[index] : NULL;
}

static void FreeKeyValues(GameplayEntity *entity)
{
    for (size_t i = 0; i < entity->keyValueCount; i++)
    {
        free(entity->keyValues[i].key);
        free(entity->keyValues[i].value);
    }
    free(entity->keyValues);
    entity->keyValues = NULL;
    entity->keyValueCount = 0;
}

bool GameplayWorldInit(GameplayWorld *world, GameplayWorldConfig config)
{
    if (!world) return false;
    *world = (GameplayWorld){0};
    if (!config.maxEntities || config.maxEntities > UINT32_MAX || !isfinite(config.tickInterval) ||
        config.tickInterval <= 0 || config.maxEntities > SIZE_MAX / sizeof(GameplayEntity))
        return false;
    world->entities = calloc(config.maxEntities, sizeof(*world->entities));
    if (!world->entities)
    {
        GameplayWorldFree(world);
        return false;
    }
    world->maxEntities = config.maxEntities;
    world->tickInterval = config.tickInterval;
    for (size_t i = 0; i < world->maxEntities; i++)
        world->entities[i].generation = 1;
    return true;
}

void GameplayWorldClear(GameplayWorld *world)
{
    if (!world || world->clearing) return;
    world->clearing = true;
    for (size_t i = 0; i < world->maxEntities; i++)
        EntityDestroy(world, (EntityHandle){(uint32_t)i, world->entities[i].generation});
    world->tickCount = 0;
    world->clearing = false;
}

void GameplayWorldFree(GameplayWorld *world)
{
    if (!world)
        return;
    GameplayWorldClear(world);
    for (size_t i = 0; i < world->classCount; i++)
    {
        free((char *)world->classes[i].classname);
        StorageFree(&world->storages[i]);
    }
    free(world->storages);
    free(world->classes);
    free(world->entities);
    *world = (GameplayWorld){0};
}

// A class that cannot describe its own payload is refused here, where the reason can be said out
// loud, rather than being found later as one entity writing over another.
static bool ClassDescribesItself(const EntityClass *type)
{
    size_t alignment = type->alignment ? type->alignment : ENTITY_ALIGNMENT_MAX;
    if (!type->size)
    {
        TraceLog(LOG_ERROR, "Entity class %s: payload size must not be zero", type->classname);
        return false;
    }
    if (alignment & (alignment - 1))
    {
        TraceLog(LOG_ERROR, "Entity class %s: alignment %zu is not a power of two", type->classname,
                 alignment);
        return false;
    }
    if (alignment > ENTITY_ALIGNMENT_MAX)
    {
        TraceLog(LOG_ERROR, "Entity class %s: alignment %zu is stricter than anything needs (%zu)",
                 type->classname, alignment, (size_t)ENTITY_ALIGNMENT_MAX);
        return false;
    }
    if (type->alignment && type->size % type->alignment)
    {
        TraceLog(LOG_ERROR, "Entity class %s: size %zu is not a whole number of its alignment %zu",
                 type->classname, type->size, type->alignment);
        return false;
    }
    if (!EntityFieldsValid(type->fields, type->fieldCount, type->size))
    {
        TraceLog(LOG_ERROR, "Entity class %s: a field is duplicated, mistyped, or outside the payload",
                 type->classname);
        return false;
    }
    return true;
}

bool EntityRegister(GameplayWorld *world, EntityClass type)
{
    if (!world || !world->entities || world->registrySealed || !type.classname || !type.classname[0])
        return false;
    if (EntityClassFind(world, type.classname))
    {
        TraceLog(LOG_ERROR, "Entity class %s: already registered", type.classname);
        return false;
    }
    if (!ClassDescribesItself(&type))
        return false;
    if (world->classCount == world->classCapacity)
    {
        size_t capacity = world->classCapacity ? world->classCapacity * 2 : 16;
        EntityClass *classes = realloc(world->classes, capacity * sizeof(*classes));
        if (!classes)
            return false;
        world->classes = classes;
        EntityStorage *storages = realloc(world->storages, capacity * sizeof(*storages));
        if (!storages)
            return false;
        world->storages = storages;
        world->classCapacity = capacity;
    }
    char *name = Duplicate(type.classname);
    if (!name)
        return false;
    // The class brings its own size, so its entities live in storage of exactly that shape.
    if (!StorageInit(&world->storages[world->classCount], type.size,
                     type.alignment ? type.alignment : ENTITY_ALIGNMENT_MAX, world->maxEntities))
    {
        TraceLog(LOG_ERROR, "Entity class %s: no room for %zu entities of %zu bytes", type.classname,
                 world->maxEntities, type.size);
        free(name);
        return false;
    }
    type.classname = name;
    world->classes[world->classCount++] = type;
    return true;
}

const EntityClass *EntityClassFind(const GameplayWorld *world, const char *classname)
{
    if (!world || !classname)
        return NULL;
    for (size_t i = 0; i < world->classCount; i++)
        if (!strcmp(world->classes[i].classname, classname))
            return &world->classes[i];
    return NULL;
}

bool EntityRegisterAll(GameplayWorld *world, const EntityClass *types, size_t count)
{
    if (count && !types) return false;
    for (size_t i = 0; i < count; i++)
        if (!EntityRegister(world, types[i])) return false;
    return true;
}

EntityHandle EntitySpawn(GameplayWorld *world, const char *classname)
{
    return EntitySpawnWith(world, classname, NULL, 0);
}

EntityHandle EntitySpawnWith(GameplayWorld *world, const char *classname,
                             const EntityProperty *properties, size_t count)
{
    const EntityClass *type = EntityClassFind(world, classname);
    if (!type || world->clearing || (count && !properties)) return ENTITY_NULL;
    // Class pointers cannot move after instances start borrowing them.
    world->registrySealed = true;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (slot->alive) continue;
        slot->alive = true;
        slot->type = type;
        slot->scheduledThinkTick = UINT64_MAX;
        slot->lastThinkTick = world->tickCount;
        void *data = StorageAcquire(&world->storages[(size_t)(type - world->classes)], (uint32_t)i);
        if (!data)
        {
            slot->alive = false;
            slot->type = NULL;
            return ENTITY_NULL;
        }
        if (type->defaults) memcpy(data, type->defaults, type->size);
        EntityHandle entity = {(uint32_t)i, slot->generation};
        for (size_t kv = 0; kv < count; kv++)
            if (!EntityKeyValue(world, entity, properties[kv].key, properties[kv].value))
            {
                fprintf(stderr, "Entity %s: invalid property %s\n", classname,
                        properties[kv].key ? properties[kv].key : "(null)");
                EntityDestroy(world, entity);
                return ENTITY_NULL;
            }
        slot->started = true;
        EntityContext context = Context(world, entity);
        if ((type->Spawn && !type->Spawn(&context)) || !EntityAlive(world, entity))
        {
            EntityDestroy(world, entity);
            return ENTITY_NULL;
        }
        return entity;
    }
    return ENTITY_NULL;
}

bool EntityDestroy(GameplayWorld *world, EntityHandle entity)
{
    GameplayEntity *slot = Lookup(world, entity);
    if (!slot || slot->destroying) return false;
    slot->destroying = true;
    if (slot->started && slot->type->Destroy)
    {
        EntityContext context = Context(world, entity);
        slot->type->Destroy(&context);
    }
    FreeKeyValues(slot);
    slot->started = false;
    slot->destroying = false;
    slot->alive = false;
    slot->scheduledThinkTick = UINT64_MAX;
    slot->generation++;
    if (!slot->generation)
        slot->generation = 1;
    // The payload goes back to its own class's storage, which is found through the type.
    EntityStorage *storage = StorageOf(world, slot);
    if (storage)
        StorageRelease(storage, entity.index);
    slot->type = NULL;
    return true;
}

bool EntityAlive(const GameplayWorld *world, EntityHandle entity)
{
    return LookupConst(world, entity) != NULL;
}

void *EntityData(GameplayWorld *world, EntityHandle entity)
{
    GameplayEntity *slot = Lookup(world, entity);
    EntityStorage *storage = StorageOf(world, slot);
    if (!storage || storage->sparse[entity.index] == ENTITY_NO_PLACE)
        return NULL;
    return storage->dense + (size_t)storage->sparse[entity.index] * storage->stride;
}

const void *EntityDataConst(const GameplayWorld *world, EntityHandle entity)
{
    return EntityData((GameplayWorld *)world, entity);
}

const char *EntityClassname(const GameplayWorld *world, EntityHandle entity)
{
    const GameplayEntity *slot = LookupConst(world, entity);
    return slot ? slot->type->classname : NULL;
}

bool EntityKeyValue(GameplayWorld *world, EntityHandle entity, const char *key, const char *value)
{
    GameplayEntity *slot = Lookup(world, entity);
    if (!slot || !key || !value || !key[0])
        return false;
    if (slot->destroying) return false;
    // Reserve storage before invoking a setter, so allocation failure cannot leave stale source data.
    char *keyCopy = Duplicate(key), *valueCopy = Duplicate(value);
    if (!keyCopy || !valueCopy) { free(keyCopy); free(valueCopy); return false; }
    size_t index = 0;
    while (index < slot->keyValueCount && strcmp(slot->keyValues[index].key, key)) index++;
    if (index == slot->keyValueCount)
    {
        if (slot->keyValueCount == UINT32_MAX) { free(keyCopy); free(valueCopy); return false; }
        EntitySceneKeyValue *items = realloc(slot->keyValues, (index + 1) * sizeof(*items));
        if (!items) { free(keyCopy); free(valueCopy); return false; }
        slot->keyValues = items;
    }
    const EntityField *field = NULL;
    for (size_t i = 0; i < slot->type->fieldCount; i++)
        if (!strcmp(slot->type->fields[i].name, key)) { field = slot->type->fields + i; break; }
    EntityContext context = Context(world, entity);
    bool ok = field ? EntityFieldSet(context.data, field, value) :
                     slot->type->KeyValue && slot->type->KeyValue(&context, key, value);
    if (!ok || !EntityAlive(world, entity)) { free(keyCopy); free(valueCopy); return false; }
    if (index < slot->keyValueCount)
    {
        free(slot->keyValues[index].key);
        free(slot->keyValues[index].value);
    }
    else slot->keyValueCount++;
    slot->keyValues[index] = (EntitySceneKeyValue){keyCopy, valueCopy};
    return true;
}

size_t EntityKeyValueCount(const GameplayWorld *world, EntityHandle entity)
{
    const GameplayEntity *slot = LookupConst(world, entity);
    return slot ? slot->keyValueCount : 0;
}

const EntitySceneKeyValue *EntityKeyValueAt(const GameplayWorld *world, EntityHandle entity, size_t index)
{
    const GameplayEntity *slot = LookupConst(world, entity);
    return slot && index < slot->keyValueCount ? &slot->keyValues[index] : NULL;
}

void EntityScheduleThink(GameplayWorld *world, EntityHandle entity, double when)
{
    GameplayEntity *slot = Lookup(world, entity);
    if (!slot)
        return;
    if (!isfinite(when))
    {
        slot->scheduledThinkTick = UINT64_MAX;
        return;
    }
    double tick = floor(0.5 + when / world->tickInterval);
    slot->scheduledThinkTick =
        tick <= 0 ? 0 : tick >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)tick;
}

double EntityScheduledThinkTime(const GameplayWorld *world, EntityHandle entity)
{
    const GameplayEntity *slot = LookupConst(world, entity);
    return slot && slot->scheduledThinkTick != UINT64_MAX
               ? (double)slot->scheduledThinkTick * world->tickInterval
               : INFINITY;
}

double GameplayTime(const GameplayWorld *world)
{
    return world ? (double)world->tickCount * world->tickInterval : 0;
}

void EntityThinkNext(EntityContext *entity)
{
    if (!entity) return;
    GameplayEntity *slot = Lookup(entity->world, entity->entity);
    if (slot && entity->world->tickCount < UINT64_MAX)
        slot->scheduledThinkTick = entity->world->tickCount + 1;
}

void EntityThinkAfter(EntityContext *entity, double delay)
{
    if (entity) EntityScheduleThink(entity->world, entity->entity, entity->now + delay);
}

void GameplayWorldStep(GameplayWorld *world)
{
    GameplayWorldStepInput(world, NULL);
}

void GameplayWorldStepInput(GameplayWorld *world, const EngineInput *input)
{
    if (!world || world->stepping || world->clearing || world->tickCount == UINT64_MAX) return;
    world->stepping = true;
    world->input = input;
    world->tickCount++;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (!slot->alive || !slot->started || slot->destroying || !slot->type->Think ||
            slot->scheduledThinkTick > world->tickCount) continue;
        EntityContext context = Context(world, (EntityHandle){(uint32_t)i, slot->generation});
        slot->scheduledThinkTick = UINT64_MAX;
        slot->lastThinkTick = world->tickCount;
        slot->type->Think(&context);
    }
    world->input = NULL;
    world->stepping = false;
}

void GameplayWorldDraw(GameplayWorld *world, void *drawContext)
{
    GameplayWorldDrawInterpolated(world, 1, drawContext);
}

void GameplayWorldDrawInterpolated(GameplayWorld *world, float alpha, void *drawContext)
{
    if (!world || world->clearing) return;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (slot->alive && slot->started && !slot->destroying && slot->type->Draw)
        {
            EntityContext context = Context(world, (EntityHandle){(uint32_t)i, slot->generation});
            context.alpha = alpha;
            context.draw = drawContext;
            slot->type->Draw(&context);
        }
    }
}
