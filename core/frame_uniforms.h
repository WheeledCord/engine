#ifndef CORE_FRAME_UNIFORMS_H
#define CORE_FRAME_UNIFORMS_H
#include "raylib.h"
#include <stdbool.h>
typedef enum FrameUniformType
{
    FRAME_FLOAT,
    FRAME_VEC2,
    FRAME_VEC3,
    FRAME_VEC4,
    FRAME_INT,
    FRAME_IVEC2,
    FRAME_IVEC3,
    FRAME_IVEC4,
    FRAME_MATRIX,
    FRAME_TEXTURE
} FrameUniformType;
typedef struct FrameUniformDecl
{
    const char *name;
    FrameUniformType type;
    int count;
} FrameUniformDecl;
typedef struct FrameUniforms
{
    FrameUniformDecl *decls;
    int count;
    Shader *shaders;
    int *locations;
    int shaderCount;
} FrameUniforms;
/* Copies declarations/names; borrows shader handles. Destroy before unloading shaders. */
bool FrameUniformsInit(FrameUniforms *r, const FrameUniformDecl *decls, int count);
bool FrameUniformsAdd(FrameUniforms *r, Shader shader);
/* One pointer per declaration, valid for this call; NULL skips a value. */
void FrameUniformsBind(const FrameUniforms *r, const void *const *values);
void FrameUniformsFree(FrameUniforms *r);
#endif
