/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The authoring demo again, except the entity lives in a script. There is no C here that knows what
// a mover is: the project loads a script, and the script declares the class and its callbacks.
#include "gameplay/script/script_pawn.h"
#include "gameplay/script/script_s7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ScriptHost host;
static bool pawn; // --pawn runs the same entity written in the other language

static bool Init(GameplayRuntime *runtime)
{
    if (!ScriptHostInit(&host, &runtime->world))
        return false;
    // Classes have to exist before the scene that places them is read.
    if (pawn)
        return ScriptPawnOpen(&host, "projects/script_demo/mover.amx");
    return ScriptS7Open(&host) && ScriptS7Load(&host, "projects/script_demo/mover.scm");
}

static bool Update(GameplayRuntime *runtime, double dt, const EngineInput *input)
{
    (void)runtime;
    (void)dt;
    // Whatever has been typed at the terminal, evaluated against the world as it is now.
    if (!pawn)
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
    if (pawn)
        ScriptPawnClose();
    else
        ScriptS7Close();
    ScriptHostFree(&host);
}

EngineApplication EngineApplicationMain(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--pawn"))
            pawn = true;
        // The build asks for the Pawn declarations here, because this is what links the frontend.
        else if (!strcmp(argv[i], "--write-pawn-include") && i + 1 < argc)
            exit(ScriptPawnWriteInclude(argv[i + 1]) ? 0 : 1);
    }
    SetTraceLogLevel(LOG_WARNING); // so the REPL prompt is not buried in raylib's startup
    static GameplayRuntime runtime;
    GameplayProject project = GameplayProjectDefault();
    project.config.title = pawn ? "Scripted demo (Pawn)" : "Scripted demo (Scheme)";
    project.scene = "projects/script_demo/start.scene";
    project.Init = Init;
    project.Update = Update;
    project.BeforeDraw = BeforeDraw;
    project.Shutdown = Shutdown;
    return GameplayApplication(&runtime, project);
}
