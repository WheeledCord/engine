#include "ui_editor.h"

#include "ui_internal.h"

#include <math.h>
#include <string.h>

static int MinInt(int a, int b) { return a < b ? a : b; }
static int MaxInt(int a, int b) { return a > b ? a : b; }
static int AbsInt(int a) { return a < 0 ? -a : a; }

// One axis of a rectangle: low and high along the snapping axis, start and end across it.
typedef struct Span
{
    int low;
    int high;
    int start;
    int end;
} Span;

typedef struct AxisSnap
{
    bool found;
    int delta;
    int distance;
    UiGuide guide;
} AxisSnap;

int UiPickRect(const UiRect *rects, size_t count, Vector2 point)
{
    if (!rects)
        return -1;
    for (size_t i = count; i-- > 0;)
        if (UiPointInRect(point, rects[i]))
            return (int)i;
    return -1;
}

static UiRect HandleRect(int size, UiRect rect, UiHandle handle)
{
    int left = rect.x;
    int middle = rect.x + rect.width / 2 - size / 2;
    int right = rect.x + rect.width - size;
    int top = rect.y;
    int centre = rect.y + rect.height / 2 - size / 2;
    int bottom = rect.y + rect.height - size;
    switch (handle)
    {
        case UI_HANDLE_TOP_LEFT: return (UiRect){left, top, size, size};
        case UI_HANDLE_TOP: return (UiRect){middle, top, size, size};
        case UI_HANDLE_TOP_RIGHT: return (UiRect){right, top, size, size};
        case UI_HANDLE_LEFT: return (UiRect){left, centre, size, size};
        case UI_HANDLE_RIGHT: return (UiRect){right, centre, size, size};
        case UI_HANDLE_BOTTOM_LEFT: return (UiRect){left, bottom, size, size};
        case UI_HANDLE_BOTTOM: return (UiRect){middle, bottom, size, size};
        case UI_HANDLE_BOTTOM_RIGHT: return (UiRect){right, bottom, size, size};
        default: return (UiRect){0};
    }
}

UiHandle UiHandleAt(const UiContext *ui, UiRect rect, Vector2 point)
{
    if (!ui)
        return UI_HANDLE_NONE;
    // Corners are tested first so they win where they overlap an edge handle.
    static const UiHandle order[] = {UI_HANDLE_TOP_LEFT,     UI_HANDLE_TOP_RIGHT,
                                     UI_HANDLE_BOTTOM_LEFT,  UI_HANDLE_BOTTOM_RIGHT,
                                     UI_HANDLE_TOP,          UI_HANDLE_BOTTOM,
                                     UI_HANDLE_LEFT,         UI_HANDLE_RIGHT};
    int size = ui->theme.editorHandleSize;
    bool roomy = rect.width >= size * 3 && rect.height >= size * 3;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++)
    {
        UiHandle handle = order[i];
        if (!roomy && handle != UI_HANDLE_BOTTOM_RIGHT)
            continue;
        if (UiPointInRect(point, UiRectInset(HandleRect(size, rect, handle), -1)))
            return handle;
    }
    return UiPointInRect(point, rect) ? UI_HANDLE_MOVE : UI_HANDLE_NONE;
}

UiRect UiClampRect(UiRect rect, UiRect bounds, int minimumSize)
{
    if (minimumSize < 1)
        minimumSize = 1;
    rect.width = MinInt(MaxInt(rect.width, minimumSize), MaxInt(bounds.width, minimumSize));
    rect.height = MinInt(MaxInt(rect.height, minimumSize), MaxInt(bounds.height, minimumSize));
    rect.x = MinInt(MaxInt(rect.x, bounds.x), bounds.x + bounds.width - rect.width);
    rect.y = MinInt(MaxInt(rect.y, bounds.y), bounds.y + bounds.height - rect.height);
    return rect;
}

static void HandleEdges(UiHandle handle, bool *left, bool *right, bool *top, bool *bottom)
{
    *left = handle == UI_HANDLE_LEFT || handle == UI_HANDLE_TOP_LEFT ||
            handle == UI_HANDLE_BOTTOM_LEFT;
    *right = handle == UI_HANDLE_RIGHT || handle == UI_HANDLE_TOP_RIGHT ||
             handle == UI_HANDLE_BOTTOM_RIGHT;
    *top = handle == UI_HANDLE_TOP || handle == UI_HANDLE_TOP_LEFT ||
           handle == UI_HANDLE_TOP_RIGHT;
    *bottom = handle == UI_HANDLE_BOTTOM || handle == UI_HANDLE_BOTTOM_LEFT ||
              handle == UI_HANDLE_BOTTOM_RIGHT;
}

static Span RectSpan(UiRect rect, bool horizontal)
{
    if (horizontal)
        return (Span){rect.x, rect.x + rect.width, rect.y, rect.y + rect.height};
    return (Span){rect.y, rect.y + rect.height, rect.x, rect.x + rect.width};
}

