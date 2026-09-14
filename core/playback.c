/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "playback.h"
#include <math.h>

/* The place in the clip, as a fractional frame. Everything else here reads off this. */
static double Position(CoreClip clip, double elapsed)
{
    if (clip.frameCount < 1 || !(clip.fps > 0) || !(elapsed > 0))
        return 0;
    double at = elapsed * (double)clip.fps;
    if (clip.loop)
        return fmod(at, (double)clip.frameCount);
    return at > clip.frameCount - 1 ? clip.frameCount - 1 : at;
}
int CoreClipFrame(CoreClip clip, double elapsed)
{
    return (int)Position(clip, elapsed);
}
void CoreClipBlend(CoreClip clip, double elapsed, int *from, int *to, float *between)
{
    double at = Position(clip, elapsed);
    int first = (int)at, second = first + 1;
    if (second >= clip.frameCount)
        second = clip.loop ? 0 : first;
    if (from)
        *from = first;
    if (to)
        *to = second;
    if (between)
        *between = (float)(at - first);
}
