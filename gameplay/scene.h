#ifndef GAMEPLAY_SCENE_H
#define GAMEPLAY_SCENE_H

#include "entity.h"

/* Reads/writes: entity "classname" { "key" "value" ... }. */
bool GameplaySceneLoad(GameplayWorld *world, const char *path, bool replaceWorld);
bool GameplaySceneWrite(const GameplayWorld *world, const char *path);

#endif
