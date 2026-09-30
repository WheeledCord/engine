# Contributing to Trench Engine

Thank you for improving the engine. Keep a change small enough that it can be reviewed, built, and reverted independently. Discuss a new subsystem or a change to an established public contract before writing a large implementation.

## Before sending a change

1. Build and run `make smoke`.
2. Add a regression check for a bug fix, and success plus expected-failure coverage for a feature.
3. Update documentation as part of the same logical commit or pull request.
4. Use a clear imperative commit subject, such as `Add sprite playback anchors` or `Store: Reject a field of the wrong type`.

## Documentation is part of the change

Update documentation when a change affects a public API, authoring workflow, visible behaviour, configuration, file format, compatibility requirement, limitation, build/deployment process, or contributor workflow. A behaviour-preserving internal refactor needs no user guide update, but does need developer documentation when it changes an invariant, ownership/lifetime rule, architecture, or maintenance workflow.

For a public C API, update its `/** ... */` documentation block in the public header in the same change. `make api-docs-update` generates the checked-in XML reference; do not edit that XML by hand. The block must describe each added or changed function's purpose, arguments, return/failure behaviour, ownership, and relevant limitations. Run `make docs-check` before committing.

Do not describe an unfinished feature as supported. Record deliberate limits in the relevant guide or in `docs/reference/compatibility.md`.

## Engine standards

- `core/` may depend only on core, raylib, and system headers. It must not depend on `gameplay/` or a project.
- The engine provides the systems a game needs (rendering, animation, collision, audio, input,
  saving, networking) and games provide their rules and content. Something belongs in the engine
  when a different kind of game could use it; stats, costs, turn structure and genre conventions
  stay in the game.
- Public APIs must state ownership, lifetime, failure behaviour, and cleanup responsibility.
- Do not add hidden process-wide game state. Pass project state through the documented context.
- Preserve the OpenGL 2.1 contract unless a deliberate, documented compatibility change is accepted.
- Treat every game-usable public engine capability as reachable from Scheme by default: a game
  written in Scheme gets it through the store's frontend, as a built-in kind or field (world3d) or
  as a call registered with `GameS7Define` beside the module it belongs to. Add it in the same
  change as the C API; do not wait for a Scheme game to discover the omission.
- Rendering, networking, input, audio, UI, animation, collision, and other normal game facilities
  are not exempt merely because their C APIs use owned state. Expose them through things and calls
  that keep the store's rules (gameplay stays replayable; presentation never feeds gameplay); never
  expose raw pointers or platform handles.
- Omitting a Scheme call is acceptable only when the operation is engine-internal plumbing, has no
  meaningful use from a game, or cannot be exposed without breaking the store's rules. Document that
  exception and its concrete reason in `gameplay/script/README.md`.
- Exercise new kinds and calls from the regression checks, including success and expected-failure
  cases, and run `make smoke`.

Read the [developer documentation](docs/developer/index.md) before changing engine internals.
