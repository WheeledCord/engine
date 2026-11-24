/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "iso_map.h"
#include "capabilities.h"
#include "file.h"
#include "iso_wall_internal.h"
#include "raymath.h"
#include "sprite_bake.h"
#include <GL/gl.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pixels of ground per metre, along and across the view: the scenery bake's 32 px/m seen from 30
   degrees up, which is also what makes one hex a metre across. */
#define METRE_X 32.0f
#define METRE_Y 16.0f
/* Where a tile's centre moves on screen one tile along +x and along +y. */
static const Vector2 tileStepX = {-48, 12}, tileStepY = {32, 24};

/* ---- small text helpers ------------------------------------------------------------------------ */

/* Splits a line into words, keeping "quoted phrases" whole. Returns how many; # ends the line. */
static int Words(char *line, char **words, int capacity)
{
    int count = 0;
    char *p = line;
    while (*p && count < capacity)
    {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p || *p == '#')
            break;
        if (*p == '"')
        {
            words[count++] = ++p;
            while (*p && *p != '"')
                p++;
        }
        else
        {
            words[count++] = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
        }
        if (*p)
            *p++ = 0;
    }
    return count;
}
/* key=value among words, or NULL. */
static const char *Option(char **words, int count, const char *key)
{
    size_t length = strlen(key);
    for (int i = 0; i < count; i++)
        if (!strncmp(words[i], key, length) && words[i][length] == '=')
            return words[i] + length + 1;
    return NULL;
}
static float OptionFloat(char **words, int count, const char *key, float fallback)
{
    const char *value = Option(words, count, key);
    return value ? (float)atof(value) : fallback;
}
static void Relative(char *out, size_t capacity, const char *base, const char *path)
{
    if (path[0] == '/')
        snprintf(out, capacity, "%s", path);
    else
        snprintf(out, capacity, "%s/%s", GetDirectoryPath(base), path);
}
static bool Copy(char *to, size_t capacity, const char *from)
{
    size_t length = strlen(from);
    if (length >= capacity)
        return false;
    memcpy(to, from, length + 1);
    return true;
}

/* ---- materials --------------------------------------------------------------------------------- */

/* A piece's or floor tile's .prop file, as the sprite maker writes it: `trench_prop 1`, then
   `key values...` lines. NULL when there is none. */
