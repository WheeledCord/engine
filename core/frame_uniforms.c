/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "frame_uniforms.h"
#include "config.h"
#include <GL/gl.h>
#include "rlgl.h"
#include <stdlib.h>
#include <string.h>
bool FrameUniformsInit(FrameUniforms *r, const FrameUniformDecl *d, int n)
{
    *r = (FrameUniforms){0};
    if (n <= 0 || !d)
        return false;
    r->decls = calloc((size_t)n, sizeof(*d));
    if (!r->decls)
        return false;
    r->count = n;
    int textures = 0;
    for (int i = 0; i < n; i++)
    {
        if (!d[i].name || d[i].count < 1 || d[i].type < FRAME_FLOAT || d[i].type > FRAME_TEXTURE ||
            (d[i].type == FRAME_TEXTURE && d[i].count != 1))
        {
            FrameUniformsFree(r);
            return false;
        }
        char *name = malloc(strlen(d[i].name) + 1);
        if (!name)
        {
            FrameUniformsFree(r);
            return false;
        }
        strcpy(name, d[i].name);
        r->decls[i] = d[i];
        r->decls[i].name = name;
        textures += d[i].type == FRAME_TEXTURE;
    }
    // Declared up front, so a card without the units says so now instead of sampling the wrong
    // texture later. Nothing here falls back to sharing raylib's units.
    GLint units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
    if (textures > 0 && units < CORE_FRAME_TEXTURE_FIRST_UNIT + textures)
    {
        TraceLog(LOG_ERROR, "Frame textures need %d texture units, this card has %d",
                 CORE_FRAME_TEXTURE_FIRST_UNIT + textures, units);
        FrameUniformsFree(r);
        return false;
    }
    return true;
}
bool FrameUniformsAdd(FrameUniforms *r, Shader shader)
{
    if (!r->count || !shader.id)
        return false;
    for (int i = 0; i < r->shaderCount; i++)
        if (r->shaders[i].id == shader.id)
            return true;
    int n = r->shaderCount + 1;
    Shader *s = malloc(sizeof(*s) * (size_t)n);
    int *l = malloc(sizeof(*l) * (size_t)n * (size_t)r->count);
    if (!s || !l)
    {
        free(s);
        free(l);
        return false;
    }
    if (r->shaderCount)
    {
        memcpy(s, r->shaders, sizeof(*s) * (size_t)r->shaderCount);
        memcpy(l, r->locations, sizeof(*l) * (size_t)r->shaderCount * (size_t)r->count);
    }
    s[n - 1] = shader;
    for (int j = 0; j < r->count; j++)
        l[(n - 1) * r->count + j] = GetShaderLocation(shader, r->decls[j].name);
    free(r->shaders);
    free(r->locations);
    r->shaders = s;
    r->locations = l;
    r->shaderCount = n;
    return true;
}
void FrameUniformsBind(const FrameUniforms *r, const void *const *v)
{
    static const int types[] = {SHADER_UNIFORM_FLOAT, SHADER_UNIFORM_VEC2, SHADER_UNIFORM_VEC3,
                                SHADER_UNIFORM_VEC4,  SHADER_UNIFORM_INT,  SHADER_UNIFORM_IVEC2,
                                SHADER_UNIFORM_IVEC3, SHADER_UNIFORM_IVEC4};
    rlDrawRenderBatchActive();
    for (int i = 0; i < r->shaderCount; i++)
    {
        int textureSlot = 0;
        for (int j = 0; j < r->count; j++)
        {
            int loc = r->locations[i * r->count + j];
            if (loc < 0 || !v[j])
                continue;
            FrameUniformDecl d = r->decls[j];
            if (d.type == FRAME_MATRIX)
            {
                rlEnableShader(r->shaders[i].id);
                rlSetUniformMatrices(loc, v[j], d.count);
                rlDisableShader();
            }
            else if (d.type == FRAME_TEXTURE)
            {
                // Not SetShaderValueTexture: that only registers the texture with raylib's batch,
                // which binds it when the batch flushes and forgets it again, while DrawMesh binds
                // material maps over the same units. A frame texture keeps a unit of its own for
                // the whole frame, so every world draw samples the texture that was bound.
                int unit = CORE_FRAME_TEXTURE_FIRST_UNIT + textureSlot++;
                rlActiveTextureSlot(unit);
                rlEnableTexture(((const Texture *)v[j])->id);
                rlActiveTextureSlot(0);
                rlEnableShader(r->shaders[i].id);
                rlSetUniform(loc, &unit, RL_SHADER_UNIFORM_INT, 1);
            }
            else
                SetShaderValueV(r->shaders[i], loc, v[j], types[d.type], d.count);
        }
    }
}
void FrameUniformsFree(FrameUniforms *r)
{
    for (int i = 0; i < r->count; i++)
        free((void *)r->decls[i].name);
    free(r->decls);
    free(r->shaders);
    free(r->locations);
    *r = (FrameUniforms){0};
}
