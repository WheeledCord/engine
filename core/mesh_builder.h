#ifndef CORE_MESH_BUILDER_H
#define CORE_MESH_BUILDER_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
typedef struct MB
{
    float *vertices, *normals, *uvs;
    size_t count, capacity;
} MB;
bool MBInit(MB *m, size_t capacity);
void MBFree(MB *m);
bool MBVert(MB *m, Vector3 position, Vector3 normal, Vector2 uv);
bool MBVertUV(MB *m, Vector3 position, Vector3 normal, float u, float v);
bool EmitQuadN(MB *m, const Vector3 positions[4], Vector3 normal, const Vector2 uvs[4]);
bool EmitQuadV(MB *m, const Vector3 positions[4], const Vector3 normals[4], const Vector2 uvs[4]);
/* Success transfers buffers to raylib and empties the builder. */
bool MBMesh(MB *m, Mesh *out);
bool MBModel(MB *m, Model *out);
typedef struct DirectionalFrame
{
    Vector3 along, across;
} DirectionalFrame;
DirectionalFrame MeshDirectionalFrame(Vector3 direction, Vector3 normal);
Vector2 MeshDirectionalUV(DirectionalFrame frame, Vector3 position, Vector2 scale);
#endif
