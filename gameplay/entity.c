/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "entity.h"

#include <math.h>
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
    if (!world || !config.maxEntities || !config.maxEntitySize || !isfinite(config.tickInterval) ||
        config.tickInterval <= 0)
        return false;
    *world = (GameplayWorld){0};
    if (config.maxEntities > SIZE_MAX / config.maxEntitySize)
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
    if (!world)
        return;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        FreeKeyValues(&world->entities[i]);
        world->entities[i].alive = false;
        world->entities[i].type = NULL;
        world->entities[i].scheduledThinkTick = UINT64_MAX;
        world->entities[i].generation++;
        if (!world->entities[i].generation)
            world->entities[i].generation = 1;
    }
    if (world->storage)
        memset(world->storage, 0, world->maxEntities * world->maxEntitySize);
    world->tickCount = 0;
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
    if (!world || !type.classname || !type.classname[0] || !type.size || type.size > world->maxEntitySize ||
        EntityClassFind(world, type.classname))
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

EntityHandle EntitySpawn(GameplayWorld *world, const char *classname)
{
    const EntityClass *type = EntityClassFind(world, classname);
    if (!type)
        return ENTITY_NULL;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (slot->alive)
            continue;
        slot->alive = true;
        slot->type = type;
        slot->scheduledThinkTick = UINT64_MAX;
        memset(world->storage + i * world->maxEntitySize, 0, world->maxEntitySize);
        EntityHandle entity = {(uint32_t)i, slot->generation};
        if (type->Spawn)
            type->Spawn(world, entity);
        return entity;
    }
    return ENTITY_NULL;
}

bool EntityDestroy(GameplayWorld *world, EntityHandle entity)
{
    GameplayEntity *slot = Lookup(world, entity);
    if (!slot)
        return false;
    FreeKeyValues(slot);
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
    if (slot->type->KeyValue && !slot->type->KeyValue(world, entity, key, value))
        return false;
    EntitySceneKeyValue *items = realloc(slot->keyValues, (slot->keyValueCount + 1) * sizeof(*items));
    if (!items)
        return false;
    slot->keyValues = items;
    EntitySceneKeyValue *item = &items[slot->keyValueCount];
    item->key = Duplicate(key);
    item->value = Duplicate(value);
    if (!item->key || !item->value)
    {
        free(item->key);
        free(item->value);
        return false;
    }
    slot->keyValueCount++;
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

void GameplayWorldStep(GameplayWorld *world)
{
    if (!world || world->tickCount == UINT64_MAX)
        return;
    world->tickCount++;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (!slot->alive || !slot->type->Think || slot->scheduledThinkTick > world->tickCount)
            continue;
        EntityHandle entity = {(uint32_t)i, slot->generation};
        slot->scheduledThinkTick = UINT64_MAX;
        slot->type->Think(world, entity, GameplayTime(world));
    }
}

void GameplayWorldDraw(GameplayWorld *world, void *drawContext)
{
    if (!world)
        return;
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        GameplayEntity *slot = &world->entities[i];
        if (slot->alive && slot->type->Draw)
            slot->type->Draw(world, (EntityHandle){(uint32_t)i, slot->generation}, drawContext);
    }
}
