/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_INPUT_H
#define CORE_INPUT_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#define CORE_KEY_COUNT 512
#define CORE_MOUSE_BUTTON_COUNT 8
#define CORE_TEXT_INPUT_COUNT 32
typedef struct EngineInput
{
    bool down[CORE_KEY_COUNT], pressed[CORE_KEY_COUNT], released[CORE_KEY_COUNT];
    bool mouseDown[CORE_MOUSE_BUTTON_COUNT], mousePressed[CORE_MOUSE_BUTTON_COUNT],
        mouseReleased[CORE_MOUSE_BUTTON_COUNT];
    Vector2 mousePosition;
    Vector2 mouseDelta;
    float wheel;
    int text[CORE_TEXT_INPUT_COUNT];
    int textCount;
} EngineInput;
/* Merge one frame into pending input. Edges and deltas survive frames without Update. */
void EngineInputAccumulate(EngineInput *pending, const EngineInput *frame);
/* After the first Update: clear edges/deltas, preserve held state for further updates. */
void EngineInputDrain(EngineInput *pending);

typedef struct EngineInputCapture
{
    bool keyboard, mouse;
} EngineInputCapture;
/* Filters both newly polled and pending input; captured presses cannot fire later. */
void EngineInputRoute(EngineInput *pending, const EngineInput *frame, EngineInputCapture capture);

typedef enum InputBindingType
{
    INPUT_KEY,
    INPUT_MOUSE_BUTTON
} InputBindingType;
typedef struct InputBinding
{
    InputBindingType type;
    int code;
} InputBinding;
typedef struct InputAction
{
    const InputBinding *bindings;
    size_t count;
} InputAction;
typedef struct InputActionState
{
    bool down, pressed, released;
} InputActionState;
/* Action declarations borrow binding arrays. Read-only; no allocation or string lookup per query.
 */
InputActionState InputActionRead(const EngineInput *input, InputAction action);
float InputAxis(const EngineInput *input, InputAction negative, InputAction positive);
/* Digital vector with diagonal length limited to one. */
Vector2 InputVector(const EngineInput *input, InputAction left, InputAction right, InputAction up,
                    InputAction down);
#endif
