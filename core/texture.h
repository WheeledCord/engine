/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_TEXTURE_H
#define CORE_TEXTURE_H

#include "raylib.h"
#include <stdbool.h>

/** @brief Options applied once after loading a texture. */
typedef struct CoreTextureOptions
{
    bool mipmaps;
    int filter;
    int wrap;
} CoreTextureOptions;

/** @brief Return point filtering, repeat wrapping, and no generated mipmaps.
 * @return Default options suitable for pixel art and other unfiltered textures. */
CoreTextureOptions CoreTextureOptionsDefault(void);

/** @brief Load a texture through the engine data root and configure its sampling.
 * @param out Receives the owned texture. It is cleared before loading and must not alias another
 * texture owner.
 * @param path Absolute, working-directory-relative, or data-root-relative image path.
 * @param options Whether to generate mipmaps and the raylib TextureFilter/TextureWrap values.
 * @return True when the image was loaded. On failure, out is an empty texture. The caller owns one
 * successful result and must release it exactly once with CoreUnloadTexture. Copies may be used as
 * non-owning references by any number of materials. */
bool CoreLoadTexture(Texture2D *out, const char *path, CoreTextureOptions options);

/** @brief Release an owned texture and clear its handle.
 * @param texture Texture returned by CoreLoadTexture; NULL and empty textures are allowed.
 * @return Nothing. Non-owning copies must not be passed to this function. */
void CoreUnloadTexture(Texture2D *texture);


#endif
