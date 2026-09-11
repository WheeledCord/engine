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
    if (!config.maxEntities || config.maxEntities > UINT32_MAX || !config.maxEntitySize || !isfinite(config.tickInterval) ||
        config.tickInterval <= 0)
        return false;
    // C99 alignment for ordinary scalar/pointer/vector payloads, including odd-sized structs.
    union Alignment { long double number; void *pointer; void (*function)(void); };
    size_t alignment = sizeof(union Alignment);
    if (config.maxEntitySize > SIZE_MAX - alignment + 1) return false;
    config.maxEntitySize = (config.maxEntitySize + alignment - 1) / alignment * alignment;
    if (config.maxEntities > SIZE_MAX / config.maxEntitySize ||
        config.maxEntities > SIZE_MAX / sizeof(GameplayEntity))
        return false;
    world->entities = calloc(config.maxEntities, sizeof(*world->entities));
    world->storage = calloc(config.maxEntities, config.maxEntitySize);
    if (!world->entities || !world->storage)
    {
        GameplayWorldFree(world);
        return false;
    }
    world->maxEntities = config.maxEntities;
    world->maxEntitySize = config.maxEntitySize;
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
        free((char *)world->classes[i].classname);
    free(world->classes);
    free(world->storage);
    free(world->entities);
    *world = (GameplayWorld){0};
}

bool EntityRegister(GameplayWorld *world, EntityClass type)
{
    if (!world || !world->storage || world->registrySealed || !type.classname || !type.classname[0] || !type.size || type.size > world->maxEntitySize ||
        EntityClassFind(world, type.classname) ||
        !EntityFieldsValid(type.fields, type.fieldCount, type.size))
        return false;
    if (world->classCount == world->classCapacity)
    {
        size_t capacity = world->classCapacity ? world->classCapacity * 2 : 16;
        EntityClass *classes = realloc(world->classes, capacity * sizeof(*classes));
        if (!classes)
            return false;
        world->classes = classes;
        world->classCapacity = capacity;
    }
    char *name = Duplicate(type.classname);
    if (!name)
        return false;
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
        void *data = world->storage + i * world->maxEntitySize;
        memset(data, 0, world->maxEntitySize);
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
    slot->type = NULL;
    slot->scheduledThinkTick = UINT64_MAX;
    slot->generation++;
    if (!slot->generation)
        slot->generation = 1;
    memset(world->storage + entity.index * world->maxEntitySize, 0, world->maxEntitySize);
    return true;
}

bool EntityAlive(const GameplayWorld *world, EntityHandle entity)
{
    return LookupConst(world, entity) != NULL;
}

void *EntityData(GameplayWorld *world, EntityHandle entity)
{
    return Lookup(world, entity) ? world->storage + entity.index * world->maxEntitySize : NULL;
}

const void *EntityDataConst(const GameplayWorld *world, EntityHandle entity)
{
    return LookupConst(world, entity) ? world->storage + entity.index * world->maxEntitySize : NULL;
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
