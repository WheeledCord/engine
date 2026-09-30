# Architecture

```text
Scheme game (a directory)  -->  gameplay: trench runner + game_s7  --+
                                                                     +-->  core  -->  raylib / ENet / s7 / system
C game (engine-build)      ------------------------------------------+
```

`core/` contains the engine's runtime systems and may not include gameplay or project code. The build checks this boundary from compiler dependency output. The world a game lives in is core's: the store (`core/store.h`: kinds, things in a tree, the ordered tick, the replay rules, snapshots, hashes and saves), networking on it (`core/store_net.h`, over ENet in `core/store_net_enet.h`), world3d (`core/world3d.h`: the built-in 3D kinds and the systems behind them), and the draw path (`core/draw_path.h`). `gameplay/` holds the runner, `trench run <dir>` (`gameplay/game.c`), and the Scheme frontend it runs a game with (`gameplay/script/game_s7.c`, with the prelude `core/scheme/kinds.scm`). A C game, such as Trenchfoot, links the engine and uses the store, world3d and the draw path directly from its own `EngineApplication`. [The store design](store.md) is the contract for all of these.

The engine provides systems; games provide their rules and content. world3d's characters collide and slide against boxes, and `core/collision2d.h` and `core/collision3d.h` answer overlap, sweep and ray queries; there is no physics simulation.

The application loop samples frame input once, builds deferred UI/capture when configured, runs zero or more simulation updates, draws the world, then draws UI. Keep simulation advancement in `Update`; `Draw` only renders an interpolated view. The runner ticks the store in `Update` and runs its presentation frame and draw path in `Draw`.
