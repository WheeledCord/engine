# Architecture

```text
project code  -->  gameplay (optional)  -->  core  -->  raylib / ENet / system
                    |                         |
                    +-- scripting              +-- window, input, rendering, UI
```

`core/` contains the engine's runtime systems and may not include gameplay or project code. The build checks this boundary from compiler dependency output. `gameplay/` builds entities, scenes, fields, systems, and scripting on top of core. The engine provides systems; games provide their rules and content. Some systems a game would expect from an engine do not exist yet: there is no physics or collision response and no transform hierarchy, and `core/collision2d.h` answers overlap and sweep queries without resolving movement. Until those systems exist, games that need them supply their own.

The application loop samples frame input once, builds deferred UI/capture when configured, runs zero or more simulation updates, draws the world, then draws UI. Keep simulation advancement in `Update`; `Draw` only renders an interpolated view.
