# Testing and regressions

Run `make smoke` for every engine change. It builds all in-tree checks, runs the graphical core smoke project, gameplay tests, and regression checks. Run `make docs-check` after documentation or public API work.

Add a regression check for every fixed bug. New features need success coverage and expected-failure coverage where they validate input or capability requirements. A sample project demonstrates real authoring use; it does not replace an automated assertion.

The engine currently has no CI. When CI is added, these two commands are the minimum required jobs.
