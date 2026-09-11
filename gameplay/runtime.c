/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "runtime.h"

static bool Init(void *context)
{
    GameplayRuntime *r = context;
    const GameplayProject *p = &r->project;
    if (p->config.fixed_dt <= 0 || (p->classCount && !p->classes))
    {
        TraceLog(LOG_ERROR, "Gameplay: supply a class table and a positive fixed_dt");
        return false;
    }
    size_t size = 1;
    for (size_t i = 0; i < p->classCount; i++)
        if (p->classes[i].size > size)
            size = p->classes[i].size;
    if (!GameplayWorldInit(&r->world,
                           (GameplayWorldConfig){p->maxEntities, size, p->config.fixed_dt}))
    {
        TraceLog(LOG_ERROR, "Gameplay: invalid world capacity or allocation failure");
        return false;
    }
    r->world.context = p->context;
    if (!EntityRegisterAll(&r->world, p->classes, p->classCount))
    {
        TraceLog(LOG_ERROR, "Gameplay: invalid or duplicate entity class/field declaration");
        return false;
    }
    r->initEntered = true;
    if (p->Init && !p->Init(r))
        return false;
    return !p->scene || GameplaySceneLoad(&r->world, p->scene, false);
}

static void FrameInput(void *context, const EngineInput *input)
{
    GameplayRuntime *r = context;
    if (r->project.FrameInput)
        r->project.FrameInput(r, input);
}

static bool Update(void *context, double dt, const EngineInput *input)
{
    GameplayRuntime *r = context;
    if (r->project.Update && !r->project.Update(r, dt, input))
        return false;
    GameplaySystemsUpdate(&r->systems, &r->world, dt);
    GameplayWorldStepInput(&r->world, input);
    return true;
}

static void Draw(void *context, float alpha)
{
    GameplayRuntime *r = context;
    if (r->project.BeforeDraw)
        r->project.BeforeDraw(r, alpha);
    GameplayWorldDrawInterpolated(&r->world, alpha, r->drawContext);
    GameplaySystemsDraw(&r->systems, &r->world, r->drawContext);
    if (r->project.AfterDraw)
        r->project.AfterDraw(r, alpha);
}

static void BuildUi(void *context, struct UiContext *ui)
{
    GameplayRuntime *r = context;
    r->project.BuildUi(r, ui);
}

static EngineInputCapture CaptureInput(void *context, const EngineInput *frame)
{
    GameplayRuntime *r = context;
    return r->project.CaptureInput(r, frame);
}

static void Shutdown(void *context)
{
    GameplayRuntime *r = context;
    GameplayWorldClear(&r->world);
    GameplaySystemsReset(&r->systems, &r->world);
    if (r->initEntered && r->project.Shutdown)
        r->project.Shutdown(r);
    GameplaySystemsFree(&r->systems);
    GameplayWorldFree(&r->world);
}

GameplayProject GameplayProjectDefault(void)
{
    return (GameplayProject){
        .config = EngineConfigDefault(), .maxEntities = 1024, .clearColor = {44, 47, 50, 255}};
}

EngineApplication GameplayApplication(GameplayRuntime *runtime, GameplayProject project)
{
    EngineApplication app = EngineApplicationDefault();
    if (!runtime)
    {
        app.config.width = 0;
        return app;
    }
    *runtime = (GameplayRuntime){.project = project};
    app.config = project.config;
    app.clearColor = project.clearColor;
    app.callbacks = (EngineProject){Init, FrameInput, Update, Draw, Shutdown};
    app.context = runtime;
    app.ui = project.ui;
    app.BuildUi = project.BuildUi ? BuildUi : NULL;
    app.CaptureInput = project.CaptureInput ? CaptureInput : NULL;
    return app;
}
