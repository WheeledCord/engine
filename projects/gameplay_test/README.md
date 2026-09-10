# Gameplay test project

This is a permanent non-graphical project that includes `gameplay/` and its `core/file.h` dependency.
It registers the `demo_counter` classname, loads `assets/demo.scene`, runs a system registry, and writes
then reloads a scene. Its scene source is project data, not an engine format extension.

```sh
make -f Makefile.core gameplay-smoke
```

It verifies that a destroyed handle is stale, a reused slot has a new generation, scene KeyValues reach
the registered type, Think runs only at its requested absolute times, and written scene data reloads.
`build/core/gameplay-roundtrip.scene` is its output artifact.
