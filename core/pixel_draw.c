/* SPDX-License-Identifier: MPL-2.0 */
#include "pixel_draw.h"
#include <math.h>
#include <stdlib.h>

static bool Valid(Vector2 a, Vector2 b)
{
    return isfinite(a.x) && isfinite(a.y) && isfinite(b.x) && isfinite(b.y) &&
           fabsf(a.x)<8388608 && fabsf(a.y)<8388608 && fabsf(b.x)<8388608 && fabsf(b.y)<8388608 &&
           fabsf(a.x-b.x)<65536 && fabsf(a.y-b.y)<65536;
}
static void Segment(Vector2 a, Vector2 b, bool end, Color colour)
{
    int x=(int)roundf(a.x),y=(int)roundf(a.y),tx=(int)roundf(b.x),ty=(int)roundf(b.y);
    int dx=abs(tx-x),dy=-abs(ty-y),sx=x<tx?1:-1,sy=y<ty?1:-1,error=dx+dy;
    for (;;)
    {
        if (x==tx && y==ty) { if(end) DrawRectangle(x,y,1,1,colour); break; }
        DrawRectangle(x,y,1,1,colour);
        int twice=2*error;
        if (twice>=dy) { error+=dy; x+=sx; }
        if (twice<=dx) { error+=dx; y+=sy; }
    }
}
bool CoreDrawPixelLine(Vector2 a, Vector2 b, Color colour)
{
    if (!Valid(a,b)) return false;
    Segment(a,b,true,colour);
    return true;
}
bool CoreDrawPixelPath(const Vector2 *points, size_t count, bool closed, Color colour)
{
    if (!points || count<2 || count>65536) return false;
    size_t segments=closed?count:count-1;
    for (size_t i=0;i<segments;i++) if (!Valid(points[i],points[(i+1)%count])) return false;
    for (size_t i=0;i<segments;i++) Segment(points[i],points[(i+1)%count],!closed && i==segments-1,colour);
    return true;
}
