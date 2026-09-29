# Scripting

Games are scripted in Scheme, through the s7 interpreter. A script reaches the engine two ways, and
both are declared once, as data:

- **Engine objects** — scripted entities, cameras, timers, audio, collision worlds, pathfinders,
  movers, network clocks, textures, and anything a game adds. Each type is a table of properties,
  methods and signals written beside the type itself (`core/object.h`). A script makes one by name,
  reads and writes its properties, calls its methods and listens to its signals, all the same way.
- **Free-standing calls** — drawing, input, time, the hex grid, small math, saving, debug drawing.
  These are rows of `script_api.def`: name, argument types, result type and the C function behind it.

The Scheme frontend is a loop over both. It names no engine function of its own.

```
script_api.def   the free-standing calls, one row each
script_api.c     their implementations, and the table built from the same rows
script.c         scripted entity classes, the entity type, and the host that owns every object
script_s7.c      Scheme: objects as Scheme values, trampolines for the rows, and a REPL
```

The reference for every object type is written from the type tables by `make script-api` into
`build/core/script-objects.md`; the rows' reference goes beside it in `script-api.md`.

## Objects

```scheme
(define cam (make 'camera2d))        ; make one by name
(set! (cam 'zoom) 2)                 ; write a property
(cam 'zoom)                          ; read it: 2.0
(cam 'follow! (vec 10 20) (dt))      ; call a method
(define t (make 'timer 0.5))         ; creation arguments, checked like any others
(connect! t 'timeout (lambda () (log "tick")))  ; listen to a signal
(free! cam)                          ; end it; the wrapper Scheme holds is refused afterwards
```

Arguments and values are checked against what the type declared, and a mistake is a Scheme error
that says what was wrong: an unknown property, a read-only one, a value of the wrong type, an object
that has already ended. Nothing quietly answers zero. `(properties obj)` and `(methods obj)` list
what an object has, its parents' members included; `(object-type obj)` names its type;
`(alive? obj)` asks whether it still exists.

The host owns every object's life. Scheme holds only a handle, so letting go of a value in Scheme
frees nothing, and a value kept past its object's end is refused rather than reaching whatever took
its place. `ScriptHostStep(host, dt)`, called once per fixed update, advances the objects that move
on their own — timers count down, movers walk, audio streams are fed — and their signals fire from
there.

## Scripted entities

A scripted class is an ordinary `EntityClass` whose Spawn, Think, Draw and Destroy call functions the
script named, so the world cannot tell it from a class written in C. Each callback is handed its
entity, as an engine object:

```scheme
(define (mover-think self)
  (self 'move-world! (vec* (input-vector) (* (self 'speed) (dt))))
  (self 'think-next!))

(define-entity "mover" '(("speed" "float"))
  '(("spawn" "mover-spawn") ("think" "mover-think")))
```

Every entity has what the `entity` type gives it — `position`, `rotation`, `classname`,
`drawn-position` and `drawn-rotation` for drawing between steps, and `destroy!`, `think-next!`,
`think-after!`, `rotate!`, `move-world!`, `move-local!`, `look-at!` and `to-local` — and its class adds
the fields it declared as properties of its own. Those fields are `EntityField` metadata too, which is
what lets a scene file place and configure a scripted entity without the script being involved:

```
entity "mover" {
    "position" "360 260"
    "speed" "220"
}
```

`spawn` answers the new entity, so a script sets it up directly; `find-first` and `find-next` walk a
class. A class cannot declare a field that would hide one every entity has, or the same field twice.
`(dt)` is one simulation step; `(elapsed)` is how long it has really been since this entity last
thought, for a class that thinks less often than every step.

`define-entity`, `vec` and the other conveniences are Scheme written in the prelude, on top of the
`class-*` rows.

## A game's own objects

A game hands scripts something of its own the same way the engine does: it describes the type, adopts
its own storage into the host, and names it.

```c
static const EngineType playerType = {.name = "player", .size = sizeof(Game),
                                      .properties = playerProperties, .propertyCount = 3,
                                      .methods = playerMethods, .methodCount = 3};
ScriptHostRegisterType(&host, &playerType);                  // (make 'player) would work too
ScriptS7DefineObject("player", EngineObjectAdopt(&host.objects, &playerType, &game));
```

`(player 'health)` and `(player 'hurt! 10 x z)` then read and change the game's own struct. The host
never frees adopted storage.

## A game's own calls

A game can also add free-standing calls at runtime, indistinguishable from the engine's rows and
checked against their declaration like any other:

```c
SCRIPT_CALL(grapple, "grapple!", SCRIPT_FLOAT, "fire a grapple at a point",
            (SCRIPT_NONE, SCRIPT_VECTOR2))
{
    return ScriptFloat(Grapple(host, a[0].as.vector2));
}
...
ScriptAddBinding(&host, &grapple_binding);   // before opening the frontend
```

One macro writes the function and the row together, so the two cannot drift. The frontend keeps a
small pool of trampolines for calls that did not exist when it was compiled, because an s7 function
registered from C carries nothing of its own to say which row it is.

## Saving, and what a script sees while it runs

