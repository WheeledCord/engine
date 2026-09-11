/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_ENGINE_H
#define CORE_ENGINE_H
#include "capabilities.h"
#include "raylib.h"
#include <stdbool.h>
#include "input.h"
typedef struct EngineConfig
{
    const char *title;
    int width, height, targetFps;
    double fixed_dt;     /* zero: variable-only; positive: accumulator */
    double max_frame_dt; /* zero: no clamp; positive: explicit catch-up time clamp */
    unsigned int windowFlags;
    CoreRequirements requirements;
    const char *engine_path; /* path to engine root containing core/; NULL: use executable directory */
} EngineConfig;
typedef struct EngineProject
{
    bool (*Init)(void *context);
    void (*FrameInput)(void *context, const EngineInput *frame);        /* once per rendered frame */
    bool (*Update)(void *context, double dt, const EngineInput *input); /* false requests exit */
    void (*Draw)(void *context, float alpha);                           /* inside BeginDrawing/EndDrawing */
    void (*Shutdown)(void *context); /* also called after partially failed Init */
} EngineProject;
struct UiContext;
typedef struct EngineApplication
{
    EngineConfig config;
    EngineProject callbacks;
    void *context;
    Color clearColor;
    struct UiContext *ui; // Optional; a zero-initialised context gets the default theme.
    void (*BuildUi)(void *context, struct UiContext *ui); // Once before updates; drawing is deferred.
    EngineInputCapture (*CaptureInput)(void *context, const EngineInput *frame); // Extra project routing.
} EngineApplication;

EngineConfig EngineConfigDefault(void);
EngineApplication EngineApplicationDefault(void);
int EngineRunApplication(const EngineApplication *application);
/* Define this only when linking the optional core/entry.c instead of your own main.
   Context, UI and declaration arrays must outlive the returned descriptor. */
EngineApplication EngineApplicationMain(int argc, char **argv);
int EngineRun(const EngineConfig *config, const EngineProject *project, void *context);
#endif
