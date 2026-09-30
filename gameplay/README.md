# Gameplay

`gameplay/` is the runner for games written in Scheme, on top of core. Core never includes it.

```
game.h   game.c          the runner: `trench run <dir>` reads engine.project, loads the game file
                         into a store with the built-in 3D kinds, and runs it windowed or headless,
                         with recording, replay, a bot, co-op, hashes and timing on request
script/game_s7.h  .c     the Scheme frontend: things as s7 values, define-kind and its handlers,
                         the five rules as errors, and the calls a game makes (script/README.md)
```

The runner is built as `build/core/trench` (`make -f Makefile.core build/core/trench`), and `make sdk`
copies it to the SDK's `bin/`. What a game author needs to run one is in
[docs/user/kinds.md](../docs/user/kinds.md); how the runner, the store, world3d and the draw path fit
together is in [docs/developer/store.md](../docs/developer/store.md) §6 and §9.6.

A game in C does not use this layer: it links the engine and uses the store (`core/store.h`,
`core/store_net.h`), world3d and the draw path directly, as Trenchfoot does.
