# Game-author guides

These guides explain how to use Trench Engine in a game. A game is usually a directory of Scheme that
`trench run` runs; a game that needs C links the engine and uses the store, world3d and the draw path
directly, as Trenchfoot does. Start with a task-focused guide, then use the API reference for exact
lifetime and error contracts.

## Start here

1. [Build the engine and start a game](getting-started.md).
2. [Run a Scheme game](kinds.md): `engine.project`, `define-kind`, the built-in kinds, co-op,
   recording and replay.

## Guides

- [Running a Scheme game](kinds.md) — `engine.project`, `trench run`, the built-in 3D kinds, co-op, recording, replay and the bot.
- [Networking](networking.md) — co-op on the store, from Scheme or from C.
- [Application loop](application-loop.md) — for C: callbacks, fixed updates, drawing, input routing and headless runs.
- [Relative mouse capture](mouse-capture.md) — focus-safe pointer locking for first-person views.
- [2D space, camera, and queries](2d-space.md) — optional camera conventions and collision queries without physics.
- [Project game state](project-game-state.md) — a C game's own state, UI capture, and persistence boundaries.
- [Diagnostics, audio, and sprite presentation](diagnostics-audio-sprites.md) — optional runtime helpers.
- [Bake a material into a texture](texture-baking.md) — a shader material evaluated once, into an image.

The store, world3d, the draw path and the runner are specified in [the store design](../developer/store.md). Rendering, UI, animation, sprites and isometric grids have their detailed source documentation in `core/README.md`. Move each topic here as it gains a task-focused guide; do not duplicate a claim without naming its canonical page.
