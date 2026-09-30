# Build the engine and start a game

Trench Engine is a C engine built on raylib and currently supports the Linux/X11 build described in the repository README. Install a C compiler, `make`, `git`, Python 3, and the X11/OpenGL development packages required by raylib.

```sh
git clone --recurse-submodules https://github.com/WheeledCord/engine.git
cd engine
make all
make -f Makefile.core build/core/trench
make smoke
```

`smoke` runs the regression, engine-new, SDK and documentation checks without a display. `make integration` runs the graphical checks and needs a working X display.

A game is a directory `trench` runs. `engine-new` writes a starter one: a `game` kind with a tilemap floor and lights, and a `walker` each player moves with WASD.

```sh
python3 tools/engine_new.py ~/games/mine --language scheme
build/core/trench run ~/games/mine
build/core/trench run ~/games/mine --headless --ticks 60   # no window
```

`make sdk` (or `make install PREFIX=~/.local`) puts `trench`, `engine-new` and `engine-build` in the SDK's `bin/`, so games are made and run from anywhere; a game that needs C is built with `engine-build`, as the root README describes.

Next: [run a Scheme game](kinds.md).
