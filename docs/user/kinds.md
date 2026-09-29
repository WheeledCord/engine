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
relative to the project directory; a model file that is missing draws as a 0.5 m cube, with one
warning.

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

A recording replayed in a fresh process with the same build reaches the same hash at every tick:

```
trench run mygame --headless --bot --ticks 600 --seed 7 --record run.replay
trench run mygame --headless --replay run.replay
```

In a window, the terminal is a REPL on the running game (`(things 'player)`, `(inspect (game))`,
`(reload)`). REPL lines are not recorded.

Phase 1 limits: models draw unlit with raylib's default shader (no lighting or fog yet) and in their
rest pose; `play-sound` ignores `:at`; the mouse is not captured.

The calls a game makes, `define-kind`, the built-in kinds and the rules are in
[the store design](../developer/store.md) (§3 and §5); the examples in the design repository's
`proposal.md` show whole games.
`tests/regression/runner_game.scm` is a minimal one.
