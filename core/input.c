/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "input.h"
#include "raymath.h"
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

static void Filter(EngineInput *input, EngineInputCapture capture)
{
    if (capture.keyboard)
    {
        memset(input->down, 0, sizeof input->down);
        memset(input->pressed, 0, sizeof input->pressed);
        memset(input->released, 0, sizeof input->released);
        input->textCount = 0;
    }
    if (capture.mouse)
    {
        memset(input->mouseDown, 0, sizeof input->mouseDown);
        memset(input->mousePressed, 0, sizeof input->mousePressed);
        memset(input->mouseReleased, 0, sizeof input->mouseReleased);
        input->mouseDelta = (Vector2){0};
        input->wheel = 0;
    }
}

void EngineInputRoute(EngineInput *pending, const EngineInput *frame, EngineInputCapture capture)
{
    if (!pending || !frame)
        return;
    EngineInput filtered = *frame;
    Filter(&filtered, capture);
    Filter(pending, capture);
    EngineInputAccumulate(pending, &filtered);
}

InputActionState InputActionRead(const EngineInput *input, InputAction action)
{
    InputActionState result = {0};
    if (!input || !action.bindings)
        return result;
    bool wasDown = false, pressed = false, released = false;
    for (size_t i = 0; i < action.count; i++)
    {
        InputBinding binding = action.bindings[i];
        bool d = false, p = false, r = false;
        if (binding.type == INPUT_KEY && binding.code > KEY_NULL && binding.code < CORE_KEY_COUNT)
        {
            d = input->down[binding.code];
            p = input->pressed[binding.code];
            r = input->released[binding.code];
        }
        else if (binding.type == INPUT_MOUSE_BUTTON && binding.code >= 0 &&
                 binding.code < CORE_MOUSE_BUTTON_COUNT)
        {
            d = input->mouseDown[binding.code];
            p = input->mousePressed[binding.code];
            r = input->mouseReleased[binding.code];
        }
        result.down |= d;
        wasDown |= (d || r) && !p;
        pressed |= p;
        released |= r;
    }
    result.pressed = pressed && !wasDown;
    result.released = released && !result.down;
    return result;
}

float InputAxis(const EngineInput *input, InputAction negative, InputAction positive)
{
    return (float)InputActionRead(input, positive).down -
           (float)InputActionRead(input, negative).down;
}

Vector2 InputVector(const EngineInput *input, InputAction left, InputAction right, InputAction up,
                    InputAction down)
{
    Vector2 v = {InputAxis(input, left, right), InputAxis(input, up, down)};
    return Vector2LengthSqr(v) > 1 ? Vector2Normalize(v) : v;
}
