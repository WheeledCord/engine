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
/* NULL vertex selects raylib's default vertex shader. No implicit fallback on error.

   Sources must open with `#version 120` and may use `#include "path"`, which GLSL itself has no
   form of: the named file is read through the engine's data root and pasted in before the driver
   sees it, nested up to eight deep. An included file is a fragment of a shader, not a shader, so it
   carries no `#version` of its own. */
bool CoreLoadShaders(const ShaderFile *files, int count, Shader *out);
void CoreUnloadShaders(Shader *shaders, int count);
#endif
