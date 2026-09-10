/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_RENDER_TARGET_H
#define CORE_RENDER_TARGET_H
#include "raylib.h"
#include <stdbool.h>
/* colourFormat=0 for depth-only. Depth is always a sampleable texture. */
bool MakeRT(RenderTexture *out, int width, int height, int colourFormat, bool depth);
void CoreUnloadRT(RenderTexture *rt);
void CoreDepthRange(float *nearPlane, float *farPlane);
#endif
