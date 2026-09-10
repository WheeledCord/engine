#include "ui_document.h"

#include "file.h"
#include "ui_containers.h"
#include "ui_internal.h"
#include "ui_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_DOCUMENT_VERSION 5
#define UI_ELEMENT_MINIMUM 8
#define UI_SURFACE_MINIMUM 16
#define UI_SURFACE_MAXIMUM 8192

static int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static const char *DefaultLabel(UiElementType type)
{
    static const char *labels[] = {"Button", "Slider", "Checkbox", "Label", "Indent", "Window"};
    return type >= 0 && type < UI_ELEMENT_COUNT ? labels[type] : "Element";
}

// Field separators would break the line-oriented format on reload.
static void CopySingleLine(char *destination, size_t capacity, const char *source)
{
    snprintf(destination, capacity, "%s", source ? source : "");
    for (char *c = destination; *c; c++)
        if (*c == '\t' || *c == '\n' || *c == '\r')
            *c = ' ';
}

void UiDocumentSetSurface(UiDocument *document, int surfaceWidth, int surfaceHeight,
                          UiSurfaceStyle style, const char *title)
{
    if (!document)
        return;
    document->surfaceWidth = ClampInt(surfaceWidth, UI_SURFACE_MINIMUM, UI_SURFACE_MAXIMUM);
    document->surfaceHeight = ClampInt(surfaceHeight, UI_SURFACE_MINIMUM, UI_SURFACE_MAXIMUM);
    document->style = style >= 0 && style < UI_SURFACE_STYLE_COUNT ? style : UI_SURFACE_PANEL;
    CopySingleLine(document->title, sizeof document->title, title);
}

void UiDocumentInit(UiDocument *document, int surfaceWidth, int surfaceHeight, UiSurfaceStyle style,
                    const char *title)
{
    if (!document)
        return;
    UiDocumentFree(document);
    UiDocumentSetSurface(document, surfaceWidth, surfaceHeight, style, title);
}

void UiDocumentFree(UiDocument *document)
{
    if (!document)
        return;
    free(document->elements);
    free(document->resolved);
    *document = (UiDocument){0};
}

bool UiDocumentCopy(UiDocument *destination, const UiDocument *source)
{
    if (!destination || !source)
        return false;
    UiElement *elements = NULL;
    if (source->count)
    {
        elements = malloc(source->count * sizeof(*elements));
        if (!elements)
            return false;
        memcpy(elements, source->elements, source->count * sizeof(*elements));
    }
    UiDocumentFree(destination);
    *destination = *source;
    destination->elements = elements;
    destination->capacity = source->count;
    // The resolve scratch belongs to the document it was built for.
    destination->resolved = NULL;
    destination->resolvedCapacity = 0;
    destination->resolvedValid = false;
    return true;
}

static int MaxInt(int a, int b) { return a > b ? a : b; }

