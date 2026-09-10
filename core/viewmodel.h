/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_VIEWMODEL_H
#define CORE_VIEWMODEL_H
#include "raylib.h"
#include <stdbool.h>
typedef struct ViewmodelProjection
{
    float fov, aspect, depth;
} ViewmodelProjection;
/* Call inside BeginMode3D; callback must not start another camera pass. */
bool DrawViewmodel(ViewmodelProjection config, void (*draw)(void *), void *context);
Matrix ViewmodelTransform(Vector3 cameraPosition, Vector3 forward, Vector3 up, Vector3 offset);
#endif
