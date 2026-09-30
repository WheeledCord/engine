# Testing and regressions

Run `make smoke` for every engine change. It runs the gameplay and headless regression checks, the engine-build tool tests and `make docs-check`, and needs no display. `make integration` runs the graphical core check and the regression runner check, which need a working X display; `make full-check` runs both.

Add a regression check for every fixed bug. New features need success coverage and expected-failure coverage where they validate input or capability requirements. A sample project demonstrates real authoring use; it does not replace an automated assertion.

The engine currently has no CI. When CI is added, these two commands are the minimum required jobs.

The regression suite includes a headless run: `tests/regression/headless_checks.c` drives `EngineRunApplication` with `headless` and `maxTicks` set (no window of its own), checks the tick counters and timings, and steps a loopback network session to check its byte counters.

Before a commit, git can also replay the recorded sessions under `sessions/` and compare their hashes, and it refuses changes to the store and the recordings unless the owner has approved them. That pre-commit hook is specified in [hooks.md](hooks.md). It is off until the owner runs `make hooks`; `make all` and `make smoke` never turn it on.
