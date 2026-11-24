/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "sprite_bake.h"
#include "render_target.h"
#include "shader.h"
#include "world_draw.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

SpriteLight SpriteLightDefault(void)
{
    /* cos 45 * sin -120, sin 45, cos 45 * cos -120: the sprite maker's Turn -120 and Rise 45. */
    return (SpriteLight){.direction = {-0.6124f, 0.7071f, -0.3536f},
                         .colour = {1.0f, 0.97f, 0.9f},
                         .ambient = {0.35f, 0.38f, 0.45f},
                         .cutoff = -2,
                         .shadowSoft = 0.25f,
                         .shadowHard = 0.75f,
                         .highlightSoft = 2,
                         .highlightHard = 3,
                         .groundShade = 0,
                         .groundHeight = 1.3f};
}
SpriteLight SpriteLightScenery(void)
{
    SpriteLight light = SpriteLightDefault();
    light.cutoff = 0.3f;
    return light;
}
bool SpriteLightValid(const SpriteLight *light, bool castsShadow)
{
    if (!light || light->shadowHard <= light->shadowSoft || light->highlightHard <= light->highlightSoft)
        return false;
    if (Vector3Length(light->direction) < 1e-4f)
        return false;
    return !castsShadow || Vector3Normalize(light->direction).y > 0.001f;
}

/* Floyd-Steinberg over one cell's shadow/highlight bands, ported from the Unity reference pipeline's
   AnimationCaptureHelper: hard cutoffs paint flat black/white, a soft band between them dithers
   toward whichever side it is closer to, and the quantization error carries into later pixels. */
static void DitherFrame(Color *color, const Color *normal, int w, int h, Vector3 light, float shadowSoft,
                        float shadowHard, float highlightSoft, float highlightHard)
{
    float *err = calloc((size_t)(w + 2) * (size_t)(h + 2), sizeof(float));
    if (!err)
        return;
#define ERR(x, y) err[(size_t)(y) * (size_t)(w + 2) + (size_t)(x)]
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int idx = y * w + x;
            if (color[idx].a < 3)
                continue;
            Vector3 n = {normal[idx].r / 255.0f * 2 - 1, normal[idx].g / 255.0f * 2 - 1,
                        normal[idx].b / 255.0f * 2 - 1};
            n = Vector3Normalize(n);
            float dotL = Vector3DotProduct(n, light);
            bool hardShadow = -dotL >= shadowHard, hardBright = dotL >= highlightHard;
            bool softShadow = -dotL >= shadowSoft && !hardShadow;
            bool softBright = dotL >= highlightSoft && !hardBright;
            unsigned char a = color[idx].a;
            if (hardShadow)
            {
                color[idx] = (Color){0, 0, 0, a};
                continue;
            }
            if (hardBright)
            {
                color[idx] = (Color){255, 255, 255, a};
                continue;
            }
            if (!softShadow && !softBright)
                continue;
            float v = softShadow ? Clamp((-dotL - shadowSoft) / (shadowHard - shadowSoft), 0, 1)
                                  : Clamp((dotL - highlightSoft) / (highlightHard - highlightSoft), 0, 1);
            float old = v + ERR(x + 1, y + 1);
            int q = old >= 0.5f ? 1 : 0;
            float e = old - (float)q;
            ERR(x + 2, y + 1) += e * 7.0f / 16.0f;
            ERR(x, y + 2) += e * 3.0f / 16.0f;
            ERR(x + 1, y + 2) += e * 5.0f / 16.0f;
            ERR(x + 2, y + 2) += e * 1.0f / 16.0f;
            if (q == 1)
                color[idx] = softShadow ? (Color){0, 0, 0, a} : (Color){255, 255, 255, a};
        }
#undef ERR
    free(err);
}
/* Ground shade, after the bands: upright surfaces dither further toward black near their foot,
   fading out groundHeight metres up, carrying the error the same Floyd-Steinberg way. */
static void DitherGround(Color *color, const Color *normal, const Color *heights, int w, int h, float shade,
                         float groundHeight)
{
    float *err = calloc((size_t)(w + 2) * (size_t)(h + 2), sizeof(float));
    if (!err)
        return;
#define ERR(x, y) err[(size_t)(y) * (size_t)(w + 2) + (size_t)(x)]
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int idx = y * w + x;
            Color c = color[idx];
            if (c.a < 3 || (c.r == 0 && c.g == 0 && c.b == 0))
                continue;
            float ny = normal[idx].g / 255.0f * 2 - 1;
            float metres = heights[idx].r / 255.0f * SPRITE_HEIGHT_RANGE;
            float v = shade * (1 - Clamp(metres / groundHeight, 0, 1)) * (1 - fabsf(ny));
            if (v <= 0.001f)
                continue;
            float old = v + ERR(x + 1, y + 1);
            int q = old >= 0.5f ? 1 : 0;
            float e = old - (float)q;
            ERR(x + 2, y + 1) += e * 7.0f / 16.0f;
            ERR(x, y + 2) += e * 3.0f / 16.0f;
            ERR(x + 1, y + 2) += e * 5.0f / 16.0f;
            ERR(x + 2, y + 2) += e * 1.0f / 16.0f;
            if (q == 1)
                color[idx] = (Color){0, 0, 0, c.a};
        }
