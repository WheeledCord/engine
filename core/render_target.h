#ifndef CORE_RENDER_TARGET_H
#define CORE_RENDER_TARGET_H
#include "raylib.h"
#include <stdbool.h>
/* colourFormat=0 for depth-only. Depth is always a sampleable texture. */
bool MakeRT(RenderTexture *out, int width, int height, int colourFormat, bool depth);
void CoreUnloadRT(RenderTexture *rt);
void CoreDepthRange(float *nearPlane, float *farPlane);
#endif