static char *ReadProp(const char *base)
{
    char path[ISO_MAP_PATH * 2 + 8];
    snprintf(path, sizeof path, "%s.prop", base);
    char *text = FileExists(path) ? CoreReadFile(path) : NULL;
    if (text && strncmp(text, "trench_prop 1", 13))
    {
        TraceLog(LOG_WARNING, "ISO MAP: %s is not a trench_prop 1 file", path);
        CoreFreeFile(text);
        return NULL;
    }
    return text;
}
/* The anchor a .sheet file gives, without loading its picture. */
static bool SheetAnchor(const char *metaPath, int *x, int *y)
{
    char *text = CoreReadFile(metaPath);
    if (!text)
        return false;
    bool haveX = false, haveY = false;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n"))
    {
        haveX |= sscanf(line, "anchorx %d", x) == 1;
        haveY |= sscanf(line, "anchory %d", y) == 1;
    }
    CoreFreeFile(text);
    return haveX && haveY;
}
/* Reads a floor tile's pattern into memory, where the ground compositor samples it. */
static bool LoadMaterial(IsoMaterial *m, const char *tilesetPath)
{
    char base[ISO_MAP_PATH * 2], image[ISO_MAP_PATH * 2 + 8], meta[ISO_MAP_PATH * 2 + 8];
    Relative(base, sizeof base, tilesetPath, m->sheet);
    snprintf(image, sizeof image, "%s.png", base);
    snprintf(meta, sizeof meta, "%s.sheet", base);
    Image picture = FileExists(image) ? LoadImage(image) : (Image){0};
    if (!picture.data || !SheetAnchor(meta, &m->anchorX, &m->anchorY))
    {
        TraceLog(LOG_ERROR, "ISO MAP: %s has no floor tile at %s.png/.sheet (make one in the sprite maker)", m->name,
                 base);
        UnloadImage(picture);
        return false;
    }
    ImageFormat(&picture, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    free(m->texels);
    m->texels = malloc((size_t)picture.width * picture.height * sizeof(Color));
    if (!m->texels)
    {
        UnloadImage(picture);
        return false;
    }
    memcpy(m->texels, picture.data, (size_t)picture.width * picture.height * sizeof(Color));
    m->width = picture.width;
    m->height = picture.height;
    UnloadImage(picture);
    m->pattern = 1;
    char *text = ReadProp(base);
    for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n"))
        sscanf(line, "pattern %d", &m->pattern);
    CoreFreeFile(text);
    m->pattern = m->pattern < 1 ? 1 : m->pattern > 16 ? 16 : m->pattern;
    long r = 0, g = 0, b = 0, n = 0;
    for (int i = 0; i < m->width * m->height; i++)
        if (m->texels[i].a > 0)
            r += m->texels[i].r, g += m->texels[i].g, b += m->texels[i].b, n++;
    n = n ? n : 1;
    m->swatch = (Color){(unsigned char)(r / n), (unsigned char)(g / n), (unsigned char)(b / n), 255};
    return true;
}

/* Prop footprints, collision and the shadow offset per stacked level. */
static void LoadProp(IsoPiece *piece, const char *base)
{
    piece->footprintCount = 0;
    piece->blocking = true;
    piece->backdrop = false;
    piece->shadowStep = (Vector2){0, 0};
    char *text = ReadProp(base);
    for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n"))
    {
        char *words[2 + ISO_MAP_FOOTPRINT * 2];
        int count = Words(line, words, (int)(sizeof words / sizeof *words));
        if (count == 2 && !strcmp(words[0], "backdrop"))
            piece->backdrop = atoi(words[1]) != 0;
        else if (count == 2 && !strcmp(words[0], "blocking"))
            piece->blocking = atoi(words[1]) != 0;
        else if (count == 3 && !strcmp(words[0], "shadow_step"))
            piece->shadowStep = (Vector2){(float)atof(words[1]), (float)atof(words[2])};
        else if (count >= 1 && !strcmp(words[0], "footprint"))
            for (int i = 1; i + 1 < count && piece->footprintCount < ISO_MAP_FOOTPRINT; i += 2)
                piece->footprint[piece->footprintCount++] = (IsoHex){atoi(words[i]), atoi(words[i + 1])};
    }
    CoreFreeFile(text);
    if (piece->footprintCount == 0)
        piece->footprint[piece->footprintCount++] = (IsoHex){0, 0}; /* it stands on its anchor hex */
}

static void FreePiece(IsoPiece *piece);
static bool LoadPiece(IsoPiece *piece, const char *tilesetPath)
{
    char base[ISO_MAP_PATH * 2], image[ISO_MAP_PATH * 2 + 16], meta[ISO_MAP_PATH * 2 + 16];
    Relative(base, sizeof base, tilesetPath, piece->sheet);
    FreePiece(piece);
    snprintf(image, sizeof image, "%s.png", base);
    snprintf(meta, sizeof meta, "%s.sheet", base);
    piece->hasArt = SpriteSheetLoad(&piece->art, image, meta);
    piece->pitched = piece->hasArt && piece->art.meta.frameCount >= ISO_ROOF_FILL;
    snprintf(image, sizeof image, "%s_shadow.png", base);
    snprintf(meta, sizeof meta, "%s_shadow.sheet", base);
    piece->hasShadow = FileExists(image) && SpriteSheetLoad(&piece->shadow, image, meta);
    for (int v = 0; v < ISO_PROP_VARIANTS; v++)
    {
        snprintf(image, sizeof image, "%s_%d.png", base, v);
        snprintf(meta, sizeof meta, "%s_%d.sheet", base, v);
        piece->hasVariant[v] = FileExists(image) && SpriteSheetLoad(&piece->variants[v], image, meta);
        snprintf(image, sizeof image, "%s_%d_shadow.png", base, v);
        snprintf(meta, sizeof meta, "%s_%d_shadow.sheet", base, v);
        piece->hasVariantShadow[v] = piece->hasVariant[v] && FileExists(image) &&
                                     SpriteSheetLoad(&piece->variantShadows[v], image, meta);
    }
    LoadProp(piece, base);
    if (!piece->hasArt && !IsoPieceSheet(piece, 0, false))
        TraceLog(LOG_WARNING, "ISO MAP: %s has no sprite at %s.png/.sheet; it will be drawn as a marker",
                 piece->name, base);
    return true;
}

int IsoPieceSwatchFrame(const IsoPiece *piece)
{
    if (piece && piece->wallSource)
        piece = piece->wallSource;
    /* Only walls have 16 frames: frame 5 is the straight north-south piece. */
    return piece && piece->hasArt && piece->art.meta.frameCount >= 16 ? 5 : 0;
}
const SpriteSheet *IsoPieceSheet(const IsoPiece *piece, int variant, bool shadow)
{
    if (!piece)
        return NULL;
    if (piece->wallSource)
        piece = piece->wallSource;
    if (variant >= 0 && variant < ISO_PROP_VARIANTS && piece->hasVariant[variant])
        return shadow ? (piece->hasVariantShadow[variant] ? &piece->variantShadows[variant] : NULL)
                      : &piece->variants[variant];
    if (shadow ? piece->hasShadow : piece->hasArt)
        return shadow ? &piece->shadow : &piece->art;
    for (int v = 0; v < ISO_PROP_VARIANTS; v++)
        if (piece->hasVariant[v])
            return shadow ? (piece->hasVariantShadow[v] ? &piece->variantShadows[v] : NULL) : &piece->variants[v];
    return NULL;
}
int IsoPieceNextVariant(const IsoPiece *piece, int from, int step)
{
    for (int i = 1; piece && i <= ISO_PROP_VARIANTS; i++)
    {
        int v = ((from + step * i) % ISO_PROP_VARIANTS + ISO_PROP_VARIANTS) % ISO_PROP_VARIANTS;
        if (piece->hasVariant[v])
            return v;
    }
    return from;
}

static void FreePiece(IsoPiece *piece)
{
    if (piece->hasArt)
        SpriteSheetUnload(&piece->art);
    if (piece->hasShadow)
        SpriteSheetUnload(&piece->shadow);
    for (int v = 0; v < ISO_PROP_VARIANTS; v++)
    {
        if (piece->hasVariant[v])
            SpriteSheetUnload(&piece->variants[v]);
        if (piece->hasVariantShadow[v])
            SpriteSheetUnload(&piece->variantShadows[v]);
    }
    memset(piece->hasVariant, 0, sizeof piece->hasVariant);
    memset(piece->hasVariantShadow, 0, sizeof piece->hasVariantShadow);
    piece->hasArt = piece->hasShadow = piece->pitched = false;
    piece->wallSource = NULL;
}

/* ---- tilesets ---------------------------------------------------------------------------------- */

static IsoMaterial *Materials(IsoTileset *t, IsoLayer layer, int **count)
{
    if (layer == ISO_LAYER_FLOOR)
        return *count = &t->floorCount, t->floors;
    if (layer == ISO_LAYER_PATH)
        return *count = &t->pathCount, t->paths;
    return NULL;
}
static IsoPiece *Pieces(IsoTileset *t, IsoLayer layer, int **count)
{
    if (layer == ISO_LAYER_BLOCK)
        return *count = &t->blockCount, t->blocks;
    if (layer == ISO_LAYER_WALL)
        return *count = &t->wallCount, t->walls;
    if (layer == ISO_LAYER_ROOF)
        return *count = &t->roofCount, t->roofs;
    if (layer == ISO_LAYER_PROP)
        return *count = &t->propCount, t->props;
    return NULL;
}

int IsoTilesetCount(const IsoTileset *t, IsoLayer layer)
{
    switch (layer)
    {
        case ISO_LAYER_FLOOR: return t->floorCount;
        case ISO_LAYER_PATH: return t->pathCount;
        case ISO_LAYER_BLOCK: return t->blockCount;
        case ISO_LAYER_WALL: return t->wallCount;
        case ISO_LAYER_ROOF: return t->roofCount;
        case ISO_LAYER_PROP: return t->propCount;
        case ISO_LAYER_OBJECT: return t->objectCount;
        default: return 0;
    }
}
const char *IsoTilesetName(const IsoTileset *t, IsoLayer layer, int i)
{
    if (i < 0 || i >= IsoTilesetCount(t, layer))
        return "";
    switch (layer)
    {
        case ISO_LAYER_FLOOR: return t->floors[i].name;
        case ISO_LAYER_PATH: return t->paths[i].name;
        case ISO_LAYER_BLOCK: return t->blocks[i].name;
        case ISO_LAYER_WALL: return t->walls[i].name;
        case ISO_LAYER_ROOF: return t->roofs[i].name;
        case ISO_LAYER_PROP: return t->props[i].name;
        case ISO_LAYER_OBJECT: return t->objects[i].name;
        default: return "";
    }
}
int IsoTilesetFind(const IsoTileset *t, IsoLayer layer, const char *name)
{
    for (int i = 0; i < IsoTilesetCount(t, layer); i++)
        if (!strcmp(IsoTilesetName(t, layer, i), name))
            return i;
    return -1;
}

int IsoTilesetAddMaterial(IsoTileset *t, IsoLayer layer, IsoMaterial material)
{
    int *count = NULL;
    IsoMaterial *list = Materials(t, layer, &count);
    if (!list || *count >= ISO_MAP_KINDS)
        return -1;
    material.texels = NULL;
    if (!LoadMaterial(&material, t->path))
        return -1;
    list[*count] = material;
    return (*count)++;
}
static bool BindWallSource(IsoTileset *t, IsoPiece *wall)
{
    for (int i = 0; i < t->blockCount; i++)
        if (!strcmp(wall->sheet, t->blocks[i].sheet))
        {
            FreePiece(wall);
            wall->wallSource = &t->blocks[i];
            return true;
        }
    return false;
}
int IsoTilesetAddPiece(IsoTileset *t, IsoLayer layer, const char *name, const char *sheet)
{
    int *count = NULL;
    IsoPiece *list = Pieces(t, layer, &count);
    if (!list || *count >= ISO_MAP_KINDS)
        return -1;
    IsoPiece *piece = &list[*count];
    *piece = (IsoPiece){0};
    if (!Copy(piece->name, sizeof piece->name, name) || !Copy(piece->sheet, sizeof piece->sheet, sheet))
        return -1;
    if (layer != ISO_LAYER_WALL || !BindWallSource(t, piece))
        LoadPiece(piece, t->path);
    return (*count)++;
}
bool IsoTilesetRefreshMaterial(IsoTileset *t, IsoLayer layer, int index)
{
    int *count = NULL;
    IsoMaterial *list = Materials(t, layer, &count);
    if (!list || index < 0 || index >= *count)
        return false;
    return LoadMaterial(&list[index], t->path);
}
bool IsoTilesetReloadPiece(IsoTileset *t, IsoLayer layer, int index)
{
    int *count = NULL;
    IsoPiece *list = Pieces(t, layer, &count);
    if (!list || index < 0 || index >= *count)
        return false;
    if (layer == ISO_LAYER_WALL && BindWallSource(t, &list[index]))
        return true;
    return LoadPiece(&list[index], t->path);
}

static Color ParseColour(const char *text, Color fallback)
{
    int r, g, b;
    if (text && sscanf(text, "%d,%d,%d", &r, &g, &b) == 3)
        return (Color){(unsigned char)r, (unsigned char)g, (unsigned char)b, 255};
    return fallback;
}

bool IsoTilesetLoad(IsoTileset *t, const char *path)
{
    memset(t, 0, sizeof *t);
    if (!path || !Copy(t->path, sizeof t->path, path))
        return false;
    char *text = CoreReadFile(path);
    if (!text)
    {
        TraceLog(LOG_ERROR, "ISO MAP: no tileset at %s", path);
        return false;
    }
    bool ok = true;
    int number = 0;
    for (char *line = text, *next; line && ok; line = next)
    {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        number++;
        char *words[32];
        int count = Words(line, words, 32);
        if (count == 0)
            continue;
        if (number == 1 || (count == 2 && !strcmp(words[0], "trench_tileset")))
        {
            ok = count == 2 && !strcmp(words[0], "trench_tileset") && !strcmp(words[1], "1");
            continue;
        }
        IsoLayer layer = !strcmp(words[0], "floor") ? ISO_LAYER_FLOOR
                         : !strcmp(words[0], "path") ? ISO_LAYER_PATH
                         : !strcmp(words[0], "block") ? ISO_LAYER_BLOCK
                         : !strcmp(words[0], "wall") ? ISO_LAYER_WALL
                         : !strcmp(words[0], "roof") ? ISO_LAYER_ROOF
                         : !strcmp(words[0], "prop") ? ISO_LAYER_PROP
                         : !strcmp(words[0], "object") ? ISO_LAYER_OBJECT
                                                       : ISO_LAYER_COUNT;
        if (layer == ISO_LAYER_COUNT || count < 3)
        {
            TraceLog(LOG_ERROR, "ISO MAP: %s:%d: expected floor, path, block, wall, roof, prop or object "
                                "with a name and a file", path, number);
            ok = false;
        }
        else if (layer == ISO_LAYER_FLOOR || layer == ISO_LAYER_PATH)
        {
            IsoMaterial m = {0};
            ok = Copy(m.name, sizeof m.name, words[1]) && Copy(m.sheet, sizeof m.sheet, words[2]);
            m.edge = OptionFloat(words, count, "edge", layer == ISO_LAYER_PATH ? 0.5f : 0);
            m.blocking = OptionFloat(words, count, "blocking", 0) != 0;
            ok = ok && IsoTilesetAddMaterial(t, layer, m) >= 0;
        }
        else if (layer == ISO_LAYER_OBJECT)
        {
            if (t->objectCount >= ISO_MAP_KINDS)
                ok = false;
            else
            {
                IsoObjectKind *o = &t->objects[t->objectCount++];
                ok = Copy(o->name, sizeof o->name, words[1]) && Copy(o->classname, sizeof o->classname, words[2]);
                o->colour = ParseColour(Option(words, count, "colour"), (Color){230, 200, 90, 255});
                for (int i = 3; i < count && ok; i++)
                    if (strncmp(words[i], "colour=", 7))
                    {
                        size_t used = strlen(o->fields);
                        ok = used + strlen(words[i]) + 2 < sizeof o->fields;
                        if (ok)
                            snprintf(o->fields + used, sizeof o->fields - used, "%s%s", used ? " " : "", words[i]);
                    }
            }
        }
        else
        {
            int added = IsoTilesetAddPiece(t, layer, words[1], words[2]);
            ok = added >= 0;
            if (ok && layer == ISO_LAYER_WALL)
            {
                IsoPiece *w = &t->walls[added];
                w->wallWidth = OptionFloat(words, count, "width", 0);
                float placement = OptionFloat(words, count, "placement", 0);
                float levels = OptionFloat(words, count, "levels", 1);
                w->wallFilled = OptionFloat(words, count, "filled", 0) != 0;
                if (!isfinite(w->wallWidth) || w->wallWidth < 0 || w->wallWidth > 1 ||
                    (w->wallWidth > 0 && w->wallWidth < .05f) || !isfinite(placement) || placement < -1 ||
                    placement > 1 || floorf(placement) != placement || !isfinite(levels) || levels < 1 ||
                    levels > ISO_MAP_LEVELS || floorf(levels) != levels)
                    ok = false;
                else
                {
                    w->wallPlacement = (int)placement;
                    w->wallLevels = (int)levels;
                }
            }
        }
        if (!ok)
            TraceLog(LOG_ERROR, "ISO MAP: %s:%d could not be read or loaded", path, number);
    }
    CoreFreeFile(text);
    if (!ok)
        IsoTilesetUnload(t);
    else
        for (int i = 0; i < t->wallCount; i++)
            BindWallSource(t, &t->walls[i]);
    return ok;
}

static void WriteMaterial(FILE *f, const char *kind, const IsoMaterial *m, bool path)
{
    fprintf(f, "%s \"%s\" \"%s\" edge=%g", kind, m->name, m->sheet, (double)m->edge);
    if (path)
        fprintf(f, " blocking=%d", m->blocking ? 1 : 0);
    fprintf(f, "\n");
}
bool IsoTilesetSave(const IsoTileset *t, const char *path)
{
    CoreAtomicFile atomic;
    FILE *f = CoreAtomicBegin(&atomic, path);
    if (!f)
        return false;
    fprintf(f, "trench_tileset 1\n# name, file relative to this one, options. Maps refer to kinds by name.\n");
    for (int i = 0; i < t->floorCount; i++)
        WriteMaterial(f, "floor", &t->floors[i], false);
    for (int i = 0; i < t->pathCount; i++)
        WriteMaterial(f, "path", &t->paths[i], true);
    for (int i = 0; i < t->blockCount; i++)
        fprintf(f, "block \"%s\" \"%s\"\n", t->blocks[i].name,
                t->blocks[i].sheet);
    for (int i = 0; i < t->wallCount; i++)
        fprintf(f, "wall \"%s\" \"%s\" width=%g placement=%d levels=%d filled=%d\n", t->walls[i].name,
                t->walls[i].sheet, (double)t->walls[i].wallWidth, t->walls[i].wallPlacement,
                t->walls[i].wallLevels > 0 ? t->walls[i].wallLevels : 1, t->walls[i].wallFilled);
    for (int i = 0; i < t->roofCount; i++)
        fprintf(f, "roof \"%s\" \"%s\"\n", t->roofs[i].name, t->roofs[i].sheet);
    for (int i = 0; i < t->propCount; i++)
        fprintf(f, "prop \"%s\" \"%s\"\n", t->props[i].name, t->props[i].sheet);
    for (int i = 0; i < t->objectCount; i++)
    {
        const IsoObjectKind *o = &t->objects[i];
        fprintf(f, "object \"%s\" %s colour=%d,%d,%d%s%s\n", o->name, o->classname, o->colour.r, o->colour.g,
                o->colour.b, o->fields[0] ? " " : "", o->fields);
    }
    return CoreAtomicCommit(&atomic, !ferror(f));
}

void IsoTilesetUnload(IsoTileset *t)
{
    for (int i = 0; i < t->floorCount; i++)
        free(t->floors[i].texels);
    for (int i = 0; i < t->pathCount; i++)
        free(t->paths[i].texels);
    for (int i = 0; i < t->blockCount; i++)
        FreePiece(&t->blocks[i]);
    for (int i = 0; i < t->wallCount; i++)
        FreePiece(&t->walls[i]);
    for (int i = 0; i < t->roofCount; i++)
        FreePiece(&t->roofs[i]);
    for (int i = 0; i < t->propCount; i++)
        FreePiece(&t->props[i]);
    memset(t, 0, sizeof *t);
}

/* ---- the map ----------------------------------------------------------------------------------- */

static size_t Tiles(const IsoMap *m)
{
    return (size_t)m->width * (size_t)m->height;
}
static size_t Hexes(const IsoMap *m)
{
    return Tiles(m) * 4;
}

bool IsoMapInit(IsoMap *m, int width, int height, const char *tileset)
{
    *m = (IsoMap){0};
    if (width < 1 || height < 1 || width > 512 || height > 512)
        return false;
    m->width = width;
    m->height = height;
    snprintf(m->tileset, sizeof m->tileset, "%s", tileset ? tileset : "");
    m->floor = malloc(Tiles(m));
    m->paths = malloc(Hexes(m));
    m->blocks = malloc(Tiles(m) * ISO_MAP_LEVELS);
    m->walls = malloc(Tiles(m));
    m->roofs = malloc(Tiles(m));
    m->roofLevel = calloc(Tiles(m), 1);
    m->wallCache = IsoWallCacheNew();
    if (!m->floor || !m->paths || !m->blocks || !m->walls || !m->roofs || !m->roofLevel || !m->wallCache)
    {
        IsoMapFree(m);
        return false;
    }
    memset(m->floor, -1, Tiles(m));
    memset(m->paths, -1, Hexes(m));
    memset(m->blocks, -1, Tiles(m) * ISO_MAP_LEVELS);
    memset(m->walls, -1, Tiles(m));
    memset(m->roofs, -1, Tiles(m));
    return true;
}
void IsoMapFree(IsoMap *m)
{
    free(m->floor);
    free(m->paths);
    free(m->blocks);
    free(m->walls);
    IsoWallCacheFree(m->wallCache);
    free(m->roofs);
    free(m->roofLevel);
    free(m->props);
    free(m->objects);
    *m = (IsoMap){0};
}
bool IsoMapCopy(IsoMap *to, const IsoMap *from)
{
    if (!IsoMapInit(to, from->width, from->height, from->tileset))
        return false;
    memcpy(to->floor, from->floor, Tiles(from));
    memcpy(to->paths, from->paths, Hexes(from));
    memcpy(to->blocks, from->blocks, Tiles(from) * ISO_MAP_LEVELS);
    memcpy(to->walls, from->walls, Tiles(from));
    memcpy(to->roofs, from->roofs, Tiles(from));
    memcpy(to->roofLevel, from->roofLevel, Tiles(from));
    for (int i = 0; i < from->propCount; i++)
    {
        int added = IsoMapAddProp(to, from->props[i].hex, from->props[i].kind);
        if (added >= 0)
            to->props[added].variant = from->props[i].variant;
    }
    for (int i = 0; i < from->objectCount; i++)
        IsoMapAddObject(to, from->objects[i].hex, from->objects[i].kind, from->objects[i].fields);
    return to->propCount == from->propCount && to->objectCount == from->objectCount;
}
bool IsoMapResize(IsoMap *m, int width, int height)
{
    IsoMap grown;
    if (!IsoMapInit(&grown, width, height, m->tileset))
        return false;
    for (int y = 0; y < height && y < m->height; y++)
        for (int x = 0; x < width && x < m->width; x++)
        {
            IsoTile t = {x, y};
            IsoMapSetFloor(&grown, t, IsoMapFloorAt(m, t));
            for (int level = 0; level < ISO_MAP_LEVELS; level++)
                IsoMapSetBlock(&grown, t, level, IsoMapBlockAt(m, t, level));
            IsoMapSetWall(&grown, t, IsoMapWallAt(m, t));
            IsoMapSetRoof(&grown, t, m->roofLevel[y * m->width + x], IsoMapRoofAt(m, t));
        }
    for (int y = 0; y < height * 2 && y < m->height * 2; y++)
        for (int x = 0; x < width * 2 && x < m->width * 2; x++)
            IsoMapSetPath(&grown, (IsoHex){x, y}, IsoMapPathAt(m, (IsoHex){x, y}));
    for (int i = 0; i < m->propCount; i++)
        if (IsoMapHexInside(&grown, m->props[i].hex))
        {
            int added = IsoMapAddProp(&grown, m->props[i].hex, m->props[i].kind);
            if (added >= 0)
                grown.props[added].variant = m->props[i].variant;
        }
    for (int i = 0; i < m->objectCount; i++)
        if (IsoMapHexInside(&grown, m->objects[i].hex))
            IsoMapAddObject(&grown, m->objects[i].hex, m->objects[i].kind, m->objects[i].fields);
    IsoMapFree(m);
    *m = grown;
    return true;
}

bool IsoMapTileInside(const IsoMap *m, IsoTile t)
{
    return t.x >= 0 && t.y >= 0 && t.x < m->width && t.y < m->height;
}
bool IsoMapHexInside(const IsoMap *m, IsoHex h)
{
    return h.x >= 0 && h.y >= 0 && h.x < m->width * 2 && h.y < m->height * 2;
}
int IsoMapFloorAt(const IsoMap *m, IsoTile t)
{
    return IsoMapTileInside(m, t) ? m->floor[t.y * m->width + t.x] : -1;
}
int IsoMapPathAt(const IsoMap *m, IsoHex h)
{
    return IsoMapHexInside(m, h) ? m->paths[h.y * m->width * 2 + h.x] : -1;
}
int IsoMapBlockAt(const IsoMap *m, IsoTile t, int level)
{
    if (!IsoMapTileInside(m, t) || level < 0 || level >= ISO_MAP_LEVELS)
        return -1;
    return m->blocks[((size_t)t.y * m->width + t.x) * ISO_MAP_LEVELS + level];
}
int IsoMapBlockHeight(const IsoMap *m, IsoTile t)
{
    for (int level = ISO_MAP_LEVELS; level > 0; level--)
        if (IsoMapBlockAt(m, t, level - 1) >= 0)
            return level;
    return 0;
}
int IsoMapRoofAt(const IsoMap *m, IsoTile t)
{
    return IsoMapTileInside(m, t) ? m->roofs[t.y * m->width + t.x] : -1;
}
int IsoMapWallAt(const IsoMap *m, IsoTile t)
{
    return m->walls && IsoMapTileInside(m, t) ? m->walls[t.y * m->width + t.x] : -1;
}
int IsoMapWallMask(const IsoMap *m, IsoTile t)
{
    return (IsoMapWallAt(m, (IsoTile){t.x, t.y - 1}) >= 0 ? 1 : 0) | (IsoMapWallAt(m, (IsoTile){t.x + 1, t.y}) >= 0 ? 2 : 0) |
           (IsoMapWallAt(m, (IsoTile){t.x, t.y + 1}) >= 0 ? 4 : 0) | (IsoMapWallAt(m, (IsoTile){t.x - 1, t.y}) >= 0 ? 8 : 0);
}
static signed char Kind(int kind)
{
    return (signed char)(kind < 0 || kind >= ISO_MAP_KINDS ? -1 : kind);
}
void IsoMapSetFloor(IsoMap *m, IsoTile t, int kind)
{
    if (IsoMapTileInside(m, t))
        m->floor[t.y * m->width + t.x] = Kind(kind);
}
void IsoMapSetPath(IsoMap *m, IsoHex h, int kind)
{
    if (IsoMapHexInside(m, h))
        m->paths[h.y * m->width * 2 + h.x] = Kind(kind);
}
void IsoMapSetBlock(IsoMap *m, IsoTile t, int level, int kind)
{
    if (IsoMapTileInside(m, t) && level >= 0 && level < ISO_MAP_LEVELS)
    {
        m->blocks[((size_t)t.y * m->width + t.x) * ISO_MAP_LEVELS + level] = Kind(kind);
    }
}
void IsoMapSetWall(IsoMap *m, IsoTile t, int kind)
{
    if (IsoMapTileInside(m, t))
    {
        m->walls[t.y * m->width + t.x] = Kind(kind);
        IsoWallInvalidate(m);
    }
}
void IsoMapSetRoof(IsoMap *m, IsoTile t, int level, int kind)
{
    if (!IsoMapTileInside(m, t))
        return;
    m->roofs[t.y * m->width + t.x] = Kind(kind);
    m->roofLevel[t.y * m->width + t.x] = (unsigned char)(level < 0 ? 0 : level > ISO_MAP_LEVELS ? ISO_MAP_LEVELS : level);
}
int IsoMapAddProp(IsoMap *m, IsoHex h, int kind)
{
    if (kind < 0 || !IsoMapHexInside(m, h))
        return -1;
    if (m->propCount == m->propCapacity)
    {
        int capacity = m->propCapacity ? m->propCapacity * 2 : 32;
        IsoMapProp *props = realloc(m->props, (size_t)capacity * sizeof *props);
        if (!props)
            return -1;
        m->props = props;
        m->propCapacity = capacity;
    }
    m->props[m->propCount] = (IsoMapProp){h, kind, 0};
    return m->propCount++;
}
void IsoMapRemoveProp(IsoMap *m, int i)
{
    if (i < 0 || i >= m->propCount)
        return;
    memmove(&m->props[i], &m->props[i + 1], (size_t)(m->propCount - i - 1) * sizeof *m->props);
    m->propCount--;
}
int IsoMapAddObject(IsoMap *m, IsoHex h, int kind, const char *fields)
{
    if (kind < 0 || !IsoMapHexInside(m, h))
        return -1;
    if (m->objectCount == m->objectCapacity)
    {
        int capacity = m->objectCapacity ? m->objectCapacity * 2 : 16;
        IsoMapObject *objects = realloc(m->objects, (size_t)capacity * sizeof *objects);
        if (!objects)
            return -1;
        m->objects = objects;
        m->objectCapacity = capacity;
    }
    IsoMapObject *o = &m->objects[m->objectCount];
    *o = (IsoMapObject){h, kind, ""};
    snprintf(o->fields, sizeof o->fields, "%s", fields ? fields : "");
    return m->objectCount++;
}
void IsoMapRemoveObject(IsoMap *m, int i)
{
    if (i < 0 || i >= m->objectCount)
        return;
    memmove(&m->objects[i], &m->objects[i + 1], (size_t)(m->objectCount - i - 1) * sizeof *m->objects);
    m->objectCount--;
}

IsoHex IsoMapFootprintHex(IsoHex at, IsoHex offset)
{
    /* The hex rows zigzag, so an offset is carried as the screen step it makes from hex (0,0) and
       resolved again from the anchor: the lattice is the same everywhere, the numbering is not. */
    Vector2 from = IsoHexToScreen(at), step = IsoHexToScreen(offset);
    return IsoScreenToHex((Vector2){from.x + step.x, from.y + step.y});
}
int IsoMapPropAt(const IsoMap *m, const IsoTileset *t, IsoHex h)
{
    for (int i = m->propCount - 1; i >= 0; i--)
    {
        const IsoMapProp *p = &m->props[i];
        if (p->hex.x == h.x && p->hex.y == h.y)
            return i;
        if (!t || p->kind >= t->propCount)
            continue;
        const IsoPiece *piece = &t->props[p->kind];
        for (int f = 0; f < piece->footprintCount; f++)
        {
            IsoHex covered = IsoMapFootprintHex(p->hex, piece->footprint[f]);
            if (covered.x == h.x && covered.y == h.y)
                return i;
        }
    }
    return -1;
}
int IsoMapObjectAt(const IsoMap *m, IsoHex h)
{
    for (int i = m->objectCount - 1; i >= 0; i--)
        if (m->objects[i].hex.x == h.x && m->objects[i].hex.y == h.y)
            return i;
    return -1;
}

/* Half a hex row up from iso_grid's tiles: see the header. */
#define TILE_SHIFT_Y -12.0f
IsoTile IsoMapTileAtPoint(Vector2 point)
{
    return IsoScreenToTile((Vector2){point.x, point.y - TILE_SHIFT_Y});
}
void IsoMapTileCorners(IsoTile tile, Vector2 out[4])
{
    IsoTileCorners(tile, out);
    for (int i = 0; i < 4; i++)
        out[i].y += TILE_SHIFT_Y;
}
Vector2 IsoMapTileCentre(IsoTile tile)
{
    Vector2 anchor = IsoTileToScreen(tile);
    return (Vector2){anchor.x - 8, anchor.y + 18 + TILE_SHIFT_Y};
}
Rectangle IsoMapBounds(const IsoMap *m)
{
    Vector2 lo = {1e30f, 1e30f}, hi = {-1e30f, -1e30f};
    IsoTile corners[4] = {{0, 0}, {m->width - 1, 0}, {0, m->height - 1}, {m->width - 1, m->height - 1}};
    for (int i = 0; i < 4; i++)
    {
        Vector2 c[4];
        IsoMapTileCorners(corners[i], c);
        for (int k = 0; k < 4; k++)
        {
            lo = Vector2Min(lo, c[k]);
            hi = Vector2Max(hi, c[k]);
        }
    }
    return (Rectangle){floorf(lo.x), floorf(lo.y), ceilf(hi.x) - floorf(lo.x), ceilf(hi.y) - floorf(lo.y)};
}

bool IsoMapHexBlocked(const IsoMap *m, const IsoTileset *t, IsoHex h)
{
    if (!IsoMapHexInside(m, h))
        return true;
    if (IsoMapBlockAt(m, IsoHexTile(h), 0) >= 0 || IsoWallBlocks(m, t, h))
        return true;
    int path = IsoMapPathAt(m, h);
    if (t && path >= 0 && path < t->pathCount && t->paths[path].blocking)
        return true;
    int prop = IsoMapPropAt(m, t, h);
    return prop >= 0 && t && m->props[prop].kind < t->propCount && t->props[m->props[prop].kind].blocking;
}
void IsoMapBuildBlocking(const IsoMap *m, const IsoTileset *t, unsigned char *out)
{
    IsoWallPrepare(m);
    int w = m->width * 2, h = m->height * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            IsoHex hex = {x, y};
            int path = IsoMapPathAt(m, hex);
            out[y * w + x] = IsoMapBlockAt(m, IsoHexTile(hex), 0) >= 0 || IsoWallBlocks(m, t, hex) ||
                             (t && path >= 0 && path < t->pathCount && t->paths[path].blocking);
        }
    for (int i = 0; t && i < m->propCount; i++)
    {
        const IsoMapProp *p = &m->props[i];
        if (p->kind >= t->propCount || !t->props[p->kind].blocking)
            continue;
        const IsoPiece *piece = &t->props[p->kind];
        for (int f = 0; f < piece->footprintCount; f++)
        {
            IsoHex covered = IsoMapFootprintHex(p->hex, piece->footprint[f]);
            if (IsoMapHexInside(m, covered))
                out[covered.y * w + covered.x] = 1;
        }
    }
}

