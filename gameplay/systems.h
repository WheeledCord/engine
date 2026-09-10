#ifndef GAMEPLAY_SYSTEMS_H
#define GAMEPLAY_SYSTEMS_H

#include "entity.h"

typedef struct GameplaySystem
{
    const char *name;
    void *context;
    void (*Update)(void *context, GameplayWorld *world, double dt);
    void (*Draw)(void *context, GameplayWorld *world, void *drawContext);
    void (*Reset)(void *context, GameplayWorld *world);
} GameplaySystem;

typedef struct GameplaySystems
{
    GameplaySystem *items;
    size_t count;
    size_t capacity;
} GameplaySystems;

bool GameplaySystemsAdd(GameplaySystems *systems, GameplaySystem system);
void GameplaySystemsUpdate(GameplaySystems *systems, GameplayWorld *world, double dt);
void GameplaySystemsDraw(GameplaySystems *systems, GameplayWorld *world, void *drawContext);
void GameplaySystemsReset(GameplaySystems *systems, GameplayWorld *world);
void GameplaySystemsFree(GameplaySystems *systems);

#endif
