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
| `--shot-every N` | in a window only: save a screenshot every N ticks, printing `shot T PATH` |
| `--shot-dir DIR` | where the screenshots go, as `DIR/shot_<tick>.png` (default: the current directory) |
| `--no-time-limit` | let handlers run as long as they take (by default one running over 50 ms is stopped) |
| `--host PORT` | host a co-op session on that UDP port |
| `--join ADDRESS:PORT` | join one, such as `--join 192.168.1.20:7777` |
| `--print-field KIND FIELD` | at the end print `field KIND FIELD VALUE` for the first thing of KIND |
| `--print-count KIND` | at the end print `count KIND N`, derived kinds included |

With no display (over ssh, say) a windowed run exits at once with `Engine: no display; run with
--headless or under a display`.

A recording replayed in a fresh process with the same build reaches the same hash at every tick:

```
trench run mygame --headless --bot --ticks 600 --seed 7 --record run.replay
trench run mygame --headless --replay run.replay
```

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

Each machine can record its side (`--record`); replaying it opens no socket, takes whether it
hosted or joined from the recording (a `--host` or `--join` given with `--replay` is ignored, with
a note) and reaches the same hashes as the live run. `tests/regression/net_game/` is a minimal
co-op game.

In a window, the terminal is a REPL on the running game (`(things 'player)`, `(inspect (game))`,
`(reload)`). REPL lines are not recorded.

The world is lit by the first `directional` light (or a default sun) over the first `ambient`
light's energy (or 0.3), with fog towards the clear colour; HUD text is drawn in the engine's bitmap
font. `rgba` takes integers or reals, rounded and clamped to 0-255, and every coordinate and size of
the `draw-*` calls may be a real. `play-sound` plays through the engine's audio, placed when given
`:at`.

`(rebind! 'action "Key")` (from a `frame` or `draw-hud` handler) gives an action that key and answers `#t`, or the
name of the action that already has the key. `(binding 'action)` answers the action's key name, or `#f`; the keys
are saved in the project's `input.map`, which the runner reads at start.

Phase 1 limits: models draw in their rest pose; only one directional light counts.

The calls a game makes, `define-kind`, the built-in kinds and the rules are in
[the store design](../developer/store.md) (§3 and §5); the examples in the design repository's
`proposal.md` show whole games.
`tests/regression/runner_game.scm` is a minimal one.
