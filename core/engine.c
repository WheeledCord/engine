/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#define _DEFAULT_SOURCE /* clock_gettime under -std=c99 */
#include "engine.h"
#include "file.h"
#include "ui.h"
#include "diagnostics_internal.h"
#include <math.h>
#include <string.h>
#include <time.h>
static EngineInput PollInput(void)
{
    EngineInput f = {0};
    for (int i = 0; i < CORE_KEY_COUNT; i++)
    {
        f.down[i] = IsKeyDown(i);
        f.pressed[i] = IsKeyPressed(i);
        f.released[i] = IsKeyReleased(i);
    }
    for (int i = 0; i < CORE_MOUSE_BUTTON_COUNT; i++)
    {
        f.mouseDown[i] = IsMouseButtonDown(i);
        f.mousePressed[i] = IsMouseButtonPressed(i);
        f.mouseReleased[i] = IsMouseButtonReleased(i);
    }
    f.mousePosition = GetMousePosition();
    f.mouseDelta = GetMouseDelta();
    f.wheel = GetMouseWheelMove();
    int codepoint = 0;
    while (f.textCount < CORE_TEXT_INPUT_COUNT && (codepoint = GetCharPressed()) != 0)
        f.text[f.textCount++] = codepoint;
    return f;
}
/* Microseconds on the monotonic clock. Unlike raylib's GetTime it needs no window. */
static double NowMicros(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* One Update call, timed. ticks counts the call as it starts, so the callback sees itself counted;
   its duration joins the tick* fields once it returns. */
static bool TimedUpdate(const EngineProject *p, void *context, double dt, const EngineInput *input,
                        CoreDiagnostics *d)
{
    d->ticks++;
    double start = NowMicros();
    bool keepGoing = !p->Update || p->Update(context, dt, input);
    double took = NowMicros() - start;
    d->tickMicrosLast = took;
    d->tickMicrosTotal += took;
    if (took > d->tickMicrosMax) d->tickMicrosMax = took;
    return keepGoing;
}

static bool TickLimitReached(const EngineConfig *c, const CoreDiagnostics *d)
{
    return c->maxTicks > 0 && d->ticks >= c->maxTicks;
}

/* The headless loop: fixed steps back to back, an empty input, no window, no drawing, no sleeping. */
static void RunHeadless(const EngineConfig *c, const EngineProject *p, void *context, CoreDiagnostics *d)
{
    const EngineInput none = {0};
    bool running = true;
    while (running)
    {
        d->frames++;
        d->updates = 0;
        d->frameDt = c->fixed_dt;
        running = TimedUpdate(p, context, c->fixed_dt, &none, d);
        d->updates++;
        if (TickLimitReached(c, d)) running = false;
    }
}

EngineConfig EngineConfigDefault(void)
{
    return (EngineConfig){.title = "Core", .width = 960, .height = 540, .targetFps = 60,
                          .fixed_dt = 1.0 / 60.0, .max_frame_dt = 0.25};
}

EngineApplication EngineApplicationDefault(void)
{
    return (EngineApplication){.config = EngineConfigDefault(), .clearColor = {44, 47, 50, 255}};
}

int EngineRun(const EngineConfig *config, const EngineProject *project, void *context)
{
    EngineApplication application = EngineApplicationDefault();
    if (config) application.config = *config;
    if (project) application.callbacks = *project;
    application.context = context;
    return EngineRunApplication(&application);
}

static const EngineConfig *runningConfig;

const EngineConfig *EngineRunningConfig(void) { return runningConfig; }

int EngineRunApplication(const EngineApplication *application)
{
    if (!application) { TraceLog(LOG_ERROR, "Engine: missing application descriptor"); return 1; }
    const EngineConfig *c = &application->config;
    const EngineProject *p = &application->callbacks;
    void *context = application->context;
    if (c->headless)
    {
        if (!isfinite(c->fixed_dt) || c->fixed_dt <= 0)
        {
            TraceLog(LOG_ERROR, "Engine: a headless run needs a positive fixed_dt");
            return 1;
        }
        if (application->BuildUi)
        {
            TraceLog(LOG_ERROR, "Engine: a headless run cannot build a UI; leave BuildUi unset");
            return 1;
        }
    }
    else if (c->width <= 0 || c->height <= 0 || c->targetFps < 0 ||
        !isfinite(c->fixed_dt) || c->fixed_dt < 0 || !isfinite(c->max_frame_dt) || c->max_frame_dt < 0)
    {
        TraceLog(LOG_ERROR, "Engine: dimensions must be positive; FPS and finite timesteps must be nonnegative");
        return 1;
    }
    if (application->BuildUi && !application->ui)
    {
        TraceLog(LOG_ERROR, "Engine: BuildUi needs a UiContext");
        return 1;
    }
    runningConfig = c;
    if (!c->headless)
    {
        SetConfigFlags(c->windowFlags);
        InitWindow(c->width, c->height, c->title ? c->title : "Core");
        if (!IsWindowReady()) { runningConfig = NULL; return 1; }
    }
    CoreSetDataRoot(c->engine_path ? c->engine_path : GetApplicationDirectory());
    if (!c->headless)
    {
        SetExitKey(KEY_NULL);
        SetTargetFPS(c->targetFps);
        if (!CoreCheckCapabilities(c->requirements)) { CloseWindow(); runningConfig = NULL; return 1; }
    }
    bool initialised = !p->Init || p->Init(context);
    if (!initialised)
        TraceLog(LOG_ERROR, "Engine: the project's Init returned false; shutting down");
    bool ownsUi = false;
    if (initialised && application->BuildUi && !application->ui->state)
    {
        initialised = UiInit(application->ui, UiThemeDefault());
        if (!initialised)
            TraceLog(LOG_ERROR, "Engine: the UI could not be initialised; shutting down");
        ownsUi = initialised;
    }
    int result = initialised ? 0 : 1;
    CoreDiagnostics diagnostics = {0};
    CoreDiagnosticsSetCurrent(&diagnostics);
    if (c->headless && initialised)
        RunHeadless(c, p, context, &diagnostics);
    double last = c->headless ? 0 : GetTime(), accumulator = 0;
    EngineInput pending = {0};
    bool running = initialised && !c->headless;
    while (running && !WindowShouldClose())
    {
        double frameStart = NowMicros();
        double now = GetTime(), dt = now - last;
        diagnostics.frameDt = dt;
        diagnostics.updates = 0;
        diagnostics.frames++;
        last = now;
        if (c->max_frame_dt > 0 && dt > c->max_frame_dt)
        {
            TraceLog(LOG_WARNING, "Frame time clamped from %.3f to %.3f seconds", dt, c->max_frame_dt);
            dt = c->max_frame_dt;
        }
        EngineInput frame = PollInput();
        if (p->FrameInput) p->FrameInput(context, &frame);
        EngineInputCapture capture = {0};
        if (application->BuildUi)
        {
            UiBeginDeferredFrame(application->ui, &frame,
                                 (UiRect){0, 0, GetScreenWidth(), GetScreenHeight()});
            application->BuildUi(context, application->ui);
            UiEndFrame(application->ui);
            capture = UiCapture(application->ui);
        }
        if (application->CaptureInput)
        {
            EngineInputCapture extra = application->CaptureInput(context, &frame);
            capture.keyboard |= extra.keyboard;
            capture.mouse |= extra.mouse;
        }
        EngineInputRoute(&pending, &frame, capture);
        float alpha = 1;
        if (c->fixed_dt > 0)
        {
            accumulator += dt;
            while (accumulator >= c->fixed_dt && running)
            {
                running = TimedUpdate(p, context, c->fixed_dt, &pending, &diagnostics);
                diagnostics.updates++;
                EngineInputDrain(&pending);
                accumulator -= c->fixed_dt;
                if (TickLimitReached(c, &diagnostics)) running = false;
            }
            alpha = (float)(accumulator / c->fixed_dt);
        }
        else
        {
            running = TimedUpdate(p, context, dt, &pending, &diagnostics);
            EngineInputDrain(&pending);
            if (TickLimitReached(c, &diagnostics)) running = false;
        }
        diagnostics.fixedBacklog = accumulator;
        if (running)
        {
            BeginDrawing();
            ClearBackground(application->clearColor);
            if (p->Draw) p->Draw(context, alpha);
            if (application->BuildUi && !UiRender(application->ui)) { result = 1; running = false; }
            EndDrawing();
        }
        double frameTook = NowMicros() - frameStart;
        diagnostics.frameMicrosLast = frameTook;
        diagnostics.frameMicrosTotal += frameTook;
        if (frameTook > diagnostics.frameMicrosMax) diagnostics.frameMicrosMax = frameTook;
    }
    if (p->Shutdown) p->Shutdown(context);
    if (ownsUi) UiFree(application->ui);
    if (!c->headless) CloseWindow();
    runningConfig = NULL;
    CoreDiagnosticsSetCurrent(NULL);
    return result;
}
