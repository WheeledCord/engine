/* SPDX-License-Identifier: MPL-2.0 */
#ifndef CORE_PIXEL_DRAW_H
#define CORE_PIXEL_DRAW_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
/* Rasterize in world pixels BEFORE the current camera transform. Unlike GL_LINES, every
   pixel is a 1x1 quad, so its width and stair steps scale just like point-filtered sprites.
   Rounds endpoints to integer world coordinates. Invalid/nonfinite/excessive paths draw
   nothing and return false. Maximum 65536 raster steps per segment. */
bool CoreDrawPixelLine(Vector2 start, Vector2 end, Color colour);
/* Connected segments own their start but not end pixel; corners are not alpha-blended twice. */
bool CoreDrawPixelPath(const Vector2 *points, size_t count, bool closed, Color colour);
#endif
