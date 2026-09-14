/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_ISO_GRID_H
#define CORE_ISO_GRID_H
#include "raylib.h"
#include <stdbool.h>

/* The arrangement Fallout 1/2 use: two grids over the same ground.

   The square grid carries the floor and roof art, one 80x36 parallelogram per cell. The hex grid
   carries everything that moves -- critters, scenery, line of sight, movement cost -- and is twice
   as fine on each axis, so one tile is exactly a 2x2 block of hexes. Fallout's own maps are 100x100
   tiles and 200x200 hexes, which is the same ground counted both ways.

   The projection is trimetric rather than a symmetric diamond: the two tile axes do not mirror each
   other. Stepping one tile in x moves (-48,+12) pixels and one tile in y moves (+32,+24), so the
   cell is 80 wide and 36 tall but leans. The hex axes are exactly half of those: +y is (+16,+12),
   and +x alternates (-32,0) and (-16,+12) so that two x steps come to (-48,+12), one whole tile.
   That is what keeps the two grids aligned to each other rather than merely overlapping.

   Screen positions here are relative to hex/tile (0,0) at the origin. Fallout's own files add a
   fixed offset per map size; a camera belongs to the project, not to the grid. */

#define ISO_TILE_WIDTH 80
#define ISO_TILE_HEIGHT 36
#define ISO_HEX_WIDTH 32
#define ISO_HEX_HEIGHT 16

typedef struct IsoHex
{
    int x, y;
} IsoHex;
typedef struct IsoTile
{
    int x, y;
} IsoTile;

/* Fallout's six rotations, in its own order: a critter's art has one view per entry. */
typedef enum IsoDir
{
    ISO_NE,
    ISO_E,
    ISO_SE,
    ISO_SW,
    ISO_W,
    ISO_NW,
    ISO_DIR_COUNT
} IsoDir;

Vector2 IsoHexToScreen(IsoHex hex);
Vector2 IsoTileToScreen(IsoTile tile);
/* Nearest hex centre to a point, by cube rounding; every point resolves to exactly one hex. */
IsoHex IsoScreenToHex(Vector2 screen);
/* The tile whose parallelogram contains the point. */
IsoTile IsoScreenToTile(Vector2 screen);

IsoHex IsoHexNeighbour(IsoHex hex, IsoDir dir);
/* Steps between two hexes along the grid, which is what a move costs before terrain is considered. */
int IsoHexDistance(IsoHex a, IsoHex b);
/* Which of the six views faces from one hex toward another; a hex compared with itself gives ISO_NE. */
IsoDir IsoHexDirection(IsoHex from, IsoHex to);

/* A tile's top-left hex, and the tile a hex falls in. */
IsoHex IsoTileHex(IsoTile tile);
IsoTile IsoHexTile(IsoHex hex);

/* The outline of one cell in screen space, for drawing the grid itself rather than what stands on
   it. Both tile the plane exactly: no gap between neighbours and no overlap. */
void IsoTileCorners(IsoTile tile, Vector2 out[4]);
void IsoHexCellCorners(IsoHex hex, Vector2 out[6]);

/* Painting order. An isometric scene has to be drawn far to near or a character walks through the
   wall it should be hidden behind, and the order is not the order things are stored in.

   Each item stands on one hex. That is the same rule Fallout works to -- its maps give every object
   a single hex, and a long wall is many pieces with one hex each -- and it is what makes the order
   exact rather than a guess: two single-hex items can never disagree about which of them is in
   front. An item spanning several hexes has no single answer, so it is built from pieces instead. */
typedef struct IsoDrawItem
{
    IsoHex hex;
    void *user; /* whatever the project needs in order to draw it */
} IsoDrawItem;
/* Orders items far to near, so painting them in order lays nearer things over further ones. Items
   sharing a hex share a depth, and keep no guaranteed order between themselves. */
void IsoDepthSort(IsoDrawItem *items, int count);
#endif
