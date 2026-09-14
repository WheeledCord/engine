# Game-author guides

These guides explain how to use Trench Engine in a game. They complement the runnable projects in `projects/`; copy a small example first, then use the API reference when you need exact contracts.

## Start here

1. [Build the engine and run a sample](getting-started.md).
2. [Create an application and game loop](application-loop.md).
3. Choose the optional [gameplay layer](gameplay.md) when you need entities and scenes.

## Guides

- [Application loop](application-loop.md) — callbacks, fixed updates, drawing, and input routing.
- [Gameplay, entities, and scenes](gameplay.md) — registered classes, fields, callbacks, and scene files.
- [Scripting](scripting.md) — Scheme, Pawn, and game-defined binding calls.

Rendering, UI, animation, sprites, and isometric movement currently have their detailed source documentation in `core/README.md`, `gameplay/README.md`, and the corresponding test projects. Move each topic here as it gains a task-focused guide; do not duplicate a claim without naming its canonical page.
