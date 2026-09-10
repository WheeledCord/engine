#include "mesh_builder.h"
#include "raymath.h"
#include <limits.h>
#include <string.h>
static bool Reserve(MB *m, size_t n)
{
    if (n <= m->capacity)
        return true;
    if (n > INT_MAX / 3 || n > UINT_MAX / (3 * sizeof(float)))
        return false;
    size_t cap = m->capacity ? m->capacity : 16;
    while (cap < n)
        cap *= 2;
    if (cap > UINT_MAX / (3 * sizeof(float)))
        cap = n;
    float *v = MemAlloc((unsigned int)(cap * 3 * sizeof(float))),
          *normal = MemAlloc((unsigned int)(cap * 3 * sizeof(float))),
          *uv = MemAlloc((unsigned int)(cap * 2 * sizeof(float)));
    if (!v || !normal || !uv)
    {
        MemFree(v);
        MemFree(normal);
        MemFree(uv);
        return false;
    }
    if (m->count)
    {
        memcpy(v, m->vertices, m->count * 3 * sizeof(float));
        memcpy(normal, m->normals, m->count * 3 * sizeof(float));
        memcpy(uv, m->uvs, m->count * 2 * sizeof(float));
    }
    MemFree(m->vertices);
    MemFree(m->normals);
    MemFree(m->uvs);
    m->vertices = v;
    m->normals = normal;
    m->uvs = uv;
    m->capacity = cap;
    return true;
}
bool MBInit(MB *m, size_t capacity)
{
    *m = (MB){0};
    return Reserve(m, capacity ? capacity : 16);
}
void MBFree(MB *m)
{
    MemFree(m->vertices);
    MemFree(m->normals);
    MemFree(m->uvs);
    *m = (MB){0};
}
bool MBVert(MB *m, Vector3 p, Vector3 n, Vector2 uv)
{
    if (!Reserve(m, m->count + 1))
        return false;
    size_t i = m->count++;
    m->vertices[i * 3] = p.x;
    m->vertices[i * 3 + 1] = p.y;
    m->vertices[i * 3 + 2] = p.z;
    m->normals[i * 3] = n.x;
    m->normals[i * 3 + 1] = n.y;
    m->normals[i * 3 + 2] = n.z;
    m->uvs[i * 2] = uv.x;
    m->uvs[i * 2 + 1] = uv.y;
    return true;
}
bool MBVertUV(MB *m, Vector3 p, Vector3 n, float u, float v)
{
    return MBVert(m, p, n, (Vector2){u, v});
}
bool EmitQuadV(MB *m, const Vector3 p[4], const Vector3 n[4], const Vector2 uv[4])
{
    if (!Reserve(m, m->count + 6))
        return false;
    const int ids[] = {0, 1, 2, 2, 1, 3};
    for (int i = 0; i < 6; i++)
        MBVert(m, p[ids[i]], n[ids[i]], uv[ids[i]]);
    return true;
}
bool EmitQuadN(MB *m, const Vector3 p[4], Vector3 n, const Vector2 uv[4])
{
    Vector3 ns[] = {n, n, n, n};
    return EmitQuadV(m, p, ns, uv);
}
bool MBMesh(MB *m, Mesh *out)
{
    *out = (Mesh){0};
    if (!m->count || m->count % 3 || m->count > INT_MAX)
        return false;
    out->vertexCount = (int)m->count;
    out->triangleCount = (int)m->count / 3;
    out->vertices = m->vertices;
    out->normals = m->normals;
    out->texcoords = m->uvs;
    UploadMesh(out, false);
    *m = (MB){0};
    if (!out->vboId || !out->vboId[0])
    {
        UnloadMesh(*out);
        *out = (Mesh){0};
        return false;
    }
    return true;
}
bool MBModel(MB *m, Model *out)
{
    Mesh mesh;
    if (!MBMesh(m, &mesh))
        return false;
    *out = LoadModelFromMesh(mesh);
    return out->meshCount > 0;
}
DirectionalFrame MeshDirectionalFrame(Vector3 direction, Vector3 normal)
{
    Vector3 n = Vector3Normalize(normal), d = Vector3Normalize(direction);
    if (Vector3LengthSqr(d) < 0.000001f)
        d = (Vector3){0, 1, 0};
    if (Vector3LengthSqr(n) < 0.000001f)
        n = (Vector3){0, 1, 0};
    Vector3 across = Vector3CrossProduct(n, d);
    if (Vector3Length(across) < 0.2f)
    {
        Vector3 up = fabsf(n.y) > 0.9f ? (Vector3){1, 0, 0} : (Vector3){0, 1, 0};
        across = Vector3Normalize(Vector3CrossProduct(up, n));
        d = Vector3CrossProduct(n, across);
    }
    else
        across = Vector3Normalize(across);
    return (DirectionalFrame){d, across};
}
Vector2 MeshDirectionalUV(DirectionalFrame f, Vector3 p, Vector2 s)
{
    return (Vector2){Vector3DotProduct(f.across, p) * s.x, Vector3DotProduct(f.along, p) * s.y};
}