static uint64_t QueryHash(uint64_t key, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++) key = (key ^ bytes[i]) * UINT64_C(1099511628211);
    return key;
}
static uint64_t QueryMapKey(const IsoMap *m)
{
    uint64_t key = QueryHash(UINT64_C(14695981039346656037), &m->width, sizeof m->width);
    return QueryHash(key, &m->height, sizeof m->height);
}
static bool QueryStorage(unsigned char **data, size_t *capacity, size_t needed)
{
    if (*capacity == needed && *data) return true;
    unsigned char *fresh = realloc(*data, needed);
    if (!fresh) return false;
    *data = fresh;
    *capacity = needed;
    return true;
}
const unsigned char *IsoMapCachedBlocking(IsoMapQueryCache *cache, const IsoMap *m, const IsoTileset *t)
{
    if (!cache || !m || m->width <= 0 || m->height <= 0 || !m->blocks || !m->paths) return NULL;
    size_t size = (size_t)m->width * (size_t)m->height * 4;
    uint64_t key = QueryMapKey(m);
    /* Only ground-level blocks affect movement. Hashing the input is much cheaper than rebuilding
       it and also catches direct array edits; Godot tile_map_layer.cpp:279 caches derived cells. */
    for (size_t i = 0; i < size / 4; i++) key = QueryHash(key, &m->blocks[i * ISO_MAP_LEVELS], 1);
    if (m->walls) key = QueryHash(key, m->walls, size / 4);
    key = QueryHash(key, m->paths, size);
    if (t)
    {
        key = QueryHash(key, &t->wallCount, sizeof t->wallCount);
        for (int i = 0; i < t->wallCount; i++)
        {
            key = QueryHash(key, &t->walls[i].wallWidth, sizeof(float));
            key = QueryHash(key, &t->walls[i].wallPlacement, sizeof(int));
            key = QueryHash(key, &t->walls[i].wallFilled, sizeof(bool));
        }
        key = QueryHash(key, &t->pathCount, sizeof t->pathCount);
        for (int i = 0; i < t->pathCount; i++) key = QueryHash(key, &t->paths[i].blocking, sizeof(bool));
        key = QueryHash(key, &t->propCount, sizeof t->propCount);
        key = QueryHash(key, &m->propCount, sizeof m->propCount);
        for (int i = 0; i < m->propCount; i++)
        {
            const IsoMapProp *p = &m->props[i];
            key = QueryHash(key, &p->hex, sizeof p->hex);
            key = QueryHash(key, &p->kind, sizeof p->kind);
            if (p->kind < 0 || p->kind >= t->propCount) continue;
            const IsoPiece *piece = &t->props[p->kind];
            key = QueryHash(key, &piece->blocking, sizeof piece->blocking);
            key = QueryHash(key, &piece->footprintCount, sizeof piece->footprintCount);
            key = QueryHash(key, piece->footprint, (size_t)piece->footprintCount * sizeof(IsoHex));
        }
    }
    if (!cache->blockingValid || cache->blockingSize != size || cache->blockingKey != key)
    {
        cache->blockingValid = false;
        if (!QueryStorage(&cache->blocking, &cache->blockingSize, size)) return NULL;
        IsoMapBuildBlocking(m, t, cache->blocking);
        cache->blockingKey = key;
        cache->blockingValid = true;
        cache->blockingBuilds++;
    }
    return cache->blocking;
}
const unsigned char *IsoMapCachedRoofRegion(IsoMapQueryCache *cache, const IsoMap *m, IsoTile tile)
{
    if (!cache || !m || m->width <= 0 || m->height <= 0 || !m->roofs) return NULL;
    size_t size = (size_t)m->width * (size_t)m->height;
    uint64_t key = QueryHash(QueryMapKey(m), m->roofs, size);
    if (!cache->roofValid || cache->hiddenSize != size || cache->roofKey != key ||
        cache->roofTile.x != tile.x || cache->roofTile.y != tile.y)
    {
        cache->roofValid = false;
        if (!QueryStorage(&cache->hidden, &cache->hiddenSize, size)) return NULL;
        IsoMapRoofRegion(m, tile, cache->hidden);
        cache->roofKey = key;
        cache->roofTile = tile;
        cache->roofValid = true;
        cache->roofBuilds++;
    }
    return cache->hidden;
}
void IsoMapQueryCacheFree(IsoMapQueryCache *cache)
{
    if (!cache) return;
    free(cache->blocking);
    free(cache->hidden);
    *cache = (IsoMapQueryCache){0};
}

