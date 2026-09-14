# engine

A C engine built on raylib, targeting OpenGL 2.1.

This is the engine. The games and the editor built with it live beside it in the workspace, each a
project of its own:

    trenchengine/
      engine/        this repository
      references/    other engines, kept to read
      Games/         dodge, collector, scripted-mover, authoring-demo, isometric
      Tools/         ui-editor, sprite-baker

`core/` is the engine: window and main loop, input, shaders, render targets, mesh building,
skeletons and animation, sprite sheets, isometric grids, an FPS camera, and an immediate-mode UI. A project declares the GPU
capabilities it needs and core checks those and nothing else. `gameplay/` is an optional entity
and scene layer above core. `tests/` holds engine-owned verification fixtures; game projects and
authoring tools live beside the repository in the workspace.

## Building

Linux, with a C compiler, make, git, python3 and the X11/GL development packages.

    git clone --recurse-submodules https://github.com/WheeledCord/engine.git
    cd engine
    make all

raylib is a submodule, compiled from source into `build/` on the first build (about 30 seconds)
and cached after that. If you cloned without `--recurse-submodules`, run
`git submodule update --init` first.

    make run           # optional graphical integration test
    make smoke         # quick gameplay, regression, and documentation checks
    make integration   # graphical rendering and runner integration checks
    make full-check    # smoke plus all integration checks
    make sdk           # the SDK games are built against
    make run-authoring # build and run a game from the workspace, through the SDK

Binaries locate the engine's own files relative to themselves, so they can be run from any working
directory.

## Checks

`make smoke` runs the fast gameplay and regression suites plus documentation checks; `make integration`
runs the graphical/runner integration checks, and `make full-check` runs both. One of the suites,
`regression_test`, holds a check for every bug that has been found and fixed here, so they stay
fixed. The UI editor carries its own suite, run from the editor: `ui-editor --smoke`. They check
behaviour rather than pixels. The UI tool's suite drives itself with scripted mouse, keyboard and
clipboard input, and exports screenshots to `build/core/`.

There is no test framework and no CI.

## Documentation

The in-repository [documentation hub](docs/index.md) separates task-focused game-author guides,
Godot-style XML API references, and engine-developer standards. Run `make docs-check`
to validate the checked-in API references. [CONTRIBUTING.md](CONTRIBUTING.md) requires documentation
to change with public behaviour and public APIs.

## Writing a project

`EngineApplicationDefault()` provides the application defaults; implement only the callbacks needed.
The optional standard entry point calls a project-defined `EngineApplicationMain`. Core-only projects
can use this directly. `GameplayApplication` additionally handles the world, class registration, scene
loading, systems and cleanup. Entity callbacks receive their payload, input, timing and project context;
field declarations provide keyvalue parsing before Spawn.

See the complete authoring example in the sibling workspace at `../Games/authoring-demo`,
[core application/input APIs](core/README.md#application-authoring), and
[gameplay authoring and migration](gameplay/README.md).

## Building a game with it

The engine builds an SDK: headers, static libraries, its runtime data, the Pawn toolchain, and a
pkg-config file describing them.

    make sdk                  # build/core/sdk
    make install PREFIX=~/.local

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

An SDK says which engine it is: `engine.pc` carries the revision it was built from, and `-dirty`
when that tree had uncommitted changes. A project can insist on one, and is not built against
another by accident:

    engine >= 0.1.0          a floor
    engine 0.1.0-ad0546a     exactly that build
    engine ad0546a           that revision, however it was versioned

Every build writes `build/engine-build.stamp` beside the binary: the engine and revision, the SDK it
came from, the compiler and its flags, and what went in. Installing the SDK puts `engine-build` and
`engine-new` on your PATH, so a game is built without reaching into the engine's directory at all.

    engine-new ~/games/mine --language scheme
    engine-build ~/games/mine        # add --sdk <dir> when nothing is installed
    ~/games/mine/build/mine

Everything in `Games/` and `Tools/` is a project of exactly that shape, built the way anyone else's
would be.

## Scripting

Scheme and Pawn, over one binding table. The script-facing API is described once as data, and each
language is a loop over that table rather than a set of hand-written bindings, so a call is bound
once however many languages read it. `gameplay/script/README.md` explains the arrangement;
`Games/scripted-mover` is the same entity written in each language, with no C left that knows what a
mover is.

## UI

Layouts are authored in the editor (`run-ui-tool`) and saved as plain text. A game loads one and
draws it through the same call the editor uses. Elements carry anchors, minimum and maximum sizes,
and containers divide space among their contents, so a layout resolves to any window size without
scaling: the 16px text and the controls keep their size and the space between them changes instead.

The UI editor lives in the sibling workspace at `../Tools/ui-editor`; `core/README.md` covers the engine modules.

## License

MPL 2.0, see `LICENSE`. Per-file copyleft: the engine can be used inside a program under other
terms, but changes to these files stay under the MPL.

## Third-party

raylib is under zlib/libpng (`raylib/LICENSE`). The UI font is GNU Unifont under the SIL Open Font
License (`core/fonts/`). The test character in `tests/integration/core` is CC0. raylib's source is
unmodified; the build only passes it different flags.
