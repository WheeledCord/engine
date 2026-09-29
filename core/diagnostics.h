/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. */
#ifndef CORE_DIAGNOSTICS_H
#define CORE_DIAGNOSTICS_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Timing of the running application, in both windowed and headless mode. frameDt is the last frame's
   duration in seconds (windowed only; zero headless). frames counts loop iterations, updates the
   Update calls of the current iteration, ticks every Update call since the run began. The tick*
   fields time each Update call and the frame* fields each whole loop iteration (windowed only), in
   microseconds from CLOCK_MONOTONIC: Last is the most recent finished one, Max the slowest, Total
   the sum. ticks counts a call as it starts, so Update sees itself counted; the micros of a tick or
   frame in progress are not yet included. Headless, frameDt is fixed_dt and each frame is one tick. */
typedef struct CoreDiagnostics
{
    double frameDt, fixedBacklog;
    uint64_t frames;
    unsigned updates;
    double tickMicrosLast, tickMicrosMax, tickMicrosTotal;
    uint64_t ticks;
    double frameMicrosLast, frameMicrosMax, frameMicrosTotal;
} CoreDiagnostics;
/** Returns the live application's latest timing snapshot during callbacks, or NULL outside an application run. */
const CoreDiagnostics *CoreDiagnosticsCurrent(void);
typedef enum CoreDebugPrimitiveKind { CORE_DEBUG_LINE, CORE_DEBUG_CIRCLE, CORE_DEBUG_RECT, CORE_DEBUG_TEXT } CoreDebugPrimitiveKind;
typedef struct CoreDebugPrimitive { CoreDebugPrimitiveKind kind; Vector2 a, b; float radius, seconds; Color color; char text[128]; } CoreDebugPrimitive;
/* font: NULL draws text in raylib's default font; otherwise in this font at its base size. */
typedef struct CoreDebug { CoreDebugPrimitive *items; size_t count, capacity; bool overlay; int x, y; const Font *font; } CoreDebug;
/** Initializes a caller-owned debug queue. overlay enables the timing readout when CoreDebugDraw is called. */
bool CoreDebugInit(CoreDebug *debug, bool overlay);
/** Releases queued primitives. Safe for a zeroed or failed context. */
void CoreDebugFree(CoreDebug *debug);
/** Queues a line for seconds; nonpositive seconds means this draw only. */
bool CoreDebugLine(CoreDebug *debug, Vector2 from, Vector2 to, Color color, float seconds);
/** Queues an outlined circle for seconds; nonpositive seconds means this draw only. */
bool CoreDebugCircle(CoreDebug *debug, Vector2 center, float radius, Color color, float seconds);
/** Queues an outlined rectangle for seconds; nonpositive seconds means this draw only. */
bool CoreDebugRect(CoreDebug *debug, Rectangle rect, Color color, float seconds);
/** Queues one diagnostic line at screen position for seconds; text is copied and too-long text is refused. */
bool CoreDebugText(CoreDebug *debug, Vector2 position, const char *text, Color color, float seconds);
/** Ages persistent primitives by dt. Call once per simulation or rendered frame, not while drawing. */
void CoreDebugUpdate(CoreDebug *debug, double dt);
/** Draws queued primitives and, when enabled, the engine timing overlay. Call inside Draw; one-frame primitives are cleared afterward. */
void CoreDebugDraw(CoreDebug *debug);
#endif
