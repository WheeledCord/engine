/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_SPRITE_SHEET_H
#define CORE_SPRITE_SHEET_H
#include "playback.h"
#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>
/* A grid atlas played back frame by frame: any producer (baked from 3D, hand-drawn, whatever) that
   fills a columns*rows grid of cellWidth*cellHeight cells in raster order, frame 0 at the top left. */
typedef struct SpriteSheetMeta
{
    int cellWidth, cellHeight, columns, rows, frameCount;
    float fps;
    bool loop;
    /* Where inside a cell the subject meets the ground, in pixels from the cell's top left. A cell
       is usually taller than what it holds, so without this a drawer has to guess and the subject
       floats. Producers work it out once; drawers use SpriteSheetGroundRect and never think of it. */
    int anchorX, anchorY;
} SpriteSheetMeta;
typedef struct SpriteSheet
{
    Texture2D atlas;
    SpriteSheetMeta meta;
    /* Per frame, set by SpriteSheetLoad: see SpriteSheetFrameOpaqueRect. NULL in a sheet built by
       hand, which then draws whole cells; SpriteSheetUnload frees it. */
    Rectangle *opaque;
} SpriteSheet;
/* sheetPath is the plain-text sidecar; imagePath the atlas texture it describes. */
bool SpriteSheetLoad(SpriteSheet *out, const char *imagePath, const char *sheetPath);
void SpriteSheetUnload(SpriteSheet *sheet);
/* Frame's source rectangle within the atlas, in raster order; frame is clamped to [0, frameCount). */
Rectangle SpriteSheetFrameRect(const SpriteSheet *sheet, int frame);
/* The part of that rectangle holding pixels with any alpha, found once at load, so a draw can skip
   the transparent margin of a cell, which costs fill and blends nothing. Zero size when the frame is
   empty (it draws nothing); the whole frame rectangle when the sheet has no scan (built by hand). */
Rectangle SpriteSheetFrameOpaqueRect(const SpriteSheet *sheet, int frame);
/* Whether drawing only the opaque rectangle, at its offset from a cell whose top left is at `corner`
   drawn at `scale`, gives exactly the pixels of the whole cell under the current rlgl transform and
   2D camera: when texels land on whole pixels from a whole-pixel corner (zoom 1 or 2, unrotated).
   Elsewhere (zoom 0.5, say) pixel centres sit on texel edges, where the texture-coordinate rounding of
   a smaller quad can pick the neighbouring texel, so drawers draw the whole cell there instead.
   Assumes raylib's one-unit-per-pixel projection. SpriteSheetDraw and IsoMapPieceDraw both ask it. */
bool SpriteSheetCanTrim(Vector2 corner, float scale);
/* The clip this sheet describes, for anything that wants to time it itself. */
CoreClip SpriteSheetClip(const SpriteSheet *sheet);
/* Elapsed seconds of playback to a frame index, by the one rule in playback.h. */
int SpriteSheetFrameAt(const SpriteSheet *sheet, double elapsed);

/* Playing a sheet. Skeletal animation keeps its own clock in Actor, and this keeps one too, so a
   caller never has to hold the time for one kind of animation and not the other.

   The sheet is held rather than copied, because a motion is often several sheets -- one per view of
   it -- and changing which one is showing should not restart the motion. */
typedef struct SpriteAnim
{
    const SpriteSheet *sheet;
    double time;
} SpriteAnim;
/* Starts a sheet from its first frame. */
void SpriteAnimPlay(SpriteAnim *anim, const SpriteSheet *sheet);
/* Shows the same motion from a different sheet -- another view of it -- keeping the time it is at. */
void SpriteAnimView(SpriteAnim *anim, const SpriteSheet *sheet);
void SpriteAnimUpdate(SpriteAnim *anim, double dt);
int SpriteAnimFrame(const SpriteAnim *anim);
/* The frame to draw, and where in its atlas it lives. */
Rectangle SpriteAnimRect(const SpriteAnim *anim);
/* Where to draw a frame, at its own size, so the sheet's ground anchor lands on `ground`. */
Rectangle SpriteSheetGroundRect(const SpriteSheet *sheet, Vector2 ground);
typedef struct SpritePresentation { Vector2 ground; float scale, rotation; Color tint; bool flipX, flipY; int layer; float order; } SpritePresentation;
typedef struct SpriteDrawItem { const SpriteSheet *sheet; const SpriteAnim *animation; SpritePresentation presentation; uint64_t sequence; } SpriteDrawItem;
/** Returns a presentation with unit scale, white tint, and an origin-ground position. */
SpritePresentation SpritePresentationDefault(void);
/** Draws frame at its anchored ground position with scale, tint, rotation and optional source flips.
    Unflipped and unrotated, only the frame's opaque rectangle is drawn where SpriteSheetCanTrim. */
void SpriteSheetDraw(const SpriteSheet *sheet, int frame, SpritePresentation presentation);
/** Draws the current frame of an animation using the same presentation rules as SpriteSheetDraw. */
void SpriteAnimDraw(const SpriteAnim *anim, SpritePresentation presentation);
/** Orders SpriteDrawItem values by layer, then order, then insertion sequence; use directly with qsort. */
int SpriteDrawItemCompare(const void *left, const void *right);
/* Writes the sidecar text file describing an atlas a producer has already exported. Overwrites only
   after the whole file reaches disk. */
bool SpriteSheetWriteMeta(const char *sheetPath, const SpriteSheetMeta *meta);
#endif
