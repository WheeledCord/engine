/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "collision2d.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "raymath.h"

struct Collision2DProxy { Collision2DShape shape; Collision2DFilter filter; void *user; uint32_t generation, stamp; bool alive; };
struct Collision2DLink { int proxy, next; };

static bool Finite(Vector2 v) { return isfinite(v.x) && isfinite(v.y); }
static bool Valid(Collision2DShape s)
{
    return Finite(s.center) && ((s.kind == COLLISION2D_CIRCLE && isfinite(s.radius) && s.radius >= 0) ||
           (s.kind == COLLISION2D_AABB && Finite(s.half)));
}
Collision2DAabb Collision2DAabbMake(Vector2 c, Vector2 h)
{
    h.x = fmaxf(h.x, 0); h.y = fmaxf(h.y, 0);
    return (Collision2DAabb){{c.x - h.x, c.y - h.y}, {c.x + h.x, c.y + h.y}};
}
static Collision2DAabb Bounds(Collision2DShape s)
{
    return Collision2DAabbMake(s.center, s.kind == COLLISION2D_CIRCLE ? (Vector2){s.radius, s.radius} :
                                                          (Vector2){fabsf(s.half.x), fabsf(s.half.y)});
}
bool Collision2DAabbOverlaps(Collision2DAabb a, Collision2DAabb b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}
static bool CircleAabb(Vector2 c, float r, Collision2DAabb b)
{
    Vector2 p = {Clamp(c.x, b.min.x, b.max.x), Clamp(c.y, b.min.y, b.max.y)};
    return Vector2DistanceSqr(c, p) <= r * r;
}
bool Collision2DOverlaps(Collision2DShape a, Collision2DShape b)
{
    if (!Valid(a) || !Valid(b)) return false;
    if (a.kind == COLLISION2D_AABB && b.kind == COLLISION2D_AABB) return Collision2DAabbOverlaps(Bounds(a), Bounds(b));
    if (a.kind == COLLISION2D_CIRCLE && b.kind == COLLISION2D_CIRCLE)
    {
        float r = a.radius + b.radius;
        return Vector2DistanceSqr(a.center, b.center) <= r * r;
    }
    return a.kind == COLLISION2D_CIRCLE ? CircleAabb(a.center, a.radius, Bounds(b)) : CircleAabb(b.center, b.radius, Bounds(a));
}
bool Collision2DShouldCollide(Collision2DFilter a, Collision2DFilter b)
{
    return (a.layer & b.mask) != 0 && (b.layer & a.mask) != 0;
}
static bool Handle(const Collision2DWorld *w, Collision2DHandle h)
{
    return w && h.index < (uint32_t)w->capacity && w->proxies[h.index].alive && w->proxies[h.index].generation == h.generation;
}
static unsigned Hash(int x, int y, int n) { return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u) % (unsigned)n; }
static bool EnsureLinks(Collision2DWorld *w, int needed)
{
    if (needed <= w->linkCapacity) return true;
    int capacity = w->linkCapacity ? w->linkCapacity : 16;
    while (capacity < needed)
    {
        if (capacity > INT_MAX / 2) return false;
        capacity *= 2;
    }
    Collision2DLink *links = realloc(w->links, (size_t)capacity * sizeof *links);
    if (!links) return false;
    w->links = links;
    w->linkCapacity = capacity;
    return true;
}
static bool Rebuild(Collision2DWorld *w)
{
    if (!w || !w->dirty) return w != NULL;
    memset(w->buckets, 0xff, (size_t)w->bucketCount * sizeof *w->buckets);
    w->linkCount = 0;
    for (int p = 0; p < w->capacity; p++) if (w->proxies[p].alive)
    {
        Collision2DAabb b = Bounds(w->proxies[p].shape);
        int x0 = (int)floorf(b.min.x / w->cellSize), x1 = (int)floorf(b.max.x / w->cellSize);
        int y0 = (int)floorf(b.min.y / w->cellSize), y1 = (int)floorf(b.max.y / w->cellSize);
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++)
        {
            if (!EnsureLinks(w, w->linkCount + 1)) return false;
            unsigned bucket = Hash(x, y, w->bucketCount);
            w->links[w->linkCount] = (Collision2DLink){p, w->buckets[bucket]};
            w->buckets[bucket] = w->linkCount++;
        }
    }
    w->dirty = false;
    return true;
}
bool Collision2DWorldInit(Collision2DWorld *w, int capacity, float cellSize)
{
    if (!w || capacity <= 0 || !isfinite(cellSize) || cellSize <= 0) return false;
    *w = (Collision2DWorld){0};
    w->capacity = capacity; w->cellSize = cellSize; w->bucketCount = capacity * 2 + 1;
    w->linkCapacity = capacity;
    w->proxies = calloc((size_t)capacity, sizeof *w->proxies);
    w->buckets = malloc((size_t)w->bucketCount * sizeof *w->buckets);
    w->links = malloc((size_t)w->linkCapacity * sizeof *w->links);
    if (!w->proxies || !w->buckets || !w->links) { Collision2DWorldFree(w); return false; }
    for (int i = 0; i < capacity; i++) w->proxies[i].generation = 1;
    w->dirty = true;
    return true;
}
void Collision2DWorldFree(Collision2DWorld *w)
{
    if (!w) return;
    free(w->proxies); free(w->links); free(w->buckets); *w = (Collision2DWorld){0};
}
Collision2DHandle Collision2DWorldAdd(Collision2DWorld *w, Collision2DShape shape, Collision2DFilter filter, void *user)
{
    if (!w || !Valid(shape)) return COLLISION2D_NULL;
    for (int i = 0; i < w->capacity; i++) if (!w->proxies[i].alive)
    {
        w->proxies[i].shape = shape; w->proxies[i].filter = filter; w->proxies[i].user = user; w->proxies[i].alive = true; w->dirty = true;
        return (Collision2DHandle){(uint32_t)i, w->proxies[i].generation};
    }
    return COLLISION2D_NULL;
}
bool Collision2DWorldRemove(Collision2DWorld *w, Collision2DHandle h)
{
    if (!Handle(w, h)) return false;
    Collision2DProxy *p = w->proxies + h.index; p->alive = false; if (!++p->generation) p->generation = 1; w->dirty = true; return true;
}
bool Collision2DWorldSetShape(Collision2DWorld *w, Collision2DHandle h, Collision2DShape shape)
{
    if (!Handle(w, h) || !Valid(shape)) return false;
    w->proxies[h.index].shape = shape; w->dirty = true; return true;
}
static int Query(Collision2DWorld *w, Collision2DAabb bounds, uint32_t mask, Collision2DShape exact, Collision2DHit *out, int cap)
{
    if (!w || cap < 0 || !Finite(bounds.min) || !Finite(bounds.max)) return 0;
    if (!Rebuild(w)) return 0;
    if (++w->queryStamp == 0) { for (int i = 0; i < w->capacity; i++) w->proxies[i].stamp = 0; ++w->queryStamp; }
    int n = 0, x0 = (int)floorf(bounds.min.x / w->cellSize), x1 = (int)floorf(bounds.max.x / w->cellSize);
    int y0 = (int)floorf(bounds.min.y / w->cellSize), y1 = (int)floorf(bounds.max.y / w->cellSize);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++)
        for (int link = w->buckets[Hash(x, y, w->bucketCount)]; link >= 0; link = w->links[link].next)
        {
            Collision2DProxy *p = w->proxies + w->links[link].proxy;
            if (p->stamp == w->queryStamp) continue;
            p->stamp = w->queryStamp;
            if ((p->filter.layer & mask) == 0 || !Collision2DOverlaps(exact, p->shape)) continue;
            if (out && n < cap) out[n] = (Collision2DHit){{(uint32_t)w->links[link].proxy, p->generation}, p->user};
            if (++n == cap && out) return n;
        }
    return n;
}
int Collision2DQueryAabb(Collision2DWorld *w, Collision2DAabb b, uint32_t mask, Collision2DHit *out, int cap)
{
    Vector2 c = {(b.min.x + b.max.x) * .5f, (b.min.y + b.max.y) * .5f};
    return Query(w, b, mask, (Collision2DShape){COLLISION2D_AABB, c,
                 {(b.max.x-b.min.x)*.5f, (b.max.y-b.min.y)*.5f}, 0}, out, cap);
}
int Collision2DQueryCircle(Collision2DWorld *w, Vector2 c, float r, uint32_t mask, Collision2DHit *out, int cap)
{
    Collision2DShape s = {COLLISION2D_CIRCLE, c, {0, 0}, r};
    return Query(w, Bounds(s), mask, s, out, cap);
}
static bool SweepCircleCircle(Vector2 c, float r, Vector2 d, Collision2DShape target, float *fraction, Vector2 *normal)
{
    Vector2 offset = Vector2Subtract(c, target.center);
    float combined = r + target.radius;
    float cterm = Vector2DotProduct(offset, offset) - combined * combined;
    if (cterm <= 0)
    {
        *fraction = 0;
        *normal = Vector2LengthSqr(offset) > 0 ? Vector2Normalize(offset) : (Vector2){0, -1};
        return true;
    }
    float a = Vector2DotProduct(d, d);
    if (a == 0) return false;
    float b = 2 * Vector2DotProduct(offset, d);
    float discriminant = b * b - 4 * a * cterm;
    if (discriminant < 0) return false;
    float time = (-b - sqrtf(discriminant)) / (2 * a);
    if (time < 0 || time > 1) return false;
    *fraction = time;
    *normal = Vector2Normalize(Vector2Add(offset, Vector2Scale(d, time)));
    return true;
}
static bool SweepCircleAabb(Vector2 c, float r, Vector2 d, Collision2DShape target, float *fraction, Vector2 *normal)
{
    Collision2DAabb b = Bounds(target);
    if (CircleAabb(c, r, b))
    {
        *fraction = 0;
        *normal = (Vector2){0, 0};
        return true;
    }
    float best = FLT_MAX;
    Vector2 bestNormal = {0, 0};
#define SIDE_HIT(time, point, sideNormal) do { \
    float candidate = (time); \
    if (candidate >= 0 && candidate <= 1 && (point) >= b.min.y && (point) <= b.max.y && candidate < best) \
    { best = candidate; bestNormal = (sideNormal); } \
} while (0)
    if (d.x > 0) SIDE_HIT((b.min.x - r - c.x) / d.x, c.y + d.y * ((b.min.x - r - c.x) / d.x), ((Vector2){-1, 0}));
    if (d.x < 0) SIDE_HIT((b.max.x + r - c.x) / d.x, c.y + d.y * ((b.max.x + r - c.x) / d.x), ((Vector2){1, 0}));
