/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_ISO_MAP_H
#define CORE_ISO_MAP_H
#include "iso_grid.h"
#include "sprite_sheet.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A map on the two grids of iso_grid.h, laid out the way Fallout's mapper and
   RPG Maker both do it: a few layers, each painted cell by cell, and a tileset
   naming what may be painted.

     floor   square grid  the ground itself: a floor tile per tile, from the
   sprite maker paths   hex grid     materials laid over the floor -- tracks,
   mud, water -- whose edges are worked out per pixel from the hexes around
   them, so a path follows the hexes it was painted on without ever looking
   hexagonal blocks  square grid  stackable wall blocks, up to ISO_MAP_LEVELS
   high, Minecraft fashion walls   square grid  thin walls, one kind per tile;
   each tile's piece is chosen from which of its four neighbours also have a
   wall (see IsoMapWallMask) roofs   square grid  a roof piece above a tile;
   roofs that touch form one building, and a game hides a building's roof while
   someone is under it, as Fallout does props   hex grid     things placed whole
   -- a house, a well, a tree -- with a footprint of hexes they occupy objects
   hex grid     entities the game spawns: where the player starts, a critter, a
   door

   The square layers use tiles shifted up half a hex row from iso_grid's tiles,
   so that one tile is exactly the ground its four hexes cover: a block on a
   tile blocks the hexes it visibly stands on.

   What any of it means -- which object is the player, when a roof is hidden,
   what a blocked hex costs -- is the game's. The map says where things are and
   draws them. */

#define ISO_MAP_LEVELS 8
/* Pixels a block stands above the one below: 1.3 m at the scenery's 32 px a
   metre, so two blocks make a storey a person stands up in. */
#define ISO_MAP_BLOCK_HEIGHT 36
#define ISO_MAP_KINDS 64 /* of each kind in a tileset */
/* Pixels a pitched roof climbs over one tile, from eave toward ridge. */
#define ISO_MAP_ROOF_RISE 24

/* A pitched roof is painted as tiles like a flat one; each tile's piece is
   chosen from where it sits in its building, the way RPG Maker's autotiles
   choose their edges. "rise_y" climbs toward +y (it faces -y), "ridge_x" is the
   ridge running along x. A roof's sheet holds them as frames in this order, as
   the sprite maker exports them; ISO_ROOF_FILL is a block of the roof's own
   material one rise high, stacked under the lifted tiles at a building's ends
   to close its gables. A sheet of one frame is a flat roof. */
typedef enum IsoRoofShape
{
    ISO_ROOF_FLAT,
    ISO_ROOF_RISE_X,
    ISO_ROOF_FALL_X,
    ISO_ROOF_RISE_Y,
    ISO_ROOF_FALL_Y,
    ISO_ROOF_RIDGE_X,
    ISO_ROOF_RIDGE_Y,
    ISO_ROOF_FILL,
    ISO_ROOF_SHAPES
} IsoRoofShape;
#define ISO_MAP_NAME 48
#define ISO_MAP_PATH 256
#define ISO_MAP_FOOTPRINT 128
/* A prop's rotations: <sheet>_0 ... <sheet>_7, whichever were exported. */
#define ISO_PROP_VARIANTS 8

/* A floor or path kind: a floor tile the sprite maker baked -- one tile, or a
   pattern of N by N tiles so a large texture does not repeat every tile -- read
   back so the ground can be composited from it. The tile's .prop file says N
   (`pattern N`). */
typedef struct IsoMaterial
{
    char name[ISO_MAP_NAME];
    char sheet[ISO_MAP_PATH]; /* <sheet>.png/.sheet/.prop, relative to the tileset
                                 file */
    /* How far its outline strays from the grid: 0 follows the tiles (floors) or
       makes round patches around the hexes (paths) exactly, 1 is ragged. */
    float edge;
    bool blocking; /* paths: water and the like, that nothing walks over */
    Color *texels; /* the baked pattern, with a tile's margin all round */
    int width, height;
    int pattern;          /* tiles the pattern spans each way */
    int anchorX, anchorY; /* the pixel at the centre of the pattern's first tile */
    Color swatch;         /* its average colour, for palettes and thumbnails */
} IsoMaterial;

