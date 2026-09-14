# Game-author guides

These guides explain how to use Trench Engine in a game. Start with a task-focused guide, then use the API reference for exact lifetime and error contracts.

## Start here

1. [Build the engine and run a sample](getting-started.md).
2. [Create an application and game loop](application-loop.md).
3. Choose the optional [gameplay layer](gameplay.md) when you need entities and scenes.

## Guides

- [Application loop](application-loop.md) — callbacks, fixed updates, drawing, and input routing.
- [Gameplay, entities, and scenes](gameplay.md) — registered classes, fields, callbacks, and scene files.
- [2D space, camera, and queries](2d-space.md) — optional camera conventions and collision queries without physics.
- [Project game state](project-game-state.md) — spawning, score, reset, UI capture, and persistence boundaries.
- [Scripting](scripting.md) — Scheme, Pawn, and game-defined binding calls.
- [Diagnostics, audio, and sprite presentation](diagnostics-audio-sprites.md) — optional runtime helpers.

Rendering, UI, animation, sprites, and isometric movement currently have their detailed source documentation in `core/README.md` and `gameplay/README.md`. Move each topic here as it gains a task-focused guide; do not duplicate a claim without naming its canonical page.
