# Testing and regressions

Run `make smoke` for every engine change. It runs the gameplay and headless regression checks, the engine-build tool tests and `make docs-check`, and needs no display. `make integration` runs the graphical core check and the regression runner check, which need a working X display; `make full-check` runs both.

Add a regression check for every fixed bug. New features need success coverage and expected-failure coverage where they validate input or capability requirements. A sample project demonstrates real authoring use; it does not replace an automated assertion.

The engine currently has no CI. When CI is added, these two commands are the minimum required jobs.
