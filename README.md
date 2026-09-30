# Trench Engine

Trench Engine is a general-purpose engine for 2D and 3D games, written in C on top of raylib and
targeting modest hardware (OpenGL 2.1, GLSL 120). The engine does the heavy lifting in C —
rendering, animation, audio, input, collision, saving and networking — and games decide what
happens. It is developed alongside real games: when one of them needs something an engine would
normally provide, it goes into the engine for every game to use, but game rules never do.

Games are meant to be written in Scheme, with scripting as easy as Godot's and powerful enough to
build new features without writing C. C remains the engine's own language, and a game drops into it
only when it truly needs to. The aim is an engine that is simple, readable and easy to use, and small
enough for one person to understand.

This repository is the engine. Games and tools built with it live beside it in the workspace, each a
project of its own:

    trenchengine/
      engine/        this repository
      references/    other engines' source, kept to read
      Games/         trenchfoot
      Tools/         ui-editor, sprite-baker, texture-baker

`core/` is the engine: window and main loop, input, shaders, render targets, mesh building,
skeletons and animation, sprite sheets, isometric grids, an FPS camera, audio, saving, an
immediate-mode UI, and the world a game lives in: the store (things of declared kinds in a tree,
ticked in a fixed order, replayable), world3d (the built-in 3D kinds: models, cameras, lights,
characters, tilemaps), the draw path, and networking on the store. A project declares the GPU
capabilities it needs and core checks those and nothing else. `gameplay/` holds the runner,
`trench`, and the Scheme frontend it runs games with. `tests/` holds the engine's own checks.

## Building

Linux, with a C compiler, make, git, python3 and the X11/GL development packages.

    git clone --recurse-submodules https://github.com/WheeledCord/engine.git
    cd engine
    make all

raylib is a submodule, compiled from source into `build/` on the first build (about 30 seconds)
and cached after that. If you cloned without `--recurse-submodules`, run
`git submodule update --init` first.

    make run           # optional graphical integration test
    make smoke         # regression, engine-new, SDK and documentation checks
    make integration   # graphical rendering integration checks
    make full-check    # smoke plus all integration checks
    make sdk           # the SDK games are built against, with trench, engine-new and engine-build

Binaries locate the engine's own files relative to themselves, so they can be run from any working
directory.

## Checks

`make smoke` runs the regression suite, the engine-build and engine-new checks (a generated Scheme
project run by the build tree's `trench` and by the SDK's, from outside the engine tree) and the
documentation checks; `make integration` runs the graphical integration check, and `make full-check`
runs both. One of the suites,
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

## Writing a game

A game is a directory with an `engine.project` and a game file in Scheme, and `trench` runs it:

    name My Game
    game main.scm

    trench run mygame                       # in a window
    trench run mygame --headless --ticks 60 # no window
    trench run mygame --host 7777           # co-op; others --join ADDRESS:7777

The game file declares its kinds of thing with `define-kind` — fields, children, and handlers such
as `(on (tick dt) ...)` — and the engine runs them: the store keeps the things, world3d moves,
collides and lights them, the draw path draws them, and co-op, recording and replay come with it.
`engine-new mygame --language scheme` writes a starter one; [Running a Scheme game](docs/user/kinds.md)
is the guide, and `examples/` and `tests/regression/runner_game.scm` are games.

A game that needs C links the engine and uses the same pieces directly — the store
(`core/store.h`), networking on it (`core/store_net.h`, `core/store_net_enet.h`), world3d and the draw
path — from its own `EngineApplication`, as Trenchfoot does. `EngineApplicationDefault()` provides
the application defaults; implement only the callbacks needed, and define `EngineApplicationMain` to
use the standard entry point. See the [core application and input APIs](core/README.md#application-authoring).

## Building a game with it

The engine builds an SDK: headers, static libraries, its runtime data, and a pkg-config file
describing them.

    make sdk                  # build/core/sdk
    make install PREFIX=~/.local

A C project lives anywhere and says only what it is, in an `engine.project` manifest — no build
rules, no path into the engine, and no list of engine parts. Every project gets the whole engine; the
linker keeps only what the game calls, and a game that defines no `main` of its own gets the
standard one:

    name trenchfoot

    source src/*.c
    assets assets
    shaders shaders

`engine-build` reads it, finds the SDK (named with `--sdk`, in `ENGINE_SDK`, vendored at `./sdk`, or
installed and found through pkg-config), compiles in parallel only the sources that changed since the
last build, links one static binary, synchronizes the project's declared
content trees (including removing stale packaged files), and puts the
engine's runtime data beside it so it runs from anywhere, and checks that nothing has reached past
the SDK into the engine's own files.

An SDK says which engine it is: `engine.pc` carries the revision it was built from, and `-dirty`
when that tree had uncommitted changes. A project can insist on one, and is not built against
another by accident:

    engine >= 0.1.0          a floor
    engine 0.1.0-ad0546a     exactly that build
    engine ad0546a           that revision, however it was versioned

Every build writes `build/engine-build.stamp` beside the binary: the engine and revision, the SDK it
came from, the compiler and its flags, and what went in. Installing the SDK puts `trench`,
`engine-build` and `engine-new` on your PATH, and the SDK's `trench` finds the engine's prelude,
shaders and font in its own `share/engine/`, so a game is made and run without reaching into the
engine's directory at all:

    engine-new ~/games/mine --language scheme
    trench run ~/games/mine

    engine-new ~/games/tool --language c
    engine-build ~/games/tool        # add --sdk <dir> when nothing is installed
    ~/games/tool/build/tool

Everything in `Games/` and `Tools/` is a project of one of those shapes, built the way anyone else's
would be.

## Scripting

Scheme, through s7, on the store: `define-kind` declares a kind's fields, children and handlers,
and the rules that keep a game replayable and networkable are errors with sentences that say what
to do instead. `gameplay/script/README.md` explains the frontend, and `docs/developer/store.md` §5
specifies it.

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