int IsoMapRoofRegion(const IsoMap *m, IsoTile tile, unsigned char *hidden)
{
    memset(hidden, 0, Tiles(m));
    if (IsoMapRoofAt(m, tile) < 0)
        return 0;
    /* Fallout's own flood fill (fallout2-ce tile.cc, tile_fill_roof): four-connected roofed tiles
       are one building. */
    int *stack = malloc(Tiles(m) * sizeof *stack);
    if (!stack)
        return 0;
    int top = 0, count = 0;
    stack[top++] = tile.y * m->width + tile.x;
    hidden[tile.y * m->width + tile.x] = 1;
    while (top)
    {
        int at = stack[--top];
        count++;
        int x = at % m->width, y = at / m->width;
        const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int s = 0; s < 4; s++)
        {
            IsoTile next = {x + steps[s][0], y + steps[s][1]};
            if (IsoMapRoofAt(m, next) < 0 || hidden[next.y * m->width + next.x])
                continue;
            hidden[next.y * m->width + next.x] = 1;
            stack[top++] = next.y * m->width + next.x;
        }
    }
    free(stack);
    return count;
}

/* ---- files ------------------------------------------------------------------------------------- */

static const char *LayerWord(IsoLayer layer)
{
    static const char *words[ISO_LAYER_COUNT] = {"floor", "path", "block", "wall", "roof", "prop", "object"};
    return layer < ISO_LAYER_COUNT ? words[layer] : "";
}