/* Anything drawn from a sprite sheet: a block, a roof, a prop. */
typedef struct IsoPiece
{
    char name[ISO_MAP_NAME];
    char sheet[ISO_MAP_PATH]; /* <sheet>.png and <sheet>.sheet, relative to the
                                 tileset */
    SpriteSheet art, shadow;  /* shadow is <sheet>_shadow, optional */
    bool hasArt, hasShadow;
    bool pitched; /* roofs: the sheet has every pitched shape */
    /* props: the hexes it stands on, as offsets from hex (0,0) where its anchor
     * is */
    IsoHex footprint[ISO_MAP_FOOTPRINT];
    int footprintCount;
    bool blocking;
    bool backdrop; /* authored floor/background plate, drawn before standing pieces */
    SpriteSheet variants[ISO_PROP_VARIANTS], variantShadows[ISO_PROP_VARIANTS];
    bool hasVariant[ISO_PROP_VARIANTS], hasVariantShadow[ISO_PROP_VARIANTS];
    /* From the piece's .prop file. A shadow is baked for the ground level; lifted
       a level it falls shadowStep pixels further along the ground. */
    Vector2 shadowStep;
    /* Walls can reuse a full block sheet. Width is a fraction of the tile (0
       preserves legacy mask sprites); placement is -1 inward, 0 centred, +1
       outward. Filled joins close the quadrants of a painted solid region. The
       art remains the same for every setting. */
    float wallWidth;
    int wallPlacement, wallLevels;
    bool wallFilled;
    const struct IsoPiece *wallSource; /* borrowed block material in this same tileset, if shared */
} IsoPiece;

/* A piece's art for a variant: that rotation if it was exported, else the first
   that was, else the plain sheet. NULL when there is none. */
/* The frame palettes show: a straight north-south piece (mask 5) for a 16-frame
 * wall, else the first. */
int IsoPieceSwatchFrame(const IsoPiece *piece);
const SpriteSheet *IsoPieceSheet(const IsoPiece *piece, int variant, bool shadow);
/* The next exported variant after `from` going `step` (+1 or -1), wrapping;
 * `from` if there is none. */
int IsoPieceNextVariant(const IsoPiece *piece, int from, int step);

/* An entity the game spawns, with the fields it is given unless an object
 * overrides them. */
typedef struct IsoObjectKind
{
    char name[ISO_MAP_NAME];
    char classname[ISO_MAP_NAME];
    char fields[ISO_MAP_PATH]; /* key=value, space separated */
    Color colour;              /* how an editor marks it */
} IsoObjectKind;

typedef enum IsoLayer
{
    ISO_LAYER_FLOOR,
    ISO_LAYER_PATH,
    ISO_LAYER_BLOCK,
    ISO_LAYER_WALL,
    ISO_LAYER_ROOF,
    ISO_LAYER_PROP,
    ISO_LAYER_OBJECT,
    ISO_LAYER_COUNT
} IsoLayer;

typedef struct IsoTileset
{
    char path[ISO_MAP_PATH];
    IsoMaterial floors[ISO_MAP_KINDS], paths[ISO_MAP_KINDS];
    IsoPiece blocks[ISO_MAP_KINDS], walls[ISO_MAP_KINDS], roofs[ISO_MAP_KINDS], props[ISO_MAP_KINDS];
    IsoObjectKind objects[ISO_MAP_KINDS];
    int floorCount, pathCount, blockCount, wallCount, roofCount, propCount, objectCount;
} IsoTileset;

/* Reads a tileset and loads everything it names. Sprite sheets need a window.
   Nothing is kept on a failure; the log says which line or which file. */
