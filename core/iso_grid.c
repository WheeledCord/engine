/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "iso_grid.h"
#include <math.h>
#include <stdlib.h>

/* Floor division and remainder by two, defined the same either side of zero, so a grid that runs
   into negative coordinates keeps the same cell shapes as one that does not. Shifting a negative
   int is left to the implementation by C99; this is not. */
static int FloorHalf(int v)
{
    return v >= 0 ? v / 2 : -((-v + 1) / 2);
}
static int Parity(int v)
{
    return v - 2 * FloorHalf(v);
}

/* Hexes are addressed by an axial pair internally: u counts steps due east, v counts steps
   southeast. Those two are 60 degrees apart, which is what makes the distance below a cube
   distance rather than a guess. */
static void HexToAxial(IsoHex hex, int *u, int *v)
{
    *u = -hex.x;
    *v = FloorHalf(hex.x) + hex.y;
}
static IsoHex AxialToHex(int u, int v)
{
    IsoHex hex;
    hex.x = -u;
    hex.y = v - FloorHalf(hex.x);
    return hex;
}

Vector2 IsoHexToScreen(IsoHex hex)
{
    int half = FloorHalf(hex.x);
    return (Vector2){(float)(-32 * hex.x + 16 * half + 16 * hex.y), (float)(12 * (half + hex.y))};
}
Vector2 IsoTileToScreen(IsoTile tile)
{
    return (Vector2){(float)(-48 * tile.x + 32 * tile.y), (float)(12 * tile.x + 24 * tile.y)};
}
IsoHex IsoScreenToHex(Vector2 screen)
{
    float v = screen.y / 12.0f;
    float u = (screen.x - 16.0f * v) / 32.0f;
    /* Cube rounding: round all three, then correct whichever moved furthest, so the result is
       always the hex whose centre is nearest rather than a cell picked by truncation. */
    float w = -u - v;
    float ru = roundf(u), rv = roundf(v), rw = roundf(w);
    float du = fabsf(ru - u), dv = fabsf(rv - v), dw = fabsf(rw - w);
    if (du > dv && du > dw)
        ru = -rv - rw;
    else if (dv > dw)
        rv = -ru - rw;
    return AxialToHex((int)ru, (int)rv);
}
IsoTile IsoScreenToTile(Vector2 screen)
{
    /* The inverse of the tile basis [(-48,+12),(+32,+24)], whose determinant is -1536. */
    float x = (32.0f * screen.y - 24.0f * screen.x) / 1536.0f;
    float y = (12.0f * screen.x + 48.0f * screen.y) / 1536.0f;
    return (IsoTile){(int)floorf(x), (int)floorf(y)};
}

/* Neighbours differ by parity of x, because the x axis zigzags: the same direction is a different
   step depending on whether the hex sits on an even or an odd column. */
