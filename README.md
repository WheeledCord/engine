# engine

A C engine built on raylib, targeting OpenGL 2.1.

`core/` is the engine: window and main loop, input, shaders, render targets, mesh building,
skeletons and animation, an FPS camera, and an immediate-mode UI. A project declares the GPU
capabilities it needs and core checks those and nothing else. `gameplay/` is an optional entity
and scene layer above core. `projects/` holds the programs built on it, including the UI layout
editor.

## Building

Linux, with a C compiler, make, git, python3 and the X11/GL development packages.

    git clone --recurse-submodules https://github.com/WheeledCord/engine.git
    cd engine
    make -f Makefile.core all

raylib is a submodule, compiled from source into `build/` on the first build (about 30 seconds)
and cached after that. If you cloned without `--recurse-submodules`, run
`git submodule update --init` first.

    make -f Makefile.core run-ui-tool   # UI layout editor
    make -f Makefile.core run           # core_test: skinned character, sampled depth
    make -f Makefile.core games         # two demo games
    make -f Makefile.core run-2d        # one of them
    make -f Makefile.core run-authoring # scene-spawned mover, input actions, UI capture
    make -f Makefile.core run-script    # the same mover, written in Scheme, with a live REPL
    make -f Makefile.core run-script-pawn # and again in Pawn, over the same bindings
    make -f Makefile.core smoke         # build everything and run the checks

Binaries locate the engine's own files relative to themselves, so they can be run from any working
directory.

## Checks

`make -f Makefile.core smoke` runs four suites and exits non-zero on failure. One of them,
`regression_test`, holds a check for every bug that has been found and fixed here, so they stay
fixed. They check
behaviour rather than pixels. The UI tool's suite drives itself with scripted mouse, keyboard and
clipboard input, and exports screenshots to `build/core/`.

There is no test framework and no CI.

## Writing a project

`EngineApplicationDefault()` provides the application defaults; implement only the callbacks needed.
The optional standard entry point calls a project-defined `EngineApplicationMain`. Core-only projects
can use this directly. `GameplayApplication` additionally handles the world, class registration, scene
loading, systems and cleanup. Entity callbacks receive their payload, input, timing and project context;
field declarations provide keyvalue parsing before Spawn.

See [the complete authoring example](projects/authoring_demo/README.md),
[core application/input APIs](core/README.md#application-authoring), and
[gameplay authoring and migration](gameplay/README.md).

## Building a game with it

The engine builds an SDK: headers, static libraries, its runtime data, the Pawn toolchain, and a
pkg-config file describing them.

    make -f Makefile.core sdk                  # build/core/sdk
    make -f Makefile.core install PREFIX=~/.local

A project lives anywhere and says only what it is, in an `engine.project` manifest — no build rules
and no path into the engine:

    name authoring_demo

    module entry
    module gameplay

    source src/*.c
    scenes scenes

`engine-build` reads it, finds the SDK (named with `--sdk`, in `ENGINE_SDK`, vendored at `./sdk`, or
installed and found through pkg-config), links one static binary, puts the project's content and the
engine's runtime data beside it so it runs from anywhere, and checks that nothing has reached past
the SDK into the engine's own files.

    engine-new ~/games/mine --language scheme
    engine-build ~/games/mine        # add --sdk <dir> when nothing is installed
    ~/games/mine/build/mine

`examples/authoring_demo` is a project of exactly that shape, built the way anyone else's would be.

## Scripting

Scheme and Pawn, over one binding table. The script-facing API is described once as data, and each
language is a loop over that table rather than a set of hand-written bindings, so a call is bound
once however many languages read it. `gameplay/script/README.md` explains the arrangement;
`projects/script_demo` is the authoring demo's entity written in each language, with no C left that
knows what a mover is.

## UI

Layouts are authored in the editor (`run-ui-tool`) and saved as plain text. A game loads one and
draws it through the same call the editor uses. Elements carry anchors, minimum and maximum sizes,
and containers divide space among their contents, so a layout resolves to any window size without
scaling: the 16px text and the controls keep their size and the space between them changes instead.

`projects/ui_tool/README.md` covers the editor, `core/README.md` the engine modules.

## License

MPL 2.0, see `LICENSE`. Per-file copyleft: the engine can be used inside a program under other
terms, but changes to these files stay under the MPL.

## Third-party

raylib is under zlib/libpng (`raylib/LICENSE`). The UI font is GNU Unifont under the SIL Open Font
License (`core/fonts/`). The test character in `projects/core_test` is CC0. raylib's source is
unmodified; the build only passes it different flags.
