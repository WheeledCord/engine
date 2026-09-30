/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The headless run of EngineRunApplication (no window, no GL) and the tick timing it fills in.
// Called from main() after the window checks; the headless runs here create no window of their own.
#include "checks.h"
#include "core/diagnostics.h"
#include "core/engine.h"
#include "core/ui.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void Check(bool ok, const char *what)
{
    if (!ok)
        printf("FAIL: %s\n", what);
    failures += !ok;
}

typedef struct HeadlessRun
{
    unsigned calls, stopAt;
    uint64_t firstTicks, lastTicks;
    double lastTotal;
    bool ticksRose, inputEmpty, dtRight, configVisible;
} HeadlessRun;

static bool HeadlessUpdate(void *context, double dt, const EngineInput *input)
{
    HeadlessRun *run = context;
    const CoreDiagnostics *d = CoreDiagnosticsCurrent();
    run->calls++;
    if (d)
    {
        if (run->calls == 1)
            run->firstTicks = d->ticks;
        else if (d->ticks <= run->lastTicks)
            run->ticksRose = false;
        run->lastTicks = d->ticks;
        run->lastTotal = d->tickMicrosTotal;
    }
    else
        run->ticksRose = false;
    if (dt != 1.0 / 60.0)
        run->dtRight = false;
    if (input->textCount != 0 || input->down[0] || input->mouseDown[0])
        run->inputEmpty = false;
    if (EngineRunningConfig() == NULL || !EngineRunningConfig()->headless)
        run->configVisible = false;
    return !run->stopAt || run->calls < run->stopAt;
}

static void HeadlessBuildUi(void *context, struct UiContext *ui)
{
    (void)context;
    (void)ui;
}

static HeadlessRun RunHeadless(uint64_t maxTicks, unsigned stopAt, int *result)
{
    HeadlessRun run = {.stopAt = stopAt, .ticksRose = true, .inputEmpty = true, .dtRight = true,
                       .configVisible = true};
    EngineApplication app = EngineApplicationDefault();
    app.config.headless = true;
    app.config.fixed_dt = 1.0 / 60.0;
    app.config.maxTicks = maxTicks;
    app.callbacks.Update = HeadlessUpdate;
    app.context = &run;
    *result = EngineRunApplication(&app);
    return run;
}

static void EngineHeadlessChecks(void)
{
    int result = -1;
    HeadlessRun run = RunHeadless(100, 0, &result);
    Check(result == 0, "a headless run stopped by maxTicks returns zero");
    Check(run.calls == 100, "a headless run with maxTicks 100 calls Update exactly 100 times");
    Check(run.firstTicks == 1 && run.lastTicks == 100 && run.ticksRose,
          "diagnostics ticks rise by one for each Update call during a headless run");
    Check(run.lastTotal > 0, "diagnostics tickMicrosTotal is above zero after a headless run");
    Check(run.dtRight && run.inputEmpty, "headless Update gets fixed_dt and an empty input");
    Check(run.configVisible, "EngineRunningConfig works during a headless run");
    Check(CoreDiagnosticsCurrent() == NULL && EngineRunningConfig() == NULL,
          "a finished headless run leaves no diagnostics or running configuration installed");

    run = RunHeadless(100, 10, &result);
    Check(result == 0 && run.calls == 10, "Update returning false at call 10 stops a headless run early");

    run = RunHeadless(0, 25, &result);
    Check(result == 0 && run.calls == 25, "a headless run with maxTicks 0 runs until Update says stop");

    run = RunHeadless(5, 0, &result);
    Check(result == 0 && run.calls == 5, "a headless run with maxTicks 5 stops after 5 Update calls");

    EngineApplication app = EngineApplicationDefault();
    HeadlessRun untouched = {0};
    app.config.headless = true;
    app.config.fixed_dt = 0;
    app.config.maxTicks = 3;
    app.callbacks.Update = HeadlessUpdate;
    app.context = &untouched;
    Check(EngineRunApplication(&app) != 0 && untouched.calls == 0,
          "headless with fixed_dt zero is refused without calling Update");
    app.config.fixed_dt = -1.0 / 60.0;
    Check(EngineRunApplication(&app) != 0 && untouched.calls == 0,
          "headless with a negative fixed_dt is refused");

    static UiContext ui;
    app.config.fixed_dt = 1.0 / 60.0;
    app.ui = &ui;
    app.BuildUi = HeadlessBuildUi;
    untouched.calls = 0;
    Check(EngineRunApplication(&app) != 0 && untouched.calls == 0,
          "headless with BuildUi set is refused without calling Update");
}

int HeadlessChecks(void)
{
    failures = 0;
    EngineHeadlessChecks();
    return failures;
}