bool IsoMapTilesetPath(const char *mapPath, char *out, size_t capacity)
{
    char *text = CoreReadFile(mapPath);
    if (!text)
        return false;
    bool found = false;
    for (char *line = strtok(text, "\n"); line && !found; line = strtok(NULL, "\n"))
    {
        char *words[4];
        int count = Words(line, words, 4);
        if (count == 2 && !strcmp(words[0], "tileset"))
        {
            Relative(out, capacity, mapPath, words[1]);
            found = true;
        }
    }
    CoreFreeFile(text);
    return found;
}

bool IsoMapSave(const IsoMap *m, const char *path, const IsoTileset *t)
{
    CoreAtomicFile atomic;
    FILE *f = CoreAtomicBegin(&atomic, path);
    if (!f)
        return false;
    fprintf(f, "trench_map 1\ntileset \"%s\"\nsize %d %d\n", m->tileset, m->width, m->height);
    /* The names each layer's numbers stand for in this file, so the tileset can change under it. */
    for (int layer = 0; layer < ISO_LAYER_COUNT; layer++)
    {
        fprintf(f, "names %s", LayerWord((IsoLayer)layer));
        for (int i = 0; i < IsoTilesetCount(t, (IsoLayer)layer); i++)
            fprintf(f, " \"%s\"", IsoTilesetName(t, (IsoLayer)layer, i));
        fprintf(f, "\n");
    }
    fprintf(f, "floor\n");
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width; x++)
        {
            int k = m->floor[y * m->width + x];
            fprintf(f, k < 0 ? "." : "%d", k);
            fputc(x + 1 < m->width ? ' ' : '\n', f);
        }
    fprintf(f, "paths\n");
    for (int y = 0; y < m->height * 2; y++)
        for (int x = 0; x < m->width * 2; x++)
        {
            int k = m->paths[y * m->width * 2 + x];
            fprintf(f, k < 0 ? "." : "%d", k);
            fputc(x + 1 < m->width * 2 ? ' ' : '\n', f);
        }
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width; x++)
        {
            for (int level = 0; level < ISO_MAP_LEVELS; level++)
            {
                int k = IsoMapBlockAt(m, (IsoTile){x, y}, level);
                if (k >= 0)
                    fprintf(f, "block %d %d %d %d\n", x, y, level, k);
            }
            int wall = IsoMapWallAt(m, (IsoTile){x, y});
            if (wall >= 0)
                fprintf(f, "wall %d %d %d\n", x, y, wall);
            int roof = IsoMapRoofAt(m, (IsoTile){x, y});
            if (roof >= 0)
                fprintf(f, "roof %d %d %d %d\n", x, y, m->roofLevel[y * m->width + x], roof);
        }
    for (int i = 0; i < m->propCount; i++)
        fprintf(f, "prop %d %d %d %d\n", m->props[i].hex.x, m->props[i].hex.y, m->props[i].kind, m->props[i].variant);
    for (int i = 0; i < m->objectCount; i++)
        fprintf(f, "object %d %d %d \"%s\"\n", m->objects[i].hex.x, m->objects[i].hex.y, m->objects[i].kind,
                m->objects[i].fields);
    return CoreAtomicCommit(&atomic, !ferror(f));
}

bool IsoMapLoad(IsoMap *m, const char *path, const IsoTileset *t)
{
    char *text = CoreReadFile(path);
    if (!text)
    {
        TraceLog(LOG_ERROR, "ISO MAP: no map at %s", path);
        return false;
    }
    IsoMap loaded = {0};
    /* map[layer][number in the file] = number in the tileset */
    static int remap[ISO_LAYER_COUNT][ISO_MAP_KINDS];
    for (int l = 0; l < ISO_LAYER_COUNT; l++)
        for (int i = 0; i < ISO_MAP_KINDS; i++)
            remap[l][i] = i < IsoTilesetCount(t, (IsoLayer)l) ? i : -1;
    int section = 0, row = 0, number = 0;
    bool ok = true, sized = false;
    char tileset[ISO_MAP_PATH] = "";
    for (char *line = text, *next; line && ok; line = next)
    {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        number++;
        char *words[1100];
        int count = Words(line, words, (int)(sizeof words / sizeof *words));
        if (count == 0)
            continue;
        if (number == 1)
        {
            ok = count == 2 && !strcmp(words[0], "trench_map") && !strcmp(words[1], "1");
            continue;
        }
        if (!strcmp(words[0], "tileset") && count == 2)
            snprintf(tileset, sizeof tileset, "%s", words[1]);
        else if (!strcmp(words[0], "size") && count == 3)
            ok = sized = IsoMapInit(&loaded, atoi(words[1]), atoi(words[2]), tileset);
        else if (!strcmp(words[0], "names") && count >= 2)
        {
            int layer = 0;
            while (layer < ISO_LAYER_COUNT && strcmp(LayerWord((IsoLayer)layer), words[1]))
                layer++;
            for (int i = 2; layer < ISO_LAYER_COUNT && i < count && i - 2 < ISO_MAP_KINDS; i++)
            {
                remap[layer][i - 2] = IsoTilesetFind(t, (IsoLayer)layer, words[i]);
                if (remap[layer][i - 2] < 0)
                    TraceLog(LOG_WARNING, "ISO MAP: %s: the tileset has no %s called \"%s\"; those cells are left empty",
                             path, words[1], words[i]);
            }
        }
        else if (!sized)
            ok = false;
        else if (!strcmp(words[0], "floor") && count == 1)
            section = 1, row = 0;
        else if (!strcmp(words[0], "paths") && count == 1)
            section = 2, row = 0;
        else if (!strcmp(words[0], "block") && count == 5)
        {
            int k = atoi(words[4]);
            IsoMapSetBlock(&loaded, (IsoTile){atoi(words[1]), atoi(words[2])}, atoi(words[3]),
                           k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_BLOCK][k] : -1);
        }
        else if (!strcmp(words[0], "wall") && count == 4)
        {
            int k = atoi(words[3]);
            IsoMapSetWall(&loaded, (IsoTile){atoi(words[1]), atoi(words[2])},
                          k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_WALL][k] : -1);
        }
        else if (!strcmp(words[0], "roof") && count == 5)
        {
            int k = atoi(words[4]);
            IsoMapSetRoof(&loaded, (IsoTile){atoi(words[1]), atoi(words[2])}, atoi(words[3]),
                          k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_ROOF][k] : -1);
        }
        else if (!strcmp(words[0], "prop") && (count == 4 || count == 5))
        {
            int k = atoi(words[3]);
            k = k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_PROP][k] : -1;
            int added = k >= 0 ? IsoMapAddProp(&loaded, (IsoHex){atoi(words[1]), atoi(words[2])}, k) : -1;
            if (added >= 0 && count == 5)
                loaded.props[added].variant = (int)Clamp((float)atoi(words[4]), 0, ISO_PROP_VARIANTS - 1);
        }
        else if (!strcmp(words[0], "object") && (count == 4 || count == 5))
        {
            int k = atoi(words[3]);
            k = k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_OBJECT][k] : -1;
            if (k >= 0)
                IsoMapAddObject(&loaded, (IsoHex){atoi(words[1]), atoi(words[2])}, k, count == 5 ? words[4] : "");
        }
        else if (section == 1 && row < loaded.height && count == loaded.width)
        {
            for (int x = 0; x < count; x++)
                if (strcmp(words[x], "."))
                {
                    int k = atoi(words[x]);
                    IsoMapSetFloor(&loaded, (IsoTile){x, row}, k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_FLOOR][k] : -1);
                }
            row++;
        }
        else if (section == 2 && row < loaded.height * 2 && count == loaded.width * 2)
        {
            for (int x = 0; x < count; x++)
                if (strcmp(words[x], "."))
                {
                    int k = atoi(words[x]);
                    IsoMapSetPath(&loaded, (IsoHex){x, row}, k >= 0 && k < ISO_MAP_KINDS ? remap[ISO_LAYER_PATH][k] : -1);
                }
            row++;
        }
        else
            ok = false;
        if (!ok)
            TraceLog(LOG_ERROR, "ISO MAP: %s:%d cannot be read", path, number);
    }
    CoreFreeFile(text);
    if (!ok || !sized)
    {
        IsoMapFree(&loaded);
        return false;
    }
    snprintf(loaded.tileset, sizeof loaded.tileset, "%s", tileset);
    IsoMapFree(m);
    *m = loaded;
    return true;
}

/* ---- the ground -------------------------------------------------------------------------------- */

