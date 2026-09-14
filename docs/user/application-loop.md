# Create an application and game loop

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

Use `Update` for simulation and `Draw` for rendering. With a positive `fixed_dt`, one rendered frame can run zero or more updates; `Draw` receives interpolation alpha and must not advance game time. Read the [EngineApplication XML reference](../api/core/EngineApplication.xml) for callback lifetime and routing details.