#undef SIDE_HIT
#define SIDE_HIT(time, point, sideNormal) do { \
    float candidate = (time); \
    if (candidate >= 0 && candidate <= 1 && (point) >= b.min.x && (point) <= b.max.x && candidate < best) \
    { best = candidate; bestNormal = (sideNormal); } \
} while (0)
    if (d.y > 0) SIDE_HIT((b.min.y - r - c.y) / d.y, c.x + d.x * ((b.min.y - r - c.y) / d.y), ((Vector2){0, -1}));
    if (d.y < 0) SIDE_HIT((b.max.y + r - c.y) / d.y, c.x + d.x * ((b.max.y + r - c.y) / d.y), ((Vector2){0, 1}));
#undef SIDE_HIT
    const Vector2 corners[] = {{b.min.x, b.min.y}, {b.max.x, b.min.y}, {b.min.x, b.max.y}, {b.max.x, b.max.y}};
    for (int i = 0; i < 4; i++)
    {
        float candidate;
        Vector2 candidateNormal;
        if (!SweepCircleCircle(c, r, d, (Collision2DShape){COLLISION2D_CIRCLE, corners[i], {0, 0}, 0}, &candidate, &candidateNormal)) continue;
        Vector2 point = Vector2Add(c, Vector2Scale(d, candidate));
        bool outsideX = i % 2 == 0 ? point.x <= b.min.x : point.x >= b.max.x;
        bool outsideY = i < 2 ? point.y <= b.min.y : point.y >= b.max.y;
        if (outsideX && outsideY && candidate < best) { best = candidate; bestNormal = candidateNormal; }
    }
    if (best == FLT_MAX) return false;
    *fraction = best;
    *normal = bestNormal;
    return true;
}
bool Collision2DSweepCircle(Collision2DWorld *w, Vector2 c, float r, Vector2 d, uint32_t mask, Collision2DSweep *out)
{
    if (!Finite(c) || !Finite(d) || !isfinite(r) || r < 0) return false;
    Collision2DAabb swept = Bounds((Collision2DShape){COLLISION2D_CIRCLE,
                                      {c.x+d.x*.5f,c.y+d.y*.5f}, {0, 0}, r + Vector2Length(d)*.5f});
    int count = Collision2DQueryAabb(w, swept, mask, NULL, 0);
    Collision2DHit *hits = count ? malloc((size_t)count * sizeof *hits) : NULL;
    if (count && !hits) return false;
    count = Collision2DQueryAabb(w, swept, mask, hits, count);
    float best = FLT_MAX;
    Collision2DSweep result = {0};
    for (int i = 0; i < count; i++)
    {
        Collision2DProxy *p = w->proxies + hits[i].handle.index;
        float fraction;
        Vector2 normal;
        bool hit = p->shape.kind == COLLISION2D_CIRCLE ?
            SweepCircleCircle(c, r, d, p->shape, &fraction, &normal) :
            SweepCircleAabb(c, r, d, p->shape, &fraction, &normal);
        if (hit && fraction < best)
        {
            best = fraction;
            result.hit = hits[i];
            result.normal = normal;
        }
    }
    free(hits);
    if (best == FLT_MAX) return false;
    result.fraction = best;
    result.point = Vector2Add(c, Vector2Scale(d, best));
    if (out) *out = result;
    return true;
}