static unsigned Hash(int x, int y, unsigned seed)
{
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float Lattice(int x, int y, unsigned seed)
{
    return (float)(Hash(x, y, seed) & 0xffff) / 65535.0f;
}
/* Smooth value noise in ground metres, -0.5..0.5: what makes a path's edge wander instead of
   following the hex outline or a perfect curve. */
static float Noise(float x, float y, unsigned seed)
{
    float total = 0, amplitude = 0.65f, frequency = 1.1f;
    for (int octave = 0; octave < 2; octave++)
    {
        float px = x * frequency, py = y * frequency;
        int ix = (int)floorf(px), iy = (int)floorf(py);
        float fx = px - ix, fy = py - iy;
        fx = fx * fx * (3 - 2 * fx);
        fy = fy * fy * (3 - 2 * fy);
        unsigned s = seed + (unsigned)octave * 97u;
        float top = Lattice(ix, iy, s) + (Lattice(ix + 1, iy, s) - Lattice(ix, iy, s)) * fx;
        float bottom = Lattice(ix, iy + 1, s) + (Lattice(ix + 1, iy + 1, s) - Lattice(ix, iy + 1, s)) * fx;
        total += (top + (bottom - top) * fy - 0.5f) * amplitude;
        amplitude *= 0.35f;
        frequency *= 3.1f;
    }
    return total;
}
static float Bayer(int x, int y)
{
    static const int matrix[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return (matrix[y & 3][x & 3] + 0.5f) / 16.0f;
}
/* A floor tile's colour at a point of the ground: the pattern is laid tile for tile on the map's own
   tiles, so it lines up with the grid and repeats every `pattern` tiles. */
static Color Texel(const IsoMaterial *m, Vector2 point)
{
    if (!m->texels)
        return (Color){255, 0, 255, 255};
    IsoTile tile = IsoMapTileAtPoint(point);
    int n = m->pattern, ix = ((tile.x % n) + n) % n, iy = ((tile.y % n) + n) % n;
    Vector2 centre = IsoMapTileCentre(tile);
    int x = (int)floorf((float)m->anchorX + tileStepX.x * (float)ix + tileStepY.x * (float)iy + point.x - centre.x);
    int y = (int)floorf((float)m->anchorY + tileStepX.y * (float)ix + tileStepY.y * (float)iy + point.y - centre.y);
    x = x < 0 ? 0 : x >= m->width ? m->width - 1 : x;
    y = y < 0 ? 0 : y >= m->height ? m->height - 1 : y;
    return m->texels[y * m->width + x];
}
/* A soft falloff over a little more than a hex, in ground metres. Summed over the hexes a path was
   painted on, it gives a field whose 0.5 contour is a smooth blob around them: one hex alone makes
   a hex-sized patch, a line of them a track with rounded ends. */
static float Kernel(float d2, float radius2)
{
    if (d2 >= radius2)
        return 0;
    float t = 1 - d2 / radius2;
    return t * t;
}

static Color GroundPixel(const IsoMap *map, const IsoTileset *ts, int px, int py, unsigned seed)
{
    Vector2 point = {(float)px + 0.5f, (float)py + 0.5f};
    float gx = point.x / METRE_X, gy = point.y / METRE_Y;
    IsoTile tile = IsoMapTileAtPoint(point);
    /* Floors change material where their tiles meet, along a line pushed about by noise. */
    int kind = IsoMapFloorAt(map, tile);
    bool mixed = false;
    for (int dy = -1; dy <= 1 && !mixed; dy++)
        for (int dx = -1; dx <= 1 && !mixed; dx++)
            mixed = IsoMapFloorAt(map, (IsoTile){tile.x + dx, tile.y + dy}) != kind;
    /* Floors meet on the tile edges unless one of them has an edge: then the corners round off and
       the seam wanders, more the larger the edge. */
    float edge = 0;
    for (int dy = -1; dy <= 1 && mixed; dy++)
        for (int dx = -1; dx <= 1; dx++)
        {
            int k = IsoMapFloorAt(map, (IsoTile){tile.x + dx, tile.y + dy});
            if (k >= 0 && k < ts->floorCount)
                edge = fmaxf(edge, ts->floors[k].edge);
        }
    if (mixed && edge > 0)
    {
        /* Each neighbouring tile votes for its material with a falloff from its centre; the
           strongest material wins, each nudged by its own noise so the seam wanders. "No floor"
           votes like any material, so holes get the same soft outline. */
        int kinds[9], kindCount = 0;
        float votes[9];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
            {
                IsoTile n = {tile.x + dx, tile.y + dy};
                int k = IsoMapFloorAt(map, n);
                Vector2 c = IsoMapTileCentre(n);
                float ddx = gx - c.x / METRE_X, ddy = gy - c.y / METRE_Y;
                float w = Kernel(ddx * ddx + ddy * ddy, 1.7f * 1.7f);
                int slot = 0;
                while (slot < kindCount && kinds[slot] != k)
                    slot++;
                if (slot == kindCount)
                    kinds[kindCount] = k, votes[kindCount++] = 0;
                votes[slot] += w;
            }
        float best = -1e30f;
        for (int i = 0; i < kindCount; i++)
        {
            float score = votes[i] + Noise(gx, gy, seed + 11u + (unsigned)(kinds[i] + 1) * 53u) * 0.7f * edge;
            if (score > best)
                best = score, kind = kinds[i];
        }
    }
    if (kind < 0 || kind >= ts->floorCount)
        return BLANK;
    Color colour = Texel(&ts->floors[kind], point);
    /* Paths: every path material near here competes, drawn in tileset order so later ones lie over
       earlier ones. */
    IsoHex centre = IsoScreenToHex(point);
    float field[ISO_MAP_KINDS];
    int present[8], presentCount = 0;
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++)
        {
            IsoHex h = {centre.x + dx, centre.y + dy};
            int p = IsoMapPathAt(map, h);
            if (p < 0 || p >= ts->pathCount)
                continue;
            bool seen = false;
            for (int i = 0; i < presentCount; i++)
                seen |= present[i] == p;
            if (!seen)
            {
                if (presentCount == 8)
                    continue;
                present[presentCount++] = p;
                field[p] = 0;
            }
            Vector2 c = IsoHexToScreen(h);
            float ddx = gx - c.x / METRE_X, ddy = gy - c.y / METRE_Y;
            field[p] += Kernel(ddx * ddx + ddy * ddy, 1.3f * 1.3f);
        }
    for (int order = 0; order < ts->pathCount && presentCount; order++)
    {
        bool here = false;
        for (int i = 0; i < presentCount; i++)
            here |= present[i] == order;
        if (!here)
            continue;
        const IsoMaterial *m = &ts->paths[order];
        float value = field[order] + Noise(gx, gy, seed + 31u * (unsigned)(order + 1)) * (0.15f + 0.6f * m->edge);
        /* A narrow stippled band at the edge, the way the rest of the art dithers its transitions. */
        const float band = 0.06f;
        float t = (value - 0.5f) / band;
        bool inside = t >= 1 || (t > -1 && (t + 1) * 0.5f > Bayer(px, py));
        if (inside)
        {
            Color overlay = Texel(m, point);
            // Invisible blocking paths must not erase the painted floor.
            if (overlay.a > 0)
                colour = overlay;
        }
    }
    return colour;
}

static void Composite(IsoGround *g, const IsoMap *map, const IsoTileset *ts, int x0, int y0, int x1, int y1)
{
    Color *pixels = g->canvas.data;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            pixels[y * g->canvas.width + x] =
                GroundPixel(map, ts, (int)g->bounds.x + x, (int)g->bounds.y + y, g->seed);
}
static void Upload(IsoGround *g, int x0, int y0, int x1, int y1)
{
    for (int r = y0 / g->chunk; r <= (y1 - 1) / g->chunk && r < g->rows; r++)
        for (int c = x0 / g->chunk; c <= (x1 - 1) / g->chunk && c < g->columns; c++)
        {
            Rectangle area = {(float)(c * g->chunk), (float)(r * g->chunk), (float)g->chunk, (float)g->chunk};
            if (area.x + area.width > g->canvas.width)
                area.width = g->canvas.width - area.x;
            if (area.y + area.height > g->canvas.height)
                area.height = g->canvas.height - area.y;
            Image part = ImageFromImage(g->canvas, area);
            Texture2D *texture = &g->chunks[r * g->columns + c];
            if (!texture->id)
            {
                *texture = LoadTextureFromImage(part);
                SetTextureFilter(*texture, TEXTURE_FILTER_POINT);
            }
            else
                UpdateTexture(*texture, part.data);
            UnloadImage(part);
        }
}

bool IsoGroundBuild(IsoGround *g, const IsoMap *map, const IsoTileset *ts)
{
    IsoGroundFree(g);
    g->bounds = IsoMapBounds(map);
    g->seed = 1013u;
    g->canvas = GenImageColor((int)g->bounds.width, (int)g->bounds.height, BLANK);
    ImageFormat(&g->canvas, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    /* Fewer, larger chunks: each is its own texture and so its own draw batch. */
    g->chunk = CoreMaxTextureSize() >= 1024 ? 512 : 256;
    g->columns = (g->canvas.width + g->chunk - 1) / g->chunk;
    g->rows = (g->canvas.height + g->chunk - 1) / g->chunk;
    g->chunks = calloc((size_t)(g->columns * g->rows), sizeof *g->chunks);
    if (!g->canvas.data || !g->chunks)
    {
        IsoGroundFree(g);
        return false;
    }
    Composite(g, map, ts, 0, 0, g->canvas.width, g->canvas.height);
    Upload(g, 0, 0, g->canvas.width, g->canvas.height);
    return true;
}
void IsoGroundRefresh(IsoGround *g, const IsoMap *map, const IsoTileset *ts, Rectangle area)
{
    if (!g->canvas.data)
        return;
    int x0 = (int)floorf(area.x - g->bounds.x), y0 = (int)floorf(area.y - g->bounds.y);
    int x1 = (int)ceilf(area.x + area.width - g->bounds.x), y1 = (int)ceilf(area.y + area.height - g->bounds.y);
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > g->canvas.width ? g->canvas.width : x1;
    y1 = y1 > g->canvas.height ? g->canvas.height : y1;
    if (x0 >= x1 || y0 >= y1)
        return;
    Composite(g, map, ts, x0, y0, x1, y1);
    Upload(g, x0, y0, x1, y1);
}
void IsoGroundDraw(const IsoGround *g, Rectangle visible)
{
    for (int r = 0; r < g->rows; r++)
        for (int c = 0; c < g->columns; c++)
        {
            const Texture2D *texture = &g->chunks[r * g->columns + c];
            Rectangle area = {g->bounds.x + c * g->chunk, g->bounds.y + r * g->chunk, (float)texture->width,
                              (float)texture->height};
            if (texture->id && CheckCollisionRecs(area, visible))
                DrawTexture(*texture, (int)area.x, (int)area.y, WHITE);
        }
}
void IsoGroundFree(IsoGround *g)
{
    for (int i = 0; g->chunks && i < g->columns * g->rows; i++)
        if (g->chunks[i].id)
            UnloadTexture(g->chunks[i]);
    free(g->chunks);
    UnloadImage(g->canvas);
    *g = (IsoGround){0};
}
#define FOG_PAD 160
/* The largest axis-aligned rectangle centred in the map's tile parallelogram shrunk by `depth` tiles,
   as the integer world points inside it (where the fog samples its field): there the fog is 0. The
   tile basis is (-48,12),(32,24) (determinant -1536), so a rectangle of half-size a,b reaches
   (24a+32b)/1536 tiles along x and (12a+48b)/1536 along y; maximise ab under both limits. */
static Rectangle FogClear(const IsoMap *map, double depth)
{
    double p = 1536 * (map->width / 2.0 - depth), q = 1536 * (map->height / 2.0 - depth);
    if (p <= 0 || q <= 0)
        return (Rectangle){0};
    double tries[3][2] = {{p / 48, p / 64}, {q / 24, q / 96}, {0, (2 * q - p) / 64}};
    tries[2][0] = (p - 32 * tries[2][1]) / 24;
    double a = 0, b = 0;
    for (int i = 0; i < 3; i++)
    {
        double ta = tries[i][0], tb = tries[i][1];
        if (ta > 0 && tb > 0 && 24 * ta + 32 * tb <= p * (1 + 1e-12) && 12 * ta + 48 * tb <= q * (1 + 1e-12) &&
            ta * tb > a * b)
            a = ta, b = tb;
    }
    double cx = -48 * (map->width / 2.0) + 32 * (map->height / 2.0);
    double cy = 12 * (map->width / 2.0) + 24 * (map->height / 2.0) + TILE_SHIFT_Y;
    double x0 = ceil(cx - a), y0 = ceil(cy - b), x1 = floor(cx + a), y1 = floor(cy + b);
    if (x1 < x0 || y1 < y0)
        return (Rectangle){0};
    return (Rectangle){(float)x0, (float)y0, (float)(x1 - x0 + 1), (float)(y1 - y0 + 1)};
}
/* A GL_ALPHA8 texture: a byte a texel, which the default shader samples as black with that alpha,
   the same as an RGBA texture of (0,0,0,a) at a quarter of the memory. raylib's loader has no alpha
   format (its one-byte format is luminance, opaque grey on GL 2.1), so this uploads it itself, with
   raylib's defaults for the rest: nearest filtering, repeat wrap, no mipmaps. id 0 on failure. */
static Texture2D AlphaTexture(const unsigned char *alpha, int width, int height)
{
    unsigned int id = 0;
    CoreCheckGraphicsErrors("before an IsoFog texture"); /* logged, so an old error is not blamed on this */
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!CoreCheckGraphicsErrors("IsoFog alpha texture") && id)
    {
        glDeleteTextures(1, &id);
        id = 0;
    }
    /* format: the nearest raylib has; read these back with glGetTexImage(GL_ALPHA), not raylib */
    return (Texture2D){id, width, height, 1, PIXELFORMAT_UNCOMPRESSED_GRAYSCALE};
}
bool IsoFogBuild(IsoFog *fog, const IsoMap *map, float depthTiles)
{
    IsoFogFree(fog);
    if (!map || map->width < 1 || map->height < 1)
        return false;
    float maxDepth = fminf(map->width, map->height) * 0.22f;
    if (maxDepth < 1.0f)
        maxDepth = 1.0f;
    if (depthTiles <= 0.0f || depthTiles > maxDepth)
        depthTiles = maxDepth;
    Rectangle bounds = IsoMapBounds(map);
    int w = (int)bounds.width + 2 * FOG_PAD, h = (int)bounds.height + 2 * FOG_PAD;
    size_t count = (size_t)w * (size_t)h;
    fog->area = (Rectangle){bounds.x - FOG_PAD, bounds.y - FOG_PAD, (float)w, (float)h};
    fog->chunk = CoreMaxTextureSize() >= 1024 ? 512 : 256;
    fog->columns = (w + fog->chunk - 1) / fog->chunk;
    fog->rows = (h + fog->chunk - 1) / fog->chunk;
    fog->chunks = calloc((size_t)(fog->columns * fog->rows), sizeof *fog->chunks);
    float *field = malloc(count * sizeof *field);
    unsigned char *density = malloc(count * 2);
    unsigned char *part = malloc((size_t)fog->chunk * (size_t)fog->chunk);
    bool ok = fog->chunks && field && density && part;
    for (int y = 0; ok && y < h; y++)
        for (int x = 0; x < w; x++)
        {
            /* Sampled at the pixel's corner, in fractional tiles (IsoMapTileAtPoint without the floor). */
            double px = fog->area.x + x, py = fog->area.y + y - TILE_SHIFT_Y;
            double tx = (32 * py - 24 * px) / 1536, ty = (12 * px + 48 * py) / 1536;
            double d = fmin(fmin(tx, map->width - tx), fmin(ty, map->height - ty));
            double t = fmin(fmax(1 - d / depthTiles, 0), 1), f = t * t * (3 - 2 * t);
            size_t i = (size_t)y * (size_t)w + (size_t)x;
            field[i] = (float)f;
            density[i] = (unsigned char)rint(f * 255);
        }
    ok = ok && SpriteDitherDensity(density, density + count, w, h);
    unsigned char *alpha = density; /* the density is spent once dithered; the alpha replaces it */
    for (size_t i = 0; ok && i < count; i++)
        alpha[i] = (unsigned char)rint(fmin(field[i] * .94 + density[count + i] / 255.0 * .06, 1) * 255);
    for (int r = 0; ok && r < fog->rows; r++)
        for (int c = 0; ok && c < fog->columns; c++)
        {
            int x0 = c * fog->chunk, y0 = r * fog->chunk;
            int cw = w - x0 < fog->chunk ? w - x0 : fog->chunk, ch = h - y0 < fog->chunk ? h - y0 : fog->chunk;
            for (int y = 0; y < ch; y++)
                memcpy(part + (size_t)y * (size_t)cw, alpha + (size_t)(y0 + y) * (size_t)w + (size_t)x0, (size_t)cw);
            fog->chunks[r * fog->columns + c] = AlphaTexture(part, cw, ch);
            ok = fog->chunks[r * fog->columns + c].id != 0;
        }
    free(field);
    free(density);
    free(part);
    if (!ok)
    {
        IsoFogFree(fog);
        return false;
    }
    fog->clear = FogClear(map, depthTiles);
    TraceLog(LOG_INFO, "IsoFog: %dx%d at %g,%g, depth %g tiles, clear middle %gx%g at %g,%g", w, h, fog->area.x,
             fog->area.y, depthTiles, fog->clear.width, fog->clear.height, fog->clear.x, fog->clear.y);
    return true;
}
void IsoFogDraw(const IsoFog *fog, Rectangle visible)
{
    for (int r = 0; r < fog->rows; r++)
        for (int c = 0; c < fog->columns; c++)
        {
            const Texture2D *texture = &fog->chunks[r * fog->columns + c];
            Rectangle area = {fog->area.x + c * fog->chunk, fog->area.y + r * fog->chunk, (float)texture->width,
                              (float)texture->height};
            if (!texture->id || !CheckCollisionRecs(area, visible))
                continue;
            Rectangle hole = GetCollisionRec(area, fog->clear);
            /* Strips change no pixel only where texels land on whole pixels (SpriteSheetCanTrim). */
            if (hole.width <= 0 || hole.height <= 0 || !SpriteSheetCanTrim((Vector2){area.x, area.y}, 1))
            {
                DrawTexture(*texture, (int)area.x, (int)area.y, WHITE);
                continue;
            }
            /* Above and below the clear part, then left and right of it. */
            Rectangle strips[4] = {{area.x, area.y, area.width, hole.y - area.y},
                                   {area.x, hole.y + hole.height, area.width, area.y + area.height - hole.y - hole.height},
                                   {area.x, hole.y, hole.x - area.x, hole.height},
                                   {hole.x + hole.width, hole.y, area.x + area.width - hole.x - hole.width, hole.height}};
            for (int i = 0; i < 4; i++)
                if (strips[i].width > 0 && strips[i].height > 0)
                    DrawTextureRec(*texture, (Rectangle){strips[i].x - area.x, strips[i].y - area.y, strips[i].width, strips[i].height},
                                   (Vector2){strips[i].x, strips[i].y}, WHITE);
        }
}
void IsoFogFree(IsoFog *fog)
{
    for (int i = 0; fog->chunks && i < fog->columns * fog->rows; i++)
        if (fog->chunks[i].id)
            UnloadTexture(fog->chunks[i]);
    free(fog->chunks);
    *fog = (IsoFog){0};
}
/* Paths reach a hex and a half past the hex painted, floors about a tile past theirs. */
Rectangle IsoGroundHexArea(IsoHex hex)
{
    Vector2 c = IsoHexToScreen(hex);
    return (Rectangle){c.x - 64, c.y - 40, 128, 80};
}
Rectangle IsoGroundTileArea(IsoTile tile)
{
    Vector2 c = IsoMapTileCentre(tile);
    return (Rectangle){c.x - 112, c.y - 64, 224, 128};
}

/* ---- things standing up ------------------------------------------------------------------------ */

static bool Visible(Vector2 ground, const SpriteSheet *sheet, Rectangle visible)
{
    if (!sheet)
        return CheckCollisionPointRec(ground, visible);
    Rectangle area = SpriteSheetGroundRect(sheet, ground);
    return CheckCollisionRecs(area, visible);
}

int IsoMapCollectPieces(const IsoMap *m, const IsoTileset *t, Rectangle visible, IsoMapPiece *out, int capacity)
{
    int count = 0;
    IsoWallPrepare(m);
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width; x++)
        {
            IsoTile tile = {x, y};
            int height = IsoMapBlockHeight(m, tile);
            Vector2 centre = IsoMapTileCentre(tile);
            for (int level = 0; level < height; level++)
            {
                int k = IsoMapBlockAt(m, tile, level);
                if (k < 0 || k >= t->blockCount)
                    continue;
                const IsoPiece *block = &t->blocks[k];
                Vector2 ground = {centre.x, centre.y - (float)(level * ISO_MAP_BLOCK_HEIGHT)};
                const SpriteSheet *sheet = block->hasArt ? &block->art : NULL;
                /* The optional second frame omits ground shading on upper levels.
                   A baked ground shadow moves with height when a step is provided. */
                int frame = 0;
                if (sheet && sheet->meta.frameCount >= 2 && level > 0)
                    frame = 1; /* a block's second frame is itself above the ground */
                bool steps = block->shadowStep.x != 0 || block->shadowStep.y != 0;
                const SpriteSheet *shadow = block->hasShadow && (level == 0 || steps) ? &block->shadow : NULL;
                Vector2 shadowAt = Vector2Add(centre, Vector2Scale(block->shadowStep, (float)level));
                if (!Visible(ground, sheet, visible))
                    continue;
                /* Split at the front corner. The left slice holds the left face and sorts on the
                   back-left hex; the right slice holds the right face and sorts on the front-right
                   hex. A character beside either face then falls on the right side of it, which one
                   depth for the whole block cannot manage. */
                IsoHex backLeft = {2 * x + 1, 2 * y}, frontRight = {2 * x, 2 * y + 1};
                if (count < capacity)
                    out[count++] = (IsoMapPiece){.kind = ISO_PIECE_BLOCK, .hex = backLeft, .order = level,
                                                 .ground = ground, .shadowAt = shadowAt, .sheet = sheet,
                                                 .shadow = shadow, .frame = frame, .sliceFrom = -1e6f, .sliceTo = -8,
                                                 .index = y * m->width + x};
                if (count < capacity)
                    out[count++] = (IsoMapPiece){.kind = ISO_PIECE_BLOCK, .hex = frontRight, .order = level,
                                                 .ground = ground, .shadowAt = shadowAt, .sheet = sheet,
                                                 .frame = frame, .sliceFrom = -8, .sliceTo = 1e6f,
                                                 .index = y * m->width + x};
            }
            /* A wall is sliced like a block on the same two hexes; its frame is its neighbour mask. */
            int k = IsoMapWallAt(m, tile);
            if (k < 0 || k >= t->wallCount)
                continue;
            const IsoPiece *wall = &t->walls[k];
            const IsoPiece *material = wall->wallSource ? wall->wallSource : wall;
            const SpriteSheet *sheet = IsoPieceSheet(wall, 0, false);
            int frame =
                wall->wallWidth <= 0 && sheet && sheet->meta.frameCount >= 16 ? IsoMapWallMask(m, tile) : 0;
            IsoWallShape shape = IsoWallShapeAt(m, tile, wall);
            float extra=shape.levels>0?(float)((shape.levels-1)*ISO_MAP_BLOCK_HEIGHT):0;
            Rectangle bounds=sheet?SpriteSheetGroundRect(sheet,centre):(Rectangle){centre.x,centre.y,1,1};
            bounds.y-=extra;bounds.height+=extra;
            if (!CheckCollisionRecs(bounds,visible)) continue;
            IsoHex backLeft = {2 * x + 1, 2 * y}, frontRight = {2 * x, 2 * y + 1};
            if (count < capacity)
                out[count++] = (IsoMapPiece){.kind = ISO_PIECE_WALL,
                                             .hex = backLeft,
                                             .ground = centre,
                                             .shadowAt = centre,
                                             .sheet = sheet,
                                             .shadow = shape.cells ? NULL : IsoPieceSheet(wall, 0, true),
                                             .frame = frame,
                                             .wallShape = shape,
                                             .wallShadowStep = material->shadowStep,
                                             .sliceFrom = -1e6f,
                                             .sliceTo = -8,
                                             .index = y * m->width + x};
            if (count < capacity)
                out[count++] = (IsoMapPiece){.kind = ISO_PIECE_WALL,
                                             .hex = frontRight,
                                             .ground = centre,
                                             .shadowAt = centre,
                                             .sheet = sheet,
                                             .frame = frame,
                                             .wallShape = shape,
                                             .sliceFrom = -8,
                                             .sliceTo = 1e6f,
                                             .index = y * m->width + x};
        }
    for (int i = 0; i < m->propCount && count < capacity; i++)
    {
        const IsoMapProp *p = &m->props[i];
        if (p->kind < 0 || p->kind >= t->propCount)
            continue;
        const IsoPiece *prop = &t->props[p->kind];
        Vector2 ground = IsoHexToScreen(p->hex);
        const SpriteSheet *sheet = IsoPieceSheet(prop, p->variant, false);
        if (!Visible(ground, sheet, visible))
            continue;
        out[count++] = (IsoMapPiece){.kind = ISO_PIECE_PROP, .backdrop = prop->backdrop, .hex = p->hex, .ground = ground, .shadowAt = ground,
                                     .sheet = sheet, .shadow = IsoPieceSheet(prop, p->variant, true),
                                     .sliceFrom = -1e6f, .sliceTo = 1e6f, .index = i};
    }
    return count;
}

