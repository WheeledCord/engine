# The pre-commit hook and protected paths (proposal A8, Part E question 7)

The proposal's rule for agent work: the gates are git hooks and protected paths, not engine
mechanisms. This is their specification.

## What the hook does

`tools/hooks/pre-commit`, a POSIX shell script, run by git before every commit once installed:

1. **Protected paths.** If the commit touches any path listed in `tools/hooks/protected.txt`
   (glob patterns, one per line), it is refused unless the environment variable
   `TRENCH_APPROVED` is set to the commit's staged tree hash (`git write-tree`), which the owner
   prints with `tools/hooks/approve` after reading the diff. A change that touches a protected path
   and any other path is refused even with approval unless `TRENCH_APPROVED_MIXED=1` is also set,
   because a change that edits code and the recordings that check it together is exactly what the
   gate exists to stop (A8). The refusal names each protected path touched and says how to approve.
2. **Build.** `make all` and `make -f Makefile.core build/core/trench`; any failure refuses the
   commit.
3. **Replays.** For every recorded session under `sessions/` (`sessions/<game>/<name>.replay` with
   `<name>.hashes` beside it, the expected `--hash-every 60` output), run
   `build/core/trench run <game dir> --headless --replay <file> --hash-every 60` and compare its
   output with the `.hashes` file line for line. Any difference refuses the commit and names the
   session and the first tick that differs. `sessions/index.txt` maps each session directory to its
   game directory (`swat-tower examples/swat-tower`).
4. Nothing opens a window; a commit that touches only `docs/` skips steps 2 and 3.

## Protected paths (the defaults from question 7)

```
tools/hooks/*
sessions/*
core/store.c
core/store.h
core/store_internal.h
core/store_save.c
core/store_net.c
core/store_net.h
```

## Installing, and who decides

`make hooks` sets `git config core.hooksPath tools/hooks`. It is not run by `make all`: the owner
of the repository turns the gate on, because once on it also stops the agents that build the
engine from changing the store without the owner's approval, which is its purpose.

## Recording a session

`tools/hooks/record-session <game dir> <name> [trench flags]` records a headless bot session
(`--headless --bot --ticks 36000 --seed N --record` by default, ten minutes) into
`sessions/<game>/<name>.replay`, replays it once in a fresh process to write `<name>.hashes`, and
refuses to write if the two runs disagree. Adding or replacing a session is a protected change.

The first sessions: SWAT Tower single player, seed 11 (the go/no-go condition 2 session), and a
SWAT Tower co-op host recording from the three-process check.
