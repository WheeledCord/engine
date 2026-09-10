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
