/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Connected walls reuse the top and two visible sides of an ordinary baked
   block. Topology follows the four cardinal connections in OpenRA's
   WithWallSpriteBody.cs:51-75, 124-159; this compositor combines surfaces
   instead of selecting 16 complete sprites. */
#include "iso_wall_internal.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct IsoWallCache
{
    unsigned char *outside;
    int *queue;
    int size;
    uint64_t key;
    bool valid;
};
struct IsoWallCache *IsoWallCacheNew(void)
{
    return calloc(1, sizeof(struct IsoWallCache));
}
void IsoWallCacheFree(struct IsoWallCache *c)
{
    if (c)
    {
        free(c->outside);
        free(c->queue);
        free(c);
    }
}
void IsoWallInvalidate(IsoMap *m)
{
    if (m->wallCache)
        m->wallCache->valid = false;
}
static bool Wall(const IsoMap *m, int x, int y)
{
    return IsoMapWallAt(m, (IsoTile){x, y}) >= 0;
}
static bool Outside(const IsoMap *m, int x, int y)
{
    return x < 0 || y < 0 || x >= m->width || y >= m->height ||
           (m->wallCache && m->wallCache->valid && m->wallCache->outside[y * m->width + x]);
}
bool IsoWallPrepare(const IsoMap *m)
{
    struct IsoWallCache *c = m->wallCache;
    if (!c || !m->walls)
        return false;
    int size = m->width * m->height;
    uint64_t key = UINT64_C(1469598103934665603);
    for (int i = 0; i < size; i++)
        key = (key ^ (unsigned char)m->walls[i]) * UINT64_C(1099511628211);
    if (c->valid && c->size == size && c->key == key)
        return true;
    c->valid = false;
    if (c->size != size)
    {
        free(c->outside);
        free(c->queue);
        c->outside = calloc((size_t)size, 1);
        c->queue = malloc((size_t)size * sizeof(int));
        c->size = size;
    }
    if (!c->outside || !c->queue)
        return false;
    memset(c->outside, 0, (size_t)size);
    int head = 0, tail = 0;
    for (int y = 0; y < m->height; y++)
        for (int x = 0; x < m->width; x++)
            if ((x == 0 || y == 0 || x == m->width - 1 || y == m->height - 1) && !Wall(m, x, y))
            {
                c->outside[y * m->width + x] = 1;
                c->queue[tail++] = y * m->width + x;
            }
    const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
    while (head < tail)
    {
        int at = c->queue[head++], x = at % m->width, y = at / m->width;
        for (int d = 0; d < 4; d++)
        {
            int nx = x + dx[d], ny = y + dy[d];
            if (nx < 0 || ny < 0 || nx >= m->width || ny >= m->height)
                continue;
            int n = ny * m->width + nx;
            if (!c->outside[n] && !Wall(m, nx, ny))
            {
                c->outside[n] = 1;
                c->queue[tail++] = n;
            }
        }
    }
    c->key = key;
    c->valid = true;
    return true;
}
IsoWallShape IsoWallShapeAt(const IsoMap *m, IsoTile t, const IsoPiece *kind)
{
    IsoWallShape s = {0};
    if (!kind || kind->wallWidth <= 0 || !isfinite(kind->wallWidth))
        return s;
    float width = Clamp(kind->wallWidth, .05f, 1);
    float u = .5f, v = .5f, shift = Clamp((float)kind->wallPlacement, -1, 1) * (1 - width) * .5f;
    int ux = Outside(m, t.x + 1, t.y) - Outside(m, t.x - 1, t.y);
    int vy = Outside(m, t.x, t.y + 1) - Outside(m, t.x, t.y - 1);
    /* An open run has no enclosed side: inward is the negative tile axis, outward
       positive. Every connected tile uses the same convention, so sections still
       meet exactly. */
    if (!ux && Outside(m, t.x + 1, t.y) && Outside(m, t.x - 1, t.y))
        ux = 1;
    if (!vy && Outside(m, t.x, t.y + 1) && Outside(m, t.x, t.y - 1))
        vy = 1;
    u += ux * shift;
    v += vy * shift;
    s.u[0] = s.v[0] = 0;
    s.u[3] = s.v[3] = 1;
    s.u[1] = u - width * .5f;
    s.u[2] = u + width * .5f;
    s.v[1] = v - width * .5f;
    s.v[2] = v + width * .5f;
    s.levels = kind->wallLevels > 0 ? kind->wallLevels : 1;
    if (s.levels > ISO_MAP_LEVELS)
        s.levels = ISO_MAP_LEVELS;
    int mask = IsoMapWallMask(m, t);
    s.cells = 1u << 4;
    if (mask & 1)
        s.cells |= 1u << 1;
    if (mask & 2)
        s.cells |= 1u << 5;
    if (mask & 4)
        s.cells |= 1u << 7;
    if (mask & 8)
        s.cells |= 1u << 3;
    if (kind->wallFilled)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++)
            {
                int bx = x ? 2 : 8, by = y ? 4 : 1;
                if ((mask & bx) && (mask & by) && Wall(m, t.x + (x ? 1 : -1), t.y + (y ? 1 : -1)))
                    s.cells |= 1u << ((y ? 2 : 0) * 3 + (x ? 2 : 0));
            }
    return s;
}
static bool Cell(const IsoWallShape *s, int x, int y)
{
    return x >= 0 && x < 3 && y >= 0 && y < 3 && (s->cells & (1u << (y * 3 + x))) &&
           s->u[x + 1] - s->u[x] > 1e-5f && s->v[y + 1] - s->v[y] > 1e-5f;
}
bool IsoWallBlocks(const IsoMap *m, const IsoTileset *set, IsoHex hex)
{
    IsoTile t = IsoHexTile(hex);
    int k = IsoMapWallAt(m, t);
    if (k < 0)
        return false;
    if (!set || k >= set->wallCount || set->walls[k].wallWidth <= 0)
        return true;
    if (m->wallCache && !m->wallCache->valid)
        IsoWallPrepare(m);
    IsoWallShape s = IsoWallShapeAt(m, t, &set->walls[k]);
    /* Intersect the hex's footprint conservatively in tile coordinates. A centred
       divider blocks the adjacent movement cells; moving it to an edge frees the
       opposite side of its tile. */
    Vector2 centre = IsoMapTileCentre(t), corners[6];
    IsoHexCellCorners(hex, corners);
    float minU = 1e6f, maxU = -1e6f, minV = 1e6f, maxV = -1e6f;
    for (int i = 0; i < 6; i++)
    {
        float px = corners[i].x - centre.x - 8, py = corners[i].y - centre.y + 18;
        float u = (32 * py - 24 * px) / 1536, v = (12 * px + 48 * py) / 1536;
        minU = fminf(minU, u);
        maxU = fmaxf(maxU, u);
        minV = fminf(minV, v);
        maxV = fmaxf(maxV, v);
    }
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            if (Cell(&s, x, y) && s.u[x] < maxU - 1e-4f && s.u[x + 1] > minU + 1e-4f &&
                s.v[y] < maxV - 1e-4f && s.v[y + 1] > minV + 1e-4f)
                return true;
    return false;
}
static Vector2 Project(float u, float v, float height)
{
    return (Vector2){8 - 48 * u + 32 * v, -18 + 12 * u + 24 * v - height};
}
typedef struct WallVertex
{
    Vector2 p, uv;
} WallVertex;
static int ClipX(const WallVertex *in, int count, WallVertex *out, float edge, bool after)
{
    int n = 0;
    for (int i = 0; i < count; i++)
    {
        WallVertex a = in[i], b = in[(i + 1) % count];
        bool ai = after ? a.p.x >= edge : a.p.x <= edge, bi = after ? b.p.x >= edge : b.p.x <= edge;
        if (ai)
            out[n++] = a;
        if (ai != bi)
        {
            float t = (edge - a.p.x) / (b.p.x - a.p.x);
            out[n++] = (WallVertex){Vector2Lerp(a.p, b.p, t), Vector2Lerp(a.uv, b.uv, t)};
        }
    }
    return n;
}
static void Quad(const IsoMapPiece *p, const Vector2 dst[4], const Vector2 src[4], Color tint)
{
    WallVertex a[12], b[12];
    Rectangle cell = SpriteSheetFrameRect(p->sheet, p->frame);
    for (int i = 0; i < 4; i++)
        a[i] = (WallVertex){
            dst[i], {cell.x + p->sheet->meta.anchorX + src[i].x, cell.y + p->sheet->meta.anchorY + src[i].y}};
    int n = ClipX(a, 4, b, p->sliceFrom, true);
    n = ClipX(b, n, a, p->sliceTo, false);
    if (n < 3)
        return;
    rlSetTexture(p->sheet->atlas.id);
    rlBegin(RL_TRIANGLES);
    rlColor4ub(tint.r, tint.g, tint.b, tint.a);
    for (int i = 1; i < n - 1; i++)
    {
        int order[3] = {0, i, i + 1};
        /* rlgl's default 2D projection expects counter-clockwise screen-space
         * triangles. */
        Vector2 ab = Vector2Subtract(a[i].p, a[0].p), ac = Vector2Subtract(a[i + 1].p, a[0].p);
        if (ab.x * ac.y - ab.y * ac.x > 0)
        {
            order[1] = i + 1;
            order[2] = i;
        }
        for (int j = 0; j < 3; j++)
        {
            WallVertex q = a[order[j]];
            rlTexCoord2f(q.uv.x / p->sheet->atlas.width, q.uv.y / p->sheet->atlas.height);
            rlVertex2f(roundf(p->ground.x) + q.p.x, roundf(p->ground.y) + q.p.y);
        }
    }
    rlEnd();
    rlSetTexture(0);
}
/* Merge occupied cells so a filled tile emits one cap, not nine little quads. */
static int Caps(const IsoWallShape *s, Rectangle out[9])
{
    unsigned used = 0;
    int count = 0;
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
        {
            if (!Cell(s, x, y) || (used & (1u << (y * 3 + x))))
                continue;
            int right = x + 1, bottom = y + 1;
            while (right < 3 && Cell(s, right, y) && !(used & (1u << (y * 3 + right))))
                right++;
            while (bottom < 3)
            {
                bool full = true;
                for (int k = x; k < right; k++)
                    full &= Cell(s, k, bottom) && !(used & (1u << (bottom * 3 + k)));
                if (!full)
                    break;
                bottom++;
            }
            for (int j = y; j < bottom; j++)
                for (int i = x; i < right; i++)
                    used |= 1u << (j * 3 + i);
            out[count++] = (Rectangle){s->u[x], s->v[y], s->u[right] - s->u[x], s->v[bottom] - s->v[y]};
        }
    return count;
}
void IsoWallDraw(const IsoMapPiece *p, Color tint)
{
    const IsoWallShape *s = &p->wallShape;
    float h = ISO_MAP_BLOCK_HEIGHT;
    /* Merge collinear exposed sides. Interior faces never enter the render batch. */
    for (int level = 0; level < s->levels; level++)
        for (int axis = 0; axis < 2; axis++)
            for (int fixed = 0; fixed < 3; fixed++)
            {
                int along = 0;
                while (along < 3)
                {
                    int x = axis ? along : fixed, y = axis ? fixed : along;
                    bool boundary = Cell(s, x, y) && !Cell(s, x + (axis ? 0 : 1), y + (axis ? 1 : 0));
                    if (!boundary)
                    {
                        along++;
                        continue;
                    }
                    int first = along++;
                    while (along < 3)
                    {
                        x = axis ? along : fixed;
                        y = axis ? fixed : along;
                        if (!Cell(s, x, y) || Cell(s, x + (axis ? 0 : 1), y + (axis ? 1 : 0)))
                            break;
                        along++;
                    }
                    float z = level * h;
                    Vector2 dst[4], src[4];
                    if (!axis)
                    {
                        float u = s->u[fixed + 1], v0 = s->v[first], v1 = s->v[along];
                        dst[0] = Project(u, v0, z);
                        dst[1] = Project(u, v1, z);
                        dst[2] = Project(u, v1, z + h);
                        dst[3] = Project(u, v0, z + h);
                        src[0] = Project(1, v0, 0);
                        src[1] = Project(1, v1, 0);
                        src[2] = Project(1, v1, h);
                        src[3] = Project(1, v0, h);
                    }
                    else
                    {
                        float v = s->v[fixed + 1], u0 = s->u[first], u1 = s->u[along];
                        dst[0] = Project(u0, v, z);
                        dst[1] = Project(u1, v, z);
                        dst[2] = Project(u1, v, z + h);
                        dst[3] = Project(u0, v, z + h);
                        src[0] = Project(u0, 1, 0);
                        src[1] = Project(u1, 1, 0);
                        src[2] = Project(u1, 1, h);
                        src[3] = Project(u0, 1, h);
                    }
                    IsoMapPiece material = *p;
                    if (level > 0 && p->sheet->meta.frameCount >= 2)
                        material.frame = 1;
                    Quad(&material, dst, src, tint);
                }
            }
    Rectangle caps[9];
    int count = Caps(s, caps);
    for (int i = 0; i < count; i++)
    {
        float u0 = caps[i].x, u1 = u0 + caps[i].width, v0 = caps[i].y, v1 = v0 + caps[i].height;
        Vector2 d[4] = {Project(u0, v0, h * s->levels), Project(u1, v0, h * s->levels),
                        Project(u1, v1, h * s->levels), Project(u0, v1, h * s->levels)};
        Vector2 a[4] = {Project(u0, v0, h), Project(u1, v0, h), Project(u1, v1, h), Project(u0, v1, h)};
        IsoMapPiece material = *p;
        if (s->levels > 1 && p->sheet->meta.frameCount >= 2)
            material.frame = 1;
        Quad(&material, d, a, tint);
    }
}
void IsoWallDrawShadow(const IsoMapPiece *p)
{
    if (p->wallShadowStep.x == 0 && p->wallShadowStep.y == 0)
        return;
    const IsoWallShape *s = &p->wallShape;
    Vector2 shift = Vector2Scale(p->wallShadowStep, (float)s->levels);
    for (int y = 0; y < 3; y++)
        for (int x = 0; x < 3; x++)
            if (Cell(s, x, y))
            {
                Vector2 a[4] = {Project(s->u[x], s->v[y], 0), Project(s->u[x + 1], s->v[y], 0),
                                Project(s->u[x + 1], s->v[y + 1], 0), Project(s->u[x], s->v[y + 1], 0)};
                for (int i = 0; i < 4; i++)
                    a[i] = Vector2Add(a[i], p->ground);
                /* Opaque projected cap and swept edges; overdraw cannot darken the
                 * shadow twice. */
                for (int i = 1; i < 3; i++)
                    DrawTriangle(Vector2Add(a[0], shift), Vector2Add(a[i], shift),
                                 Vector2Add(a[i + 1], shift), BLACK);
                for (int i = 0; i < 4; i++)
                {
                    Vector2 b = a[(i + 1) % 4], c = Vector2Add(b, shift), d = Vector2Add(a[i], shift);
                    Vector2 ab = Vector2Subtract(b, a[i]);
                    if (ab.x * shift.y - ab.y * shift.x < 0)
                    {
                        DrawTriangle(a[i], b, c, BLACK);
                        DrawTriangle(a[i], c, d, BLACK);
                    }
                    else
                    {
                        DrawTriangle(a[i], c, b, BLACK);
                        DrawTriangle(a[i], d, c, BLACK);
                    }
                }
            }
}
