# Build and run a first project

Trench Engine is a C engine built on raylib and currently supports the Linux/X11 build described in the repository README. Install a C compiler, `make`, `git`, Python 3, and the X11/OpenGL development packages required by raylib.

```sh
git clone --recurse-submodules https://github.com/WheeledCord/engine.git
cd engine
make smoke
```

`smoke` builds the engine and runs its gameplay, regression and documentation checks without a display. `make integration` runs the graphical checks and needs a working X display.

To build a game outside this repository, create an SDK and use `engine-new` and `engine-build` as described in the root README. `engine-new` writes a project you can build straight away.

Next: [create an application](application-loop.md).
