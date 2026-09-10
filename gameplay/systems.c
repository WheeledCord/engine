#include "systems.h"

#include <stdlib.h>

bool GameplaySystemsAdd(GameplaySystems *systems, GameplaySystem system)
{
    if (!systems || !system.name || !system.name[0])
        return false;
    if (systems->count == systems->capacity)
    {
        size_t capacity = systems->capacity ? systems->capacity * 2 : 8;
        GameplaySystem *items = realloc(systems->items, capacity * sizeof(*items));
        if (!items)
            return false;
        systems->items = items;
        systems->capacity = capacity;
    }
    systems->items[systems->count++] = system;
    return true;
}

void GameplaySystemsUpdate(GameplaySystems *systems, GameplayWorld *world, double dt)
{
    for (size_t i = 0; systems && i < systems->count; i++)
        if (systems->items[i].Update)
            systems->items[i].Update(systems->items[i].context, world, dt);
}

void GameplaySystemsDraw(GameplaySystems *systems, GameplayWorld *world, void *drawContext)
{
    for (size_t i = 0; systems && i < systems->count; i++)
        if (systems->items[i].Draw)
            systems->items[i].Draw(systems->items[i].context, world, drawContext);
}

void GameplaySystemsReset(GameplaySystems *systems, GameplayWorld *world)
{
    for (size_t i = 0; systems && i < systems->count; i++)
        if (systems->items[i].Reset)
            systems->items[i].Reset(systems->items[i].context, world);
}

void GameplaySystemsFree(GameplaySystems *systems)
{
    if (!systems)
        return;
    free(systems->items);
    *systems = (GameplaySystems){0};
}