#undef ERR
    free(err);
}
void SpriteDither(Color *colour, const Color *normal, int width, int height, const SpriteLight *light)
{
    if (!colour || !normal || !light || width < 1 || height < 1)
        return;
    /* The baker has always passed the light exactly as given; the normal is what gets normalised. */
    DitherFrame(colour, normal, width, height, light->direction, light->shadowSoft, light->shadowHard,
                light->highlightSoft, light->highlightHard);
}
bool SpriteDitherDensity(const unsigned char *density, unsigned char *ink, int width, int height)
{
    if (!density || !ink || width < 1 || height < 1)
        return false;
    size_t count = (size_t)width * (size_t)height;
    Color *colour = malloc(count * sizeof *colour), *normal = malloc(count * sizeof *normal);
    if (!colour || !normal)
    {
        free(colour);
        free(normal);
        return false;
    }
    for (size_t i = 0; i < count; i++)
    {
        float z = density[i] == 0 ? 0 : density[i] == 255 ? -1 : -(.25f + .5f * density[i] / 255.0f);
        colour[i] = WHITE;
        normal[i] = (Color){(unsigned char)roundf((sqrtf(fmaxf(0, 1 - z * z)) * .5f + .5f) * 255), 128,
                            (unsigned char)roundf((z * .5f + .5f) * 255), 255};
    }
    DitherFrame(colour, normal, width, height, (Vector3){0, 0, 1}, .25f, .75f, 2, 3);
    for (size_t i = 0; i < count; i++)
    {
        bool black = colour[i].r == 0 && colour[i].g == 0 && colour[i].b == 0;
        // Endpoints must stay exact even after normal-buffer quantization.
        ink[i] = density[i] == 255 || (density[i] > 0 && black) ? 255 : 0;
    }
    free(colour);
    free(normal);
    return true;
}
/* The same projection drives the colour image, pivot and ground-plane shadow. No image-space
   silhouette shear: a point at ground height stays at exactly the same screen coordinate. */
Matrix SpriteGroundProjection(Vector3 light, float height)
{
    Matrix m = MatrixIdentity();
    m.m4 = -light.x / light.y;
    m.m5 = 0.00001f; /* keep the normal matrix invertible; sub-pixel at any supported bake size */
    m.m6 = -light.z / light.y;
    m.m12 = light.x / light.y * height;
    m.m13 = height;
    m.m14 = light.z / light.y * height;
    return m;
}

bool SpriteModelSkinned(const Model *model)
{
    for (int i = 0; model && i < model->meshCount; i++)
        if (model->meshes[i].boneCount > 0 && model->meshes[i].boneIds && model->meshes[i].boneWeights)
            return true;
    return false;
}

static bool LoadVariant(SpriteRenderer *r, int skinned)
{
    if (r->loaded[skinned])
        return true;
    const char *vertex = skinned ? "core/shaders/skinning.vs" : "core/shaders/lit.vs";
    const ShaderFile files[3] = {{vertex, "core/shaders/toonlit.fs"},
                                 {vertex, "core/shaders/normal_encode.fs"},
                                 {vertex, "core/shaders/height_encode.fs"}};
    if (!CoreLoadShaders(files, 3, r->shaders[skinned]))
        return false;
    const Shader *toon = &r->shaders[skinned][0];
    if (toon->locs[SHADER_LOC_VERTEX_NORMAL] < 0 || toon->locs[SHADER_LOC_MATRIX_NORMAL] < 0 ||
        (skinned && toon->locs[SHADER_LOC_BONE_MATRICES] < 0))
    {
        TraceLog(LOG_ERROR, "SPRITE BAKE: the %s shaders lack the surface interface they need",
                 skinned ? "skinned" : "static");
        CoreUnloadShaders(r->shaders[skinned], 3);
        return false;
    }
    for (int pass = 0; pass < 3; pass++)
        if (!FrameUniformsAdd(&r->uniforms, r->shaders[skinned][pass]))
            return false;
    r->loaded[skinned] = true;
    return true;
}

