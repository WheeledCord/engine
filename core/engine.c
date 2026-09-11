/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "engine.h"
#include "file.h"
#include "ui.h"
#include <math.h>
#include <string.h>
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

int EngineRunApplication(const EngineApplication *application)
{
    if (!application) { TraceLog(LOG_ERROR, "Engine: missing application descriptor"); return 1; }
    const EngineConfig *c = &application->config;
    const EngineProject *p = &application->callbacks;
    void *context = application->context;
    if (c->width <= 0 || c->height <= 0 || c->targetFps < 0 ||
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
    SetConfigFlags(c->windowFlags);
    InitWindow(c->width, c->height, c->title ? c->title : "Core");
    if (!IsWindowReady()) return 1;
    CoreSetDataRoot(c->engine_path ? c->engine_path : GetApplicationDirectory());
    SetExitKey(KEY_NULL);
    SetTargetFPS(c->targetFps);
    if (!CoreCheckCapabilities(c->requirements)) { CloseWindow(); return 1; }
    bool initialised = !p->Init || p->Init(context);
    bool ownsUi = false;
    if (initialised && application->BuildUi && !application->ui->state)
    {
        initialised = UiInit(application->ui, UiThemeDefault());
        ownsUi = initialised;
    }
    int result = initialised ? 0 : 1;
    double last = GetTime(), accumulator = 0;
    EngineInput pending = {0};
    bool running = initialised;
    while (running && !WindowShouldClose())
    {
        double now = GetTime(), dt = now - last;
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
                running = !p->Update || p->Update(context, c->fixed_dt, &pending);
                EngineInputDrain(&pending);
                accumulator -= c->fixed_dt;
            }
            alpha = (float)(accumulator / c->fixed_dt);
        }
        else
        {
            running = !p->Update || p->Update(context, dt, &pending);
            EngineInputDrain(&pending);
        }
        if (running)
        {
            BeginDrawing();
            ClearBackground(application->clearColor);
            if (p->Draw) p->Draw(context, alpha);
            if (application->BuildUi && !UiRender(application->ui)) { result = 1; running = false; }
            EndDrawing();
        }
    }
    if (p->Shutdown) p->Shutdown(context);
    if (ownsUi) UiFree(application->ui);
    CloseWindow();
    return result;
}
