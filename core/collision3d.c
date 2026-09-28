/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "collision3d.h"

#include "raymath.h"

void CoreCollision3DClear(CoreCollision3D *collision)
{
    if (!collision)
        return;
    collision->count = 0;
}

bool CoreCollision3DAdd(CoreCollision3D *collision, const Model *model, unsigned layers, int tag)
{
    if (!collision || !model || collision->count >= CORE_COLLISION3D_MAX)
        return false;
    collision->entries[collision->count++] = (CoreCollision3DEntry){model, layers, tag};
    return true;
}

CoreRayHit CoreCollision3DRay(const CoreCollision3D *collision, Ray ray, float maxDistance, unsigned mask)
{
    CoreRayHit best = {0};
    best.distance = maxDistance;
    if (!collision)
        return (CoreRayHit){0};
    for (int i = 0; i < collision->count; i++)
    {
        const CoreCollision3DEntry *entry = &collision->entries[i];
        if ((entry->layers & mask) == 0 || !entry->model)
            continue;
        for (int mi = 0; mi < entry->model->meshCount; mi++)
        {
            RayCollision hit = GetRayCollisionMesh(ray, entry->model->meshes[mi], entry->model->transform);
            if (hit.hit && hit.distance > 0 && hit.distance < best.distance)
            {
                best.hit = true;
                best.distance = hit.distance;
                best.point = hit.point;
                best.normal = hit.normal;
                best.tag = entry->tag;
            }
        }
    }
    if (best.hit)
    {
        best.normal = Vector3Normalize(best.normal);
        if (Vector3DotProduct(best.normal, ray.direction) > 0)
            best.normal = Vector3Negate(best.normal);
    }
    return best;
}
