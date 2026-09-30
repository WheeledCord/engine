# Running a Scheme game

A Scheme-only project is a directory with an `engine.project` file and a game file:

```
mygame/
  engine.project     name My Game
                     game main.scm
  main.scm           (define-kind game ...) and the rest of the game
```

`game <file.scm>` names the game file, relative to the directory; `name` titles the window. Run it
with the `trench` tool, which `make -f Makefile.core build/core/trench` builds and `make sdk` copies to
the SDK's `bin/`:

```
trench run mygame [flags]
```

`engine-new mygame --language scheme` (`tools/engine_new.py` in the engine, `engine-new` in an SDK)
writes a starter game to begin from: a `game` with a tilemap floor and lights that spawns a walker for
each player who joins, moved with WASD, and a HUD line. The SDK's `trench` finds the engine's prelude,
shaders and font in the SDK's `share/engine/`; the build tree's finds them under `core/`.

The runner declares the built-in 3D kinds, loads the game file, then spawns a `game` if the file did
not spawn one itself (a file with no `game` kind is refused). A game usually spawns its players from
`game`'s `start` handler; the local player is owner 1. Models, textures, sounds and images are named
relative to the project directory; a model file that is missing draws as a magenta 0.25 m cube
standing on the model's origin (on a character, an upright 0.5 x 1.6 x 0.5 m box standing on its
feet), and a missing texture as mid-grey, each with one warning.

A handler that runs longer than 50 ms is stopped, and the error names it and its `(on ...)` line:
`the tick handler of rusher #34 ran for over 50 ms and was stopped; the loop at swat-tower.scm:212
may never end`. What it did before it was stopped stays done. The clock is checked when the handler
calls a procedure of its own or an engine function; a loop that calls neither (only Scheme's
built-ins such as `+`) is not caught. `--no-time-limit` turns the limit off, for a debugging session.
**Ctrl+C** in the terminal ends the run as closing the window does (the recording is closed and
`--save` written); a second Ctrl+C kills the process at once.

The window opens at 80% of the monitor at 16:9, with vsync. It captures the mouse while it has
focus; **Escape releases the mouse** (and captures it again), and never reaches the game as an
action. **F3** toggles a diagnostics overlay (tick and frame times, fps, draw counts, things, and
how many assets are missing). The last three errors raised in handlers show along the bottom of the
screen for five seconds, prefixed `game:`, besides being printed.

