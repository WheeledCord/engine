/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_SPRITE_BAKE_H
#define CORE_SPRITE_BAKE_H
#include "frame_uniforms.h"
#include "raylib.h"
#include <stdbool.h>

/* Turning a lit 3D model into pixel art: the pipeline Dead Cells is known for, which draws the model
   and keeps the picture rather than the model.

   Each picture is rendered twice from the same camera: once through a hard two-band toon shader, and
   once into a buffer holding the world-space direction every pixel faces. Knowing the direction, the
   soft bands between light and shadow are dithered to flat black and white by Floyd-Steinberg, so the
   result reads as drawn rather than rendered. Any tool that makes sprites -- a command-line baker, an
   editor with a live preview -- goes through these calls, so everything baked shares one look. */

typedef struct SpriteLight
{
    Vector3 direction;       /* toward the light, world space; need not be normalised */
    Vector3 colour, ambient; /* the two bands of the toon pass */
    float cutoff;            /* dot(N,L) at or above which a surface is lit; -2 lights everything */
    float shadowSoft, shadowHard;       /* the band dithered toward black, as -dot(N,L) */
    float highlightSoft, highlightHard; /* the band dithered toward white, as dot(N,L); >1 is off */
    /* Upright surfaces dither toward black at their foot, by up to groundShade (0 off, 1 black at
       the ground) and fading out groundHeight metres up, the way a wall darkens toward the ground.
       Flat ground and tops are left alone. */
    float groundShade, groundHeight;
} SpriteLight;

/* The engine's light: from behind and to the left of the camera (turned -120 degrees about the
   vertical, 45 up), so of the two faces a block shows the camera the left is just lit and the right
   in shade. Albedo is left unlit, black dithered in from the normals only, white highlights off. */
SpriteLight SpriteLightDefault(void);
/* For scenery: the same direction, with the toon pass's own two bands on as well. */
SpriteLight SpriteLightScenery(void);
/* False, with nothing done, for thresholds out of order or a light that does not point upward
   when a ground shadow will be asked for. */
bool SpriteLightValid(const SpriteLight *light, bool castsShadow);

/* Dithers the shadow and highlight bands of one rendered frame in place. colour and normal are
   width*height pixels; normal packs -1..1 into 0..255. Pixels with almost no alpha are left alone. */
void SpriteDither(Color *colour, const Color *normal, int width, int height, const SpriteLight *light);
/* The same Floyd-Steinberg over a density field (0 none .. 255 full, width*height bytes): ink[i] is
   255 where it puts a black pixel, else 0; 0 and 255 stay exact. The density goes in as a normal
   tilted away from a light straight ahead, through SpriteDither's soft shadow band, so it dithers as
   `sprite-baker --dither-mask` always has. False when out of memory. */
bool SpriteDitherDensity(const unsigned char *density, unsigned char *ink, int width, int height);

/* Squashes geometry onto the horizontal plane at `height` along the light, which is what a planar
   cast shadow is. Used for both the shadow picture and for framing it. */
Matrix SpriteGroundProjection(Vector3 light, float height);

/* The GPU half: shaders for skinned and unskinned meshes, and the two render targets a frame needs.
   Create it after the window exists. */
typedef struct SpriteRenderer
{
    Shader shaders[2][3]; /* [skinned][0 toon, 1 normals, 2 heights] */
    bool loaded[2];
    FrameUniforms uniforms;
    bool uniformsReady;
    /* Discard geometry below world Y=0 in every pass, before shadow projection.
       Off by default. Set before drawing/baking; this does not modify or copy the model. */
    bool clipGround;
    RenderTexture colour, normal, heights;
    int width, height;
} SpriteRenderer;

bool SpriteRendererInit(SpriteRenderer *renderer);
void SpriteRendererFree(SpriteRenderer *renderer);
/* One finished frame: the model, as it is currently posed, drawn with transform through camera into
   a width*height image with the bands already dithered. The image belongs to the caller; an empty
   image (data NULL) means the render failed. */
Image SpriteRenderFrame(SpriteRenderer *renderer, const Model *model, Matrix transform, Camera camera,
                        int width, int height, const SpriteLight *light);
/* The model's planar shadow on the ground at groundHeight, opaque black on clear, through the same
   camera so it registers with the frame. */
Image SpriteRenderShadow(SpriteRenderer *renderer, const Model *model, Matrix transform, Camera camera,
                         int width, int height, const SpriteLight *light, float groundHeight);
/* How high above the ground every pixel of the frame is: red is the height over SPRITE_HEIGHT_RANGE
   metres (0 to 255), alpha says whether anything is there. */
#define SPRITE_HEIGHT_RANGE 8.0f
Image SpriteRenderHeight(SpriteRenderer *renderer, const Model *model, Matrix transform, Camera camera, int width,
                         int height);
/* Draws the model with the toon pass inside the caller's own BeginMode3D, for a live view of what
   the bake will shade: the same shaders and bands, without the dither. */
bool SpriteRendererDraw(SpriteRenderer *renderer, const Model *model, Matrix transform, const SpriteLight *light);
/* Taming a texture before it is baked. Pixel art reads by its shapes; a photographic texture with
   fine noise comes out of the dither as speckle. These run on the CPU, on the texture's own pixels,
   in this order: denoise, blur, flatten and brightness, then fewer colours. */
typedef struct SpriteTextureFilter
{
    int denoise;      /* 3x3 median passes: removes speckle, keeps edges */
    float blur;       /* blur radius in texels */
    float flatten;    /* 0..1: pull every colour toward the texture's average, lowering contrast */
    float brightness; /* multiplier; 1 (or 0) leaves it */
    int colours;      /* reduce to this many source colours; below 2 leaves them */
} SpriteTextureFilter;
SpriteTextureFilter SpriteTextureFilterNone(void);
bool SpriteTextureFilterActive(const SpriteTextureFilter *filter);
/* Filters an image in place; it is converted to RGBA8 first. Alpha is kept. */
void SpriteFilterImage(Image *image, const SpriteTextureFilter *filter);
/* Reduces RGB to at most `colours` existing, visible source colours using median-cut groups.
   Retains the darkest and brightest source tones instead of averaging the groups. Alpha and fully
   transparent pixels are unchanged. No-op for NULL, fewer pixels than colours, or colours outside
   2..256. Allocation failure leaves the input unchanged. */
void SpriteReducePalette(Color *pixels, int count, int colours);

/* Whether a model needs the skinning shader: any mesh with bones does, and drawing an unskinned mesh
   through it would gather every vertex onto the origin. */
bool SpriteModelSkinned(const Model *model);
#endif
