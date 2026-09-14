/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. */
#include "diagnostics.h"
#include "diagnostics_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static CoreDiagnostics *current;
const CoreDiagnostics *CoreDiagnosticsCurrent(void) { return current; }
void CoreDiagnosticsSetCurrent(CoreDiagnostics *value) { current = value; }
static bool Add(CoreDebug *d, CoreDebugPrimitive item)
{
    if (!d) return false;
    if (d->count == d->capacity)
    {
        size_t n = d->capacity ? d->capacity * 2 : 32;
        void *p = realloc(d->items, n * sizeof *d->items);
        if (!p) return false;
        d->items = p;
        d->capacity = n;
    }
    d->items[d->count++] = item;
    return true;
}
bool CoreDebugInit(CoreDebug *d, bool overlay)
{
    if (!d) return false;
    *d = (CoreDebug){.overlay = overlay, .x = 8, .y = 8};
    return true;
}
void CoreDebugFree(CoreDebug *d) { if (d) { free(d->items); *d = (CoreDebug){0}; } }
bool CoreDebugLine(CoreDebug *d, Vector2 a, Vector2 b, Color c, float s)
{ return Add(d, (CoreDebugPrimitive){.kind = CORE_DEBUG_LINE, .a = a, .b = b, .color = c, .seconds = s}); }
bool CoreDebugCircle(CoreDebug *d, Vector2 a, float r, Color c, float s)
{ return r >= 0 && Add(d, (CoreDebugPrimitive){.kind = CORE_DEBUG_CIRCLE, .a = a, .radius = r, .color = c, .seconds = s}); }
bool CoreDebugRect(CoreDebug *d, Rectangle r, Color c, float s)
{ return Add(d, (CoreDebugPrimitive){.kind = CORE_DEBUG_RECT, .a = {r.x, r.y}, .b = {r.width, r.height}, .color = c, .seconds = s}); }
bool CoreDebugText(CoreDebug *d, Vector2 a, const char *text, Color c, float s)
{
    if (!text || strlen(text) >= 128) return false;
    CoreDebugPrimitive p = {.kind = CORE_DEBUG_TEXT, .a = a, .color = c, .seconds = s};
    strcpy(p.text, text);
    return Add(d, p);
}
void CoreDebugUpdate(CoreDebug *d, double dt)
{
    if (!d || dt < 0) return;
    size_t out = 0;
    for (size_t i = 0; i < d->count; i++)
    {
        CoreDebugPrimitive p = d->items[i];
        if (p.seconds > 0) p.seconds -= (float)dt;
        if (p.seconds > 0 || d->items[i].seconds <= 0) d->items[out++] = p;
    }
    d->count = out;
}
void CoreDebugDraw(CoreDebug *d)
{
    if (!d) return;
    if (d->overlay)
    {
        const CoreDiagnostics *s = CoreDiagnosticsCurrent();
        if (s)
        {
            char line[128];
            snprintf(line, sizeof line, "fps %d | updates %u | backlog %.2f ms", GetFPS(), s->updates, s->fixedBacklog * 1000);
            DrawText(line, d->x, d->y, 10, LIME);
        }
    }
    for (size_t i = 0; i < d->count; i++)
    {
        const CoreDebugPrimitive *p = &d->items[i];
        if (p->kind == CORE_DEBUG_LINE) DrawLineV(p->a, p->b, p->color);
        else if (p->kind == CORE_DEBUG_CIRCLE) DrawCircleLines((int)p->a.x, (int)p->a.y, p->radius, p->color);
        else if (p->kind == CORE_DEBUG_RECT) DrawRectangleLines((int)p->a.x, (int)p->a.y, (int)p->b.x, (int)p->b.y, p->color);
        else DrawText(p->text, (int)p->a.x, (int)p->a.y, 10, p->color);
    }
    size_t persistent = 0;
    for (size_t i = 0; i < d->count; i++)
        if (d->items[i].seconds > 0) d->items[persistent++] = d->items[i];
    d->count = persistent;
}
