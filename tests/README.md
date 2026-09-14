# Engine verification

Nothing in this directory is a game project. These are fixtures and executable checks owned by the
engine, arranged by cost and dependency:

- `integration/gameplay/` is a non-graphical integration check for entities, scenes, systems, input,
  and transforms. Run `make test-gameplay`.
- `regression/` preserves behavioral checks for fixed bugs. Run `make test-regression`.
- `integration/core/` needs a working X/GL display and verifies rendering, shaders, animation, UI,
  and render targets. Run `make integration`.

`make smoke` runs the first two plus `docs-check`; it is the routine pre-commit command.
`make full-check` adds every graphical and runner integration check. `make` itself only builds the
engine libraries and authoring tools.

Keep fixtures small and specific to the behavior they verify. A playable showcase or a game belongs
outside this repository.
