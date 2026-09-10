#include "shader.h"
#include "config.h"
#include "file.h"
#include "rlgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *Source(const char *path)
{
    char *s = CoreReadFile(path);
    if (!s)
    {
        TraceLog(LOG_ERROR, "Shader file unreadable: %s", path);
        return NULL;
    }
    const char *nl = strchr(s, '\n');
    if (strncmp(s, "#version 120", 12) || !nl)
    {
        TraceLog(LOG_ERROR, "Shader must start with #version 120: %s", path);
        CoreFreeFile(s);
        return NULL;
    }
    char define[96];
    snprintf(define, sizeof define, "#define CORE_BONE_CAPACITY %d\n#line 2\n", CORE_BONE_CAPACITY);
    size_t head = (size_t)(nl + 1 - s), n = strlen(s) + strlen(define) + 1;
    char *out = malloc(n);
    if (out)
    {
        memcpy(out, s, head);
        strcpy(out + head, define);
        strcat(out, nl + 1);
    }
    CoreFreeFile(s);
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
bool CoreLoadShaders(const ShaderFile *files, int count, Shader *out)
{
    if (!files || !out || count < 0)
        return false;
    memset(out, 0, sizeof(*out) * (size_t)count);
    for (int i = 0; i < count; i++)
    {
        char *vs = files[i].vertex ? Source(files[i].vertex) : NULL;
        char *fs = files[i].fragment ? Source(files[i].fragment) : NULL;
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
