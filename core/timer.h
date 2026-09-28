/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_TIMER_H
#define CORE_TIMER_H

#include "object.h"
#include <stdbool.h>

/* Something that happens after a while, as an event rather than a countdown a game keeps itself.
   A "timer" engine type counts down as its pool steps and emits "timeout" when it runs out, then
   starts again unless it is one-shot -- the same order Godot's Timer uses (scene/main/timer.cpp).
   Created with an optional wait in seconds and an optional one-shot flag; it starts running. */
typedef struct CoreTimer
{
    float wait;     /* seconds from start to timeout */
    float left;     /* seconds still to go */
    bool oneShot;   /* stop after one timeout instead of starting again */
    bool running;
} CoreTimer;

extern const EngineType CoreTimerType;

#endif
