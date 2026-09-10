/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCENE_H
#define GAMEPLAY_SCENE_H

#include "entity.h"

/* Reads/writes: entity "classname" { "key" "value" ... }. */
bool GameplaySceneLoad(GameplayWorld *world, const char *path, bool replaceWorld);
bool GameplaySceneWrite(const GameplayWorld *world, const char *path);

#endif