bool SpriteRendererInit(SpriteRenderer *r)
{
    *r = (SpriteRenderer){0};
    FrameUniformDecl decls[] = {
        {"lightDir", FRAME_VEC3, 1},
        {"lightColor", FRAME_VEC3, 1},
        {"ambientColor", FRAME_VEC3, 1},
        {"toonCutoff", FRAME_FLOAT, 1},
        {"clipGround", FRAME_INT, 1},
        {"clipTransform", FRAME_MATRIX, 1},
    };
    if (!FrameUniformsInit(&r->uniforms, decls, 6))
        return false;
    r->uniformsReady = true;
    /* Static shaders are what every prop needs; skinned ones load the first time a rig is drawn. */
    return LoadVariant(r, 0);
}

void SpriteRendererFree(SpriteRenderer *r)
{
    for (int skinned = 0; skinned < 2; skinned++)
        if (r->loaded[skinned])
            CoreUnloadShaders(r->shaders[skinned], 3);
    if (r->uniformsReady)
        FrameUniformsFree(&r->uniforms);
    CoreUnloadRT(&r->colour);
    CoreUnloadRT(&r->normal);
    CoreUnloadRT(&r->heights);
    *r = (SpriteRenderer){0};
}

static bool Prepare(SpriteRenderer *r, const Model *model, Matrix transform, int width, int height, const SpriteLight *light,
                    int *variant)
{
    if (!model || model->meshCount < 1 || width < 1 || height < 1 || width > 4096 || height > 4096 ||
        !light)
        return false;
    *variant = SpriteModelSkinned(model) ? 1 : 0;
    if (!LoadVariant(r, *variant))
        return false;
    if (r->width != width || r->height != height)
    {
        CoreUnloadRT(&r->colour);
        CoreUnloadRT(&r->normal);
        CoreUnloadRT(&r->heights);
        if (!MakeRT(&r->colour, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, true) ||
            !MakeRT(&r->normal, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, true) ||
            !MakeRT(&r->heights, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, true))
        {
            r->width = r->height = 0;
            return false;
        }
        r->width = width;
        r->height = height;
    }
    Vector3 direction = Vector3Normalize(light->direction);
    int clip = r->clipGround;
    const void *values[] = {&direction, &light->colour, &light->ambient, &light->cutoff, &clip, &transform};
    FrameUniformsBind(&r->uniforms, values);
    return true;
}

static Image Capture(RenderTexture target, const Model *model, Matrix transform, Camera camera, Shader shader,
                     Color tint)
{
    WorldSurf held = {model, transform, tint};
    BeginTextureMode(target);
    ClearBackground(BLANK);
    BeginMode3D(camera);
    DrawWorldSurfaces(&held, 1, &shader);
    EndMode3D();
    EndTextureMode();
    Image image = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&image);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    return image;
}

Image SpriteRenderFrame(SpriteRenderer *r, const Model *model, Matrix transform, Camera camera, int width,
                        int height, const SpriteLight *light)
{
    int variant = 0;
    if (!Prepare(r, model, transform, width, height, light, &variant))
        return (Image){0};
    Image colour = Capture(r->colour, model, transform, camera, r->shaders[variant][0], WHITE);
    Image normal = Capture(r->normal, model, transform, camera, r->shaders[variant][1], WHITE);
    Image heights = light->groundShade > 0
                        ? Capture(r->heights, model, transform, camera, r->shaders[variant][2], WHITE)
                        : (Image){0};
    if (colour.data && normal.data)
        SpriteDither(colour.data, normal.data, width, height, light);
    if (colour.data && normal.data && heights.data && light->groundHeight > 0)
        DitherGround(colour.data, normal.data, heights.data, width, height, light->groundShade, light->groundHeight);
    UnloadImage(normal);
    UnloadImage(heights);
    return colour;
}
Image SpriteRenderHeight(SpriteRenderer *r, const Model *model, Matrix transform, Camera camera, int width,
                         int height)
{
    int variant = 0;
    SpriteLight light = SpriteLightDefault();
    if (!Prepare(r, model, transform, width, height, &light, &variant))
        return (Image){0};
    return Capture(r->heights, model, transform, camera, r->shaders[variant][2], WHITE);
}

Image SpriteRenderShadow(SpriteRenderer *r, const Model *model, Matrix transform, Camera camera, int width,
                         int height, const SpriteLight *light, float groundHeight)
{
    int variant = 0;
    if (!SpriteLightValid(light, true) || !Prepare(r, model, transform, width, height, light, &variant))
        return (Image){0};
    Matrix projected = MatrixMultiply(transform, SpriteGroundProjection(light->direction, groundHeight));
    /* Flattening turns some faces over, and all of them belong in the silhouette. */
    rlDisableBackfaceCulling();
    Image shadow = Capture(r->colour, model, projected, camera, r->shaders[variant][0], BLACK);
    rlEnableBackfaceCulling();
    return shadow;
}

