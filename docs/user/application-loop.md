# Create an application and game loop

This is for a game in C. A Scheme game's loop is the runner's (`trench run`, [Running a Scheme game](kinds.md)); a C game on the store ticks the store from its own `Update`, as Trenchfoot does.

An application supplies an `EngineApplication` descriptor. `EngineRunApplication` owns window lifetime and invokes the callbacks you provide: `Init`, `FrameInput`, zero or more fixed `Update` calls, `Draw`, and `Shutdown`.

```c
#include "core/engine.h"

static bool Update(void *context, double dt, const EngineInput *input)
{
    (void)context;
    (void)dt;
    return !input->pressed[KEY_ESCAPE];
}

EngineApplication EngineApplicationMain(int argc, char **argv)
{
    (void)argc; (void)argv;
    EngineApplication app = EngineApplicationDefault();
    app.callbacks.Update = Update;
    return app;
}
```

`EngineConfigDefault` sets `config.windowFlags = FLAG_VSYNC_HINT`, so a window waits for the display's refresh unless a project replaces the flags without it.

Use `Update` for simulation and `Draw` for rendering. With a positive `fixed_dt`, one rendered frame can run zero or more updates; `Draw` receives interpolation alpha and must not advance game time. Read the [EngineApplication XML reference](../api/core/EngineApplication.xml) for callback lifetime and routing details.

## Running without a window

Set `config.headless = true` to run the same callbacks with no window, no OpenGL, no audio and no drawing: for a server, a test, a benchmark or a replay. A headless run skips window creation, the capability check, frame pacing and the exit key, never calls `FrameInput` or `Draw`, and refuses a `BuildUi` (`EngineRunApplication` returns nonzero). `Init` and `Shutdown` still run, `CoreSetDataRoot` still runs, and `EngineRunningConfig` works. `fixed_dt` must be positive: the loop calls `Update(context, fixed_dt, &input)` back to back with an empty `EngineInput` and never sleeps, so it runs as fast as `Update` allows. It stops when `Update` returns false.

`config.maxTicks` limits the number of `Update` calls in either mode; zero means no limit. A headless run with `maxTicks = 100` returns after exactly 100 updates. `CoreDiagnosticsCurrent()` reports `ticks` and the per-tick timings (`tickMicrosLast`, `tickMicrosMax`, `tickMicrosTotal`) in both modes, and the whole-frame timings (`frameMicros*`) in the windowed loop.
