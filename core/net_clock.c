/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* clock_gettime and CLOCK_MONOTONIC are POSIX, and -std=c99 alone hides them. This has to precede
   every include in the translation unit to take effect. */
#define _POSIX_C_SOURCE 199309L

#include "net_clock.h"
#include <math.h>
#include <string.h>
#include <time.h>

double CoreNetClockNow(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

bool CoreNetClockInit(CoreNetClock *clock, int tickRate, int sendRate)
{
    if (!clock)
        return false;
    memset(clock, 0, sizeof *clock);
    if (tickRate <= 0 || sendRate <= 0)
        return false;
    if (sendRate > tickRate)
        sendRate = tickRate;      /* a snapshot of a step not yet simulated carries nothing new */
    clock->tickInterval = 1.0 / (double)tickRate;
    clock->sendInterval = 1.0 / (double)sendRate;
    return true;
}

int CoreNetClockAdvance(CoreNetClock *clock, double dt, int maxTicks)
{
    if (!clock || clock->tickInterval <= 0.0 || dt <= 0.0)
        return 0;
    clock->tickAccumulator += dt;
    clock->sendAccumulator += dt;
    int due = 0;
    while (clock->tickAccumulator >= clock->tickInterval && (maxTicks <= 0 || due < maxTicks))
    {
        clock->tickAccumulator -= clock->tickInterval;
        due++;
    }
    /* A stall longer than the ceiling is dropped rather than banked. Banking it would demand the
       whole backlog on the next frame, which takes longer still -- the spiral of death. */
    if (maxTicks > 0 && clock->tickAccumulator > clock->tickInterval * (double)maxTicks)
        clock->tickAccumulator = 0.0;
    return due;
}

uint32_t CoreNetClockTicked(CoreNetClock *clock)
{
    if (!clock)
        return 0;
    return ++clock->tick;
}

bool CoreNetClockShouldSend(CoreNetClock *clock)
{
    if (!clock || clock->sendInterval <= 0.0 || clock->sendAccumulator < clock->sendInterval)
        return false;
    clock->sendAccumulator = fmod(clock->sendAccumulator, clock->sendInterval);
    return true;
}

bool CoreNetInterpolatorInit(CoreNetInterpolator *interp, int tickRate, int sendRate)
{
    if (!interp)
        return false;
    memset(interp, 0, sizeof *interp);
    if (tickRate <= 0 || sendRate <= 0)
        return false;
    if (sendRate > tickRate)
        sendRate = tickRate;
    interp->tickInterval = 1.0 / (double)tickRate;
    /* The delay, in ticks: however many ticks a snapshot interval spans, times how many snapshots
       of slack to keep. At 60 ticks and 20 snapshots that is 3 ticks per snapshot and 6 ticks of
       delay -- 100ms, which is where Source's default cl_interp lands too. */
    interp->delayTicks = ((double)tickRate / (double)sendRate) * CORE_NET_INTERP_SNAPSHOTS;
    interp->rate = 1.0;
    return true;
}

void CoreNetInterpolatorSnapshot(CoreNetInterpolator *interp, uint32_t tick)
{
    if (!interp || interp->tickInterval <= 0.0)
        return;
    double incoming = (double)tick;
    if (!interp->started)
    {
        interp->started = true;
        interp->latestTick = incoming;
        interp->renderTick = incoming - interp->delayTicks;
        interp->rate = 1.0;
        return;
    }
    /* UDP reorders: a packet that overtook an older one must not drag the clock backwards. */
    if (incoming > interp->latestTick)
        interp->latestTick = incoming;
}

void CoreNetInterpolatorAdvance(CoreNetInterpolator *interp, double dt)
{
    if (!interp || !interp->started || interp->tickInterval <= 0.0 || dt <= 0.0)
        return;
    double target = interp->latestTick - interp->delayTicks;
    double error = target - interp->renderTick;
    /* A long way out is a join, a stall, or the server restarting: jump, because easing across a
       gap that size would take visibly long. */
    if (fabs(error) > interp->delayTicks * 4.0)
    {
        interp->renderTick = target;
        interp->rate = 1.0;
        return;
    }
    /* Otherwise do not move the render point -- play it back slightly faster or slower until it
       lines up. Adding a correction to the position makes the whole world jump by that much;
       changing the rate makes everything continue smoothly and merely arrive on time. This is what
       RobustToolbox does with TickTimingAdjustment, sized from how far its state buffer is from the
       depth it wants. The bound keeps the change too small to read as fast-forward. */
    double want = 1.0 + error * CORE_NET_RETIME_GAIN;
    if (want < 1.0 - CORE_NET_RETIME_LIMIT) want = 1.0 - CORE_NET_RETIME_LIMIT;
    if (want > 1.0 + CORE_NET_RETIME_LIMIT) want = 1.0 + CORE_NET_RETIME_LIMIT;
    interp->rate += (want - interp->rate) * fmin(dt * 4.0, 1.0);
    interp->renderTick += (dt / interp->tickInterval) * interp->rate;
}

double CoreNetInterpolatorRenderTick(const CoreNetInterpolator *interp)
{
    if (!interp || !interp->started)
        return 0.0;
    return interp->renderTick;
}

// ---- as engine types -----------------------------------------------------------------------------
static bool ClockCreate(EngineCall *call)
{
    return CoreNetClockInit(call->data, call->arguments[0].as.integer, call->arguments[1].as.integer);
}
static bool ClockAdvance(EngineCall *call)
{
    call->result = EngineInt(CoreNetClockAdvance(call->data, call->arguments[0].as.number,
                                                 call->arguments[1].as.integer));
    return true;
}
static bool ClockTicked(EngineCall *call)
{
    call->result = EngineInt((int)CoreNetClockTicked(call->data));
    return true;
}
static bool ClockShouldSend(EngineCall *call)
{
    call->result = EngineBool(CoreNetClockShouldSend(call->data));
    return true;
}
static bool ClockTick(const void *object, EngineValue *out)
{
    *out = EngineInt((int)((const CoreNetClock *)object)->tick);
    return true;
}
static const EngineProperty clockProperties[] = {
    ENGINE_COMPUTED("tick", ENGINE_INT, ENGINE_PROPERTY_READ_ONLY, ClockTick, NULL,
                    "the number of the last simulated step"),
};
static const EngineMethod clockMethods[] = {
    {"advance!", ENGINE_INT, {ENGINE_FLOAT, ENGINE_INT}, 2, ClockAdvance,
     "add elapsed seconds; answers how many fixed steps are due, at most the ceiling given"},
    {"ticked!", ENGINE_INT, {ENGINE_NONE}, 0, ClockTicked, "count one simulated step; answers its tick"},
    {"should-send?", ENGINE_BOOL, {ENGINE_NONE}, 0, ClockShouldSend,
     "whether a snapshot is due; true at most once per send interval"},
};
const EngineType CoreNetClockType = {
    .name = "net-clock",
    .size = sizeof(CoreNetClock),
    .properties = clockProperties,
    .propertyCount = 1,
    .methods = clockMethods,
    .methodCount = sizeof clockMethods / sizeof clockMethods[0],
    .createArguments = {ENGINE_INT, ENGINE_INT},
    .createArgumentCount = 2,
    .createRequired = 2,
    .create = ClockCreate,
    .help = "a server's fixed simulation tick and its lower snapshot rate",
};

static bool InterpolatorCreate(EngineCall *call)
{
    return CoreNetInterpolatorInit(call->data, call->arguments[0].as.integer,
                                   call->arguments[1].as.integer);
}
static bool InterpolatorSnapshot(EngineCall *call)
{
    if (call->arguments[0].as.integer < 0)
    {
        call->error = "a tick cannot be negative";
        return false;
    }
    CoreNetInterpolatorSnapshot(call->data, (uint32_t)call->arguments[0].as.integer);
    return true;
}
static bool InterpolatorAdvance(EngineCall *call)
{
    CoreNetInterpolatorAdvance(call->data, call->arguments[0].as.number);
    return true;
}
static bool InterpolatorRenderTick(const void *object, EngineValue *out)
{
    *out = EngineFloat((float)CoreNetInterpolatorRenderTick(object));
    return true;
}
static const EngineProperty interpolatorProperties[] = {
    ENGINE_COMPUTED("render-tick", ENGINE_FLOAT, ENGINE_PROPERTY_READ_ONLY, InterpolatorRenderTick,
                    NULL, "the server tick to sample replicated objects at, fractional"),
};
static const EngineMethod interpolatorMethods[] = {
    {"snapshot!", ENGINE_NONE, {ENGINE_INT}, 1, InterpolatorSnapshot,
     "record the server tick of a snapshot as it arrives"},
    {"advance!", ENGINE_NONE, {ENGINE_FLOAT}, 1, InterpolatorAdvance,
     "move the drawn moment forward by elapsed seconds"},
};
const EngineType CoreNetInterpolatorType = {
    .name = "net-interpolator",
    .size = sizeof(CoreNetInterpolator),
    .properties = interpolatorProperties,
    .propertyCount = 1,
    .methods = interpolatorMethods,
    .methodCount = sizeof interpolatorMethods / sizeof interpolatorMethods[0],
    .createArguments = {ENGINE_INT, ENGINE_INT},
    .createArgumentCount = 2,
    .createRequired = 2,
    .create = InterpolatorCreate,
    .help = "a client's clock, drawing the world far enough in the past to interpolate",
};
