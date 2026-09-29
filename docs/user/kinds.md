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
relative to the project directory; a model file that is missing draws as a magenta 0.25 m cube and
a missing texture as mid-grey, each with one warning.

The window opens at 80% of the monitor at 16:9, with vsync. It captures the mouse while it has
focus; **Escape releases the mouse** (and captures it again), and never reaches the game as an
action. **F3** toggles a diagnostics overlay (tick and frame times, fps, draw counts, things, and
how many assets are missing). The last three errors raised in handlers show along the bottom of the
screen for five seconds, prefixed `game:`, besides being printed.

| Flag | Meaning |
|---|---|
| `--headless` | no window, GL or audio; ticks as fast as the CPU allows |
| `--ticks N` | stop after N ticks |
| `--seed S` | world seed (default: from the clock) |
| `--record FILE` | record the session's input and player commands |
| `--replay FILE` | replay a recording; refused when the game's kinds changed since it was made |
| `--hash-every N` | print `tick T hash H` every N ticks; the last tick's line is always printed |
| `--bot` | synthesize input from the seed: a random walk over the actions and the mouse |
| `--bench` | print tick (and, in a window, frame) microseconds p50/p99/max and the draw stats |
| `--save FILE` / `--load FILE` | save the world on exit / load one after the game file |
| `--present` | with `--headless` only: also run the `frame`, `-changed` and `draw-hud` handlers each tick, drawing nothing; `--bench` then prints the HUD calls per frame |
| `--shot-every N` | in a window only: save a screenshot every N ticks, printing `shot T PATH` |
| `--shot-dir DIR` | where the screenshots go, as `DIR/shot_<tick>.png` (default: the current directory) |

A recording replayed in a fresh process with the same build reaches the same hash at every tick:

```
trench run mygame --headless --bot --ticks 600 --seed 7 --record run.replay
trench run mygame --headless --replay run.replay
```

In a window, the terminal is a REPL on the running game (`(things 'player)`, `(inspect (game))`,
`(reload)`). REPL lines are not recorded.

The world is lit by the first `directional` light (or a default sun) over the first `ambient`
light's energy (or 0.3), with fog towards the clear colour; HUD text is drawn in the engine's bitmap
font. `rgba` takes integers or reals, rounded and clamped to 0-255, and every coordinate and size of
the `draw-*` calls may be a real. `play-sound` plays through the engine's audio, placed when given
`:at`.

Phase 1 limits: models draw in their rest pose; only one directional light counts.

The calls a game makes, `define-kind`, the built-in kinds and the rules are in
[the store design](../developer/store.md) (§3 and §5); the examples in the design repository's
`proposal.md` show whole games.
`tests/regression/runner_game.scm` is a minimal one.