static void Consider(AxisSnap *best, int edge, int target, int start, int end, int threshold)
{
    int delta = target - edge;
    int distance = AbsInt(delta);
    if (distance > threshold || (best->found && distance >= best->distance))
        return;
    best->found = true;
    best->delta = delta;
    best->distance = distance;
    best->guide = (UiGuide){true, target, start, end};
}

static AxisSnap SnapAxis(UiRect rect, UiSnapConfig config, bool horizontal, bool moveLow,
                         bool moveHigh, bool translate)
{
    AxisSnap best = {0};
    Span moving = RectSpan(rect, horizontal);
    Span area = RectSpan(config.bounds, horizontal);
    int centre = (moving.low + moving.high) / 2;
    int gap = MaxInt(0, config.gap);

    if (moveLow)
    {
        Consider(&best, moving.low, area.low, area.start, area.end, config.threshold);
        Consider(&best, moving.low, area.low + gap, area.start, area.end, config.threshold);
    }
    if (moveHigh)
    {
        Consider(&best, moving.high, area.high, area.start, area.end, config.threshold);
        Consider(&best, moving.high, area.high - gap, area.start, area.end, config.threshold);
    }
    if (translate)
        Consider(&best, centre, (area.low + area.high) / 2, area.start, area.end, config.threshold);

    for (size_t i = 0; i < config.otherCount; i++)
    {
        Span other = RectSpan(config.others[i], horizontal);
        int start = MinInt(moving.start, other.start);
        int end = MaxInt(moving.end, other.end);
        if (moveLow)
        {
            Consider(&best, moving.low, other.low, start, end, config.threshold);
            Consider(&best, moving.low, other.high, start, end, config.threshold);
            Consider(&best, moving.low, other.high + gap, start, end, config.threshold);
        }
        if (moveHigh)
        {
            Consider(&best, moving.high, other.high, start, end, config.threshold);
            Consider(&best, moving.high, other.low, start, end, config.threshold);
            Consider(&best, moving.high, other.low - gap, start, end, config.threshold);
        }
        if (translate)
            Consider(&best, centre, (other.low + other.high) / 2, start, end, config.threshold);
    }
    return best;
}

static int SnapToStep(int value, int origin, int step)
{
    if (step < 2)
        return value;
    int offset = value - origin;
    int rounded = (int)lroundf((float)offset / (float)step) * step;
    return origin + rounded;
}

static void ApplyAxis(int *low, int *size, int delta, bool moveLow, bool translate)
{
    if (translate)
        *low += delta;
    else if (moveLow)
    {
        *low += delta;
        *size -= delta;
    }
    else
        *size += delta;
}

UiSnapResult UiSnapRect(UiRect *rect, UiHandle handle, UiSnapConfig config)
{
    UiSnapResult result = {0};
    if (!rect || handle == UI_HANDLE_NONE)
        return result;
    bool translate = handle == UI_HANDLE_MOVE;
    bool edgeLeft = false;
    bool edgeRight = false;
    bool edgeTop = false;
    bool edgeBottom = false;
    HandleEdges(handle, &edgeLeft, &edgeRight, &edgeTop, &edgeBottom);
    bool left = translate || edgeLeft;
    bool right = translate || edgeRight;
    bool top = translate || edgeTop;
    bool bottom = translate || edgeBottom;

    if (config.step > 1)
    {
        if (translate)
        {
            rect->x = SnapToStep(rect->x, config.bounds.x, config.step);
            rect->y = SnapToStep(rect->y, config.bounds.y, config.step);
        }
        else
        {
            if (left)
            {
                int snapped = SnapToStep(rect->x, config.bounds.x, config.step);
                rect->width += rect->x - snapped;
                rect->x = snapped;
            }
            if (right)
                rect->width = SnapToStep(rect->x + rect->width, config.bounds.x, config.step) - rect->x;
            if (top)
            {
                int snapped = SnapToStep(rect->y, config.bounds.y, config.step);
                rect->height += rect->y - snapped;
                rect->y = snapped;
            }
            if (bottom)
                rect->height =
                    SnapToStep(rect->y + rect->height, config.bounds.y, config.step) - rect->y;
        }
    }

    // A resized element prefers whole conventional rows, so stacks stay on the same rhythm.
    if (!translate && config.rowHeight > 0 && config.threshold > 0 && (top || bottom))
    {
        int rows = (rect->height + config.rowHeight / 2) / config.rowHeight;
        if (rows < 1)
            rows = 1;
        int target = rows * config.rowHeight;
        if (AbsInt(target - rect->height) <= config.threshold)
        {
            if (top)
            {
                rect->y += rect->height - target;
                rect->height = target;
            }
            else
                rect->height = target;
        }
    }

    if (config.threshold > 0)
    {
        AxisSnap x = SnapAxis(*rect, config, true, left, right, translate);
        if (x.found)
        {
            ApplyAxis(&rect->x, &rect->width, x.delta, left, translate);
            result.vertical = x.guide;
        }
        AxisSnap y = SnapAxis(*rect, config, false, top, bottom, translate);
        if (y.found)
        {
            ApplyAxis(&rect->y, &rect->height, y.delta, top, translate);
            result.horizontal = y.guide;
        }
    }
    return result;
}

