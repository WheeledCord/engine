#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Start a project that builds against the engine SDK.

Writes a manifest, a source file that runs, and the directories a game keeps its content in. The
project knows nothing about where the engine is; engine-build finds that."""
import argparse
import pathlib
import sys

C_MAIN = '''#include "core/engine.h"

static float x = 40;

static bool Update(void *context, double dt, const EngineInput *input)
{
    (void)context;
    if (input->down[KEY_D] || input->down[KEY_RIGHT])
        x += 120 * (float)dt;
    if (input->down[KEY_A] || input->down[KEY_LEFT])
        x -= 120 * (float)dt;
    return !input->pressed[KEY_ESCAPE];
}

static void Draw(void *context, float alpha)
{
    (void)context;
    (void)alpha;
    DrawRectangle((int)x, 200, 48, 48, (Color){102, 191, 255, 255});
    DrawText("A and D move. Escape quits.", 16, 16, 20, RAYWHITE);
}

EngineApplication EngineApplicationMain(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    EngineApplication application = EngineApplicationDefault();
    application.config.title = "%(name)s";
    application.config.fixed_dt = 0;
    application.callbacks.Update = Update;
    application.callbacks.Draw = Draw;
    return application;
}
'''

SCHEME_MAIN = '''#include "gameplay/script/script_s7.h"

static ScriptHost host;

static bool Init(GameplayRuntime *runtime)
{
    if (!ScriptHostInit(&host, &runtime->world) || !ScriptS7Open(&host))
        return false;
    return ScriptS7Load(&host, "%(name)s.scm");
}

static bool Update(GameplayRuntime *runtime, double dt, const EngineInput *input)
{
    (void)runtime;
    (void)dt;
    ScriptS7Repl(&host); // anything typed at the terminal, against the running world
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
    project.config.title = "%(name)s";
    project.scene = "scenes/start.scene";
    project.Init = Init;
    project.Update = Update;
    project.BeforeDraw = BeforeDraw;
    project.Shutdown = Shutdown;
    return GameplayApplication(&runtime, project);
}
'''

SCHEME_SCRIPT = '''; %(name)s. Every call here comes from the engine's binding table.

(define (thing-spawn)
  (think-next))

(define (thing-think)
  (move-world! (vec* (input-vector) (* 200 (dt))))
  (think-next))

(define (thing-draw)
  (draw-rect (interpolated) (vec 32 32) (rgba 102 191 255 255)))

(define-entity "thing"
  '()
  '(("spawn" "thing-spawn") ("think" "thing-think") ("draw" "thing-draw")))
'''

SCENE = '''entity "thing" {
    "position" "320 240"
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--name', help='the binary to build (default: the directory name)')
    parser.add_argument('--language', choices=('c', 'scheme'), default='c')
    args = parser.parse_args()

    directory = args.directory
    name = args.name or directory.resolve().name
    if directory.exists() and any(directory.iterdir()):
        sys.exit(f'engine-new: {directory} is not empty')
    (directory / 'src').mkdir(parents=True, exist_ok=True)
    for content in ('assets', 'scenes', 'shaders'):
        (directory / content).mkdir(exist_ok=True)

    manifest = [f'name {name}', '', 'module entry']
    if args.language == 'scheme':
        manifest += ['module gameplay', 'module script-s7']
        (directory / 'src' / 'main.c').write_text(SCHEME_MAIN % {'name': name})
        (directory / f'{name}.scm').write_text(SCHEME_SCRIPT % {'name': name})
        (directory / 'scenes' / 'start.scene').write_text(SCENE)
        manifest += ['', 'source src/*.c', f'script {name}.scm', 'scenes scenes']
    else:
        (directory / 'src' / 'main.c').write_text(C_MAIN % {'name': name})
        manifest += ['', 'source src/*.c']
    manifest += ['assets assets', 'shaders shaders', '']
    (directory / 'engine.project').write_text('\n'.join(manifest))
    (directory / 'README.md').write_text(
        f'# {name}\n\n```sh\nengine-build .        # add --sdk <dir> when the SDK is not installed\n'
        './build/{name}\n```\n')
    print(f'engine-new: {directory}. Build it with engine-build {directory}')


if __name__ == '__main__':
    main()
