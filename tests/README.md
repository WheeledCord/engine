# Engine verification

Nothing in this directory is a game project. These are fixtures and executable checks owned by the
engine, arranged by cost and dependency:

- `regression/` preserves behavioral checks for fixed bugs and covers the store, world3d, the draw
  path, networking and the runner. It opens a hidden window, so it needs a display (`xvfb-run`
  without one). Run `make test-regression`.
- `integration/core/` needs a working X/GL display and verifies rendering, shaders, animation, UI,
  and render targets. Run `make integration`.

`make smoke` runs the regression suite, `tools/test_engine_build.py` (engine-build's contracts, and a
project from `engine-new --language scheme` run by the build tree's and the SDK's `trench`) and
`docs-check`; it is the routine pre-commit command. `make full-check` adds the graphical integration
check. `make` itself only builds the engine libraries.

Keep fixtures small and specific to the behavior they verify. A playable showcase or a game belongs
outside this repository.
