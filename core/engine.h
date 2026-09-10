/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_ENGINE_H
#define CORE_ENGINE_H
#include "capabilities.h"
#include "raylib.h"
#include <stdbool.h>
#define CORE_KEY_COUNT 512
#define CORE_MOUSE_BUTTON_COUNT 8
#define CORE_TEXT_INPUT_COUNT 32
typedef struct EngineInput
{
    bool down[CORE_KEY_COUNT], pressed[CORE_KEY_COUNT], released[CORE_KEY_COUNT];
    bool mouseDown[CORE_MOUSE_BUTTON_COUNT], mousePressed[CORE_MOUSE_BUTTON_COUNT],
        mouseReleased[CORE_MOUSE_BUTTON_COUNT];
    Vector2 mousePosition;
    Vector2 mouseDelta;
    float wheel;
    int text[CORE_TEXT_INPUT_COUNT];
    int textCount;
} EngineInput;
/* Merge one frame into pending input. Edges and deltas survive frames without Update. */
void EngineInputAccumulate(EngineInput *pending, const EngineInput *frame);
/* After the first Update: clear edges/deltas, preserve held state for further updates. */
void EngineInputDrain(EngineInput *pending);
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
int EngineRun(const EngineConfig *config, const EngineProject *project, void *context);
#endif
