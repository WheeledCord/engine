/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_WORLD_DRAW_H
#define CORE_WORLD_DRAW_H
#include "raylib.h"
typedef struct WorldSurf
{
    const Model *model;
    Matrix transform;
    Color tint;
} WorldSurf;
/* transform is the complete model-to-world transform; models/materials are not mutated. */
void DrawWorldSurfaces(const WorldSurf *surfaces, int count, const Shader *override);
#endif