bool IsoTilesetLoad(IsoTileset *tileset, const char *path);
bool IsoTilesetSave(const IsoTileset *tileset, const char *path);
void IsoTilesetUnload(IsoTileset *tileset);
/* How many kinds a layer has, and the name of one, for palettes. */
int IsoTilesetCount(const IsoTileset *tileset, IsoLayer layer);
const char *IsoTilesetName(const IsoTileset *tileset, IsoLayer layer, int index);
int IsoTilesetFind(const IsoTileset *tileset, IsoLayer layer, const char *name);
/* Adds a kind, loading what it names; -1 when the layer is full or its files
 * will not load. */
int IsoTilesetAddMaterial(IsoTileset *tileset, IsoLayer layer, IsoMaterial material);
int IsoTilesetAddPiece(IsoTileset *tileset, IsoLayer layer, const char *name, const char *sheet);
/* Reads a floor or path kind's tile again, after the sprite maker rewrote it.
 */
bool IsoTilesetRefreshMaterial(IsoTileset *tileset, IsoLayer layer, int index);
/* Re-reads a piece's sheets and footprint after a tool rewrote them. */
bool IsoTilesetReloadPiece(IsoTileset *tileset, IsoLayer layer, int index);

typedef struct IsoMapProp
{
    IsoHex hex;
    int kind;
    int variant; /* which rotation, 0 to ISO_PROP_VARIANTS - 1 */
} IsoMapProp;
typedef struct IsoMapObject
{
    IsoHex hex;
    int kind;
    char fields[ISO_MAP_PATH]; /* overrides of the kind's fields */
} IsoMapObject;

typedef struct IsoMap
{
    int width, height;          /* in tiles; the hex grid is twice each */
    char tileset[ISO_MAP_PATH]; /* relative to the map file */
    signed char *floor;         /* width*height, -1 for none */
    signed char *paths;         /* (2*width)*(2*height), -1 for none */
    signed char *blocks;        /* width*height*ISO_MAP_LEVELS, level-major within a tile */
    signed char *walls;         /* width*height */
    signed char *roofs;         /* width*height */
    unsigned char *roofLevel;   /* how many blocks up a tile's roof sits */
    IsoMapProp *props;
    int propCount, propCapacity;
    IsoMapObject *objects;
    int objectCount, objectCapacity;
    struct IsoWallCache *wallCache; /* owned derived topology; not copied or serialized */
} IsoMap;

bool IsoMapInit(IsoMap *map, int width, int height, const char *tileset);
void IsoMapFree(IsoMap *map);
/* Copies a whole map, for undo. */
bool IsoMapCopy(IsoMap *to, const IsoMap *from);
/* Grows or shrinks the map, keeping what is inside the new size. */
bool IsoMapResize(IsoMap *map, int width, int height);
/* Kinds are stored by name in the file and matched to the tileset on load, so
   reordering or adding to a tileset never repaints a map. A name the tileset
   lacks is reported and left empty. */
bool IsoMapLoad(IsoMap *map, const char *path, const IsoTileset *tileset);
bool IsoMapSave(const IsoMap *map, const char *path, const IsoTileset *tileset);
/* The tileset path a map file names, resolved against the map's own directory.
 */
bool IsoMapTilesetPath(const char *mapPath, char *out, size_t capacity);

bool IsoMapTileInside(const IsoMap *map, IsoTile tile);
bool IsoMapHexInside(const IsoMap *map, IsoHex hex);
int IsoMapFloorAt(const IsoMap *map, IsoTile tile);
int IsoMapPathAt(const IsoMap *map, IsoHex hex);
int IsoMapBlockAt(const IsoMap *map, IsoTile tile, int level);
/* How many blocks are stacked on a tile, counting to the highest. */
int IsoMapBlockHeight(const IsoMap *map, IsoTile tile);
int IsoMapRoofAt(const IsoMap *map, IsoTile tile);
int IsoMapWallAt(const IsoMap *map, IsoTile tile);
/* Which neighbours of a tile have a wall of any kind: bit 0 (x,y-1), 1 (x+1,y),
   2 (x,y+1), 3 (x-1,y), as OpenRA's WithWallSpriteBody and Space Station 14's
   cardinal icon smoothing do. A wall's sheet holds its 16 pieces as frames in
   mask order; a sheet of fewer frames draws frame 0 everywhere. */
