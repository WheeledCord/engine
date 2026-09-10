# Engine

Reusable C engine layers built on raylib.

`core/` provides the window loop, input buffering, rendering helpers, shaders,
animation, camera, viewmodel, and immediate mode UI. `gameplay/` provides the
optional entity, scene, and systems layer on top of core.

Projects supply their own content and application entry point. The original
game, project examples, development scripts, generated builds, and game assets
are intentionally kept out of this repository.

## Dependency

raylib is included as a Git submodule. Clone with:

```sh
git clone --recurse-submodules https://github.com/WheeledCord/engine.git
```

The public API and module notes are documented in [`core/README.md`](core/README.md)
and [`gameplay/README.md`](gameplay/README.md).

