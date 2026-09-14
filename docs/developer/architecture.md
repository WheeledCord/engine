# Architecture

```text
project code  -->  gameplay (optional)  -->  core  -->  raylib / system
                    |                         |
                    +-- scripting              +-- window, input, rendering, UI
```

`core/` contains reusable runtime mechanisms and may not include gameplay or project code. The build checks this boundary from compiler dependency output. `gameplay/` is optional and owns entities, scenes, fields, systems, and scripting integration. Projects own content and policy: game rules, collision and terrain decisions, input mappings, map size, and application-specific state.

The application loop samples frame input once, builds deferred UI/capture when configured, runs zero or more simulation updates, draws the world, then draws UI. Keep simulation advancement in `Update`; `Draw` only renders an interpolated view.
