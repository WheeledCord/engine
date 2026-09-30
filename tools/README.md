# Tools

`check_core_dependencies.py` keeps core's includes inside core, raylib and the system. The build runs
it before compiling anything.

`write_engine_pc.py` writes the `engine.pc` for an SDK tree, with the paths that tree was put at and
the engine revision it was built from, so anything built against it can name the engine it used.
`make sdk` and `make install` both call it, which is why an SDK works from wherever it is.

`engine_build.py` and `engine_new.py` are installed into an SDK as `engine-build` and `engine-new`,
beside `trench`. `engine-new --language scheme` starts a Scheme game, a directory `trench run` runs
with nothing to build; `engine-new --language c` starts a C project, and `engine-build` reads its
manifest, finds the SDK, and produces a static binary with the project's content and the engine's
runtime data beside it. `test_engine_build.py` checks both (`make smoke`).

`trench/` is the runner's `main`; the runner itself is `gameplay/game.c`.
