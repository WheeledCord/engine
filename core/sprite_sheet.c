/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "sprite_sheet.h"
#include "file.h"
#include <limits.h>
#include "texture.h"
#include "rlgl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool ParseInt(const char *value, int *out)
{
    char *end;
    long n = strtol(value, &end, 10);
    if (end == value || *end || n < 1 || n > 100000)
        return false;
    *out = (int)n;
    return true;
}
static bool ParseOffset(const char *value, int *out)
{
    char *end;
    long n = strtol(value, &end, 10);
    if (end == value || *end || n < 0 || n > 100000)
        return false;
    *out = (int)n;
    return true;
}
static bool ParseFps(const char *value, float *out)
{
    char *end;
    float f = strtof(value, &end);
    if (end == value || *end || !isfinite(f) || f <= 0)
        return false;
    *out = f;
    return true;
}
static bool ParseBool(const char *value, bool *out)
{
    if (!strcmp(value, "true") || !strcmp(value, "1"))
    {
        *out = true;
        return true;
    }
    if (!strcmp(value, "false") || !strcmp(value, "0"))
    {
        *out = false;
        return true;
    }
    return false;
}
static bool ParseMeta(const char *text, SpriteSheetMeta *out)
{
    bool haveW = false, haveH = false, haveCols = false, haveRows = false, haveFrames = false,
         haveFps = false, haveLoop = false, haveAnchorX = false, haveAnchorY = false;
    *out = (SpriteSheetMeta){0};
    char line[256];
    const char *p = text;
    while (*p)
    {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof line)
            return false;
        memcpy(line, p, len);
        line[len] = 0;
        p += len + (nl ? 1 : 0);
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#' || (s[0] == '/' && s[1] == '/'))
            continue;
        char key[64], value[192], extra[8];
        /* Exactly a key and a value. Anything trailing is a typo worth refusing rather than a
           comment worth ignoring. */
        if (sscanf(s, "%63s %191s %7s", key, value, extra) != 2)
            return false;
        if (!strcmp(key, "cellwidth"))
            haveW = ParseInt(value, &out->cellWidth);
        else if (!strcmp(key, "cellheight"))
            haveH = ParseInt(value, &out->cellHeight);
        else if (!strcmp(key, "columns"))
            haveCols = ParseInt(value, &out->columns);
        else if (!strcmp(key, "rows"))
            haveRows = ParseInt(value, &out->rows);
        else if (!strcmp(key, "frames"))
            haveFrames = ParseInt(value, &out->frameCount);
        else if (!strcmp(key, "fps"))
            haveFps = ParseFps(value, &out->fps);
        else if (!strcmp(key, "loop"))
            haveLoop = ParseBool(value, &out->loop);
        else if (!strcmp(key, "anchorx"))
            haveAnchorX = ParseOffset(value, &out->anchorX);
        else if (!strcmp(key, "anchory"))
            haveAnchorY = ParseOffset(value, &out->anchorY);
        else
            return false;
    }
    if (!haveW || !haveH || !haveCols || !haveRows || !haveFrames || !haveFps || !haveLoop ||
        !haveAnchorX || !haveAnchorY)
        return false;
    if (out->anchorX > out->cellWidth || out->anchorY > out->cellHeight)
        return false;
    return out->frameCount <= out->columns * out->rows;
}
/* Each frame's bounding box of pixels with any alpha, in atlas coordinates. NULL (draw whole cells)
   when the image does not cover the grid, since a cell past its edge samples wrapped texels. */
