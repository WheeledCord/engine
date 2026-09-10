/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_ENTITY_H
#define GAMEPLAY_ENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GameplayWorld GameplayWorld;

typedef struct EntityHandle
{
    uint32_t index;
    uint32_t generation;
} EntityHandle;

#define ENTITY_NULL ((EntityHandle){UINT32_MAX, 0})

typedef void (*EntitySpawnFn)(GameplayWorld *world, EntityHandle entity);
typedef bool (*EntityKeyValueFn)(GameplayWorld *world, EntityHandle entity, const char *key,
                                 const char *value);
typedef void (*EntityThinkFn)(GameplayWorld *world, EntityHandle entity, double now);
typedef void (*EntityDrawFn)(GameplayWorld *world, EntityHandle entity, void *drawContext);

typedef struct EntityClass
{
    const char *classname;
    size_t size;
    EntitySpawnFn Spawn;
    EntityKeyValueFn KeyValue;
    EntityThinkFn Think;
    EntityDrawFn Draw;
} EntityClass;

typedef struct EntitySceneKeyValue
{
    char *key;
    char *value;
} EntitySceneKeyValue;

typedef struct GameplayEntity
{
    uint32_t generation;
    uint32_t keyValueCount;
    bool alive;
    uint64_t scheduledThinkTick;
    const EntityClass *type;
    EntitySceneKeyValue *keyValues;
} GameplayEntity;

struct GameplayWorld
{
    GameplayEntity *entities;
    unsigned char *storage;
    EntityClass *classes;
    size_t maxEntities;
    size_t maxEntitySize;
    size_t classCount;
    size_t classCapacity;
    uint64_t tickCount;
    double tickInterval;
};

typedef struct GameplayWorldConfig
{
    size_t maxEntities;
    size_t maxEntitySize;
    double tickInterval;
} GameplayWorldConfig;

bool GameplayWorldInit(GameplayWorld *world, GameplayWorldConfig config);
void GameplayWorldFree(GameplayWorld *world);
void GameplayWorldClear(GameplayWorld *world);

bool EntityRegister(GameplayWorld *world, EntityClass type);
const EntityClass *EntityClassFind(const GameplayWorld *world, const char *classname);

EntityHandle EntitySpawn(GameplayWorld *world, const char *classname);
bool EntityDestroy(GameplayWorld *world, EntityHandle entity);
bool EntityAlive(const GameplayWorld *world, EntityHandle entity);
void *EntityData(GameplayWorld *world, EntityHandle entity);
const void *EntityDataConst(const GameplayWorld *world, EntityHandle entity);
const char *EntityClassname(const GameplayWorld *world, EntityHandle entity);

bool EntityKeyValue(GameplayWorld *world, EntityHandle entity, const char *key, const char *value);
size_t EntityKeyValueCount(const GameplayWorld *world, EntityHandle entity);
const EntitySceneKeyValue *EntityKeyValueAt(const GameplayWorld *world, EntityHandle entity, size_t index);

void EntityScheduleThink(GameplayWorld *world, EntityHandle entity, double when);
double EntityScheduledThinkTime(const GameplayWorld *world, EntityHandle entity);
double GameplayTime(const GameplayWorld *world);
void GameplayWorldStep(GameplayWorld *world);
void GameplayWorldDraw(GameplayWorld *world, void *drawContext);

#endif