bool UiEditRect(UiContext *ui, UiRect bounds, UiRect *rect, UiRectEditor *editor,
                const UiSnapConfig *snap, UiSnapResult *guides)
{
    if (guides)
        *guides = (UiSnapResult){0};
    if (!ui || !ui->state || !rect || !editor)
        return false;
    UiState *state = ui->state;
    Vector2 mouse = state->input.mousePosition;
    uint64_t id = UiPointerId(editor, 0x5245435445444954ULL);
    int minimum = MaxInt(8, ui->theme.editorHandleSize);
    UiHandle hovered = UiHandleAt(ui, *rect, mouse);
    if (hovered != UI_HANDLE_NONE)
        UiMarkMouse(ui);
    if (UiInputAllowed(ui) && hovered != UI_HANDLE_NONE &&
        state->input.mousePressed[MOUSE_BUTTON_LEFT] && !state->activeId)
    {
        state->activeId = id;
        editor->handle = hovered;
        editor->grab = mouse;
        editor->origin = *rect;
    }

    UiRect before = *rect;
    if (state->activeId == id && editor->handle != UI_HANDLE_NONE &&
        state->input.mouseDown[MOUSE_BUTTON_LEFT])
    {
        int dx = (int)lroundf(mouse.x - editor->grab.x);
        int dy = (int)lroundf(mouse.y - editor->grab.y);
        UiRect next = editor->origin;
        bool left = false;
        bool right = false;
        bool top = false;
        bool bottom = false;
        HandleEdges(editor->handle, &left, &right, &top, &bottom);
        if (editor->handle == UI_HANDLE_MOVE)
        {
            next.x += dx;
            next.y += dy;
        }
        if (left)
        {
            next.x += dx;
            next.width -= dx;
        }
        if (right)
            next.width += dx;
        if (top)
        {
            next.y += dy;
            next.height -= dy;
        }
        if (bottom)
            next.height += dy;
        if (next.width < minimum)
        {
            if (left)
                next.x = editor->origin.x + editor->origin.width - minimum;
            next.width = minimum;
        }
        if (next.height < minimum)
        {
            if (top)
                next.y = editor->origin.y + editor->origin.height - minimum;
            next.height = minimum;
        }
        *rect = next;
        if (snap)
        {
            UiSnapResult snapped = UiSnapRect(rect, editor->handle, *snap);
            if (guides)
                *guides = snapped;
        }
        *rect = UiClampRect(*rect, bounds, minimum);
    }
    if (!state->input.mouseDown[MOUSE_BUTTON_LEFT])
        editor->handle = UI_HANDLE_NONE;
    return memcmp(&before, rect, sizeof before) != 0;
}

void UiDrawRectSelection(UiContext *ui, UiRect rect)
{
    if (!ui)
        return;
    // Two outlines so the selection reads on both the light face and the dark workspace.
    UiRect outer = UiRectInset(rect, -2);
    DrawRectangleLines(outer.x, outer.y, outer.width, outer.height, ui->theme.white);
    DrawRectangleLines(rect.x - 1, rect.y - 1, rect.width + 2, rect.height + 2, ui->theme.black);
    int size = ui->theme.editorHandleSize;
    bool roomy = rect.width >= size * 3 && rect.height >= size * 3;
    static const UiHandle handles[] = {UI_HANDLE_TOP_LEFT,    UI_HANDLE_TOP,
                                       UI_HANDLE_TOP_RIGHT,   UI_HANDLE_LEFT,
                                       UI_HANDLE_RIGHT,       UI_HANDLE_BOTTOM_LEFT,
                                       UI_HANDLE_BOTTOM,      UI_HANDLE_BOTTOM_RIGHT};
    for (size_t i = 0; i < sizeof handles / sizeof handles[0]; i++)
    {
        if (!roomy && handles[i] != UI_HANDLE_BOTTOM_RIGHT)
            continue;
        UiRect handle = HandleRect(size, rect, handles[i]);
        DrawRectangle(handle.x, handle.y, handle.width, handle.height, ui->theme.white);
        DrawRectangle(handle.x + 1, handle.y + 1, MaxInt(0, handle.width - 2),
                      MaxInt(0, handle.height - 2), ui->theme.black);
    }
}

void UiDrawGuides(UiContext *ui, UiSnapResult guides, Color color)
{
    if (!ui)
        return;
    if (guides.vertical.active)
        DrawRectangle(guides.vertical.position, guides.vertical.start, 1,
                      MaxInt(1, guides.vertical.end - guides.vertical.start), color);
    if (guides.horizontal.active)
        DrawRectangle(guides.horizontal.start, guides.horizontal.position,
                      MaxInt(1, guides.horizontal.end - guides.horizontal.start), 1, color);
}
