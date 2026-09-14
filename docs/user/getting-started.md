# Build and run a first project

Trench Engine is a C engine built on raylib and currently supports the Linux/X11 build described in the repository README. Install a C compiler, `make`, `git`, Python 3, and the X11/OpenGL development packages required by raylib.

```sh
git clone --recurse-submodules https://github.com/WheeledCord/engine.git
cd engine
make smoke
```

`smoke` builds the core, gameplay, scripting, and regression projects, then runs their checks. A working X display is required for the graphical checks.

To build a game outside this repository, create an SDK and use `engine-new` and `engine-build` as described in the root README. Start from a generated core-only project unless you need scenes and entities, in which case enable the gameplay module.

Next: [create an application](application-loop.md).
