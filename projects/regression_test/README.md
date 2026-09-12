# Regression checks

One check per bug that has been found and fixed in the engine, so none of them come back unnoticed.
Every check asserts on a value the program reads back: a pixel out of the framebuffer, a resolved
rectangle, a return code.

```sh
./build/core/regression_test           # everything that shares one window
./build/core/regression_test --runner  # what needs the engine's own loop and window
```

Both run as part of `make -f Makefile.core smoke`.

What is covered: frame textures surviving a model draw, input not reaching clipped or disabled
widgets, the keyboard being released when a field stops being drawn, presses surviving a relabel,
containers drawing over and clipping their contents, stacks dividing along the axis they were laid
out in whatever order they were added, shrinking that stops at each minimum without overlapping,
layouts refusing to load when a line is damaged, saves that leave the previous file intact when they
fail, documents reporting which element was used, one forward convention across the camera and the
transform helpers, the deferred UI drawing the same pixels as the immediate one, and gameplay
ticking at the rate the engine actually runs.
