# Authoring example

Run from the repository root:

```
make -f Makefile.core run-authoring
```

WASD or arrow keys move the square in world space. Q/E rotate it; Space moves along its local arrow.
Click the text field and type: movement stops on the same frame
that focus is acquired. Click outside to resume. The window close button quits.

`main.c` is the complete application. The moving type supplies an ordinary struct, defaults, two
property declarations, and Spawn/Think/Draw callbacks. `start.scene` supplies instance overrides.
There is no manual world setup, model loading, camera, main loop or shutdown callback.

`EngineApplicationMain` returns a descriptor. It uses the optional standard entry point; an existing
project with its own main can instead call `EngineRunApplication` directly. The entire build declaration
in Makefile.core is:

```make
$(eval $(call core_project,authoring_demo,gameplay,entry))
```

The UI is built once before simulation and its drawing is submitted afterward. Labels and controls
use the existing theme, indents and window chrome. No render target or additional GPU requirements
are introduced. The example borrows the engine's external bitmap font; it does not package it.
