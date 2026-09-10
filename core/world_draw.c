/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "world_draw.h"
#include <config.h> /* raylib build configuration: material map array size */
void DrawWorldSurfaces(const WorldSurf *s, int count, const Shader *override)
{
    for (int i = 0; i < count; i++)
    {
        const Model *m = s[i].model;
        if (!m)
            continue;
        for (int j = 0; j < m->meshCount; j++)
        {
            int k = m->meshMaterial[j];
            if (k < 0 || k >= m->materialCount)
                continue;
            Material material = m->materials[k];
            MaterialMap maps[MAX_MATERIAL_MAPS];
            for (int a = 0; a < MAX_MATERIAL_MAPS; a++)
                maps[a] = material.maps[a];
            material.maps = maps;
            Color c = maps[MATERIAL_MAP_DIFFUSE].color, t = s[i].tint;
            maps[MATERIAL_MAP_DIFFUSE].color =
                (Color){c.r * t.r / 255, c.g * t.g / 255, c.b * t.b / 255, c.a * t.a / 255};
            if (override)
                material.shader = *override;
            DrawMesh(m->meshes[j], material, s[i].transform);
        }
    }
}