int IsoMapWallMask(const IsoMap *map, IsoTile tile);
void IsoMapSetFloor(IsoMap *map, IsoTile tile, int kind);
void IsoMapSetPath(IsoMap *map, IsoHex hex, int kind);
void IsoMapSetBlock(IsoMap *map, IsoTile tile, int level, int kind);
void IsoMapSetRoof(IsoMap *map, IsoTile tile, int level, int kind);
void IsoMapSetWall(IsoMap *map, IsoTile tile, int kind);
/* Props and objects: index of the new one, or -1. */
int IsoMapAddProp(IsoMap *map, IsoHex hex, int kind);
void IsoMapRemoveProp(IsoMap *map, int index);
int IsoMapAddObject(IsoMap *map, IsoHex hex, int kind, const char *fields);
void IsoMapRemoveObject(IsoMap *map, int index);
/* The prop whose footprint covers a hex, or -1. */
int IsoMapPropAt(const IsoMap *map, const IsoTileset *tileset, IsoHex hex);
int IsoMapObjectAt(const IsoMap *map, IsoHex hex);

/* The map's own tiles: the ground a tile's four hexes cover, and the tile under
 * a point. */
IsoTile IsoMapTileAtPoint(Vector2 point);
void IsoMapTileCorners(IsoTile tile, Vector2 out[4]);
Vector2 IsoMapTileCentre(IsoTile tile);
/* Where a prop's footprint lands when its anchor is on `at`. */
IsoHex IsoMapFootprintHex(IsoHex at, IsoHex offset);
/* The world rectangle the map's ground covers. */
Rectangle IsoMapBounds(const IsoMap *map);

/* Whether anything stands in the way on a hex: a block or wall on its tile, a
   blocking path, a blocking prop's footprint, or the edge of the map. */
bool IsoMapHexBlocked(const IsoMap *map, const IsoTileset *tileset, IsoHex hex);
/* The same for every hex at once, into (2*width)*(2*height) bytes, row-major: 1
 * blocked. */
void IsoMapBuildBlocking(const IsoMap *map, const IsoTileset *tileset, unsigned char *out);
/* The building a tile's roof belongs to: every roofed tile connected to it,
   marked 1 in `hidden` (width*height bytes, cleared first). Returns how many; 0
   when the tile has no roof. */
int IsoMapRoofRegion(const IsoMap *map, IsoTile tile, unsigned char *hidden);

/* Optional derived-data cache. Zero-initialise, then free when done. Results
   are borrowed until the next call/free. Keys are computed from map contents,
   so edits through public arrays, undo, resizing and changed tileset footprints
   are detected without caller invalidation. */
typedef struct IsoMapQueryCache
{
    unsigned char *blocking, *hidden;
    size_t blockingSize, hiddenSize;
    uint64_t blockingKey, roofKey;
    bool blockingValid, roofValid;
    IsoTile roofTile;
    unsigned blockingBuilds, roofBuilds;
} IsoMapQueryCache;
/* NULL for invalid arguments or allocation failure. */
const unsigned char *IsoMapCachedBlocking(IsoMapQueryCache *cache, const IsoMap *map,
                                          const IsoTileset *tileset);
const unsigned char *IsoMapCachedRoofRegion(IsoMapQueryCache *cache, const IsoMap *map, IsoTile tile);
void IsoMapQueryCacheFree(IsoMapQueryCache *cache);

/* The ground -- floor and paths -- composited once into textures and drawn as a
   few quads. It is rebuilt only where the map changes, which is what lets an
   editor paint it live. */
typedef struct IsoGround
{
    Rectangle bounds;
    Image canvas;
    Texture2D *chunks;
    int chunk; /* side of a chunk texture: 512, or 256 where the device offers
                  less than 1024 */
    int columns, rows;
    unsigned seed;
} IsoGround;
bool IsoGroundBuild(IsoGround *ground, const IsoMap *map, const IsoTileset *tileset);
/* Recomposites the part of the ground inside `area` (world pixels) after an
 * edit there. */
