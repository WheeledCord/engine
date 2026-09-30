/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_VIEWMODEL_H
#define CORE_VIEWMODEL_H
#include "raylib.h"
#include <stdbool.h>
/* fov: vertical, in degrees. depth: the share of the depth range the pass squeezes into, nearest
   the eye (1 for all of it). */
typedef struct ViewmodelProjection
{
    float fov, aspect, depth;
} ViewmodelProjection;
/* Call inside BeginMode3D; callback must not start another camera pass. False, drawing nothing,
   for a field of view outside (0, 180) or an aspect or depth out of range. */
bool DrawViewmodel(ViewmodelProjection config, void (*draw)(void *), void *context);
/* The same pass with the depth buffer cleared first, so nothing drawn before it (a wall the camera
   is against) hides what it draws, and with clip planes of its own: while the callback runs,
   rlgl's clip planes are nearPlane and farPlane (metres), so a draw path begun inside it culls and
   projects with them too; they are put back after. The project runner's viewmodel pass
   (docs/developer/store.md §3.1). False, drawing nothing, as DrawViewmodel, or unless
   0 < nearPlane < farPlane. */
bool DrawViewmodelCleared(ViewmodelProjection config, float nearPlane, float farPlane, void (*draw)(void *),
                          void *context);
Matrix ViewmodelTransform(Vector3 cameraPosition, Vector3 forward, Vector3 up, Vector3 offset);
#endif
