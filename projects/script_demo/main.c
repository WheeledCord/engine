/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The authoring demo again, except the entity lives in a script. There is no C here that knows what
// a mover is: the project loads a script, and the script declares the class and its callbacks.
#include "gameplay/script/script_s7.h"
#include <stdio.h>
#include <string.h>

static ScriptHost host;

static bool Init(GameplayRuntime *runtime)
{
    if (!ScriptHostInit(&host, &runtime->world) || !ScriptS7Open(&host))
        return false;
    // Classes have to exist before the scene that places them is read.
    return ScriptS7Load(&host, "projects/script_demo/mover.scm");
}

static bool Update(GameplayRuntime *runtime, double dt, const EngineInput *input)
{
    (void)runtime;
    (void)dt;
    // Whatever has been typed at the terminal, evaluated against the world as it is now.
    ScriptS7Repl(&host);
    ScriptHostFlush(&host);
    return !input->pressed[KEY_ESCAPE];
}

static void BeforeDraw(GameplayRuntime *runtime, float alpha)
{
    (void)runtime;
    ScriptHostSetAlpha(&host, alpha);
}

static void Shutdown(GameplayRuntime *runtime)
{
    (void)runtime;
    ScriptS7Close();
    ScriptHostFree(&host);
}

EngineApplication EngineApplicationMain(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static GameplayRuntime runtime;
    GameplayProject project = GameplayProjectDefault();
    project.config.title = "Scripted demo";
    project.entitySize = ScriptEntitySize(); // classes arrive from the script, not a table
    project.scene = "projects/script_demo/start.scene";
    project.Init = Init;
    project.Update = Update;
    project.BeforeDraw = BeforeDraw;
    project.Shutdown = Shutdown;
    printf("s7 REPL: type an expression, for example (spawn \"mover\" (vec 200 200))\n> ");
    fflush(stdout);
    return GameplayApplication(&runtime, project);
}