bool SpriteRendererDraw(SpriteRenderer *r, const Model *model, Matrix transform, const SpriteLight *light)
{
    if (!model || model->meshCount < 1 || !light)
        return false;
    int variant = SpriteModelSkinned(model) ? 1 : 0;
    if (!LoadVariant(r, variant))
        return false;
    Vector3 direction = Vector3Normalize(light->direction);
    int clip = r->clipGround;
    const void *values[] = {&direction, &light->colour, &light->ambient, &light->cutoff, &clip, &transform};
    FrameUniformsBind(&r->uniforms, values);
    WorldSurf held = {model, transform, WHITE};
    DrawWorldSurfaces(&held, 1, &r->shaders[variant][0]);
    return true;
}

/* ---- textures ---------------------------------------------------------------------------------- */

typedef struct Box
{
    int from, to; /* into the pixel list */
} Box;
static int Channel(Color c, int k)
{
    return k == 0 ? c.r : k == 1 ? c.g : c.b;
}
static int SortChannel;
static int ByChannel(const void *a, const void *b)
{
    Color x = *(const Color *)a, y = *(const Color *)b;
    int difference = Channel(x, SortChannel) - Channel(y, SortChannel);
    if (difference)
        return difference;
    if (x.r != y.r)
        return (int)x.r - y.r;
    if (x.g != y.g)
        return (int)x.g - y.g;
    return (int)x.b - y.b;
}
static long PaletteDistance(Color a, Color b)
{
    long r = (long)a.r - b.r, g = (long)a.g - b.g, blue = (long)a.b - b.b;
    return 3 * r * r + 4 * g * g + 2 * blue * blue;
}
static int PaletteLuma(Color c)
{
    return 2126 * c.r + 7152 * c.g + 722 * c.b;
}
static int ClosestPalette(Color colour, const Color *palette, int count, int skip)
{
    int best = 0;
    long distance = 1L << 30;
    for (int b = 0; b < count; b++)
    {
        if (b == skip)
            continue;
        long candidate = PaletteDistance(colour, palette[b]);
        if (candidate < distance)
        {
            distance = candidate;
            best = b;
        }
    }
    return best;
}
void SpriteReducePalette(Color *texels, int count, int colours)
{
    if (!texels || colours < 2 || colours > 256 || count < colours)
        return;
    Color *sorted = malloc((size_t)count * sizeof *sorted);
    Box boxes[256];
    if (!sorted)
        return;
    /* Keep real source colours, as raylib/rtextures.c:3084-3120 does when extracting
       a palette. Median cut still allocates the limited slots across the texture's colours. */
    int visible = 0;
    Color darkest = {0}, brightest = {0};
    for (int i = 0; i < count; i++)
        if (texels[i].a)
        {
            Color c = texels[i];
            if (!visible || PaletteLuma(c) < PaletteLuma(darkest))
                darkest = c;
            if (!visible || PaletteLuma(c) > PaletteLuma(brightest))
                brightest = c;
            sorted[visible++] = c;
        }
    if (visible <= colours)
    {
        free(sorted);
        return;
    }
    int boxCount = 1;
    boxes[0] = (Box){0, visible};
    while (boxCount < colours)
    {
        int widest = -1, widestRange = 0, widestChannel = 0;
        for (int b = 0; b < boxCount; b++)
            for (int k = 0; k < 3; k++)
            {
                int lo = 255, hi = 0;
                for (int i = boxes[b].from; i < boxes[b].to; i++)
                {
                    int v = Channel(sorted[i], k);
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
                if (boxes[b].to - boxes[b].from > 1 && hi - lo > widestRange)
                    widest = b, widestRange = hi - lo, widestChannel = k;
            }
        if (widest < 0)
            break;
        Box *box = &boxes[widest];
        SortChannel = widestChannel;
        qsort(sorted + box->from, (size_t)(box->to - box->from), sizeof *sorted, ByChannel);
        int middle = (box->from + box->to) / 2;
        boxes[boxCount++] = (Box){middle, box->to};
        box->to = middle;
    }
    Color palette[256];
    for (int b = 0; b < boxCount; b++)
        palette[b] = sorted[(boxes[b].from + boxes[b].to) / 2];
    /* Reserve the source's darkest and brightest tones. Averaging these groups used to
       pull both ends toward the middle, washing out stone/mortar and highlights. */
    int darkSlot = ClosestPalette(darkest, palette, boxCount, -1);
    palette[darkSlot] = darkest;
    if (boxCount > 1)
        palette[ClosestPalette(brightest, palette, boxCount, darkSlot)] = brightest;
    for (int i = 0; i < count; i++)
    {
        if (!texels[i].a)
            continue;
        Color selected = palette[ClosestPalette(texels[i], palette, boxCount, -1)];
        texels[i].r = selected.r;
        texels[i].g = selected.g;
        texels[i].b = selected.b;
    }
    free(sorted);
}

SpriteTextureFilter SpriteTextureFilterNone(void)
{
    return (SpriteTextureFilter){.brightness = 1};
}
bool SpriteTextureFilterActive(const SpriteTextureFilter *f)
{
    return f && (f->blur > 0.01f || f->denoise > 0 || f->flatten > 0.001f || f->colours >= 2 ||
                 (f->brightness > 0 && fabsf(f->brightness - 1) > 0.001f));
}

/* One pass of a box blur along rows or columns, wrapping at the edges because textures tile. */
static void BoxPass(Color *out, const Color *in, int w, int h, int radius, bool vertical)
{
    int length = vertical ? h : w, lines = vertical ? w : h;
    for (int line = 0; line < lines; line++)
        for (int i = 0; i < length; i++)
        {
            int sum[4] = {0};
            for (int k = -radius; k <= radius; k++)
            {
                int j = ((i + k) % length + length) % length;
                Color c = vertical ? in[j * w + line] : in[line * w + j];
                sum[0] += c.r, sum[1] += c.g, sum[2] += c.b, sum[3] += c.a;
            }
            int n = radius * 2 + 1;
            Color *to = vertical ? &out[i * w + line] : &out[line * w + i];
            *to = (Color){(unsigned char)(sum[0] / n), (unsigned char)(sum[1] / n), (unsigned char)(sum[2] / n),
                          (unsigned char)(sum[3] / n)};
        }
}
static int ByValue(const void *a, const void *b)
{
    return *(const unsigned char *)a - *(const unsigned char *)b;
}
/* The median of each 3x3 neighbourhood, channel by channel: speckle goes, edges stay. */
static void MedianPass(Color *out, const Color *in, int w, int h)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            unsigned char r[9], g[9], b[9];
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++, n++)
                {
                    Color c = in[((y + dy + h) % h) * w + (x + dx + w) % w];
                    r[n] = c.r, g[n] = c.g, b[n] = c.b;
                }
            qsort(r, 9, 1, ByValue);
            qsort(g, 9, 1, ByValue);
            qsort(b, 9, 1, ByValue);
            out[y * w + x] = (Color){r[4], g[4], b[4], in[y * w + x].a};
        }
}

