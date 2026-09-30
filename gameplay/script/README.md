# Scripting

Games are written in Scheme, through the s7 interpreter, against the store: a game declares its kinds
of thing with `define-kind`, and the engine runs their handlers. `docs/user/kinds.md` is how a game
author runs one; this page is how the frontend is put together.

`game_s7.c` is how a game written in Scheme reaches the store (`core/store.h`,
`docs/developer/store.md` §5). There is one interpreter per process. `GameS7Open` loads the prelude
`core/scheme/kinds.scm` (`define-kind`, its code walk, `define-actions`, `clamp`, `map-for-each`); `GameS7LoadGame` loads a game file into a fresh
environment under the rootlet and then freezes it, so a top-level `set!` fails with rule 2's
message; `GameS7Reload` loads it again into another environment and swaps the handlers in,
migrating a kind whose fields changed with `StoreRedeclareKind` (its things and those of derived
kinds keep the fields whose name and type are unchanged); a file that fails declares those kinds
back and restores the world, and in a networked session (`GameS7Network.session`) a reload that
changes a kind's fields or adds a kind is refused. `GameS7Eval` is the REPL; in a networked session
it refuses writes to things this machine does not own with rule 5's message.

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
  `load-game`, `snapshot`, `restore`. `local-player`, `players`, `host-game` and `join-game` answer
  from the runner's network (`GameS7SetNetwork`; without it: player 1, `(1)`, and host-game and
  join-game raise that they need the runner); host-game and join-game are refused in gameplay
  handlers.
- A handler that raises is reported as `<kind> #<index> <event>: <message> (<file>:<line>)` at most
  once per kind and event per second, and the game carries on. The five rules' messages are
  proposal A4's.

Other modules add their calls the same way, with
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

A call a game makes is a C function registered with `GameS7Define` (or `GameS7DefineTyped` when it
never calls back into Scheme), beside the module it belongs to, as world3d and the runner register
theirs. Exercise it from the regression checks (`tests/regression/game_checks.c` for the frontend,
`runner_checks.c` for a whole game), with a success and an expected-failure case.
