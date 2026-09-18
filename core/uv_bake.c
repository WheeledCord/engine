/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "uv_bake.h"
#include "rlgl.h"
#include <config.h> /* raylib build configuration: material map array size */
#include <stdlib.h>
#include <string.h>

bool CoreBakeMeshUV(Mesh mesh, Material material, RenderTexture target, Matrix transform)
{
    if (!mesh.vertexCount || !mesh.texcoords || !material.shader.id || !target.id)
        return false;
    BeginTextureMode(target);
    // The layout is flat and triangles overlap in UV only where the unwrap made them: a depth test
    // would drop the later one for reasons that have nothing to do with the surface. Culling has to
    // go for the same reason -- a bake has no camera, so whether a triangle winds toward one is not
    // a fact about the surface, and a back-facing patch of the layout would come out empty.
    rlDisableDepthTest();
    rlDisableBackfaceCulling();
    // Blending has to go too, and for a sharper reason than the other two: a bake stores what the
    // material computed, and the default blend multiplies colour by alpha on the way in. A surface
    // map that carries height in alpha would come out with its colour scaled by its own height.
    rlDisableColorBlend();
    DrawMesh(mesh, material, transform);
    rlEnableColorBlend();
    rlEnableBackfaceCulling();
    rlEnableDepthTest();
    EndTextureMode();
    return true;
}
/* One ring of growth, shared by the two dilation entry points. Returns how many texels it filled,
   so the caller that must not stop early can tell when there is nothing left to do. */
static int DilateRing(Color *pixels, Color *previous, int w, int h)
{
    int filled = 0;
    memcpy(previous, pixels, (size_t)w * (size_t)h * sizeof *previous);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            if (previous[y * w + x].a)
                continue; // already surface
            int r = 0, g = 0, b = 0, a = 0, found = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    Color n = previous[ny * w + nx];
                    if (!n.a)
                        continue;
                    r += n.r;
                    g += n.g;
                    b += n.b;
                    a += n.a;
                    found++;
                }
            if (!found)
                continue;
            pixels[y * w + x] = (Color){(unsigned char)(r / found), (unsigned char)(g / found),
                                        (unsigned char)(b / found), (unsigned char)(a / found)};
            filled++;
        }
    return filled;
}

bool CoreDilateUVSeams(Image *image, int rings)
{
    if (!image || !image->data || image->format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        return false;
    if (rings <= 0)
        return true;
    int w = image->width, h = image->height;
    Color *pixels = (Color *)image->data;
    Color *previous = malloc((size_t)w * (size_t)h * sizeof *previous);
    if (!previous)
        return false;
    for (int ring = 0; ring < rings; ring++)
        DilateRing(pixels, previous, w, h);
    free(previous);
    return true;
}

bool CoreFillUVBackground(Image *image)
{
    if (!image || !image->data || image->format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
        return false;
    int w = image->width, h = image->height;
    Color *pixels = (Color *)image->data;
    Color *previous = malloc((size_t)w * (size_t)h * sizeof *previous);
    if (!previous)
        return false;
    /* Each pass reaches one texel further, so the worst case is the longest run of empty space,
       bounded by the image itself. Stopping when a pass fills nothing ends it as soon as the image
       is full -- and also ends it immediately when there was nothing written to spread from, which
       is the false below rather than a silent success on an untouched image. */
    int filled = 0, total = 0;
    while ((filled = DilateRing(pixels, previous, w, h)) > 0)
        total += filled;
    free(previous);
    return total > 0;
}
