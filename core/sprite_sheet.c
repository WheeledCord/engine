/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "sprite_sheet.h"
#include "file.h"
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
    Texture2D atlas = LoadTexture(imagePath);
    if (!atlas.id)
        return false;
    out->atlas = atlas;
    out->meta = meta;
    return true;
}
void SpriteSheetUnload(SpriteSheet *sheet)
{
    if (sheet->atlas.id)
        UnloadTexture(sheet->atlas);
    *sheet = (SpriteSheet){0};
}
Rectangle SpriteSheetFrameRect(const SpriteSheet *sheet, int frame)
{
    if (frame < 0)
        frame = 0;
    if (frame >= sheet->meta.frameCount)
        frame = sheet->meta.frameCount - 1;
    int column = frame % sheet->meta.columns, row = frame / sheet->meta.columns;
    return (Rectangle){(float)(column * sheet->meta.cellWidth), (float)(row * sheet->meta.cellHeight),
                       (float)sheet->meta.cellWidth, (float)sheet->meta.cellHeight};
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
