/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "texture.h"
#include "file.h"

CoreTextureOptions CoreTextureOptionsDefault(void)
{
    return (CoreTextureOptions){false, TEXTURE_FILTER_POINT, TEXTURE_WRAP_REPEAT};
}

bool CoreLoadTexture(Texture2D *out, const char *path, CoreTextureOptions options)
{
    if (!out)
        return false;
    *out = (Texture2D){0};
    if (!path || !*path)
        return false;
    char resolved[1024];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    if (!actual || !FileExists(actual))
        return false;
    Texture2D texture = LoadTexture(actual);
    if (!texture.id)
        return false;
    if (options.mipmaps)
        GenTextureMipmaps(&texture);
    SetTextureFilter(texture, options.filter);
    SetTextureWrap(texture, options.wrap);
    *out = texture;
    return true;
}

void CoreUnloadTexture(Texture2D *texture)
{
    if (!texture)
        return;
    if (texture->id)
        UnloadTexture(*texture);
    *texture = (Texture2D){0};
}
