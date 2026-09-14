# Contributing to Trench Engine

Thank you for improving the engine. Keep a change small enough that it can be reviewed, built, and reverted independently. Discuss a new subsystem or a change to an established public contract before writing a large implementation.

## Before sending a change

1. Build and run `make smoke`.
2. Add a regression check for a bug fix, and success plus expected-failure coverage for a feature.
3. Update documentation as part of the same logical commit or pull request.
4. Use a clear imperative commit subject, such as `Add sprite playback anchors` or `Gameplay: Reject invalid entity fields`.

## Documentation is part of the change

Update documentation when a change affects a public API, authoring workflow, visible behaviour, configuration, file format, compatibility requirement, limitation, build/deployment process, or contributor workflow. A behaviour-preserving internal refactor needs no user guide update, but does need developer documentation when it changes an invariant, ownership/lifetime rule, architecture, or maintenance workflow.

For a public C API, update its `/** ... */` documentation block in the public header in the same change. `make api-docs-update` generates the checked-in XML reference; do not edit that XML by hand. The block must describe each added or changed function's purpose, arguments, return/failure behaviour, ownership, and relevant limitations. Run `make docs-check` before committing.

Do not describe an unfinished feature as supported. Record deliberate limits in the relevant guide or in `docs/reference/compatibility.md`.

## Engine standards

- `core/` may depend only on core, raylib, and system headers. It must not depend on `gameplay/` or a project.
- The engine supplies reusable mechanisms; games own policy such as controls, collision rules, map size, content, and game-specific state.
- Public APIs must state ownership, lifetime, failure behaviour, and cleanup responsibility.
- Do not add hidden process-wide game state. Pass project state through the documented context.
- Preserve the OpenGL 2.1 contract unless a deliberate, documented compatibility change is accepted.
- New script-facing calls belong in the shared binding table so Scheme and Pawn agree.

Read the [developer documentation](docs/developer/index.md) before changing engine internals.
