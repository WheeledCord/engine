/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_PLAYBACK_H
#define CORE_PLAYBACK_H
#include <stdbool.h>

/* Where a clip has got to after a given time: one rule, written once.

   A clip is a count of frames, a rate, and whether it starts over. Time times rate gives a place in
   it, which wraps when it loops and stops on the last frame when it does not. Skeletal animation
   and sprite sheets both ask this question and must not answer it differently, so neither of them
   owns it. Time before zero reads as the first frame rather than off the front. */
typedef struct CoreClip
{
    int frameCount;
    float fps;
    bool loop;
} CoreClip;

/* The frame showing at this time. */
int CoreClipFrame(CoreClip clip, double elapsed);
/* The two frames either side of this time and how far between them, for playback that blends.
   `to` is the same frame as `from` where there is nothing ahead to blend into. */
void CoreClipBlend(CoreClip clip, double elapsed, int *from, int *to, float *between);
#endif
