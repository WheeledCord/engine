/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "timer.h"

static bool TimerCreate(EngineCall *call)
{
    CoreTimer *timer = call->data;
    timer->wait = call->count > 0 ? call->arguments[0].as.number : 1.0f;
    timer->oneShot = call->count > 1 && call->arguments[1].as.boolean;
    if (!(timer->wait > 0.0f))
    {
        call->error = "a timer's wait must be more than zero";
        return false;
    }
    timer->left = timer->wait;
    timer->running = true;
    return true;
}

static void TimerStep(EngineObjects *objects, EngineObjectId self, void *data, float dt)
{
    CoreTimer *timer = data;
    if (!timer->running)
        return;
    timer->left -= dt;
    if (timer->left >= 0.0f)
        return;
    // Rescheduled before anyone hears of it, so a handler that restarts or stops it wins.
    if (timer->oneShot)
        timer->running = false;
    else
        timer->left += timer->wait;
    EngineObjectEmit(objects, self, "timeout", NULL, 0);
}

static bool TimerStart(EngineCall *call)
{
    CoreTimer *timer = call->data;
    timer->left = timer->wait;
    timer->running = true;
    return true;
}

static bool TimerStop(EngineCall *call)
{
    ((CoreTimer *)call->data)->running = false;
    return true;
}

static bool TimerSetWait(void *object, const EngineValue *value)
{
    if (!(value->as.number > 0.0f))
        return false;
    ((CoreTimer *)object)->wait = value->as.number;
    return true;
}

static bool TimerGetWait(const void *object, EngineValue *out)
{
    *out = EngineFloat(((const CoreTimer *)object)->wait);
    return true;
}

static const EngineProperty timerProperties[] = {
    ENGINE_COMPUTED("wait", ENGINE_FLOAT, ENGINE_PROPERTY_SAVE, TimerGetWait, TimerSetWait,
                    "seconds from start to timeout; more than zero"),
    ENGINE_FIELD("one-shot", CoreTimer, oneShot, ENGINE_BOOL, ENGINE_PROPERTY_SAVE,
                 "stop after one timeout instead of starting again"),
    ENGINE_FIELD("time-left", CoreTimer, left, ENGINE_FLOAT, ENGINE_PROPERTY_READ_ONLY,
                 "seconds until the next timeout"),
    ENGINE_FIELD("running", CoreTimer, running, ENGINE_BOOL, ENGINE_PROPERTY_READ_ONLY,
                 "counting down"),
};
static const EngineMethod timerMethods[] = {
    {"start!", ENGINE_NONE, {ENGINE_NONE}, 0, TimerStart, "count down from the full wait"},
    {"stop!", ENGINE_NONE, {ENGINE_NONE}, 0, TimerStop, "stop counting"},
};
static const char *const timerSignals[] = {"timeout"};

const EngineType CoreTimerType = {
    .name = "timer",
    .size = sizeof(CoreTimer),
    .properties = timerProperties,
    .propertyCount = sizeof timerProperties / sizeof timerProperties[0],
    .methods = timerMethods,
    .methodCount = sizeof timerMethods / sizeof timerMethods[0],
    .signals = timerSignals,
    .signalCount = 1,
    .createArguments = {ENGINE_FLOAT, ENGINE_BOOL},
    .createArgumentCount = 2,
    .createRequired = 0,
    .create = TimerCreate,
    .step = TimerStep,
    .help = "counts down and emits timeout, then starts again unless one-shot",
};
