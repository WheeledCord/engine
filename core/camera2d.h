/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_CAMERA2D_H
#define CORE_CAMERA2D_H

#include "raylib.h"
#include <stdbool.h>

/* Optional camera convention for Transform2D worlds: +X right, +Y down, zoom is screen pixels per
   world pixel, and position is the centre of the view. It owns no window or scene object. */
typedef struct CoreCamera2D
{
    Vector2 previous, position;
    Vector2 viewport;
    float zoom;
    float followRate; /* zero snaps; positive is exponential response in reciprocal seconds */
    Rectangle bounds;
    bool clampBounds;
} CoreCamera2D;

/**
 * @brief Returns a camera centred at the origin with unit zoom.
 * @return A caller-owned camera value with a 960 by 540 viewport.
 */
CoreCamera2D CoreCamera2DDefault(void);
/**
 * @brief Sets the viewport used for screen/world conversion and bounds clamping.
 * @param camera Camera to configure.
 * @param viewport Screen dimensions in pixels; nonpositive axes disable clamping on that axis.
 * @return No value.
 */
void CoreCamera2DSetViewport(CoreCamera2D *camera, Vector2 viewport);
/**
 * @brief Moves a camera toward a world-space target and records its previous position.
 * @param camera Camera to update.
 * @param target Desired world-space view centre.
 * @param dt Elapsed seconds; negative values behave as zero.
 * @return No value.
 */
void CoreCamera2DFollow(CoreCamera2D *camera, Vector2 target, float dt);
/**
 * @brief Returns a fixed-update camera position interpolated for drawing.
 * @param camera Camera to sample.
 * @param alpha Remaining fixed-step fraction, normally in [0, 1].
 * @return Interpolated world-space view centre.
 */
Vector2 CoreCamera2DInterpolated(const CoreCamera2D *camera, float alpha);
/**
 * @brief Converts a world point to screen pixels around the viewport centre.
 * @param camera Camera defining the view.
 * @param alpha Fixed-step interpolation fraction.
 * @param world World-space point.
 * @return Screen-space point in pixels.
 */
Vector2 CoreCamera2DWorldToScreen(const CoreCamera2D *camera, float alpha, Vector2 world);
/**
 * @brief Converts screen pixels to world coordinates.
 * @param camera Camera defining the view.
 * @param alpha Fixed-step interpolation fraction.
 * @param screen Screen-space point in pixels.
 * @return World-space point.
 */
Vector2 CoreCamera2DScreenToWorld(const CoreCamera2D *camera, float alpha, Vector2 screen);
#endif
