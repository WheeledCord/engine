/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UV_BAKE_H
#define CORE_UV_BAKE_H
#include "raylib.h"
#include <stdbool.h>

/**
 * @brief Draws a mesh into its own UV layout, evaluating a material shader per texel.
 *
 * The mesh is rasterized flat into the target: its texture coordinate becomes the clip position, so
 * every texel the mesh covers in UV space receives that surface point. Pair with
 * core/shaders/uv_unwrap.vs, which does the UV-to-clip mapping and passes object position and
 * normal through as varyings; a material shader written against object space then evaluates exactly
 * as it does when the mesh is drawn normally. Depth testing is off and nothing is cleared, so
 * several meshes sharing one layout can be baked into one target by calling this once each. Depth
 * testing, back-face culling and colour blending are all off while it draws: what the material
 * computed is what lands in the target, alpha included, so alpha is free to carry something other
 * than coverage.
 *
 * The caller owns the target and the shader, and both must outlive the call. Requires a current
 * rendering context. Meshes whose UVs overlap or fall outside 0..1 bake the overlapping region more
 * than once, last draw winning; this routine does not unwrap, validate or repack UVs.
 * @param mesh Borrowed mesh; it must carry texture coordinates.
 * @param material Borrowed material carrying the shader and any maps it samples; a material whose
 * surface is itself built from a texture binds that texture here, as when drawing normally.
 * @param target Borrowed render target, already created; its size is the texture resolution.
 * @param transform Object-to-world matrix the shader sees as matModel; identity bakes object space.
 * @return True when the mesh was drawn, false when an argument is missing or unusable.
 */
bool CoreBakeMeshUV(Mesh mesh, Material material, RenderTexture target, Matrix transform);

/**
 * @brief Grows baked texels outward over the empty space around them.
 *
 * A triangle rasterized into UV space covers whole texels only in its interior; along its edges the
 * filtered lookups a renderer does reach past it into texels no triangle wrote, which appear as
 * unwritten seams across the model. This floods each empty texel with the average of its written
 * neighbours, repeated for the given number of rings, so those lookups land on surface colour.
 * Texels with alpha zero are the empty ones. Filled texels receive the averaged RGBA value of
 * their written neighbours, preserving alpha payloads such as surface height instead of replacing
 * them with opacity. Written alpha must therefore be nonzero; encode a true zero payload as the
 * smallest nonzero value before dilation.
 *
 * The image is modified in place and must be PIXELFORMAT_UNCOMPRESSED_R8G8B8A8.
 * @param image Borrowed image, modified in place; NULL is rejected.
 * @param rings How many texels to grow by; zero or less does nothing.
 * @return True when the image was processed, false when it is NULL or the wrong format.
 */
bool CoreDilateUVSeams(Image *image, int rings);
#endif
