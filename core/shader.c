/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "shader.h"
#include "config.h"
#include "file.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#define CORE_INCLUDE_DEPTH 8

// GLSL 120 has no #include, so shaders that want to share a function have to copy it. This expands
// `#include "path"` before the source reaches the driver: the named file is read through the same
// data root as everything else and pasted in, with #line directives either side so a compile error
// still names a line the author can find. An included file is a fragment, not a shader, so it must
// not carry its own #version.
static char *Expand(const char *path, int depth)
{
    char *s = CoreReadFile(path);
    if (!s)
    {
        TraceLog(LOG_ERROR, "Shader file unreadable: %s", path);
        return NULL;
    }
    if (depth >= CORE_INCLUDE_DEPTH)
    {
        TraceLog(LOG_ERROR, "Shader include nested deeper than %d: %s", CORE_INCLUDE_DEPTH, path);
        CoreFreeFile(s);
        return NULL;
    }
    char *out = malloc(1);
    if (!out)
    {
        CoreFreeFile(s);
        return NULL;
    }
    out[0] = 0;
    size_t used = 0;
    int line = 1;
    for (char *cursor = s; cursor && *cursor;)
    {
        char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) + 1 : strlen(cursor);
        char *piece = NULL;
        bool freePiece = false;
        char quoted[512];
        if (sscanf(cursor, " #include \"%511[^\"]\"", quoted) == 1)
        {
            char *inner = Expand(quoted, depth + 1);
            if (!inner)
            {
                free(out);
                CoreFreeFile(s);
                return NULL;
            }
            // #line in GLSL names a number, not a file: restore the parent's count after the paste.
            size_t n = strlen(inner) + 64;
            piece = malloc(n);
            if (piece)
                snprintf(piece, n, "#line 1\n%s\n#line %d\n", inner, line + 1);
            free(inner);
            freePiece = true;
            if (!piece)
            {
                free(out);
                CoreFreeFile(s);
                return NULL;
            }
            length = strlen(piece);
        }
        else
            piece = cursor;
        char *grown = realloc(out, used + length + 1);
        if (!grown)
        {
            if (freePiece)
                free(piece);
            free(out);
            CoreFreeFile(s);
            return NULL;
        }
        out = grown;
        memcpy(out + used, piece, length);
        used += length;
        out[used] = 0;
        if (freePiece)
            free(piece);
        cursor = end ? end + 1 : NULL;
        line++;
    }
    CoreFreeFile(s);
    return out;
}

/* The file with its includes expanded and, after #version, CORE_BONE_CAPACITY and then `defines`
   (NULL for none). */
static char *Source(const char *path, const char *defines)
{
    char *s = Expand(path, 0);
    if (!s)
        return NULL;
    const char *nl = strchr(s, '\n');
    if (strncmp(s, "#version 120", 12) || !nl)
    {
        TraceLog(LOG_ERROR, "Shader must start with #version 120: %s", path);
        free(s);
        return NULL;
    }
    char define[512];
    if ((size_t)snprintf(define, sizeof define, "#define CORE_BONE_CAPACITY %d\n%s%s#line 2\n", CORE_BONE_CAPACITY,
                         defines ? defines : "", defines && *defines ? "\n" : "") >= sizeof define)
    {
        TraceLog(LOG_ERROR, "Shader defines too long for %s", path);
        free(s);
        return NULL;
    }
    size_t head = (size_t)(nl + 1 - s), n = strlen(s) + strlen(define) + 1;
    char *out = malloc(n);
    if (out)
    {
        memcpy(out, s, head);
        strcpy(out + head, define);
        strcat(out, nl + 1);
    }
    free(s); // Expand returns malloc'd text, not a CoreReadFile buffer
    return out;
}
void CoreUnloadShaders(Shader *shaders, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (shaders[i].id && shaders[i].id != rlGetShaderIdDefault())
            UnloadShader(shaders[i]);
        else if (shaders[i].locs)
            MemFree(shaders[i].locs);
        shaders[i] = (Shader){0};
    }
}
static bool Load(const ShaderFile *files, int count, const char *defines, Shader *out)
{
    if (!files || !out || count < 0)
        return false;
    memset(out, 0, sizeof(*out) * (size_t)count);
    for (int i = 0; i < count; i++)
    {
        char *vs = files[i].vertex ? Source(files[i].vertex, defines) : NULL;
        char *fs = files[i].fragment ? Source(files[i].fragment, defines) : NULL;
        if ((files[i].vertex && !vs) || !fs)
        {
            free(vs);
            free(fs);
            CoreUnloadShaders(out, count);
            return false;
        }
        out[i] = LoadShaderFromMemory(vs, fs);
        free(vs);
        free(fs);
        if (!out[i].id || out[i].id == rlGetShaderIdDefault())
        {
            TraceLog(LOG_ERROR, "Required shader failed: %s / %s",
                     files[i].vertex ? files[i].vertex : "default vertex", files[i].fragment);
            CoreUnloadShaders(out, count);
            return false;
        }
    }
    return true;
}
bool CoreLoadShaders(const ShaderFile *files, int count, Shader *out) { return Load(files, count, NULL, out); }
bool CoreLoadShaderDefined(const ShaderFile *file, const char *defines, Shader *out)
{
    return Load(file, file ? 1 : 0, defines, out);
}
