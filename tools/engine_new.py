#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Start a project.

`--language scheme` (the usual kind) writes a directory `trench run` runs: an engine.project naming
the game file, and a starter game in Scheme. `--language c` writes a C project that engine-build
builds against the SDK: a manifest, a source file that runs, and the directories a game keeps its
content in. Neither knows where the engine is; trench and engine-build find that."""
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

SCHEME_GAME = ''';;;; %(name)s: a starter game. `trench run .` in this directory runs it (docs/user/kinds.md in
;;;; the engine lists the flags; `trench run . --headless --ticks 60` runs it without a window).

;; The keys a player presses, by name. Handlers ask for actions, never for keys.
(define-actions (left "A") (right "D") (forward "W") (back "S"))

;; A player's character: it walks on the floor, and its owner watches it from behind and above.
(define-kind walker
  (is character)
  (child body (model :at (vec3 0 0.9 0)))            ; no mesh yet: drawn as an upright box
  (child eye (camera :at (vec3 0 6 7) :rotation (vec3 -0.7 0 0) :for-owner #t))
  (on (tick dt)
    (let ((v (input-vector 'left 'right 'forward 'back)))
      (set! velocity (vec3 (* 5 (vx v)) (- (vy velocity) (* 9.8 dt)) (* 5 (vz v))))
      (move-and-slide!))))

;; The game: the world's root. It holds the floor and the lights, and gives every player who joins
;; (the local player at start, and each machine that joins a co-op session) a walker of their own.
(define-kind game
  (child floor (tilemap :width 16 :depth 16))
  (child sun (light :type 'directional :energy 1.0 :rotation (vec3 0.5 0 0.3)))
  (child sky (light :type 'ambient :energy 0.35))
  (on (player-joined p)
    (spawn 'walker :owner p :at (vec3 (+ 6 (* 2 p)) 0.5 8)))
  ;; Presentation: runs on every machine, each frame, after the world is drawn.
  (on (draw-hud)
    (draw-text (format #f "~A: player ~A of ~A. WASD walks; Escape frees the mouse."
                       "%(name)s" (local-player) (length (players)))
               16 16 :size 20 :color 'white)))
'''

SCHEME_README = '''# %(name)s

A Trench Engine game in Scheme. `engine.project` names it and its game file, `%(name)s.scm`, which
declares its kinds with `define-kind`. Run it with the engine's `trench` tool:

```sh
trench run .                          # in a window
trench run . --headless --ticks 60    # no window, 60 ticks, for a quick check
trench run . --host 7777              # host a co-op session; others --join ADDRESS:7777
```

In a window the terminal is a REPL on the running game: `(things 'walker)`, `(inspect (game))`, and
`(reload)` after editing `%(name)s.scm`. Models, textures and sounds are named relative to this
directory.
'''

C_README = '''# %(name)s

```sh
engine-build .        # add --sdk <dir> when the SDK is not installed
./build/%(name)s
```

`build/engine-build.stamp` says which engine built it.
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=pathlib.Path)
    parser.add_argument('--name', help='the project\'s name (default: the directory name)')
    parser.add_argument('--language', choices=('c', 'scheme'), default='c')
    args = parser.parse_args()

    directory = args.directory
    name = args.name or directory.resolve().name
    if directory.exists() and any(directory.iterdir()):
        sys.exit(f'engine-new: {directory} is not empty')
    directory.mkdir(parents=True, exist_ok=True)
    if args.language == 'scheme':
        # A Scheme game is a directory the runner reads: no sources, nothing to build.
        (directory / 'engine.project').write_text(f'name {name}\ngame {name}.scm\n')
        (directory / f'{name}.scm').write_text(SCHEME_GAME % {'name': name})
        (directory / 'README.md').write_text(SCHEME_README % {'name': name})
        print(f'engine-new: {directory}. Run it with trench run {directory}')
        return
    (directory / 'src').mkdir(exist_ok=True)
    for content in ('assets', 'shaders'):
        (directory / content).mkdir(exist_ok=True)
    manifest = [f'name {name}',
                '',
                '# Pin the engine this is meant for, so another one cannot be used by accident:',
                '#   engine >= 0.1.0        a floor',
                '#   engine 0.1.0-ad0546a   exactly that build',
                '',
                'source src/*.c',
                'assets assets',
                'shaders shaders',
                '']
    (directory / 'src' / 'main.c').write_text(C_MAIN % {'name': name})
    (directory / 'engine.project').write_text('\n'.join(manifest))
    (directory / 'README.md').write_text(C_README % {'name': name})
    print(f'engine-new: {directory}. Build it with engine-build {directory}')


if __name__ == '__main__':
    main()
