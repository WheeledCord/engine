#ifndef CORE_CAPABILITIES_H
#define CORE_CAPABILITIES_H
#include <stdbool.h>
typedef struct CoreRequirements
{
    int vertexUniformComponents, fragmentUniformComponents, textureUnits, varyingFloats;
} CoreRequirements;
bool CoreCheckCapabilities(CoreRequirements requirements);
bool CoreCheckGraphicsErrors(const char *where);
#endif