static bool Encloses(UiRect outer, UiRect inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

static bool IsContainer(UiElementType type)
{
    return type == UI_ELEMENT_INDENT || type == UI_ELEMENT_WINDOW;
}

// A container is the content area, so it follows the width it is given, the way a block element or
// a width-sizable view does. A plain control keeps the size it was drawn at and stays put.
static unsigned DefaultAnchors(UiElementType type)
{
    if (IsContainer(type))
        return UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP | UI_ANCHOR_BOTTOM;
    return UI_ANCHOR_LEFT | UI_ANCHOR_TOP;
}

// An indent is a region its contents share, so by default they divide it. A window element is a
// surface in its own right, so its contents stay where they were put.
static UiFlow DefaultFlow(UiElementType type)
{
    return type == UI_ELEMENT_INDENT ? UI_FLOW_AUTO : UI_FLOW_FREE;
}

UiRect UiDocumentContainerContent(const UiContext *ui, const UiElement *element, UiRect rect)
{
    if (!ui || !element)
        return rect;
    if (element->type == UI_ELEMENT_WINDOW)
        return (UiRect){rect.x + ui->theme.frameWidth,
                        rect.y + ui->theme.frameWidth + ui->theme.titleHeight,
                        MaxInt(0, rect.width - ui->theme.frameWidth * 2),
                        MaxInt(0, rect.height - ui->theme.frameWidth * 2 - ui->theme.titleHeight)};
    return UiRectInset(rect, ui->theme.indentWidth);
}

int UiDocumentContainerOf(const UiDocument *document, size_t index)
{
    if (!document || index >= document->count)
        return -1;
    UiRect rect = document->elements[index].rect;
    int best = -1;
    long smallest = 0;
    for (size_t i = 0; i < document->count; i++)
    {
        const UiElement *candidate = &document->elements[i];
        if (i == index || !IsContainer(candidate->type) || !Encloses(candidate->rect, rect))
            continue;
        long area = (long)candidate->rect.width * candidate->rect.height;
        if (best >= 0 && area >= smallest)
            continue;
        best = (int)i;
        smallest = area;
    }
    return best;
}

bool UiDocumentInsideIndent(const UiDocument *document, size_t index)
{
    int container = UiDocumentContainerOf(document, index);
    return container >= 0 && document->elements[container].type == UI_ELEMENT_INDENT;
}

UiElement *UiDocumentAdd(UiDocument *document, UiElementType type, UiRect rect, const char *label)
{
    if (!document || type < 0 || type >= UI_ELEMENT_COUNT || rect.width <= 0 || rect.height <= 0)
        return NULL;
    if (document->count == document->capacity)
    {
        size_t capacity = document->capacity ? document->capacity * 2 : 16;
        UiElement *elements = realloc(document->elements, capacity * sizeof(*elements));
        if (!elements)
            return NULL;
        document->elements = elements;
        document->capacity = capacity;
    }
    UiElement *element = &document->elements[document->count++];
    *element = (UiElement){type, rect, "", 0.5f, false, DefaultAnchors(type), 0, 0, 0, 0,
                           DefaultFlow(type),
                           {type == UI_ELEMENT_BUTTON ? UI_ALIGN_CENTER : UI_ALIGN_START,
                            UI_ALIGN_CENTER}};
    document->resolvedValid = false;
    CopySingleLine(element->label, sizeof element->label, label ? label : DefaultLabel(type));
    return element;
}

bool UiDocumentRemove(UiDocument *document, size_t index)
{
    if (!document || index >= document->count)
        return false;
    memmove(&document->elements[index], &document->elements[index + 1],
            (document->count - index - 1) * sizeof(document->elements[0]));
    document->count--;
    return true;
}

bool UiDocumentSwap(UiDocument *document, size_t a, size_t b)
{
    if (!document || a >= document->count || b >= document->count || a == b)
        return false;
    UiElement held = document->elements[a];
    document->elements[a] = document->elements[b];
    document->elements[b] = held;
    return true;
}

// The floor is the minimum the layout carries, which the editor fills in from the label. Nothing is
// guessed here: an unset minimum only keeps an element from vanishing.
static void ResizeFloor(const UiContext *ui, const UiElement *element, int *width, int *height)
{
    (void)ui;
    *width = MaxInt(element->minWidth, UI_ELEMENT_MINIMUM);
    *height = MaxInt(element->minHeight, UI_ELEMENT_MINIMUM);
}

// AUTO reads the axis back off the contents: a bar of buttons side by side divides left to right,
// a stack divides top to bottom.
static UiFlow EffectiveFlow(const UiDocument *document, int container)
{
    if (container < 0)
        return UI_FLOW_FREE;
    UiFlow flow = document->elements[container].flow;
    if (flow != UI_FLOW_AUTO)
        return flow;
    int spanX = 0;
    int spanY = 0;
    int first = -1;
    for (size_t i = 0; i < document->count; i++)
    {
        if (UiDocumentContainerOf(document, i) != container)
            continue;
        UiRect rect = document->elements[i].rect;
        if (first < 0)
        {
            first = (int)i;
            continue;
        }
        UiRect other = document->elements[first].rect;
        spanX = MaxInt(spanX, rect.x + rect.width / 2 - (other.x + other.width / 2));
        spanY = MaxInt(spanY, rect.y + rect.height / 2 - (other.y + other.height / 2));
    }
    if (spanX < 0)
        spanX = -spanX;
    if (spanY < 0)
        spanY = -spanY;
    return spanY > spanX ? UI_FLOW_COLUMN : UI_FLOW_ROW;
}

// Direct children of a container, in the order they were added.
static int ChildOrdinal(const UiDocument *document, size_t index, int container, int *total)
{
    int ordinal = -1;
    int seen = 0;
    for (size_t i = 0; i < document->count; i++)
    {
        if (UiDocumentContainerOf(document, i) != container)
            continue;
        if (i == index)
            ordinal = seen;
        seen++;
    }
    *total = seen;
    return ordinal;
}

static int NestingDepth(const UiDocument *document, size_t index)
{
    int depth = 0;
    int container = UiDocumentContainerOf(document, index);
    while (container >= 0 && depth < (int)document->count)
    {
        depth++;
        container = UiDocumentContainerOf(document, (size_t)container);
    }
    return depth;
}

// One axis of the anchor rule. Offsets are relative to the containing region.
static bool StretchesOn(const UiElement *element, bool horizontal)
{
    unsigned low = horizontal ? UI_ANCHOR_LEFT : UI_ANCHOR_TOP;
    unsigned high = horizontal ? UI_ANCHOR_RIGHT : UI_ANCHOR_BOTTOM;
    return (element->anchors & low) && (element->anchors & high);
}

// Two things pinned to both edges each want the whole change, which would run them into each other.
// Siblings that stretch on the same axis divide the change between them, in proportion to the size
// they were drawn at, which leaves every gap between them exactly as it was. One that reaches its
// limit stops there and hands what it did not take to the others, so the row still fits its margins.
#define UI_QUEUE_CAPACITY 64

static bool ShareStretch(const UiDocument *document, size_t index, int container, bool horizontal,
                         int delta, int *shift, int *grow)
{
    const UiElement *element = &document->elements[index];
    if (!StretchesOn(element, horizontal))
        return false;
    int mine = horizontal ? element->rect.width : element->rect.height;
    int low = horizontal ? element->rect.x : element->rect.y;
    int across = horizontal ? element->rect.y : element->rect.x;
    int acrossSize = horizontal ? element->rect.height : element->rect.width;

    int sizes[UI_QUEUE_CAPACITY];
    int limits[UI_QUEUE_CAPACITY];
    int lows[UI_QUEUE_CAPACITY];
    int queue = 0;
    int me = -1;
    for (size_t i = 0; i < document->count && queue < UI_QUEUE_CAPACITY; i++)
    {
        const UiElement *other = &document->elements[i];
        if (UiDocumentContainerOf(document, i) != container || !StretchesOn(other, horizontal))
            continue;
        int size = horizontal ? other->rect.width : other->rect.height;
        int otherLow = horizontal ? other->rect.x : other->rect.y;
        int otherAcross = horizontal ? other->rect.y : other->rect.x;
        int otherAcrossSize = horizontal ? other->rect.height : other->rect.width;
        // My queue along this axis is the things that follow me on it and share my band across it:
        // in a grid, my row when dividing width, my column when dividing height. Anything merely
        // beside me takes the whole change itself.
        if (i != index && (otherLow < low + mine && low < otherLow + size))
            continue;
        if (i != index &&
            !(otherAcross < across + acrossSize && across < otherAcross + otherAcrossSize))
            continue;
        if (i == index)
            me = queue;
        sizes[queue] = size;
        limits[queue] = horizontal ? other->maxWidth : other->maxHeight;
        lows[queue] = otherLow;
        queue++;
    }
    if (queue < 2 || me < 0)
        return false;

    int growth[UI_QUEUE_CAPACITY] = {0};
    bool settled[UI_QUEUE_CAPACITY] = {false};
    int remaining = delta;
    for (int pass = 0; pass <= queue; pass++)
    {
        int total = 0;
        for (int k = 0; k < queue; k++)
            if (!settled[k])
                total += sizes[k];
        if (total <= 0)
            break;
        // Prefix sums, so the shares add up to the whole change with no pixel lost or doubled.
        int taken = 0;
        int before = 0;
        int over = -1;
        for (int k = 0; k < queue && over < 0; k++)
        {
            if (settled[k])
                continue;
            before += sizes[k];
            int through = remaining * before / total;
            growth[k] = through - taken;
            taken = through;
            if (limits[k] > 0 && sizes[k] + growth[k] > limits[k])
                over = k;
        }
        if (over < 0)
            break;
        growth[over] = MaxInt(0, limits[over] - sizes[over]);
        settled[over] = true;
        remaining -= growth[over];
    }

    *grow = growth[me];
    *shift = 0;
    for (int k = 0; k < queue; k++)
        if (lows[k] < low || (lows[k] == low && k < me))
            *shift += growth[k];
    return true;
}

static void ResolveAxis(int authoredLow, int authoredSize, int authoredExtent, int resolvedExtent,
                        bool pinLow, bool pinHigh, int minimum, int limit, int *low, int *size)
{
    int delta = resolvedExtent - authoredExtent;
    *low = authoredLow;
    *size = authoredSize;
    if (pinLow && pinHigh)
        *size += delta;
    else if (pinHigh)
        *low += delta;
    else if (!pinLow)
    {
        // Neither edge pinned: the element floats with the middle of the region.
        float ratio = authoredExtent > 0 ? (float)resolvedExtent / (float)authoredExtent : 1.0f;
        int centre = (int)lroundf(((float)authoredLow + authoredSize * 0.5f) * ratio);
        *low = centre - authoredSize / 2;
    }
    if (*size < minimum)
        *size = minimum;
    if (limit > 0 && *size > limit)
        *size = limit;
}

static void ElementNeed(const UiContext *ui, const UiDocument *document, size_t index, int *width,
                        int *height);

static int ConstrainSize(int size, int minimum, int maximum)
{
    size = MaxInt(size, minimum);
    // An explicit maximum wins if the authored minimum is larger, as in ResolveAxis.
    return maximum > 0 && size > maximum ? maximum : size;
}

// Equal shares between each child's limits. Share remaining space among unconstrained siblings;
// if all children hit their maximum, leave the surplus at the end of the region.
static UiRect FlowCell(const UiContext *ui, const UiDocument *document, size_t index,
                       int container, UiRect area, bool horizontal)
{
    int extent = horizontal ? area.width : area.height;
    int low = 0;
    int high = MaxInt(0, extent);
    while (low < high)
    {
        int level = low + (high - low + 1) / 2;
        long long used = 0;
        for (size_t i = 0; i < document->count; i++)
        {
            if (UiDocumentContainerOf(document, i) != container)
                continue;
            int width, height;
            ElementNeed(ui, document, i, &width, &height);
            const UiElement *child = &document->elements[i];
            int maximum = horizontal ? child->maxWidth : child->maxHeight;
            used += ConstrainSize(level, horizontal ? width : height, maximum);
        }
        if (used <= extent)
            low = level;
        else
            high = level - 1;
    }

    int used = 0;
    int flexible = 0;
    for (size_t i = 0; i < document->count; i++)
    {
        if (UiDocumentContainerOf(document, i) != container)
            continue;
        int width, height;
        ElementNeed(ui, document, i, &width, &height);
        int minimum = horizontal ? width : height;
        const UiElement *child = &document->elements[i];
        int maximum = horizontal ? child->maxWidth : child->maxHeight;
        used += ConstrainSize(low, minimum, maximum);
        flexible += minimum <= low && (maximum <= 0 || low < maximum);
    }
    int remainder = MaxInt(0, extent - used);
    int ordinal = 0;
    int offset = 0;
    for (size_t i = 0; i < document->count; i++)
    {
        if (UiDocumentContainerOf(document, i) != container)
            continue;
        int width, height;
        ElementNeed(ui, document, i, &width, &height);
        int minimum = horizontal ? width : height;
        const UiElement *child = &document->elements[i];
        int maximum = horizontal ? child->maxWidth : child->maxHeight;
        int size = ConstrainSize(low, minimum, maximum);
        if (minimum <= low && (maximum <= 0 || low < maximum) && flexible)
        {
            // Prefix rounding accounts for every pixel, even when shares are unequal.
            size += remainder * (ordinal + 1) / flexible - remainder * ordinal / flexible;
            ordinal++;
        }
        if (i == index)
            return horizontal
                       ? (UiRect){area.x + offset, area.y, size,
                                  ConstrainSize(area.height, height, child->maxHeight)}
                       : (UiRect){area.x, area.y + offset,
                                  ConstrainSize(area.width, width, child->maxWidth), size};
        offset += size;
    }
    return (UiRect){0};
}

const UiRect *UiDocumentResolve(UiContext *ui, UiDocument *document, int width, int height)
{
    if (!ui || !document)
        return NULL;
    if (document->resolvedCapacity < document->count)
    {
        UiRect *grown = realloc(document->resolved, MaxInt(1, (int)document->count) * sizeof(*grown));
        if (!grown)
            return NULL;
        document->resolved = grown;
        document->resolvedCapacity = document->count;
    }
    for (size_t i = 0; i < document->count; i++)
        document->resolved[i] = document->elements[i].rect;

    UiRect authoredSurface = {0, 0, document->surfaceWidth, document->surfaceHeight};
    UiRect resolvedSurface = {0, 0, MaxInt(1, width), MaxInt(1, height)};
    // Containers must land before their contents, so work outwards in.
    for (int depth = 0; depth <= (int)document->count; depth++)
    {
        bool any = false;
        for (size_t i = 0; i < document->count; i++)
        {
            if (NestingDepth(document, i) != depth)
                continue;
            any = true;
            const UiElement *element = &document->elements[i];
            int container = UiDocumentContainerOf(document, i);
            UiRect authored = authoredSurface;
            UiRect area = resolvedSurface;
            if (container >= 0)
            {
                authored = UiDocumentContainerContent(ui, &document->elements[container],
                                                      document->elements[container].rect);
                area = UiDocumentContainerContent(ui, &document->elements[container],
                                                  document->resolved[container]);
            }
            int total = 0;
            int ordinal = container >= 0 ? ChildOrdinal(document, i, container, &total) : -1;
            UiFlow flow = EffectiveFlow(document, container);
            if (flow != UI_FLOW_FREE && ordinal >= 0 && total > 0)
            {
                // Flow divides the region flush, so the contents keep meeting bezel to bezel.
                document->resolved[i] = FlowCell(ui, document, i, container, area,
                                                flow == UI_FLOW_ROW);
                continue;
            }
            UiRect out = {0};
            int floorWidth = 0;
            int floorHeight = 0;
            int shift = 0;
            int grow = 0;
            ResizeFloor(ui, element, &floorWidth, &floorHeight);
            if (ShareStretch(document, i, container, true, area.width - authored.width, &shift, &grow))
            {
                out.x = element->rect.x - authored.x + shift;
                out.width = MaxInt(element->rect.width + grow, floorWidth);
                if (element->maxWidth > 0 && out.width > element->maxWidth)
                    out.width = element->maxWidth;
            }
            else
                ResolveAxis(element->rect.x - authored.x, element->rect.width, authored.width,
                            area.width, (element->anchors & UI_ANCHOR_LEFT) != 0,
                            (element->anchors & UI_ANCHOR_RIGHT) != 0, floorWidth,
                            element->maxWidth, &out.x, &out.width);
            if (ShareStretch(document, i, container, false, area.height - authored.height, &shift,
                             &grow))
            {
                out.y = element->rect.y - authored.y + shift;
                out.height = MaxInt(element->rect.height + grow, floorHeight);
                if (element->maxHeight > 0 && out.height > element->maxHeight)
                    out.height = element->maxHeight;
            }
            else
                ResolveAxis(element->rect.y - authored.y, element->rect.height, authored.height,
                            area.height, (element->anchors & UI_ANCHOR_TOP) != 0,
                            (element->anchors & UI_ANCHOR_BOTTOM) != 0, floorHeight,
                            element->maxHeight, &out.y, &out.height);
            out.x += area.x;
            out.y += area.y;
            document->resolved[i] = out;
        }
        if (!any && depth > 0)
            break;
    }
    document->resolvedWidth = resolvedSurface.width;
    document->resolvedHeight = resolvedSurface.height;
    document->resolvedValid = true;
    return document->resolved;
}

// What one element needs of its containing region, given how it is pinned.
static void ElementNeed(const UiContext *ui, const UiDocument *document, size_t index, int *width,
                        int *height)
{
    const UiElement *element = &document->elements[index];
    int needWidth = 0;
    int needHeight = 0;
    ResizeFloor(ui, element, &needWidth, &needHeight);
    if (IsContainer(element->type))
    {
        int insideWidth = 0;
        int insideHeight = 0;
        int children = 0;
        UiFlow flow = EffectiveFlow(document, (int)index);
        for (size_t i = 0; i < document->count; i++)
        {
            if (UiDocumentContainerOf(document, i) != (int)index)
                continue;
            int childWidth = 0;
            int childHeight = 0;
            ElementNeed(ui, document, i, &childWidth, &childHeight);
            children++;
            insideWidth = flow == UI_FLOW_ROW ? insideWidth + childWidth
                                              : MaxInt(insideWidth, childWidth);
            insideHeight = flow == UI_FLOW_COLUMN ? insideHeight + childHeight
                                                  : MaxInt(insideHeight, childHeight);
        }
        if (children)
        {
            // Along the flow, each child only reserves its own minimum.
            UiRect chrome = UiDocumentContainerContent(ui, element, element->rect);
            needWidth = MaxInt(needWidth, insideWidth + element->rect.width - chrome.width);
            needHeight = MaxInt(needHeight, insideHeight + element->rect.height - chrome.height);
        }
    }
    int container = UiDocumentContainerOf(document, index);
    UiRect area = {0, 0, document->surfaceWidth, document->surfaceHeight};
    if (container >= 0)
        area = UiDocumentContainerContent(ui, &document->elements[container],
                                          document->elements[container].rect);
    int left = element->rect.x - area.x;
    int top = element->rect.y - area.y;
    int right = area.width - left - element->rect.width;
    int bottom = area.height - top - element->rect.height;
    bool pinLeft = (element->anchors & UI_ANCHOR_LEFT) != 0;
    bool pinRight = (element->anchors & UI_ANCHOR_RIGHT) != 0;
    bool pinTop = (element->anchors & UI_ANCHOR_TOP) != 0;
    bool pinBottom = (element->anchors & UI_ANCHOR_BOTTOM) != 0;
    // Anything sized by its container gives space back down to its minimum. Anything else keeps the
    // room it was drawn at, or the surface would shrink until it clipped a control that never gets
    // any smaller.
    bool shared = EffectiveFlow(document, container) != UI_FLOW_FREE;
    if (!shared && !(pinLeft && pinRight))
        needWidth = MaxInt(needWidth, element->rect.width);
    if (!shared && !(pinTop && pinBottom))
        needHeight = MaxInt(needHeight, element->rect.height);
    needWidth = ConstrainSize(needWidth, 0, element->maxWidth);
    needHeight = ConstrainSize(needHeight, 0, element->maxHeight);
    if (shared)
    {
        *width = needWidth;
        *height = needHeight;
        return;
    }
    *width = needWidth + (pinLeft ? left : 0) + (pinRight ? MaxInt(0, right) : 0);
    *height = needHeight + (pinTop ? top : 0) + (pinBottom ? MaxInt(0, bottom) : 0);
}

void UiDocumentMinimumSize(const UiContext *ui, const UiDocument *document, int *width, int *height)
{
    int smallestWidth = UI_SURFACE_MINIMUM;
    int smallestHeight = UI_SURFACE_MINIMUM;
    if (ui && document)
        for (size_t i = 0; i < document->count; i++)
        {
            if (UiDocumentContainerOf(document, i) >= 0)
                continue;
            int needWidth = 0;
            int needHeight = 0;
            ElementNeed(ui, document, i, &needWidth, &needHeight);
            smallestWidth = MaxInt(smallestWidth, needWidth);
            smallestHeight = MaxInt(smallestHeight, needHeight);
        }
    if (width)
        *width = smallestWidth;
    if (height)
        *height = smallestHeight;
}

bool UiDocumentSave(const UiDocument *document, const char *path)
{
    if (!document || !path)
        return false;
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    char title[UI_DOCUMENT_TITLE_CAPACITY];
    CopySingleLine(title, sizeof title, document->title);
    fprintf(file, "core_ui_document %d\n", UI_DOCUMENT_VERSION);
    fprintf(file, "surface %d %d %d\t%s\n", document->surfaceWidth, document->surfaceHeight,
            (int)document->style, title);
    for (size_t i = 0; i < document->count; i++)
    {
        const UiElement *element = &document->elements[i];
        char label[UI_ELEMENT_LABEL_CAPACITY];
        CopySingleLine(label, sizeof label, element->label);
        fprintf(file, "%d %d %d %d %d %.9g %d %u %d %d %d %d %d %d %d\t%s\n", element->type,
                element->rect.x, element->rect.y, element->rect.width, element->rect.height,
                element->value, element->checked, element->anchors, element->minWidth,
                element->minHeight, (int)element->flow, element->maxWidth, element->maxHeight,
                (int)element->textAlignment.horizontal, (int)element->textAlignment.vertical, label);
    }
    bool ok = !ferror(file);
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

bool UiDocumentLoad(UiDocument *document, const char *path)
{
    if (!document || !path)
        return false;
    // Loading follows the engine root so a layout ships with its project; saving never redirects,
    // because the caller is naming where the file should be written.
    char resolved[1024];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    FILE *file = actual ? fopen(actual, "rb") : NULL;
    if (!file)
        return false;
    char line[512];
    int version = 0;
    if (!fgets(line, sizeof line, file) || sscanf(line, "core_ui_document %d", &version) != 1 ||
        version < 1 || version > UI_DOCUMENT_VERSION)
    {
        fclose(file);
        return false;
    }
    // Version 1 files carry no surface line; they described a layout with no defined target size.
    UiDocument loaded = {0};
    UiDocumentSetSurface(&loaded, 320, 240, UI_SURFACE_PANEL, "");
    while (fgets(line, sizeof line, file))
    {
        char *tail = strchr(line, '\t');
        if (!tail)
            continue;
        *tail++ = '\0';
        tail[strcspn(tail, "\r\n")] = '\0';
        int width = 0;
        int height = 0;
        int style = 0;
        if (sscanf(line, "surface %d %d %d", &width, &height, &style) == 3)
        {
            UiDocumentSetSurface(&loaded, width, height, (UiSurfaceStyle)style, tail);
            continue;
        }
        int type = 0;
        int checked = 0;
        UiRect rect = {0};
        float value = 0;
        unsigned anchors = 0; // replaced by the type's default when the file predates anchors
        int minWidth = 0;
        int minHeight = 0;
        int flow = UI_FLOW_AUTO;
        int maxWidth = 0;
        int maxHeight = 0;
        int horizontal = 0;
        int vertical = 0;
        // Layouts written before anchors existed take the same default as a new element.
        int fields = sscanf(line, "%d %d %d %d %d %f %d %u %d %d %d %d %d %d %d", &type, &rect.x, &rect.y,
                            &rect.width, &rect.height, &value, &checked, &anchors, &minWidth,
                            &minHeight, &flow, &maxWidth, &maxHeight, &horizontal, &vertical);
        if ((fields != 7 && fields != 11 && fields != 13 && fields != 15) || type < 0 || type >= UI_ELEMENT_COUNT ||
            rect.width <= 0 || rect.height <= 0)
            continue;
        UiElement *element = UiDocumentAdd(&loaded, (UiElementType)type, rect, tail);
        if (!element)
        {
            UiDocumentFree(&loaded);
            fclose(file);
            return false;
        }
        element->value = value;
        element->checked = checked != 0;
        if (fields >= 11)
        {
            element->anchors = anchors & (UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP |
                                          UI_ANCHOR_BOTTOM);
            element->minWidth = MaxInt(0, minWidth);
            element->minHeight = MaxInt(0, minHeight);
            element->flow = flow >= 0 && flow < UI_FLOW_COUNT ? (UiFlow)flow : DefaultFlow(element->type);
        }
        if (fields >= 13)
        {
            element->maxWidth = MaxInt(0, maxWidth);
            element->maxHeight = MaxInt(0, maxHeight);
        }
        if (fields >= 15)
        {
            element->textAlignment.horizontal = (UiAlign)ClampInt(horizontal, UI_ALIGN_START, UI_ALIGN_END);
            element->textAlignment.vertical = (UiAlign)ClampInt(vertical, UI_ALIGN_START, UI_ALIGN_END);
        }
    }
    fclose(file);
    UiDocumentFree(document);
    *document = loaded;
    return true;
}

static void SurfaceInsets(const UiContext *ui, UiSurfaceStyle style, int *left, int *top)
{
    switch (style)
    {
        case UI_SURFACE_PANEL:
            *left = ui->theme.frameWidth;
            *top = ui->theme.frameWidth;
            return;
        case UI_SURFACE_WINDOW:
            *left = ui->theme.frameWidth;
            *top = ui->theme.frameWidth + ui->theme.titleHeight;
            return;
        default:
            *left = 0;
            *top = 0;
            return;
    }
}

UiRect UiDocumentOuterRectSized(const UiContext *ui, const UiDocument *document, Vector2 origin,
                                int width, int height)
{
    if (!ui || !document)
        return (UiRect){0};
    int left = 0;
    int top = 0;
    SurfaceInsets(ui, document->style, &left, &top);
    return (UiRect){(int)origin.x, (int)origin.y, width + left * 2, height + top + left};
}

UiRect UiDocumentOuterRect(const UiContext *ui, const UiDocument *document, Vector2 origin)
{
    if (!document)
        return (UiRect){0};
    return UiDocumentOuterRectSized(ui, document, origin, document->surfaceWidth,
                                    document->surfaceHeight);
}

UiRect UiDocumentContentRectSized(const UiContext *ui, const UiDocument *document, Vector2 origin,
                                  int width, int height)
{
    if (!ui || !document)
        return (UiRect){0};
    int left = 0;
    int top = 0;
    SurfaceInsets(ui, document->style, &left, &top);
    return (UiRect){(int)origin.x + left, (int)origin.y + top, width, height};
}

UiRect UiDocumentContentRect(const UiContext *ui, const UiDocument *document, Vector2 origin)
{
    if (!document)
        return (UiRect){0};
    return UiDocumentContentRectSized(ui, document, origin, document->surfaceWidth,
                                      document->surfaceHeight);
}

bool UiDocumentDrawElement(UiContext *ui, UiElement *element, Vector2 origin, bool interactive)
{
    if (!ui || !element)
        return false;
    UiRect rect = element->rect;
    rect.x += (int)origin.x;
    rect.y += (int)origin.y;
    switch (element->type)
    {
        case UI_ELEMENT_BUTTON:
            return UiButtonAligned(ui, rect, element->label,
                                   interactive ? UI_BUTTON_DEFAULT : UI_BUTTON_NO_INPUT,
                                   element->textAlignment);
        case UI_ELEMENT_SLIDER:
            return UiSliderEx(ui, rect, &element->value, 0, 1, interactive);
        case UI_ELEMENT_CHECKBOX:
            return UiCheckboxEx(ui, rect, element->label, &element->checked, interactive);
        case UI_ELEMENT_LABEL:
            UiLabelAligned(ui, rect, element->label, element->textAlignment);
            return false;
        case UI_ELEMENT_INDENT:
            UiDrawIndent(ui, rect);
            return false;
        case UI_ELEMENT_WINDOW:
            UiDrawWindow(ui, rect, element->label, NULL, NULL);
            return false;
        default:
            return false;
    }
}

void UiDocumentDrawSurfaceSized(UiContext *ui, const UiDocument *document, Vector2 origin,
                                int width, int height)
{
    if (!ui || !ui->state || !document)
        return;
    UiRect outer = UiDocumentOuterRectSized(ui, document, origin, width, height);
    if (document->style == UI_SURFACE_PANEL)
        UiDrawFrame(ui, outer);
    else if (document->style == UI_SURFACE_WINDOW)
    {
        UiDrawFrame(ui, outer);
        UiDrawTitleBar(ui,
                       (UiRect){outer.x + ui->theme.frameWidth, outer.y + ui->theme.frameWidth,
                                outer.width - ui->theme.frameWidth * 2, ui->theme.titleHeight},
                       document->title);
    }
}

void UiDocumentDrawSurface(UiContext *ui, const UiDocument *document, Vector2 origin)
{
    if (!document)
        return;
    UiDocumentDrawSurfaceSized(ui, document, origin, document->surfaceWidth,
                               document->surfaceHeight);
}

static bool DrawElementsAt(UiContext *ui, UiDocument *document, UiRect content, const UiRect *rects,
                           bool interactive)
{
    bool changed = false;
    UiClipState previous = UiPushClip(ui, content);
    for (size_t i = 0; i < document->count; i++)
    {
        UiElement *element = &document->elements[i];
        UiRect placed = rects ? rects[i] : element->rect;
        placed.x += content.x;
        placed.y += content.y;
        // A button already sharing a placed indent keeps its own bevel but adds no second well.
        if (element->type == UI_ELEMENT_BUTTON && UiDocumentInsideIndent(document, i))
        {
            UiButtonFlags flags = interactive ? UI_BUTTON_BARE
                                              : (UiButtonFlags)(UI_BUTTON_BARE | UI_BUTTON_NO_INPUT);
            changed |= UiButtonAligned(ui, placed, element->label, flags, element->textAlignment);
            continue;
        }
        UiRect authored = element->rect;
        element->rect = (UiRect){placed.x - content.x, placed.y - content.y, placed.width,
                                 placed.height};
        changed |= UiDocumentDrawElement(ui, element, (Vector2){(float)content.x, (float)content.y},
                                         interactive);
        element->rect = authored;
    }
    UiRestoreClip(ui, previous);
    return changed;
}

bool UiDocumentDrawElements(UiContext *ui, UiDocument *document, Vector2 origin, bool interactive)
{
    if (!ui || !ui->state || !document)
        return false;
    UiRect content = UiDocumentContentRect(ui, document, origin);
    if (content.width <= 0 || content.height <= 0)
        return false;
    return DrawElementsAt(ui, document, content, NULL, interactive);
}

bool UiDocumentDraw(UiContext *ui, UiDocument *document, Vector2 origin, bool interactive)
{
    UiDocumentDrawSurface(ui, document, origin);
    return UiDocumentDrawElements(ui, document, origin, interactive);
}

bool UiDocumentDrawSized(UiContext *ui, UiDocument *document, Vector2 origin, int width, int height,
                         bool interactive)
{
    if (!ui || !ui->state || !document)
        return false;
    const UiRect *rects = UiDocumentResolve(ui, document, width, height);
    UiDocumentDrawSurfaceSized(ui, document, origin, width, height);
    UiRect content = UiDocumentContentRectSized(ui, document, origin, width, height);
    if (content.width <= 0 || content.height <= 0)
        return false;
    return DrawElementsAt(ui, document, content, rects, interactive);
}
