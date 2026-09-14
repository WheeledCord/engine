# Tools

`check_core_dependencies.py` keeps core's includes inside core, raylib and the system. The build runs
it before compiling anything.

`write_engine_pc.py` writes the `engine.pc` for an SDK tree, with the paths that tree was put at and
the engine revision it was built from, so anything built against it can name the engine it used.
`make sdk` and `make install` both call it, which is why an SDK works from wherever it is.

`engine_build.py` and `engine_new.py` are installed into an SDK as `engine-build` and `engine-new`.
They are the whole of a project's build: `engine-new` starts one, `engine-build` reads its manifest,
finds the SDK, and produces a static binary with the project's content and the engine's runtime data
beside it.

`write_script_api.py` writes Markdown and JSON from the script binding table. Run
`make script-api` to create both in `build/core/`; `make sdk` packages them under
`share/engine/docs/`. This keeps the runtime bindings, both script frontends, and their reference
on one contract.
