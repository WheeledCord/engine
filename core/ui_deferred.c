/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "ui_internal.h"
#include <stdlib.h>
#include <string.h>

typedef enum UiCommandKind
{
    UI_RECT_COMMAND,
    UI_TEXT_COMMAND,
    UI_CUSTOM_COMMAND
} UiCommandKind;
struct UiCommand
{
    UiCommandKind kind;
    UiRect rect;
    UiClipState clip;
    Color color;
    size_t text;
    int fontSize, spacing;
    UiContentFn draw;
    void *user;
};

static struct UiCommand *Command(UiContext *ui)
{
    UiState *s = ui->state;
    if (s->commandFailed)
        return NULL;
    if (s->commandCount == s->commandCapacity)
    {
        size_t capacity = s->commandCapacity ? s->commandCapacity * 2 : 256;
        struct UiCommand *items = realloc(s->commands, capacity * sizeof(*items));
        if (!items)
        {
            s->commandFailed = true;
            return NULL;
        }
        s->commands = items;
        s->commandCapacity = capacity;
    }
    struct UiCommand *command = &s->commands[s->commandCount++];
    *command = (struct UiCommand){.clip = s->clip};
    return command;
}

void UiFill(UiContext *ui, UiRect rect, Color color)
{
    if (!ui || !ui->state || rect.width <= 0 || rect.height <= 0)
        return;
    if (!ui->state->deferred)
    {
        DrawRectangle(rect.x, rect.y, rect.width, rect.height, color);
        return;
    }
    struct UiCommand *command = Command(ui);
    if (command)
    {
        command->kind = UI_RECT_COMMAND;
        command->rect = rect;
        command->color = color;
    }
}

void UiQueueText(UiContext *ui, int x, int y, const char *text, Color color)
{
    UiState *s = ui->state;
    size_t length = strlen(text) + 1;
    if (length > SIZE_MAX - s->textCount)
    {
        s->commandFailed = true;
        return;
    }
    size_t needed = s->textCount + length;
    if (needed > s->textCapacity)
    {
        size_t capacity = needed > s->textCapacity * 2 ? needed : s->textCapacity * 2;
        char *buffer = realloc(s->commandText, capacity);
        if (!buffer)
        {
            s->commandFailed = true;
            return;
        }
        s->commandText = buffer;
        s->textCapacity = capacity;
    }
    struct UiCommand *command = Command(ui);
    if (!command)
        return;
    command->kind = UI_TEXT_COMMAND;
    command->rect = (UiRect){x, y, 0, 0};
    command->color = color;
    command->fontSize = ui->theme.fontSize;
    command->spacing = ui->theme.textSpacing;
    command->text = s->textCount;
    memcpy(s->commandText + s->textCount, text, length);
    s->textCount += length;
}

void UiDrawCustom(UiContext *ui, UiRect rect, UiContentFn draw, void *user)
{
    if (!ui || !ui->state || !draw)
        return;
    if (!ui->state->deferred)
    {
        draw(ui, rect, user);
        return;
    }
    struct UiCommand *command = Command(ui);
    if (command)
    {
        command->kind = UI_CUSTOM_COMMAND;
        command->rect = rect;
        command->draw = draw;
        command->user = user;
    }
}

void UiBeginDeferredFrame(UiContext *ui, const EngineInput *input, UiRect screen)
{
    UiBeginFrame(ui, input, screen);
    if (!ui || !ui->state)
        return;
    ui->state->deferred = true;
    ui->state->commandCount = ui->state->textCount = 0;
    ui->state->commandFailed = false;
}

EngineInputCapture UiCapture(const UiContext *ui)
{
    if (!ui || !ui->state)
        return (EngineInputCapture){0};
    return (EngineInputCapture){.keyboard =
                                    ui->state->keyboardConsumed || ui->state->menuRoot != NULL,
                                .mouse = UiConsumesMouse(ui)};
}

static bool SameClip(UiClipState a, UiClipState b)
{
    return a.active == b.active &&
           (!a.active || (a.rect.x == b.rect.x && a.rect.y == b.rect.y &&
                          a.rect.width == b.rect.width && a.rect.height == b.rect.height));
}

bool UiRender(UiContext *ui)
{
    if (!ui || !ui->state)
        return false;
    UiState *s = ui->state;
    s->deferred = false;
    if (s->commandFailed)
    {
        TraceLog(LOG_ERROR, "UI drawing command allocation failed");
        return false;
    }
    UiClipState previous = s->clip;
    for (size_t i = 0; i < s->commandCount; i++)
    {
        struct UiCommand c = s->commands[i];
        if (!SameClip(s->clip, c.clip))
            UiRestoreClip(ui, c.clip);
        if (c.kind == UI_RECT_COMMAND)
            UiFill(ui, c.rect, c.color);
        else if (c.kind == UI_TEXT_COMMAND)
            DrawTextEx(ui->theme.font, s->commandText + c.text,
                       (Vector2){(float)c.rect.x, (float)c.rect.y}, (float)c.fontSize,
                       (float)c.spacing, c.color);
        else
        {
            c.draw(ui, c.rect, c.user);
            UiRestoreClip(ui, c.clip);
        }
    }
    UiRestoreClip(ui, previous);
    return true;
}
