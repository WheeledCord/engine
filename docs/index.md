# Trench Engine documentation

A game is a directory of Scheme that `trench run` runs: it declares its kinds of thing with
`define-kind`, and the engine keeps them in the store, moves, lights and draws them with world3d and
the draw path, and shares them in co-op. A game that needs C uses the store, world3d and the draw
path directly, as Trenchfoot does.

Choose the documentation for the work you are doing:

- **Making a game:** start with the [game-author guides](user/index.md), then [running a Scheme game](user/kinds.md).
- **Looking up a public C contract:** use the [XML API reference](api/README.md), and the [store design](developer/store.md) for the store, world3d, the draw path and the runner.
- **Finding an engine facility:** read the [feature catalogue](reference/feature-catalogue.md).
- **Changing the engine:** read the [developer guide](developer/index.md) and [contribution rules](../CONTRIBUTING.md).

The repository is the canonical source of documentation. A future GitHub wiki may mirror these pages for browsing, but changes are reviewed here with the code they describe.
