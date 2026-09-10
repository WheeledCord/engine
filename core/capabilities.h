#ifndef CORE_CAPABILITIES_H
#define CORE_CAPABILITIES_H
#include <stdbool.h>
/* What a project needs of the GPU. Core enforces exactly this and nothing else: a project that
   declares nothing boots with no probe, no shader compiled and no limit demanded beyond core's own
   OpenGL 2.1 build contract. Anything declared and unavailable is fatal; there are no fallbacks. */
typedef struct CoreRequirements
{
    int vertexUniformComponents, fragmentUniformComponents, textureUnits, varyingFloats;
    int vertexAttributes;
    bool gpuSkinning;     /* core's skinning shader interface, and the bone matrix budget it costs */
    bool renderTargets;   /* framebuffer objects */
    bool sampleableDepth; /* depth attached as a texture rather than a renderbuffer */
} CoreRequirements;
bool CoreCheckCapabilities(CoreRequirements requirements);
bool CoreCheckGraphicsErrors(const char *where);
#endif