static const int neighbours[2][ISO_DIR_COUNT][2] = {
    /* even x: NE, E, SE, SW, W, NW */
    {{-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {0, -1}},
    /* odd x */
    {{-1, -1}, {-1, 0}, {0, 1}, {1, 0}, {1, -1}, {0, -1}},
};
IsoHex IsoHexNeighbour(IsoHex hex, IsoDir dir)
{
    if (dir < 0 || dir >= ISO_DIR_COUNT)
        return hex;
    const int *step = neighbours[Parity(hex.x)][dir];
    return (IsoHex){hex.x + step[0], hex.y + step[1]};
}
int IsoHexDistance(IsoHex a, IsoHex b)
{
    int au, av, bu, bv;
    HexToAxial(a, &au, &av);
    HexToAxial(b, &bu, &bv);
    int du = bu - au, dv = bv - av, dw = -du - dv;
    du = du < 0 ? -du : du;
    dv = dv < 0 ? -dv : dv;
    dw = dw < 0 ? -dw : dw;
    return (du + dv + dw) / 2;
}
IsoDir IsoHexDirection(IsoHex from, IsoHex to)
{
    /* Compared on screen rather than in grid coordinates: the six views are what a viewer sees, and
       the projection is what decides which of them a step looks like. */
    static const float unit[ISO_DIR_COUNT][2] = {
        {16.0f / 20.0f, -12.0f / 20.0f}, {1.0f, 0.0f},  {16.0f / 20.0f, 12.0f / 20.0f},
        {-16.0f / 20.0f, 12.0f / 20.0f}, {-1.0f, 0.0f}, {-16.0f / 20.0f, -12.0f / 20.0f},
    };
    Vector2 a = IsoHexToScreen(from), b = IsoHexToScreen(to);
    float dx = b.x - a.x, dy = b.y - a.y;
    if (dx == 0 && dy == 0)
        return ISO_NE;
    IsoDir best = ISO_NE;
    float bestDot = -1e30f;
    for (int i = 0; i < ISO_DIR_COUNT; i++)
    {
        float dot = dx * unit[i][0] + dy * unit[i][1];
        if (dot > bestDot)
        {
            bestDot = dot;
            best = (IsoDir)i;
        }
    }
    return best;
}

IsoHex IsoTileHex(IsoTile tile)
{
    return (IsoHex){tile.x * 2, tile.y * 2};
}
IsoTile IsoHexTile(IsoHex hex)
{
    return (IsoTile){FloorHalf(hex.x), FloorHalf(hex.y)};
}

void IsoTileCorners(IsoTile tile, Vector2 out[4])
{
    /* The cell is the parallelogram the two tile axes span from the tile's anchor, which is why it
       leans: the axes are not mirror images of each other. Its area is 1536, the determinant of
       those axes, so the cells meet edge to edge. */
    Vector2 anchor = IsoTileToScreen(tile);
    out[0] = anchor;
    out[1] = (Vector2){anchor.x - 48, anchor.y + 12};
    out[2] = (Vector2){anchor.x - 16, anchor.y + 36};
    out[3] = (Vector2){anchor.x + 32, anchor.y + 24};
}
void IsoHexCellCorners(IsoHex hex, Vector2 out[6])
{
    /* The ground a hex owns, which is the region IsoScreenToHex actually hands to it: a regular
       hexagon in hex space carried through the projection, not the nearest-centre region measured
       on screen. Those are different, because the projection leaves the six steps at different
       screen lengths, and only this one agrees with what a click selects.

       It comes out 32 across and 16 tall, with upright left and right edges and a point at top and
       bottom. Its area is 384, the determinant of the hex axes, so the cells tile exactly. */
    static const float shape[6][2] = {{16, -4}, {16, 4},   {0, 8},
                                      {-16, 4}, {-16, -4}, {0, -8}};
    Vector2 centre = IsoHexToScreen(hex);
    for (int i = 0; i < 6; i++)
        out[i] = (Vector2){centre.x + shape[i][0], centre.y + shape[i][1]};
}

static int DepthOrder(const void *lhs, const void *rhs)
{
    const IsoDrawItem *a = lhs, *b = rhs;
    Vector2 pa = IsoHexToScreen(a->hex), pb = IsoHexToScreen(b->hex);
    /* Screen y is depth here: of the six steps, the four that move toward or away from the viewer
       change it and the two level ones do not, so equal y really is equal depth rather than a
       coincidence. x then hex break the remaining ties, only so that the result is the same every
       time it is asked for. */
    if (pa.y != pb.y)
        return pa.y < pb.y ? -1 : 1;
    if (pa.x != pb.x)
        return pa.x < pb.x ? -1 : 1;
    if (a->hex.x != b->hex.x)
        return a->hex.x < b->hex.x ? -1 : 1;
    if (a->hex.y != b->hex.y)
        return a->hex.y < b->hex.y ? -1 : 1;
    return 0;
}
void IsoDepthSort(IsoDrawItem *items, int count)
{
    if (count > 1)
        qsort(items, (size_t)count, sizeof *items, DepthOrder);
}
