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