| Flag | Meaning |
|---|---|
| `--headless` | no window, GL or audio; ticks as fast as the CPU allows (at 60 ticks a second in a live session) |
| `--ticks N` | stop after N ticks |
| `--seed S` | world seed (default: from the clock) |
| `--record FILE` | record the session's input, player commands and the packets that arrived |
| `--replay FILE` | replay a recording; refused when the game's kinds changed since it was made |
| `--hash-every N` | print `tick T hash H` every N ticks; the last tick's line is always printed |
| `--bot` | synthesize input from the seed: a random walk over the actions and the mouse |
| `--bot-until N` | with `--bot`: no input after tick N, so the world comes to rest |
| `--bench` | print tick (and, in a window, frame) microseconds p50/p99/max and the draw stats; in a session also `net sent X B/s received Y B/s` |
| `--save FILE` / `--load FILE` | save the world on exit / load one after the game file |
| `--present` | with `--headless` only: also run the `frame`, `-changed` and `draw-hud` handlers each tick, drawing nothing; `--bench` then prints the HUD calls per frame |
| `--shot-every N` | in a window only: save a screenshot every N ticks (on the first frame at or after each multiple, named by the multiple, since a frame can run several ticks), printing `shot T PATH` |
| `--shot-dir DIR` | where the screenshots go, as `DIR/shot_<tick>.png` (default: the current directory) |
| `--no-time-limit` | let handlers run as long as they take (by default one running over 50 ms is stopped) |
| `--host PORT` | host a co-op session on that UDP port |
| `--join ADDRESS:PORT` | join one, such as `--join 192.168.1.20:7777` |
| `--print-field KIND FIELD` | at the end print `field KIND FIELD VALUE` for the first thing of KIND |
| `--print-count KIND` | at the end print `count KIND N`, derived kinds included |
| `--print-draw-position KIND` | in a window, with each `--shot-every` screenshot print `draw-position KIND TICK X Y Z`: where the first thing of KIND was drawn (a socket's bone included) |
| `--skin-on-cpu` | in a window: skin every animated model on the CPU, as models of more than 24 bones always are (for comparing the two paths) |
| `--no-static-batch` | in a window: draw every `:static` model as itself instead of in its region's batch (for comparing the two) |
| `--repl-file FILE` | evaluate each line `TICK TEXT` of FILE at the REPL just before tick TICK, as if typed then (recorded like typed lines; ignored while replaying) |

With no display (over ssh, say) a windowed run exits at once with `Engine: no display; run with
--headless or under a display`.

A recording replayed in a fresh process with the same build reaches the same hash at every tick:

```
trench run mygame --headless --bot --ticks 600 --seed 7 --record run.replay
trench run mygame --headless --replay run.replay
```

## Models, animation and sockets

| Kind | Fields | What it does |
|---|---|---|
| `model` | `mesh` (file), `animation` (symbol), `animation-speed` (1), `spin`, `tint`, `for-owner`, `hidden-for-owner`, `viewmodel` | Draws each mesh of its file. With `:animation 'wave` a skinned glTF plays its clip `wave`, looping; setting `animation` to another name starts that clip from its beginning, and `animation-speed` scales the clock (2 is twice as fast, 0 holds the frame). No `animation`, or a name the file lacks (one warning per name), holds the rest pose. Models of up to 24 bones are skinned on the GPU, larger ones on the CPU. Animation is presentation only: it runs where things are drawn, never headless, and nothing in gameplay can read it. |
| `socket` | `bone` (name), `of` (child names, up to 4) | A node that follows a bone in drawing: `(child hand (socket :bone "hand.R" :of (arms body) :at (vec3 0.3 1 -0.4)))`. It follows the first model of `:of` drawn on this machine (so the first-person arms for their owner, the body for everyone else), or with no `:of` its parent model, and everything under it is drawn with it. Gameplay reads the socket at its own `:at`, the rest pose: `(world-position item)` of a held item does not wave with the hand, so a game plays the same on every machine and in a replay. With no bone, no animation or nothing drawn to follow, it is drawn at its `:at` too. |

## Static things, lights, viewmodels and sounds

| Kind or field | Fields | What it does |
|---|---|---|
| `:static #t` on any node | `static` | For things that never move: crates, lamps, rubble. From the first frame it is drawn, a static model, and every model under a static thing, is merged with the others standing in the same 8x8-cell region of the tilemap (or, off every tilemap, with all the others) into one mesh per material, so 200 crates of two tints on a floor draw as a few batches instead of 200 draws. Moving a batched thing afterwards, by writing its `position`, `rotation` or `scale` or those of any node above it, is an error on the machine that owns it, reported at the next frame: `position on crate #3 changed, but crate is :static; remove :static if it moves` (or `an ancestor of crate #3 moved it, ...` when a node above it moved); the write stands (so the game plays the same headless and in a replay) and the thing is drawn as itself from then on. Other machines see their owner's move without an error and batch the thing again where it now stands. Removing one rebuilds its region's batch at the next frame. Models that animate, `spin`, have a `cull-distance` or are `viewmodel`s are not batched. |
| `light` | `type` (`'ambient`, `'directional`, `'point`), `energy` (1), `color` (1 1 1), `range` (10) | The first `ambient` light's energy lights everything evenly (0.3 without one); the first `directional` light shines the way its rotation turns (0 -1 0) (a default sun from above without one: a dark scene uses a directional light of energy 0); `'point` lights shine from where they are, per pixel, falling off as `energy * max(0, 1 - d / range)^2`. Of the visible point lights whose range reaches into the view, the four nearest the camera light the frame; the rest wait until they are among those four. Skinned models are lit the same way. |
| `camera` | `fov` (75), `for-owner`, `viewmodel-fov` (60) | `:for-owner #t` makes it the local player's view. `viewmodel-fov` is the vertical field of view, in degrees, of that player's viewmodels. |
| `model` with `:viewmodel #t` | `viewmodel`, `for-owner`, `hidden-for-owner` | For its owner it is drawn after the world, from the camera's place with the camera's `viewmodel-fov`, between 0.01 and 10 m and over a cleared depth buffer, so a gun held against a wall never goes into it. On other machines it is drawn like any model; give it `:for-owner #t` to draw it only for its owner, and give the third-person copy `:hidden-for-owner #t`. |
| `sound` | `stream` (file), `volume` (1), `playing` | While `playing` is `#t`, loops `stream` (a `.wav` or `.ogg` beside the project, loaded whole) from where the thing is: at `volume` next to the camera, half at 15 m, silent from 30 m, panned to the side of the camera it is on. Setting `playing` to `#f` stops it; removing the thing stops it. Headless, and on a machine with no audio device, nothing plays (a missing file, or no device, is one warning per sound thing). |

## Co-op

One machine hosts and the others join, from the command line or from the game:

```
trench run mygame --host 7777                  # player 1, and owner of the world
trench run mygame --join 192.168.1.20:7777     # players 2 to 16, one machine each
```

`(host-game port)` and `(join-game "address" port)` do the same while the game runs. They come
from a key press, so call them from a `frame` handler or the REPL; a gameplay handler that calls
them is an error, because gameplay code runs again on replay and on other machines. `(local-player)`
is this machine's player (1 on the host; a client's id once the host has welcomed it) and
`(players)` lists everyone in the session.

A game needs nothing more for co-op than to spawn each player's things in `game`'s
`player-joined` handler, `:owner` that player:

```scheme
(on (player-joined p)
  (spawn 'soldier :owner p :at (spot-for p)))
```

