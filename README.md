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
    make -f Makefile.core smoke         # build everything and run the checks

Binaries locate the engine's own files relative to themselves, so they can be run from any working
directory.

## Checks

`make -f Makefile.core smoke` runs three suites and exits non-zero on failure. They check
behaviour rather than pixels. The UI tool's suite drives itself with scripted mouse, keyboard and
clipboard input, and exports screenshots to `build/core/`.

There is no test framework and no CI.

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