`save-set-number!`/`save-set-string!` remember values under keys, `save-get-number`/`save-get-string`
read them back, and `save-write`/`save-read` round-trip them through a file with the engine's own
atomic save: a failed or interrupted write leaves the old file intact. `debug-line`,
`debug-circle`, `debug-rect` and `debug-text` queue primitives that stay on screen for as many
seconds as asked; `ScriptHostUpdate`, called from Draw, draws and ages them. Mouse state joins the
keyboard through `mouse-position`, `mouse-delta`, `mouse-wheel`, `mouse-down?` and `mouse-pressed?`.

`InputMap` is not bound yet: its public API takes an up-front definition table and its rebind dialog
needs a `UiContext`. The raw key and mouse calls cover what a script asks input for until it is
described as an engine type.

## The store's frontend (`game_s7.c`)

A second, separate Scheme frontend runs games written against the store (`core/store.h`,
`docs/developer/store.md` §5). It has its own interpreter and leaves `script_s7.c` and the binding
table alone. `GameS7Open` loads the prelude `core/scheme/kinds.scm` (`define-kind`, its code walk,
`define-actions`, `clamp`, `map-for-each`); `GameS7LoadGame` loads a game file into a fresh
environment under the rootlet and then freezes it, so a top-level `set!` fails with rule 2's
message; `GameS7Reload` loads it again into another environment and swaps the handlers in, refusing
when a kind's fields changed; `GameS7Eval` is the REPL.

- A thing is a c-object whose value word is its store handle, one object per live handle, so `eq?`
  works. `(t 'field)` reads a declared child, then a field; `(set! (t 'field) v)` writes through
  the store, which applies the rules. A map field reads as a view (`(ammo 'pistol)`, `map-keys`,
  `map-values`, `map-remove!`, `map-for-each`), a grid as a view (`grid-ref`, `grid-set!`,
  `grid-fill!`, `grid-fill-rect!`, `grid-width`, `grid-height`), a list or set as a fresh list.
- Inside a kind's handlers and helpers, fields, children (nested ones too) and helpers are plain
  names; the walk turns them into `(%field self i)`, `(%set-field! self i v)`, `(%child self slot)`
  and `((%helper-ref self 'name) self ...)`.
- Calls: `spawn` (`:at`, `:owner`, `:parent`, any field), `remove`, `attach!`, `detach!`, `parent`,
  `children`, `first-child`, `child`, `is?`, `kind-of`, `things`, `game`, `local-player`,
  `players`, `send`, `after`, `go`, `random`, `tick-time`, `held?`, `pressed?`, `input-vector`,
  `mouse-motion`, the vec3 calls (`vec3 vx vy vz v+ v- v* vscale vlength vdistance vnormalize vdot
  vcross rotate-y heading aim spread`), and at the REPL `inspect`, `reload`, `save-game`,
  `load-game`, `snapshot`, `restore`. `host-game` and `join-game` say networking comes in phase 2.
- A handler that raises is reported as `<kind> #<index> <event>: <message> (<file>:<line>)` at most
  once per kind and event per second, and the game carries on. The five rules' messages are
  proposal A4's.

None of these calls is a `script_api.def` row, and none is meant to become one: they take keyword
arguments (`(spawn 'soldier :at v :owner 2)`), things, map and grid views and vec3 float-vectors,
and the table has no types for any of those. Other modules add theirs the same way, with
`GameS7Define`, `GameS7DefineTyped` (for calls that never call back into Scheme) and
`GameS7DefineMethod`, converting with `GameS7Thing`, `GameS7ToId`, `GameS7ToValue`,
`GameS7FromValue`, `GameS7Vec3`, `GameS7ToVec3` and `GameS7KeywordArg`. The world calls
(`raycast`, `move-and-slide!`, `nearest` ...) and the presentation calls (`draw-text`,
`play-sound` ...) are registered that way by world3d and the runner; until they are, a handler that
calls one reports it as unbound.

A child declared `:local #t` is spawned and then made local (`StoreMarkLocal`), with its own
children: presentation handlers may write its fields and gameplay handlers may not read them. Saves
leave it out, and `(load-game path)` (or `GameS7RestoreLocalChildren` after a runner's `StoreLoad`)
spawns it again from the declaration.

Not yet: `:up` on `detach!` tilts by the normal's slopes and is exact only for an upright normal;
`:keep-world` needs a `world-position` call registered by world3d.

## Adding to the engine

- A thing a script makes, holds and changes is an engine type: describe its properties, methods and
  signals in a table beside it and register it in `ScriptHostInit`. Scheme has it at once, and
  `make script-api` documents it.
- A free-standing call is a row in `script_api.def` with its implementation in `script_api.c`.

Exercise either from the regression checks, with a success and an expected-failure case.

## Not reachable from scripts

`CoreNetSyncObserve` and `CoreNetSyncSerialize` take a C function pointer that the engine calls in
both directions, and replicated objects are not engine types yet, so networking beyond the clocks is
C-only for now.

`CoreFillUVBackground` and the rest of `core/uv_bake.h` are offline tooling: they run in the texture
baker, before a game exists, and have no meaning at runtime.
