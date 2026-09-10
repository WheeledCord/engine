#include "engine.h"
#include "file.h"
#include <math.h>
#include <string.h>
void EngineInputAccumulate(EngineInput *p, const EngineInput *f)
{
    for (int i = 0; i < CORE_KEY_COUNT; i++)
    {
        p->down[i] = f->down[i];
        p->pressed[i] |= f->pressed[i];
        p->released[i] |= f->released[i];
    }
    for (int i = 0; i < CORE_MOUSE_BUTTON_COUNT; i++)
    {
        p->mouseDown[i] = f->mouseDown[i];
        p->mousePressed[i] |= f->mousePressed[i];
        p->mouseReleased[i] |= f->mouseReleased[i];
    }
    p->mousePosition = f->mousePosition;
    p->mouseDelta.x += f->mouseDelta.x;
    p->mouseDelta.y += f->mouseDelta.y;
    p->wheel += f->wheel;
    for (int i = 0; i < f->textCount && p->textCount < CORE_TEXT_INPUT_COUNT; i++)
        p->text[p->textCount++] = f->text[i];
}
void EngineInputDrain(EngineInput *p)
{
    memset(p->pressed, 0, sizeof p->pressed);
    memset(p->released, 0, sizeof p->released);
    memset(p->mousePressed, 0, sizeof p->mousePressed);
    memset(p->mouseReleased, 0, sizeof p->mouseReleased);
    p->mouseDelta = (Vector2){0};
    p->wheel = 0;
    p->textCount = 0;
}
static EngineInput PollInput(void)
{
    EngineInput f = {0};
    for (int i = 0; i < CORE_KEY_COUNT; i++)
    {
        f.down[i] = IsKeyDown(i);
        f.pressed[i] = IsKeyPressed(i);
        f.released[i] = IsKeyReleased(i);
    }
    for (int i = 0; i < CORE_MOUSE_BUTTON_COUNT; i++)
    {
        f.mouseDown[i] = IsMouseButtonDown(i);
        f.mousePressed[i] = IsMouseButtonPressed(i);
        f.mouseReleased[i] = IsMouseButtonReleased(i);
    }
    f.mousePosition = GetMousePosition();
    f.mouseDelta = GetMouseDelta();
    f.wheel = GetMouseWheelMove();
    int codepoint = 0;
    while (f.textCount < CORE_TEXT_INPUT_COUNT && (codepoint = GetCharPressed()) != 0)
        f.text[f.textCount++] = codepoint;
    return f;
}
int EngineRun(const EngineConfig *c, const EngineProject *p, void *context)
{
    if (!c || !p || !p->Init || !p->Update || !p->Draw || c->width <= 0 || c->height <= 0 ||
        !isfinite(c->fixed_dt) || c->fixed_dt < 0 || !isfinite(c->max_frame_dt) || c->max_frame_dt < 0)
        return 1;
    SetConfigFlags(c->windowFlags);
    InitWindow(c->width, c->height, c->title ? c->title : "Core");
    CoreSetDataRoot(c->engine_path ? c->engine_path : GetApplicationDirectory());
    if (!IsWindowReady())
        return 1;
    SetExitKey(KEY_NULL);
    SetTargetFPS(c->targetFps);
    if (!CoreCheckCapabilities(c->requirements))
    {
        CloseWindow();
        return 1;
    }
    if (!p->Init(context))
    {
        if (p->Shutdown)
            p->Shutdown(context);
        CloseWindow();
        return 1;
    }
    double last = GetTime(), accumulator = 0;
    EngineInput pending = {0};
    bool running = true;
    while (running && !WindowShouldClose())
    {
        double now = GetTime(), dt = now - last;
        last = now;
        if (c->max_frame_dt > 0 && dt > c->max_frame_dt)
        {
            TraceLog(LOG_WARNING, "Frame time clamped from %.3f to %.3f seconds", dt, c->max_frame_dt);
            dt = c->max_frame_dt;
        }
        EngineInput frame = PollInput();
        EngineInputAccumulate(&pending, &frame);
        if (p->FrameInput)
            p->FrameInput(context, &frame);
        float alpha = 1;
        if (c->fixed_dt > 0)
        {
            accumulator += dt;
            while (accumulator >= c->fixed_dt && running)
            {
                running = p->Update(context, c->fixed_dt, &pending);
                EngineInputDrain(&pending);
                accumulator -= c->fixed_dt;
            }
            alpha = (float)(accumulator / c->fixed_dt);
        }
        else
        {
            running = p->Update(context, dt, &pending);
            EngineInputDrain(&pending);
        }
        if (running)
        {
            BeginDrawing();
            p->Draw(context, alpha);
            EndDrawing();
        }
    }
    if (p->Shutdown)
        p->Shutdown(context);
    CloseWindow();
    return 0;
}
