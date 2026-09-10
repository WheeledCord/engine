/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_SHADER_H
#define CORE_SHADER_H
#include "raylib.h"
#include <stdbool.h>
typedef struct ShaderFile
{
    const char *vertex, *fragment;
} ShaderFile;
/* NULL vertex selects raylib's default vertex shader. No implicit fallback on error. */
bool CoreLoadShaders(const ShaderFile *files, int count, Shader *out);
void CoreUnloadShaders(Shader *shaders, int count);
#endif