void SpriteFilterImage(Image *image, const SpriteTextureFilter *f)
{
    if (!image || !image->data || !SpriteTextureFilterActive(f))
        return;
    ImageFormat(image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    int w = image->width, h = image->height, count = w * h;
    Color *pixels = image->data, *scratch = malloc((size_t)count * sizeof *scratch);
    if (!scratch)
        return;
    for (int pass = 0; pass < f->denoise && pass < 8; pass++)
    {
        MedianPass(scratch, pixels, w, h);
        memcpy(pixels, scratch, (size_t)count * sizeof *pixels);
    }
    int radius = (int)lroundf(f->blur);
    for (int pass = 0; radius > 0 && pass < 2; pass++) /* two box passes: close enough to a gaussian */
    {
        BoxPass(scratch, pixels, w, h, radius, false);
        BoxPass(pixels, scratch, w, h, radius, true);
    }
    if (f->flatten > 0.001f || (f->brightness > 0 && fabsf(f->brightness - 1) > 0.001f))
    {
        double mean[3] = {0};
        for (int i = 0; i < count; i++)
            mean[0] += pixels[i].r, mean[1] += pixels[i].g, mean[2] += pixels[i].b;
        for (int k = 0; k < 3; k++)
            mean[k] /= count;
        float keep = 1 - Clamp(f->flatten, 0, 1), gain = f->brightness > 0 ? f->brightness : 1;
        for (int i = 0; i < count; i++)
        {
            unsigned char *c = &pixels[i].r;
            for (int k = 0; k < 3; k++)
                c[k] = (unsigned char)Clamp(((float)mean[k] + ((float)c[k] - (float)mean[k]) * keep) * gain, 0, 255);
        }
    }
    free(scratch);
    if (f->colours >= 2)
        SpriteReducePalette(pixels, count, f->colours);
}
