# Scripting

Games are scripted in Scheme, through the s7 interpreter. The script-facing API is described once,
as data, in `script_api.def`: name, argument types, result type, and the C function behind it. The
Scheme frontend is a loop over that table plus the conversions Scheme needs — it names no engine
function of its own. Adding a call is one row.

```
script_api.def   every call a script can make, one row each
script_api.c     the implementations, and the table built from the same rows
script.c         scripted entity classes and the callbacks the world sees
script_s7.c      Scheme: trampolines generated from the same rows, and a REPL
```

## The table

```c
SCRIPT_BINDING(move_world, "move-world!", SCRIPT_NONE, "move along the world axes",
               (SCRIPT_NONE, SCRIPT_VECTOR2))
```

One macro emits both the C prototype and the table row, so an implementation cannot drift from what
the table says it is. Types are `none bool int float vector2 vector3 string entity`. Every call is
checked against its declaration — arity and types — before it runs, so the frontend never has to
trust what a script passed it. The argument list is parenthesised and starts with `SCRIPT_NONE` only because
C99 cannot pass an empty one; the sentinel is dropped when the table is built.

## Scripted entities

A scripted class is an ordinary `EntityClass` whose Spawn, Think, Draw and Destroy call functions the
script named, so the world cannot tell it from a class written in C. Each one carries a transform,
where it was a step ago so drawing can interpolate, and the fields it declared. Those fields are
`EntityField` metadata like any other, which is what lets a scene file place and configure a scripted
entity without the script being involved:

```
entity "mover" {
    "position" "360 260"
    "speed" "220"
}
```

A scripted class brings its own size and alignment when it registers, like any other class, so a
project does not have to reserve room for classes that do not exist yet.

Scripts can coordinate their own entities without a game-specific C call: `find-first` and
`find-next` walk one named class, while `entity-position`, `set-entity-position!`,
`entity-get`, `entity-set-number!`, `entity-get-vector`, and `entity-set-vector!` access a live
scripted entity named by its handle. A stale handle, an unknown field, or a handle for a C-only
entity is safe: reads return zero values and writes do nothing. The API deliberately does not expose
C payloads to scripts.

## Scheme

An application can register a scripted Mover (or any other class) with no game-specific C code that
knows its behaviour. A running application may also evaluate expressions against its own world, so
`(spawn "mover" (vec 200 200))` can create another actor when that is part of the project's tooling.

`define-entity`, `vec` and the other conveniences are Scheme written in the prelude, on top of the
`class-*` rows; the engine's side of it is still only the table.

## Small queries

Beside the entity calls, the table carries the generic queries a callback keeps reaching for:
`random-int`, the vector questions (`vec-length`, `vec-distance`, `vec-normalize`) that raymath
answers everywhere else in the engine, `text-width` so drawn text can be centred, and `sprite-size`,
which names a sheet the way `draw-sprite` does and reports one frame's size in pixels — a zero
vector for a sheet nobody has loaded. All of these are ordinary rows, with the same type checking
as anything else.

## Resource handles

Caller-owned engine services — cameras, audio, collision worlds, pathfinders — are exposed through
a generational handle system. A script receives an opaque integer from `camera-create` or
`collision-create`, passes it to the operations on that kind, and releases it with
`camera-destroy`. A stale handle (from a previous destroy) or a handle of the wrong kind is
rejected: operations return zero values and writes do nothing, just as stale entity handles do.
No raw pointer or platform handle ever reaches a script.

The pools live in `ScriptHost` and are freed by `ScriptHostFree`. Call `ScriptHostUpdate(host, dt)`
once per rendered frame from the project's Draw callback: it pumps music streams and ages and
draws the debug queue, which must draw inside BeginDrawing/EndDrawing.

| Kind | Create | Operations | Destroy |
| --- | --- | --- | --- |
| Camera2D | `camera-create` | position, zoom, follow, world-to-screen, screen-to-world, interpolated, viewport | `camera-destroy` |
| Audio | `audio-create` | add-bus, bus-volume!, bus-muted!, play-sound, play-music, stop-music | `audio-destroy` |
| Collision | `collision-create` | add-circle, add-aabb, remove, move-circle, move-aabb, query-circle, query-aabb, sweep-circle | `collision-destroy` |
| Pathfinder | `pathfinder-create` | block!, unblock!, blocked?, solve, path-length, path-get | `pathfinder-destroy` |
| Mover | `mover-create` | go-to, truncate!, remaining, stop!, moving?, update!, screen, hex | `mover-destroy` |

A pathfinder's blocked hexes are the answer `pathfinder-block!` gives `IsoBlockedFn`, so `solve`
and `mover-go-to` route around exactly what a script marked; the solved route stays with the
pathfinder for `pathfinder-path-length` and `pathfinder-path-get` to read. A mover holds its own
route and clock, and a script advances it with `mover-update!` from its think callback.

## Saving, and what a script sees while it runs

`save-set-number!`/`save-set-string!` remember values under keys, `save-get-number`/`save-get-string`
read them back, and `save-write`/`save-read` round-trip them through a file with the engine's own
atomic save: a failed or interrupted write leaves the old file intact. `debug-line`,
`debug-circle`, `debug-rect` and `debug-text` queue primitives that stay on screen for as many
seconds as asked; they appear and expire through `ScriptHostUpdate`. Mouse state joins the
keyboard through `mouse-position`, `mouse-delta`, `mouse-wheel`, `mouse-down?` and `mouse-pressed?`.

`InputMap` is deliberately not bound: its public API takes an up-front definition table and its
rebind dialog needs a `UiContext`, so a script would be driving C UI through a pointer. The raw
key, mouse and action bindings cover what a script asks input for; rebinding stays with the
project's own code, where the UI already lives.

## A game's own calls

The engine's rows are fixed when the engine is built, but a game adds its own at runtime, and they
are indistinguishable from the engine's afterwards: checked against their declaration like any other
row.

```c
SCRIPT_CALL(grapple, "grapple!", SCRIPT_FLOAT, "fire a grapple at a point",
            (SCRIPT_NONE, SCRIPT_VECTOR2))
{
    return ScriptFloat(Grapple(host, a[0].as.vector2));
}
...
ScriptAddBinding(&host, &grapple_binding);   // before opening a frontend
```

One macro writes the function and the row together, as in the engine's own table, so the two cannot
drift. Add them before opening the Scheme frontend, which registers every row when it opens.

The frontend keeps a small pool of trampolines for calls that did not exist when it was compiled,
because an s7 function registered from C carries nothing of its own to say which row it is.

## Adding a call to the engine

Add the row to `script_api.def` and give it an implementation in `script_api.c`. Scheme has it on
the next build, and `make script-api` writes the reference for it into `build/core/`. Exercise it
through `ScriptInvoke` in the regression checks, with a success and an expected-failure case.

## Bindings deliberately omitted

`CoreNetSyncObserve` and `CoreNetSyncSerialize` take a C function pointer that the engine calls in
both directions. A script cannot be handed one, and inventing a per-language callback registry to
fake it would be a second binding mechanism beside the table -- the thing this file exists to avoid.
Scripts get the state itself instead: the clocks are bound, and a replicated object's fields are
reachable through the schema. `CoreNetObjectIsMine` is bindable and should be bound when the
replicated-object handles are.

`CoreFillUVBackground` and the rest of `core/uv_bake.h` are offline tooling: they run in the texture
baker, before a game exists, and have no meaning at runtime.
