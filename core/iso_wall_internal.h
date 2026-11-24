/* Internal wall composition. No owned graphics resources or script-visible
 * lifecycle. */
#ifndef CORE_ISO_WALL_INTERNAL_H
#define CORE_ISO_WALL_INTERNAL_H
#include "iso_map.h"
struct IsoWallCache *IsoWallCacheNew(void);
void IsoWallCacheFree(struct IsoWallCache *cache);
void IsoWallInvalidate(IsoMap *map);
bool IsoWallPrepare(const IsoMap *map);
IsoWallShape IsoWallShapeAt(const IsoMap *map, IsoTile tile, const IsoPiece *kind);
bool IsoWallBlocks(const IsoMap *map, const IsoTileset *tileset, IsoHex hex);
void IsoWallDraw(const IsoMapPiece *piece, Color tint);
void IsoWallDrawShadow(const IsoMapPiece *piece);
#endif
