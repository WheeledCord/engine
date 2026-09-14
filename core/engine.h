/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_ENGINE_H
#define CORE_ENGINE_H
#include "capabilities.h"
#include "raylib.h"
#include <stdbool.h>
#include "input.h"
#include "diagnostics.h"
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

/**
 * @brief Returns the default configuration for a fixed-step application.
 *
 * The returned value is owned by the caller and may be changed before starting an application.
 * @return A 960 by 540, 60 FPS configuration with a 1/60 second fixed step.
 */
EngineConfig EngineConfigDefault(void);
/**
 * @brief Returns an application descriptor with core defaults.
 *
 * The returned value does not create a window or allocate persistent resources.
 * @return An EngineApplication with default configuration, clear colour, and no callbacks.
 */
EngineApplication EngineApplicationDefault(void);
/**
 * @brief Runs an application's window and callback lifecycle.
 *
 * The application and every object it borrows must remain valid until this call returns.
 * @param application Borrowed application descriptor; NULL is rejected.
 * @return Zero after normal exit, or nonzero after configuration, initialization, or render failure.
 */
int EngineRunApplication(const EngineApplication *application);
/**
 * @brief Returns the configuration installed for the current application lifecycle.
 *
 * Use this only from an application callback and do not retain the returned pointer.
 * @return Borrowed running configuration, or NULL when no configuration is installed.
 */
const EngineConfig *EngineRunningConfig(void);
/**
 * @brief Supplies a project descriptor when linking core/entry.c.
 *
 * Define this only instead of a custom main. Context, UI, and declaration arrays must outlive the run.
 * @param argc Process argument count supplied by the entry point.
 * @param argv Process argument vector supplied by the entry point.
 * @return A descriptor borrowed by the entry point for the application's full run.
 */
EngineApplication EngineApplicationMain(int argc, char **argv);
/**
 * @brief Runs the lower-level core loop with explicit configuration and callbacks.
 *
 * This is a convenience wrapper around EngineRunApplication; all arguments are borrowed for the run.
 * @param config Borrowed configuration, or NULL for EngineConfigDefault.
 * @param project Borrowed callbacks, or NULL for no callbacks.
 * @param context Borrowed project context passed to callbacks.
 * @return Zero after normal exit, or nonzero after startup or render failure.
 */
int EngineRun(const EngineConfig *config, const EngineProject *project, void *context);
#endif
