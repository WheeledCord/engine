/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. */
#ifndef GAMEPLAY_DEBUG_H
#define GAMEPLAY_DEBUG_H
#include "core/diagnostics.h"
#include "entity.h"
/** Queues one line per registered class with its current living-entity count. Call before CoreDebugDraw. */
void GameplayDebugEntityCounts(const GameplayWorld *world, CoreDebug *debug, Vector2 position, Color color);
#endif
