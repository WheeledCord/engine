# engine

A small C engine on top of raylib, targeting OpenGL 2.1. It came out of a WW1 trench game and
keeps that project's habits: integer pixels, no asset pipeline, no fallbacks, and a UI that looks
like it belongs on a machine from 1998.

`core/` is the engine: window and loop, input, shaders, render targets, meshes, skeletal animation,
an FPS camera, and an immediate-mode UI. It knows nothing about any particular game — a project says
what it needs and core enforces exactly that. `gameplay/` adds an optional entity/scene layer.
`projects/` holds the things built on it, including the UI layout editor.

## Building

Linux, with a C compiler, make, git, python3 and the usual X11/GL development packages.

    git clone --recurse-submodules https://github.com/WheeledCord/engine.git
    cd engine
    make -f Makefile.core all

raylib is a submodule and is compiled from source into `build/` on the first build (~30s), then
cached. If you cloned without `--recurse-submodules`, run `git submodule update --init` first.

    make -f Makefile.core run-ui-tool   # the UI layout editor
    make -f Makefile.core run           # core_test: skinned character, sampled depth
    make -f Makefile.core games         # two small demo games
    make -f Makefile.core run-2d        # ... one of them
    make -f Makefile.core smoke         # build everything and run the checks

The binaries find the engine's own files relative to themselves, so they run from anywhere, not
just the repository root.

## Checks

`make -f Makefile.core smoke` runs three suites and returns non-zero on failure. They assert
behaviour rather than pixels — the UI tool's suite drives itself with scripted mouse, keyboard and
clipboard input and checks the results, then exports screenshots to `build/core/` to look at.

There is no separate test framework and no CI yet.

## The UI

The interesting part. Layouts are authored in the editor (`run-ui-tool`) and saved as plain text,
then loaded by a game through the same drawing call the editor uses, so the two cannot drift.
Elements carry anchors, minimum and maximum sizes, and containers share themselves among their
contents, so a layout resolves to any window size without anything being scaled: text stays 16px
and a button stays a button. `projects/ui_tool/README.md` explains the editor,
`core/README.md` the engine layers.

## Third-party

raylib is under zlib/libpng (`raylib/LICENSE`). The UI font is GNU Unifont under the SIL Open Font
License (`core/fonts/`). The test character in `projects/core_test` is CC0. Nothing else is
vendored, and raylib's source is unmodified — the build only passes it different flags.
