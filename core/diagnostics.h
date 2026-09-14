/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. */
#ifndef CORE_DIAGNOSTICS_H
#define CORE_DIAGNOSTICS_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct CoreDiagnostics { double frameDt, fixedBacklog; uint64_t frames; unsigned updates; } CoreDiagnostics;
/** Returns the live application's latest timing snapshot during callbacks, or NULL outside an application run. */
const CoreDiagnostics *CoreDiagnosticsCurrent(void);
typedef enum CoreDebugPrimitiveKind { CORE_DEBUG_LINE, CORE_DEBUG_CIRCLE, CORE_DEBUG_RECT, CORE_DEBUG_TEXT } CoreDebugPrimitiveKind;
typedef struct CoreDebugPrimitive { CoreDebugPrimitiveKind kind; Vector2 a, b; float radius, seconds; Color color; char text[128]; } CoreDebugPrimitive;
typedef struct CoreDebug { CoreDebugPrimitive *items; size_t count, capacity; bool overlay; int x, y; } CoreDebug;
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