The engine sends `player-joined` for the host's own player at start and for each client as it
joins; a joining machine spawns no `game` of its own and gets the host's world. When a player
leaves, the things they spawned go and `game` hears `player-left`. Every handler runs on the
machine that owns its thing; `play-sound` and `burst` from gameplay code are shown on every machine.
A client whose game declares other kinds than the host's is refused with the kinds that differ
(`kinds differ: crate (missing on the host)`); that, a join the host never answers within 5 s, and
the host leaving each print `run: the session ended: <why>` and end the run with exit status 1.
Ctrl+C on a client leaves the session cleanly. A headless host that reaches its `--ticks` waits up
to 5 s for its clients to finish theirs before it leaves.

**Carrying things.** A thing is owned by whoever owns the root of the tree it hangs from, so picking
something up is `(attach! item (child soldier 'hand) :at (vec3 0 0 0))` and putting it down is
`(detach! item :at point :up normal :yaw y)`; only the item's owner may do either. A free item is
the host's, so a player asks for it with a message (`(send item 'grab self)`), and the item's `grab`
handler, on the host, attaches it and answers; from then on the holder's machine runs the item's
handlers and its drop is instant. `(first-child hand)` is what a hand holds. A `socket` child
(`(child hand (socket :bone "hand.R" :of (arms body)))`) is where a held thing hangs; it is drawn
on the hand bone while the model animates, and gameplay reads it at its own `:at`. When a holder's player leaves, or the host removes a
holder, or a client removes its own, the item is detached where the hand was, becomes the host's,
and its `(on (orphaned))` runs there, once, so it can seat itself on the floor. `(on (parent-changed was now))` runs on every machine
(presentation; `was` is `#f` when the thing first appears) for pickup and drop sounds.
`(aimed-at 'kind distance)` from a handler looks through the first camera under its thing and finds
areas by their shape (a sphere of `:radius`, or `(is area :shape (box 0.4 0.3 0.4))` for a box), `raycast` takes `:ignore (list self item)`, and `(draw-ring x y r fill)` draws
a hold ring. The whole case is `examples/carried-item/`; `tests/regression/net_carry/` runs it on
three machines.

Each machine can record its side (`--record`); replaying it opens no socket, takes whether it
hosted or joined from the recording (a `--host` or `--join` given with `--replay` is ignored, with
a note) and reaches the same hashes as the live run. `tests/regression/net_game/` is a minimal
co-op game.

In a window, the terminal is a REPL on the running game (`(things 'player)`, `(inspect (game))`,
`(reload)`). REPL code runs outside handlers, so the rules do not stop it, with one exception: in a
co-op session, writing a field of a thing another player owns is refused (`hp on probe #1 belongs
to player 2, and this REPL runs for player 1, so it can't write it ...`), because that player's
machine would overwrite it at once; send the thing a message instead.

**`(reload)`** loads the game file again and swaps in its handlers and helpers. A kind whose fields
changed is migrated: every thing of that kind, and of kinds that extend it, keeps each field whose
name and type are unchanged, a new field starts at its default, a removed field is dropped, and a
field whose type changed (a real that became an integer, say) starts at its new default. Each
dropped or reset field is printed once per kind, such as `STORE: kind unit declared again: field tag
was removed; 3 thing(s) dropped it`. A file that fails to load keeps the old handlers, and puts the
kinds it had changed, and the world, back as they were. A snapshot taken with `(snapshot)` before a kind changed can no longer be
restored. A kind's base can't change while the game runs, and `define-kind` typed at the REPL may
not change a kind's fields (edit the file and `(reload)`). In a co-op session every machine must
declare the same kinds, so a reload there that changes a kind's fields or adds a kind is refused
with a message saying to restart the session; a reload that changes only handlers and helpers is
fine.

**The REPL in recordings.** Each REPL line is recorded, with the tick it ran before, so a replay
evaluates it again at the same point (its answer is not printed) and reaches the same hashes. For a
REPL that types at known ticks, `--repl-file FILE` reads lines `TICK TEXT` (such as `30 (set!
((game) 'score) 42)`) and evaluates each just before tick TICK, printing `repl: ANSWER`; it is
ignored while replaying, since the recording holds the lines. A line whose output depends on a file
it reads, such as `(reload)` after the game file changed again, replays as the file is now.

The world is lit by the first `directional` light (or a default sun) and up to four point lights
over the first `ambient` light's energy (or 0.3), with fog towards the clear colour (see the table
above); HUD text is drawn in the engine's bitmap
font. `rgba` takes integers or reals, rounded and clamped to 0-255, and every coordinate and size of
the `draw-*` calls may be a real. `play-sound` plays through the engine's audio, placed when given
`:at`.

`(rebind! 'action "Key")` (from a `frame` or `draw-hud` handler) gives an action that key and answers `#t`, or the
name of the action that already has the key. `(binding 'action)` answers the action's key name, or `#f`; the keys
are saved in the project's `input.map`, which the runner reads at start.

Phase 1 limits: only one directional light and the four nearest point lights count; there are no
shadows.

The calls a game makes, `define-kind`, the built-in kinds and the rules are in
[the store design](../developer/store.md) (§3 and §5); the examples in the design repository's
`proposal.md` show whole games.
`tests/regression/runner_game.scm` is a minimal one.