static int RoofOrder(const void *a, const void *b)
{
    /* Far to near by the tile each piece covers, not by how high it has been lifted. */
    const IsoMapPiece *l = a, *r = b;
    Vector2 pl = IsoHexToScreen(l->hex), pr = IsoHexToScreen(r->hex);
    if (pl.y != pr.y)
        return pl.y < pr.y ? -1 : 1;
    if (pl.x != pr.x)
        return pl.x < pr.x ? -1 : 1;
    return l->order - r->order;
}
/* How many roofed tiles run on from this one in a direction, at the same level. */
static int Run(const IsoMap *m, IsoTile t, int dx, int dy)
{
    int level = m->roofLevel[t.y * m->width + t.x], run = 0;
    for (IsoTile n = {t.x + dx, t.y + dy}; IsoMapRoofAt(m, n) >= 0 && m->roofLevel[n.y * m->width + n.x] == level;
         n.x += dx, n.y += dy)
        run++;
    return run;
}
/* Where a tile sits in its roof: which piece, and how many rises up from the eaves. The ridge runs
   along the building's longer side; rows climb from both eaves toward it. */
static IsoRoofShape RoofShape(const IsoMap *m, IsoTile t, int *rises)
{
    int left = Run(m, t, -1, 0), right = Run(m, t, 1, 0), back = Run(m, t, 0, -1), front = Run(m, t, 0, 1);
    bool alongX = left + right >= back + front;
    int before = alongX ? back : left, after = alongX ? front : right, depth = before + after + 1;
    *rises = before < after ? before : after;
    if (depth % 2 == 1 && before == after)
        return alongX ? ISO_ROOF_RIDGE_X : ISO_ROOF_RIDGE_Y;
    if (before < after)
        return alongX ? ISO_ROOF_RISE_Y : ISO_ROOF_RISE_X;
    return alongX ? ISO_ROOF_FALL_Y : ISO_ROOF_FALL_X;
}
int IsoMapCollectRoofs(const IsoMap *m, const IsoTileset *t, Rectangle visible, const unsigned char *hidden,
                       IsoMapPiece *out, int capacity)
{
    int count = 0;
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width && count < capacity; x++)
        {
            int k = m->roofs[y * m->width + x];
            if (k < 0 || k >= t->roofCount || (hidden && hidden[y * m->width + x]))
                continue;
            int level = m->roofLevel[y * m->width + x], rises = 0;
            const IsoPiece *roof = &t->roofs[k];
            const SpriteSheet *sheet = roof->hasArt ? &roof->art : NULL;
            int frame = roof->pitched ? (int)RoofShape(m, (IsoTile){x, y}, &rises) : 0;
            Vector2 centre = IsoMapTileCentre((IsoTile){x, y});
            Vector2 ground = {centre.x, centre.y - (float)(level * ISO_MAP_BLOCK_HEIGHT + rises * ISO_MAP_ROOF_RISE)};
            if (!Visible(ground, sheet, visible))
                continue;
            /* At the building's ends, the lifted tiles stand on blocks of the roof's own material. */
            bool fills = rises > 0 && sheet && sheet->meta.frameCount > ISO_ROOF_FILL;
            bool end = false;
            const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int s = 0; s < 4 && fills; s++)
            {
                IsoTile n = {x + steps[s][0], y + steps[s][1]};
                end |= IsoMapRoofAt(m, n) < 0 || m->roofLevel[n.y * m->width + n.x] != level;
            }
            for (int r = 0; end && r < rises && r < 15 && count < capacity; r++)
                out[count++] = (IsoMapPiece){.kind = ISO_PIECE_ROOF, .hex = {2 * x, 2 * y + 1}, .order = 16 * level + r,
                                             .ground = {centre.x, centre.y - (float)(level * ISO_MAP_BLOCK_HEIGHT +
                                                                                       r * ISO_MAP_ROOF_RISE)},
                                             .sheet = sheet, .frame = ISO_ROOF_FILL, .sliceFrom = -1e6f,
                                             .sliceTo = 1e6f, .index = y * m->width + x};
            if (count >= capacity)
                break;
            out[count++] = (IsoMapPiece){.kind = ISO_PIECE_ROOF, .hex = {2 * x, 2 * y + 1}, .order = 16 * level + 15,
                                         .ground = ground, .shadowAt = ground, .sheet = sheet, .frame = frame,
                                         .sliceFrom = -1e6f, .sliceTo = 1e6f, .index = y * m->width + x};
        }
    qsort(out, (size_t)count, sizeof *out, RoofOrder);
    return count;
}

