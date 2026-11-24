/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_ISO_MAP_SPAWN_H
#define GAMEPLAY_ISO_MAP_SPAWN_H
#include "core/iso_map.h"
#include "entity.h"

/* Spawns the entities a map's object layer places, the way a scene file spawns its entities.

   Each object becomes one entity of its kind's class, given "hex" as "x y", then the kind's fields,
   then the object's own, so an object overrides its kind. A class that takes a "position" -- every
   scripted entity does -- also gets the hex's world point there, so it starts standing where it
   was put. Fields are key=value words; a value may be quoted to hold spaces.

   Returns how many spawned. An object whose class is missing or refuses its fields is logged and
   skipped, not fatal: a map is edited more often than the game that plays it. */
int IsoMapSpawnObjects(GameplayWorld *world, const IsoMap *map, const IsoTileset *tileset);
#endif