void IsoGroundRefresh(IsoGround *ground, const IsoMap *map, const IsoTileset *tileset, Rectangle area);
void IsoGroundDraw(const IsoGround *ground, Rectangle visible);
void IsoGroundFree(IsoGround *ground);
/* Fog at the map's edge, built from the map's bounds alone: clear `depthTiles`
   tiles inside the edge of its tile parallelogram, thickening (smoothstep) to
   solid black at that edge and over the 160 px around IsoMapBounds, dithered by
   SpriteDitherDensity and blended 94% density, 6% dither. Black with alpha, in
   chunk textures like the ground but GL_ALPHA8 (a byte a texel; read back with
   glGetTexImage(GL_ALPHA), which raylib cannot do); drawn last over the scene,
   skipping its clear middle. */
typedef struct IsoFog
{
    Rectangle area;  /* world pixels the fog covers: IsoMapBounds padded 160 px */
    Rectangle clear; /* world pixels inside the shrunk parallelogram, all alpha 0;
                        drawing skips them */
    Texture2D *chunks;
    int chunk, columns, rows;
} IsoFog;
bool IsoFogBuild(IsoFog *fog, const IsoMap *map, float depthTiles);
void IsoFogDraw(const IsoFog *fog, Rectangle visible);
void IsoFogFree(IsoFog *fog);
/* The world area an edit to a hex or tile can change, edges and all. */
Rectangle IsoGroundHexArea(IsoHex hex);
Rectangle IsoGroundTileArea(IsoTile tile);

/* Everything the map draws standing up, as items for the depth sort alongside a
   game's own. Blocks and walls come as two slices, each on its own hex, so a
   character beside a block is drawn on the correct side of it; props come whole
   on their anchor hex. Roofs are listed separately: they go over everything, as
   Fallout draws them. */
typedef enum IsoMapPieceKind
{
    ISO_PIECE_BLOCK,
    ISO_PIECE_WALL,
    ISO_PIECE_PROP,
    ISO_PIECE_ROOF
} IsoMapPieceKind;
/* A connected footprint split on four u/v coordinates into at most nine
   occupied rectangles. Collected pieces own these values; they borrow only the
   usual sprite atlas. */
typedef struct IsoWallShape
{
    float u[4], v[4];
    unsigned cells;
    int levels;
} IsoWallShape;
typedef struct IsoMapPiece
{
    IsoMapPieceKind kind;
    bool backdrop;
    IsoHex hex;       /* depth */
    int order;        /* within the hex, so stacks draw bottom up: the level (roofs: see
                         IsoMapCollectRoofs) */
    Vector2 ground;   /* where the sheet's anchor goes */
    Vector2 shadowAt; /* where the shadow's anchor goes */
    const SpriteSheet *sheet, *shadow;
    int frame;                /* of both sheets */
    float sliceFrom, sliceTo; /* columns of the frame this piece draws, relative
                                 to the anchor */
    IsoWallShape wallShape;   /* cells != 0: clipped surfaces from a full block sheet */
    Vector2 wallShadowStep;
    int index; /* into the map's props, or the tile index for blocks, walls and
                  roofs */
} IsoMapPiece;
/* Fills out with what is inside `visible`; returns how many (never more than
 * capacity). */
int IsoMapCollectPieces(const IsoMap *map, const IsoTileset *tileset, Rectangle visible, IsoMapPiece *out,
                        int capacity);
/* Roofs, far to near, with the fills under a pitched roof's lifted tiles at its
   ends just before the tile they hold up (order 16 * level + rise, the tile
   itself 16 * level + 15). `hidden` (width*height, may be NULL) marks tiles
   whose roof is not drawn. */
int IsoMapCollectRoofs(const IsoMap *map, const IsoTileset *tileset, Rectangle visible,
                       const unsigned char *hidden, IsoMapPiece *out, int capacity);
void IsoMapPieceDraw(const IsoMapPiece *piece, Color tint);
void IsoMapPieceDrawShadow(const IsoMapPiece *piece);
#endif
