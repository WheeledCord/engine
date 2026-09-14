/* This Source Code Form is subject to the terms of the Mozilla Public License, v2.0. */
#include "debug.h"
#include <stdio.h>
void GameplayDebugEntityCounts(const GameplayWorld *world, CoreDebug *debug, Vector2 position, Color color)
{
    if (!world || !debug) return;
    char line[128];
    snprintf(line, sizeof line, "entities %zu/%zu", (size_t)0, world->maxEntities);
    size_t all = 0;
    for (size_t type = 0; type < world->classCount; type++)
    {
        size_t count = 0;
        for (EntityHandle at = EntityFirst(world, world->classes[type].classname); EntityAlive(world, at);
             at = EntityNext(world, at, world->classes[type].classname)) count++;
        all += count;
        snprintf(line, sizeof line, "%s: %zu", world->classes[type].classname, count);
        CoreDebugText(debug, position, line, color, 0);
        position.y += 12;
    }
    snprintf(line, sizeof line, "entities: %zu/%zu", all, world->maxEntities);
    CoreDebugText(debug, (Vector2){position.x, position.y}, line, color, 0);
}