static void DrawSlice(const SpriteSheet *sheet, int frame, Vector2 ground, float from, float to, Color tint)
{
    Vector2 corner = {roundf(ground.x) - sheet->meta.anchorX, roundf(ground.y) - sheet->meta.anchorY};
    Rectangle source = SpriteSheetFrameRect(sheet, frame);
    /* Only the slice's opaque columns and the frame's opaque rows: the rest would blend nothing. */
    Rectangle opaque = SpriteSheetCanTrim(corner, 1) ? SpriteSheetFrameOpaqueRect(sheet, frame) : source;
    float first = opaque.x - source.x, top = opaque.y - source.y;
    float left = sheet->meta.anchorX + from, right = sheet->meta.anchorX + to;
    left = left < first ? first : left;
    right = right > first + opaque.width ? first + opaque.width : right;
    if (right <= left || opaque.height <= 0)
        return;
    Rectangle part = {source.x + left, opaque.y, right - left, opaque.height};
    Vector2 at = {corner.x + left, corner.y + top};
    DrawTextureRec(sheet->atlas, part, at, tint);
}
void IsoMapPieceDraw(const IsoMapPiece *p, Color tint)
{
    if (p->wallShape.cells && p->sheet)
    {
        IsoWallDraw(p, tint);
        return;
    }
    if (p->sheet)
    {
        DrawSlice(p->sheet, p->frame, p->ground, p->sliceFrom, p->sliceTo, tint);
        return;
    }
    /* No art yet: a marker the size of a hex, so a placed thing is never invisible. */
    Vector2 c[6];
    IsoHexCellCorners((IsoHex){0, 0}, c);
    for (int i = 0; i < 6; i++)
        DrawLineV(Vector2Add(p->ground, c[i]), Vector2Add(p->ground, c[(i + 1) % 6]), (Color){255, 0, 255, 255});
}
void IsoMapPieceDrawShadow(const IsoMapPiece *p)
{
    if (p->wallShape.cells)
    {
        IsoWallDrawShadow(p);
        return;
    }
    if (p->shadow)
    {
        /* Upper blocks use the same shadow frame as ground-level blocks. */
        int frames = p->shadow->meta.frameCount > 0 ? p->shadow->meta.frameCount : 1;
        DrawSlice(p->shadow, p->frame % frames, p->shadowAt, -1e6f, 1e6f, WHITE);
    }
}
