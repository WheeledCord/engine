/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gameplay/entity.h"
#include "gameplay/scene.h"
#include "gameplay/systems.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Counter
{
    int thinks;
    float period;
    char name[64];
} Counter;

typedef struct Test
{
    int failures;
    int systemUpdates;
} Test;

static void Check(Test *test, bool condition, const char *message)
{
    printf("%s: %s\n", message, condition ? "PASS" : "FAIL");
    if (!condition)
        test->failures++;
}

static void Spawn(GameplayWorld *world, EntityHandle entity)
{
    Counter *counter = EntityData(world, entity);
    counter->period = 1.0f;
    EntityScheduleThink(world, entity, GameplayTime(world) + counter->period);
}

static bool KeyValue(GameplayWorld *world, EntityHandle entity, const char *key, const char *value)
{
    Counter *counter = EntityData(world, entity);
    if (!strcmp(key, "name"))
    {
        snprintf(counter->name, sizeof(counter->name), "%s", value);
        return true;
    }
    if (!strcmp(key, "period"))
    {
        char *end = NULL;
        float period = strtof(value, &end);
        if (!end || *end || period <= 0)
            return false;
        counter->period = period;
        EntityScheduleThink(world, entity, GameplayTime(world) + period);
        return true;
    }
    return false;
}

static void Think(GameplayWorld *world, EntityHandle entity, double now)
{
    Counter *counter = EntityData(world, entity);
    counter->thinks++;
    EntityScheduleThink(world, entity, now + counter->period);
}

static void SystemUpdate(void *context, GameplayWorld *world, double dt)
{
    Test *test = context;
    (void)dt;
    test->systemUpdates++;
    GameplayWorldStep(world);
}

static EntityHandle FindCounter(const GameplayWorld *world)
{
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        EntityHandle entity = {(uint32_t)i, world->entities[i].generation};
        if (EntityAlive(world, entity) && !strcmp(EntityClassname(world, entity), "demo_counter"))
            return entity;
    }
    return ENTITY_NULL;
}

int main(void)
{
    Test test = {0};
    GameplayWorld world;
    GameplaySystems systems = {0};
    bool started = GameplayWorldInit(&world, (GameplayWorldConfig){8, sizeof(Counter), 0.1}) &&
                   EntityRegister(&world, (EntityClass){"demo_counter", sizeof(Counter), Spawn, KeyValue,
                                                        Think, NULL}) &&
                   GameplaySystemsAdd(&systems, (GameplaySystem){"world", &test, SystemUpdate, NULL, NULL}) &&
                   GameplaySceneLoad(&world, "projects/gameplay_test/assets/demo.scene", true);
    Check(&test, started, "gameplay setup and scene load");
    if (started)
    {
        EntityHandle first = FindCounter(&world);
        Counter *counter = EntityData(&world, first);
        Check(&test,
              counter && !strcmp(counter->name, "scene counter") && fabsf(counter->period - 0.2f) < 0.001f,
              "classname KeyValue configures flat entity payload");

        EntityHandle stale = EntitySpawn(&world, "demo_counter");
        Check(&test, EntityDestroy(&world, stale) && !EntityAlive(&world, stale),
              "destroy invalidates generational handle");
        EntityHandle reused = EntitySpawn(&world, "demo_counter");
        Check(&test, reused.index == stale.index && reused.generation != stale.generation,
              "reused slot receives a new generation");
        EntityDestroy(&world, reused);

        for (int i = 0; i < 10; i++)
            GameplaySystemsUpdate(&systems, &world, 0.1);
        counter = EntityData(&world, first);
        Check(&test, test.systemUpdates == 10 && counter && counter->thinks == 5,
              "system registry advances due Think only when scheduled");

        Check(&test, GameplaySceneWrite(&world, "build/core/gameplay-roundtrip.scene"),
              "scene writer preserves source keyvalues");
        Check(&test, GameplaySceneLoad(&world, "build/core/gameplay-roundtrip.scene", true),
              "written scene reloads through classname registry");
    }
    GameplaySystemsFree(&systems);
    GameplayWorldFree(&world);
    printf("GAMEPLAY TEST failures=%d\n", test.failures);
    return test.failures ? 1 : 0;
}
