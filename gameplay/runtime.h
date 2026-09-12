/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_RUNTIME_H
#define GAMEPLAY_RUNTIME_H
#include "scene.h"
#include "systems.h"

typedef struct GameplayRuntime GameplayRuntime;
typedef struct GameplayProject
{
    EngineConfig config;
    const EntityClass *classes;
    size_t classCount, maxEntities;
    // Room to reserve in every entity's payload for classes that appear later, as a script's do.
    size_t entitySize;
    const char *scene;
    void *context;
    struct UiContext *ui;
    Color clearColor;
    bool (*Init)(GameplayRuntime *runtime); // World/classes ready; runs before scene spawning.
    void (*FrameInput)(GameplayRuntime *runtime, const EngineInput *frame);
    bool (*Update)(GameplayRuntime *runtime, double dt, const EngineInput *input);
    void (*BeforeDraw)(GameplayRuntime *runtime, float alpha); // E.g. BeginMode3D.
    void (*AfterDraw)(GameplayRuntime *runtime, float alpha);  // E.g. EndMode3D.
    void (*BuildUi)(GameplayRuntime *runtime, struct UiContext *ui);
    EngineInputCapture (*CaptureInput)(GameplayRuntime *runtime, const EngineInput *frame);
    void (*Shutdown)(
        GameplayRuntime *runtime); // Entities destroyed; project resources still valid.
} GameplayProject;

struct GameplayRuntime
{
    GameplayWorld world;
    GameplaySystems systems;
    GameplayProject project;
    void *drawContext;
    bool initEntered;
};

GameplayProject GameplayProjectDefault(void);
/* Creates engine callbacks for a caller-owned runtime. Class metadata/context must outlive it.
   This adapter uses fixed gameplay ticks; variable-only projects use EngineRunApplication. */
EngineApplication GameplayApplication(GameplayRuntime *runtime, GameplayProject project);
#endif