static Rectangle *OpaqueRects(Image image, const SpriteSheetMeta *m)
{
    if (image.width < m->columns * m->cellWidth || image.height < m->rows * m->cellHeight)
        return NULL;
    Color *pixels = LoadImageColors(image);
    Rectangle *rects = calloc((size_t)m->frameCount, sizeof *rects);
    if (!pixels || !rects)
    {
        UnloadImageColors(pixels);
        free(rects);
        return NULL;
    }
    for (int f = 0; f < m->frameCount; f++)
    {
        int cx = f % m->columns * m->cellWidth, cy = f / m->columns * m->cellHeight;
        int x0 = INT_MAX, y0 = INT_MAX, x1 = -1, y1 = -1;
        for (int y = 0; y < m->cellHeight; y++)
            for (int x = 0; x < m->cellWidth; x++)
                if (pixels[(cy + y) * image.width + cx + x].a)
                {
                    x0 = x < x0 ? x : x0;
                    x1 = x > x1 ? x : x1;
                    y0 = y < y0 ? y : y0;
                    y1 = y;
                }
        rects[f] = x1 < 0 ? (Rectangle){(float)cx, (float)cy, 0, 0}
                          : (Rectangle){(float)(cx + x0), (float)(cy + y0), (float)(x1 - x0 + 1), (float)(y1 - y0 + 1)};
    }
    UnloadImageColors(pixels);
    return rects;
}
bool SpriteSheetLoad(SpriteSheet *out, const char *imagePath, const char *sheetPath)
{
    *out = (SpriteSheet){0};
    char *text = CoreReadFile(sheetPath);
    if (!text)
        return false;
    SpriteSheetMeta meta;
    bool ok = ParseMeta(text, &meta);
    CoreFreeFile(text);
    if (!ok)
        return false;
    /* What CoreLoadTexture does, keeping the decoded image long enough to find each frame's opaque
       area: raylib's LoadTexture is LoadImage plus LoadTextureFromImage, so the upload is the same. */
    char resolved[1024];
    const char *actual = CoreResolvePath(imagePath, resolved, sizeof resolved);
    Image image = actual && FileExists(actual) ? LoadImage(actual) : (Image){0};
    if (!image.data)
        return false;
    Texture2D atlas = LoadTextureFromImage(image);
    if (atlas.id)
    {
        CoreTextureOptions options = CoreTextureOptionsDefault();
        SetTextureFilter(atlas, options.filter);
        SetTextureWrap(atlas, options.wrap);
        out->opaque = OpaqueRects(image, &meta);
    }
    UnloadImage(image);
    if (!atlas.id)
        return false;
    out->atlas = atlas;
    out->meta = meta;
    return true;
}
void SpriteSheetUnload(SpriteSheet *sheet)
{
    CoreUnloadTexture(&sheet->atlas);
    free(sheet->opaque);
    *sheet = (SpriteSheet){0};
}
static int ClampFrame(const SpriteSheet *sheet, int frame)
{
    if (frame >= sheet->meta.frameCount)
        frame = sheet->meta.frameCount - 1;
    return frame < 0 ? 0 : frame;
}
Rectangle SpriteSheetFrameRect(const SpriteSheet *sheet, int frame)
{
    frame = ClampFrame(sheet, frame);
    int column = frame % sheet->meta.columns, row = frame / sheet->meta.columns;
    return (Rectangle){(float)(column * sheet->meta.cellWidth), (float)(row * sheet->meta.cellHeight),
                       (float)sheet->meta.cellWidth, (float)sheet->meta.cellHeight};
}
Rectangle SpriteSheetFrameOpaqueRect(const SpriteSheet *sheet, int frame)
{
    return sheet->opaque ? sheet->opaque[ClampFrame(sheet, frame)] : SpriteSheetFrameRect(sheet, frame);
}
bool SpriteSheetCanTrim(Vector2 corner, float scale)
{
    /* rlgl applies its push/pop transform to vertices first, then the modelview (the 2D camera). */
    Matrix t = rlGetMatrixTransform(), m = rlGetMatrixModelview();
    if (t.m1 != 0 || t.m4 != 0 || m.m1 != 0 || m.m4 != 0)
        return false;
    float sx = m.m0 * t.m0 * scale, sy = m.m5 * t.m5 * scale;
    float x = m.m0 * (t.m0 * corner.x + t.m12) + m.m12, y = m.m5 * (t.m5 * corner.y + t.m13) + m.m13;
    return sx >= 1 && sy >= 1 && sx == floorf(sx) && sy == floorf(sy) && x == floorf(x) && y == floorf(y);
}
CoreClip SpriteSheetClip(const SpriteSheet *sheet)
{
    return (CoreClip){sheet->meta.frameCount, sheet->meta.fps, sheet->meta.loop};
}
int SpriteSheetFrameAt(const SpriteSheet *sheet, double elapsed)
{
    return CoreClipFrame(SpriteSheetClip(sheet), elapsed);
}
void SpriteAnimPlay(SpriteAnim *anim, const SpriteSheet *sheet)
{
    anim->sheet = sheet;
    anim->time = 0;
}
void SpriteAnimView(SpriteAnim *anim, const SpriteSheet *sheet)
{
    anim->sheet = sheet;
}
void SpriteAnimUpdate(SpriteAnim *anim, double dt)
{
    if (dt > 0)
        anim->time += dt;
}
int SpriteAnimFrame(const SpriteAnim *anim)
{
    return anim->sheet ? SpriteSheetFrameAt(anim->sheet, anim->time) : 0;
}
Rectangle SpriteAnimRect(const SpriteAnim *anim)
{
    return anim->sheet ? SpriteSheetFrameRect(anim->sheet, SpriteAnimFrame(anim))
                       : (Rectangle){0, 0, 0, 0};
}
Rectangle SpriteSheetGroundRect(const SpriteSheet *sheet, Vector2 ground)
{
    return (Rectangle){ground.x - (float)sheet->meta.anchorX, ground.y - (float)sheet->meta.anchorY,
                       (float)sheet->meta.cellWidth, (float)sheet->meta.cellHeight};
}
SpritePresentation SpritePresentationDefault(void) { return (SpritePresentation){.scale = 1, .tint = WHITE}; }
void SpriteSheetDraw(const SpriteSheet *sheet, int frame, SpritePresentation p)
{
    if (!sheet || !sheet->atlas.id || p.scale < 0) return;
    Rectangle source = SpriteSheetFrameRect(sheet, frame);
    Vector2 corner = {p.ground.x - sheet->meta.anchorX * p.scale, p.ground.y - sheet->meta.anchorY * p.scale};
    if (!p.flipX && !p.flipY && p.rotation == 0 && SpriteSheetCanTrim(corner, p.scale))
    {
        /* Only the opaque part, offset from the cell's corner: the rest would blend nothing. */
        Rectangle part = SpriteSheetFrameOpaqueRect(sheet, frame);
        if (part.width <= 0 || part.height <= 0) return;
        DrawTexturePro(sheet->atlas, part, (Rectangle){corner.x + (part.x - source.x) * p.scale,
                       corner.y + (part.y - source.y) * p.scale, part.width * p.scale, part.height * p.scale},
                       (Vector2){0}, 0, p.tint);
        return;
    }
    /* shortcut: flipped frames draw whole cells, because the flip below shifts the source a cell over
       (raylib already mirrors a negative width) and so samples the next cell; trim them once fixed. */
    float width = source.width * p.scale, height = source.height * p.scale;
    float anchorX = p.flipX ? source.width - sheet->meta.anchorX : sheet->meta.anchorX;
    float anchorY = p.flipY ? source.height - sheet->meta.anchorY : sheet->meta.anchorY;
    if (p.flipX) { source.x += source.width; source.width = -source.width; }
    if (p.flipY) { source.y += source.height; source.height = -source.height; }
    DrawTexturePro(sheet->atlas, source, (Rectangle){p.ground.x-anchorX*p.scale,p.ground.y-anchorY*p.scale,width,height}, (Vector2){0}, p.rotation, p.tint);
}
void SpriteAnimDraw(const SpriteAnim *anim, SpritePresentation p) { if(anim && anim->sheet) SpriteSheetDraw(anim->sheet, SpriteAnimFrame(anim), p); }
int SpriteDrawItemCompare(const void *left, const void *right)
{
    const SpriteDrawItem *a=left,*b=right;
    if(a->presentation.layer != b->presentation.layer) return a->presentation.layer < b->presentation.layer ? -1 : 1;
    if(a->presentation.order != b->presentation.order) return a->presentation.order < b->presentation.order ? -1 : 1;
    return a->sequence == b->sequence ? 0 : (a->sequence < b->sequence ? -1 : 1);
}
bool SpriteSheetWriteMeta(const char *sheetPath, const SpriteSheetMeta *meta)
{
    if (meta->cellWidth < 1 || meta->cellHeight < 1 || meta->columns < 1 || meta->rows < 1 ||
        meta->frameCount < 1 || meta->frameCount > meta->columns * meta->rows || !isfinite(meta->fps) ||
        meta->fps <= 0 || meta->anchorX < 0 || meta->anchorY < 0 || meta->anchorX > meta->cellWidth ||
        meta->anchorY > meta->cellHeight)
        return false;
    CoreAtomicFile atomic;
    FILE *f = CoreAtomicBegin(&atomic, sheetPath);
    if (!f)
        return false;
    bool ok = fprintf(f,
                      "cellwidth %d\ncellheight %d\ncolumns %d\nrows %d\nframes %d\nfps %g\n"
                      "loop %s\nanchorx %d\nanchory %d\n",
                      meta->cellWidth, meta->cellHeight, meta->columns, meta->rows, meta->frameCount,
                      (double)meta->fps, meta->loop ? "true" : "false", meta->anchorX,
                      meta->anchorY) > 0;
    return CoreAtomicCommit(&atomic, ok);
}
