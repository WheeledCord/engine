/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_document.h"

#include "file.h"
#include "ui_containers.h"
#include "ui_internal.h"
#include "ui_layout.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_DOCUMENT_VERSION 6
#define UI_ELEMENT_MINIMUM 8
#define UI_SURFACE_MINIMUM 16
#define UI_SURFACE_MAXIMUM 8192

// Identities are unique across every document in the program; copies keep theirs on purpose, so an
// undo snapshot still names the same elements.
static unsigned nextElementId = 1;

static int ClampInt(int value, int minimum, int maximum)
{
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int MaxInt(int a, int b) { return a > b ? a : b; }

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

// A name is one token in the file, so anything but letters, digits, '_', '.' and '-' becomes '_'.
static void CopyName(char *destination, size_t capacity, const char *source)
{
    snprintf(destination, capacity, "%s", source ? source : "");
    for (char *c = destination; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
              *c == '_' || *c == '.' || *c == '-'))
            *c = '_';
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
    free(document->work);
    *document = (UiDocument){0};
    document->activated = -1;
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
    // Scratch belongs to the document it was built for.
    destination->resolved = NULL;
    destination->resolvedCapacity = 0;
    destination->resolvedValid = false;
    destination->work = NULL;
    destination->workCapacity = 0;
    return true;
}

static bool Encloses(UiRect outer, UiRect inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

static bool SameRect(UiRect a, UiRect b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
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

// The innermost container that geometrically holds an element. Two identical rectangles are
// ordered by position in the document, so they can never each claim the other.
static int EnclosingContainer(const UiDocument *document, size_t index, size_t limit)
{
    UiRect rect = document->elements[index].rect;
    int best = -1;
    long smallest = 0;
    for (size_t i = 0; i < limit; i++)
    {
        const UiElement *candidate = &document->elements[i];
        if (i == index || !IsContainer(candidate->type) || !Encloses(candidate->rect, rect) ||
            (SameRect(candidate->rect, rect) && i > index))
            continue;
        long area = (long)candidate->rect.width * candidate->rect.height;
        if (best >= 0 && area >= smallest)
            continue;
        best = (int)i;
        smallest = area;
    }
    return best;
}

int UiDocumentContainerOf(const UiDocument *document, size_t index)
{
    if (!document || index >= document->count)
        return -1;
    return document->elements[index].parent;
}

bool UiDocumentInsideIndent(const UiDocument *document, size_t index)
{
    int container = UiDocumentContainerOf(document, index);
    return container >= 0 && document->elements[container].type == UI_ELEMENT_INDENT;
}

bool UiDocumentIsAncestor(const UiDocument *document, int ancestor, size_t index)
{
    if (!document || ancestor < 0 || index >= document->count)
        return false;
    int step = document->elements[index].parent;
    for (size_t guard = 0; step >= 0 && guard < document->count; guard++)
    {
        if (step == ancestor)
            return true;
        step = document->elements[step].parent;
    }
    return false;
}

bool UiDocumentSetParent(UiDocument *document, size_t index, int parent)
{
    if (!document || index >= document->count || parent >= (int)document->count || parent == (int)index)
        return false;
    if (parent >= 0 && (!IsContainer(document->elements[parent].type) ||
                        UiDocumentIsAncestor(document, (int)index, (size_t)parent)))
        return false;
    document->elements[index].parent = parent < 0 ? -1 : parent;
    document->resolvedValid = false;
    return true;
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
    size_t index = document->count++;
    UiElement *element = &document->elements[index];
    *element = (UiElement){type, rect, "", 0.5f, false, DefaultAnchors(type), 0, 0, 0, 0,
                           DefaultFlow(type),
                           {type == UI_ELEMENT_BUTTON ? UI_ALIGN_CENTER : UI_ALIGN_START,
                            UI_ALIGN_CENTER},
                           -1, "", nextElementId++};
    element->parent = EnclosingContainer(document, index, index);
    document->resolvedValid = false;
    CopySingleLine(element->label, sizeof element->label, label ? label : DefaultLabel(type));
    return element;
}

// Deletes the marked elements in one pass and renumbers every parent link that survives.
static void RemoveMarked(UiDocument *document, const bool *doomed)
{
    size_t count = document->count;
    int *moved = malloc(count * sizeof(*moved));
    if (!moved)
        return;
    size_t kept = 0;
    for (size_t i = 0; i < count; i++)
        moved[i] = doomed[i] ? -1 : (int)kept++;
    // An orphan climbs to the nearest ancestor that is staying.
    for (size_t i = 0; i < count; i++)
    {
        if (doomed[i])
            continue;
        int parent = document->elements[i].parent;
        while (parent >= 0 && doomed[parent])
            parent = document->elements[parent].parent;
        document->elements[i].parent = parent >= 0 ? moved[parent] : -1;
    }
    for (size_t i = 0; i < count; i++)
        if (!doomed[i])
            document->elements[moved[i]] = document->elements[i];
    document->count = kept;
    document->resolvedValid = false;
    free(moved);
}

bool UiDocumentRemove(UiDocument *document, size_t index)
{
    if (!document || index >= document->count)
        return false;
    bool *doomed = calloc(document->count, sizeof(*doomed));
    if (!doomed)
        return false;
    doomed[index] = true;
    RemoveMarked(document, doomed);
    free(doomed);
    return true;
}

size_t UiDocumentRemoveTree(UiDocument *document, size_t index)
{
    if (!document || index >= document->count)
        return 0;
    bool *doomed = calloc(document->count, sizeof(*doomed));
    if (!doomed)
        return 0;
    size_t removed = 0;
    for (size_t i = 0; i < document->count; i++)
        if (i == index || UiDocumentIsAncestor(document, (int)index, i))
        {
            doomed[i] = true;
            removed++;
        }
    RemoveMarked(document, doomed);
    free(doomed);
    return removed;
}

bool UiDocumentSwap(UiDocument *document, size_t a, size_t b)
{
    if (!document || a >= document->count || b >= document->count || a == b)
        return false;
    UiElement held = document->elements[a];
    document->elements[a] = document->elements[b];
    document->elements[b] = held;
    for (size_t i = 0; i < document->count; i++)
    {
        int *parent = &document->elements[i].parent;
        if (*parent == (int)a)
            *parent = (int)b;
        else if (*parent == (int)b)
            *parent = (int)a;
    }
    document->resolvedValid = false;
    return true;
}

UiElement *UiDocumentFind(UiDocument *document, const char *name)
{
    if (!document || !name || !*name)
        return NULL;
    for (size_t i = 0; i < document->count; i++)
        if (!strcmp(document->elements[i].name, name))
            return &document->elements[i];
    return NULL;
}

const UiElement *UiDocumentActivated(const UiDocument *document)
{
    if (!document || document->activated < 0 || document->activated >= (int)document->count)
        return NULL;
    return &document->elements[document->activated];
}

void UiElementMinimumSize(const UiContext *ui, UiElementType type, const char *label, int *width,
                          int *height)
{
    if (!ui || !width || !height)
        return;
    const UiTheme *theme = &ui->theme;
    int text = UiTextWidth((UiContext *)ui, label ? label : "");
    switch (type)
    {
        case UI_ELEMENT_LABEL:
            *width = text;
            *height = theme->fontSize;
            return;
        case UI_ELEMENT_CHECKBOX:
            *width = theme->checkboxSize + theme->padding + text;
            *height = theme->itemHeight;
            return;
        case UI_ELEMENT_SLIDER:
            *width = theme->sliderKnobWidth * 3;
            *height = theme->itemHeight;
            return;
        case UI_ELEMENT_INDENT:
            *width = theme->indentWidth * 2 + theme->itemHeight;
            *height = theme->indentWidth * 2 + theme->itemHeight;
            return;
        case UI_ELEMENT_WINDOW:
            *width = theme->frameWidth * 2 + theme->padding * 2 + text;
            *height = theme->frameWidth * 2 + theme->titleHeight + theme->itemHeight;
            return;
        default: // a button: its label, padding, and the bevels of both itself and its indent
            *width = text + theme->padding * 2 + (theme->indentWidth + theme->outsetWidth) * 2;
            *height = theme->itemHeight;
            return;
    }
}

// ---- The containment tree ------------------------------------------------------------------------
// Children are listed in document order, which is also their drawing order. order[] visits every
// container before its contents. The root's list lives at index count.
typedef struct Tree
{
    int *first; // count + 1 entries
    int *next;
    int *order;
    int *needWidth; // what each element needs of itself, contents included
    int *needHeight;
    int *spanWidth; // what each element needs of its container's region, margins included
    int *spanHeight;
    int count;
} Tree;

static bool BuildTree(UiDocument *document, Tree *tree)
{
    size_t n = document->count;
    size_t needed = n * 8 + 1;
    if (document->workCapacity < needed)
    {
        int *grown = realloc(document->work, needed * sizeof(*grown));
        if (!grown)
            return false;
        document->work = grown;
        document->workCapacity = needed;
    }
    int *w = document->work;
    *tree = (Tree){w, w + n + 1, w + n * 2 + 1, w + n * 3 + 1, w + n * 4 + 1, w + n * 5 + 1,
                   w + n * 6 + 1, (int)n};
    for (size_t i = 0; i <= n; i++)
        tree->first[i] = -1;
    for (size_t i = n; i-- > 0;)
    {
        int parent = document->elements[i].parent;
        int slot = parent >= 0 && parent < (int)n ? parent : (int)n;
        tree->next[i] = tree->first[slot];
        tree->first[slot] = (int)i;
    }
    int head = 0;
    int tail = 0;
    for (int child = tree->first[n]; child >= 0; child = tree->next[child])
        tree->order[tail++] = child;
    while (head < tail)
        for (int child = tree->first[tree->order[head++]]; child >= 0; child = tree->next[child])
            tree->order[tail++] = child;
    return tail == (int)n; // anything unreached sits in a parent cycle
}

// AUTO reads the axis off the contents: side by side divides left to right, a stack divides top
// to bottom. The spread of the centres decides, so the order they were added in does not matter.
static UiFlow EffectiveFlow(const UiDocument *document, const Tree *tree, int container)
{
    if (container < 0)
        return UI_FLOW_FREE;
    UiFlow flow = document->elements[container].flow;
    if (flow != UI_FLOW_AUTO)
        return flow;
    int left = 0, right = 0, top = 0, bottom = 0;
    bool any = false;
    for (int i = tree->first[container]; i >= 0; i = tree->next[i])
    {
        UiRect rect = document->elements[i].rect;
        int x = rect.x + rect.width / 2;
        int y = rect.y + rect.height / 2;
        left = any && left < x ? left : x;
        right = any && right > x ? right : x;
        top = any && top < y ? top : y;
        bottom = any && bottom > y ? bottom : y;
        any = true;
    }
    return bottom - top > right - left ? UI_FLOW_COLUMN : UI_FLOW_ROW;
}

// One axis of the anchor rule. Offsets are relative to the containing region.
static bool StretchesOn(const UiElement *element, bool horizontal)
{
    unsigned low = horizontal ? UI_ANCHOR_LEFT : UI_ANCHOR_TOP;
    unsigned high = horizontal ? UI_ANCHOR_RIGHT : UI_ANCHOR_BOTTOM;
    return (element->anchors & low) && (element->anchors & high);
}

static int ConstrainSize(int size, int minimum, int maximum)
{
    size = MaxInt(size, minimum);
    // An explicit maximum wins if the authored minimum is larger.
    return maximum > 0 && size > maximum ? maximum : size;
}

// Siblings that stretch on the same axis divide the change between them, in proportion to the size
// they were drawn at, which leaves every gap between them exactly as it was. My queue along an axis
// is the siblings that follow me on it and share my band across it: in a grid, my row when dividing
// width, my column when dividing height.
#define UI_QUEUE_CAPACITY 64

static int BuildQueue(const UiDocument *document, const Tree *tree, size_t index, bool horizontal,
                      int *members)
{
    const UiElement *element = &document->elements[index];
    int low = horizontal ? element->rect.x : element->rect.y;
    int mine = horizontal ? element->rect.width : element->rect.height;
    int across = horizontal ? element->rect.y : element->rect.x;
    int acrossSize = horizontal ? element->rect.height : element->rect.width;
    int slot = element->parent >= 0 ? element->parent : tree->count;
    int queue = 0;
    for (int i = tree->first[slot]; i >= 0 && queue < UI_QUEUE_CAPACITY; i = tree->next[i])
    {
        const UiElement *other = &document->elements[i];
        if (!StretchesOn(other, horizontal))
            continue;
        int size = horizontal ? other->rect.width : other->rect.height;
        int otherLow = horizontal ? other->rect.x : other->rect.y;
        int otherAcross = horizontal ? other->rect.y : other->rect.x;
        int otherAcrossSize = horizontal ? other->rect.height : other->rect.width;
        if (i != (int)index && otherLow < low + mine && low < otherLow + size)
            continue; // overlaps me along the axis: stacked, not queued
        if (i != (int)index &&
            !(otherAcross < across + acrossSize && across < otherAcross + otherAcrossSize))
            continue; // not in my band
        members[queue++] = i;
    }
    // In order along the axis, so gaps and shifts read left to right.
    for (int a = 1; a < queue; a++)
        for (int b = a; b > 0; b--)
        {
            const UiRect *x = &document->elements[members[b - 1]].rect;
            const UiRect *y = &document->elements[members[b]].rect;
            if ((horizontal ? x->x : x->y) <= (horizontal ? y->x : y->y))
                break;
            int held = members[b - 1];
            members[b - 1] = members[b];
            members[b] = held;
        }
    return queue;
}

// Divides the change across the queue. A member that would pass its minimum or its maximum stops
// exactly there and hands the rest to the others, so sizes and positions always add up: the queue
// never overlaps itself, and when even the minimums do not fit it runs past the far edge instead.
static bool ShareStretch(const UiDocument *document, const Tree *tree, size_t index, bool horizontal,
                         int delta, int *shift, int *grow)
{
    if (!StretchesOn(&document->elements[index], horizontal))
        return false;
    int members[UI_QUEUE_CAPACITY];
    int queue = BuildQueue(document, tree, index, horizontal, members);
    if (queue < 2)
        return false;
    int sizes[UI_QUEUE_CAPACITY];
    int floors[UI_QUEUE_CAPACITY];
    int limits[UI_QUEUE_CAPACITY];
    int growth[UI_QUEUE_CAPACITY] = {0};
    bool settled[UI_QUEUE_CAPACITY] = {false};
    int me = -1;
    for (int k = 0; k < queue; k++)
    {
        const UiElement *member = &document->elements[members[k]];
        sizes[k] = horizontal ? member->rect.width : member->rect.height;
        floors[k] = horizontal ? tree->needWidth[members[k]] : tree->needHeight[members[k]];
        limits[k] = horizontal ? member->maxWidth : member->maxHeight;
        if (members[k] == (int)index)
            me = k;
    }
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
            int through = (int)((long long)remaining * before / total);
            growth[k] = through - taken;
            taken = through;
            int size = sizes[k] + growth[k];
            if (size < floors[k] || (limits[k] > 0 && size > limits[k]))
                over = k;
        }
        if (over < 0)
            break;
        growth[over] = ConstrainSize(sizes[over] + growth[over], floors[over], limits[over]) - sizes[over];
        settled[over] = true;
        remaining -= growth[over];
    }
    *grow = growth[me];
    *shift = 0;
    for (int k = 0; k < me; k++)
        *shift += growth[k];
    return true;
}

// What a queue needs along its axis: the outer margins, every member at its floor, and the gaps.
static int QueueSpan(const UiDocument *document, const Tree *tree, const int *members, int queue,
                     bool horizontal, UiRect area)
{
    const UiRect *first = &document->elements[members[0]].rect;
    const UiRect *last = &document->elements[members[queue - 1]].rect;
    int span = horizontal ? first->x - area.x : first->y - area.y;
    span += horizontal ? area.x + area.width - (last->x + last->width)
                       : area.y + area.height - (last->y + last->height);
    for (int k = 0; k < queue; k++)
    {
        const UiRect *rect = &document->elements[members[k]].rect;
        span += horizontal ? tree->needWidth[members[k]] : tree->needHeight[members[k]];
        if (k + 1 < queue)
        {
            const UiRect *next = &document->elements[members[k + 1]].rect;
            span += horizontal ? next->x - (rect->x + rect->width) : next->y - (rect->y + rect->height);
        }
    }
    return MaxInt(0, span);
}

static UiRect AuthoredArea(const UiContext *ui, const UiDocument *document, int container)
{
    if (container < 0)
        return (UiRect){0, 0, document->surfaceWidth, document->surfaceHeight};
    const UiElement *element = &document->elements[container];
    return UiDocumentContainerContent(ui, element, element->rect);
}

// What one child needs of its container's region, given how it is pinned.
static void ChildSpan(const UiContext *ui, const UiDocument *document, const Tree *tree, size_t index,
                      bool shared)
{
    const UiElement *element = &document->elements[index];
    int needWidth = tree->needWidth[index];
    int needHeight = tree->needHeight[index];
    if (shared)
    {
        // Along a flow, a child reserves only its own minimum.
        tree->spanWidth[index] = needWidth;
        tree->spanHeight[index] = needHeight;
        return;
    }
    UiRect area = AuthoredArea(ui, document, element->parent);
    for (int axis = 0; axis < 2; axis++)
    {
        bool horizontal = axis == 0;
        int need = horizontal ? needWidth : needHeight;
        int low = horizontal ? element->rect.x - area.x : element->rect.y - area.y;
        int size = horizontal ? element->rect.width : element->rect.height;
        int high = (horizontal ? area.width : area.height) - low - size;
        bool pinLow = element->anchors & (horizontal ? UI_ANCHOR_LEFT : UI_ANCHOR_TOP);
        bool pinHigh = element->anchors & (horizontal ? UI_ANCHOR_RIGHT : UI_ANCHOR_BOTTOM);
        int limit = horizontal ? element->maxWidth : element->maxHeight;
        int span;
        int members[UI_QUEUE_CAPACITY];
        int queue = pinLow && pinHigh ? BuildQueue(document, tree, index, horizontal, members) : 0;
        if (queue >= 2)
            span = QueueSpan(document, tree, members, queue, horizontal, area);
        else
        {
            // Anything not sized by its container keeps the room it was drawn at, or the surface
            // would shrink until it clipped a control that never gets any smaller.
            if (!(pinLow && pinHigh))
                need = ConstrainSize(MaxInt(need, size), 0, limit);
            span = need + (pinLow ? low : 0) + (pinHigh ? MaxInt(0, high) : 0);
        }
        if (horizontal)
            tree->spanWidth[index] = span;
        else
            tree->spanHeight[index] = span;
    }
}

// Children before parents: each container's need is folded up from what its contents need.
static void MeasureTree(const UiContext *ui, const UiDocument *document, const Tree *tree)
{
    for (int k = tree->count; k-- > 0;)
    {
        int index = tree->order[k];
        const UiElement *element = &document->elements[index];
        int width = MaxInt(element->minWidth, UI_ELEMENT_MINIMUM);
        int height = MaxInt(element->minHeight, UI_ELEMENT_MINIMUM);
        if (IsContainer(element->type) && tree->first[index] >= 0)
        {
            UiFlow flow = EffectiveFlow(document, tree, index);
            int insideWidth = 0;
            int insideHeight = 0;
            for (int child = tree->first[index]; child >= 0; child = tree->next[child])
            {
                ChildSpan(ui, document, tree, (size_t)child, flow != UI_FLOW_FREE);
                insideWidth = flow == UI_FLOW_ROW ? insideWidth + tree->spanWidth[child]
                                                  : MaxInt(insideWidth, tree->spanWidth[child]);
                insideHeight = flow == UI_FLOW_COLUMN ? insideHeight + tree->spanHeight[child]
                                                      : MaxInt(insideHeight, tree->spanHeight[child]);
            }
            UiRect chrome = UiDocumentContainerContent(ui, element, element->rect);
            width = MaxInt(width, insideWidth + element->rect.width - chrome.width);
            height = MaxInt(height, insideHeight + element->rect.height - chrome.height);
        }
        tree->needWidth[index] = ConstrainSize(width, 0, element->maxWidth);
        tree->needHeight[index] = ConstrainSize(height, 0, element->maxHeight);
    }
    for (int child = tree->first[tree->count]; child >= 0; child = tree->next[child])
        ChildSpan(ui, document, tree, (size_t)child, false);
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
    *size = ConstrainSize(*size, minimum, limit);
}

// Equal shares between each child's limits. Remaining space goes to the unconstrained siblings; if
// every child is at its maximum, the surplus stays at the end of the region.
static void FlowCells(const UiDocument *document, const Tree *tree, int container, UiRect area,
                      bool horizontal, UiRect *out)
{
    int extent = MaxInt(0, horizontal ? area.width : area.height);
    int low = 0;
    int high = extent;
    while (low < high)
    {
        int level = low + (high - low + 1) / 2;
        long long used = 0;
        for (int i = tree->first[container]; i >= 0; i = tree->next[i])
        {
            const UiElement *child = &document->elements[i];
            used += ConstrainSize(level, horizontal ? tree->spanWidth[i] : tree->spanHeight[i],
                                  horizontal ? child->maxWidth : child->maxHeight);
        }
        if (used <= extent)
            low = level;
        else
            high = level - 1;
    }
    int used = 0;
    int flexible = 0;
    for (int i = tree->first[container]; i >= 0; i = tree->next[i])
    {
        const UiElement *child = &document->elements[i];
        int minimum = horizontal ? tree->spanWidth[i] : tree->spanHeight[i];
        int maximum = horizontal ? child->maxWidth : child->maxHeight;
        used += ConstrainSize(low, minimum, maximum);
        flexible += minimum <= low && (maximum <= 0 || low < maximum);
    }
    int remainder = MaxInt(0, extent - used);
    int ordinal = 0;
    int offset = 0;
    for (int i = tree->first[container]; i >= 0; i = tree->next[i])
    {
        const UiElement *child = &document->elements[i];
        int minimum = horizontal ? tree->spanWidth[i] : tree->spanHeight[i];
        int maximum = horizontal ? child->maxWidth : child->maxHeight;
        int size = ConstrainSize(low, minimum, maximum);
        if (minimum <= low && (maximum <= 0 || low < maximum) && flexible)
        {
            // Prefix rounding accounts for every pixel, even when shares are unequal.
            size += remainder * (ordinal + 1) / flexible - remainder * ordinal / flexible;
            ordinal++;
        }
        out[i] = horizontal ? (UiRect){area.x + offset, area.y, size,
                                       ConstrainSize(area.height, tree->spanHeight[i], child->maxHeight)}
                            : (UiRect){area.x, area.y + offset,
                                       ConstrainSize(area.width, tree->spanWidth[i], child->maxWidth), size};
        offset += size;
    }
}

static void ResolveChildren(UiDocument *document, const Tree *tree, int container,
                            UiRect authored, UiRect area)
{
    UiRect *out = document->resolved;
    int slot = container >= 0 ? container : tree->count;
    UiFlow flow = EffectiveFlow(document, tree, container);
    if (flow != UI_FLOW_FREE && tree->first[slot] >= 0)
    {
        // Flow divides the region flush, so the contents keep meeting bezel to bezel.
        FlowCells(document, tree, container, area, flow == UI_FLOW_ROW, out);
        return;
    }
    for (int i = tree->first[slot]; i >= 0; i = tree->next[i])
    {
        const UiElement *element = &document->elements[i];
        UiRect placed = {0};
        int shift = 0;
        int grow = 0;
        if (ShareStretch(document, tree, (size_t)i, true, area.width - authored.width, &shift, &grow))
        {
            placed.x = element->rect.x - authored.x + shift;
            placed.width = element->rect.width + grow;
        }
        else
            ResolveAxis(element->rect.x - authored.x, element->rect.width, authored.width, area.width,
                        (element->anchors & UI_ANCHOR_LEFT) != 0,
                        (element->anchors & UI_ANCHOR_RIGHT) != 0, tree->needWidth[i],
                        element->maxWidth, &placed.x, &placed.width);
        if (ShareStretch(document, tree, (size_t)i, false, area.height - authored.height, &shift,
                         &grow))
        {
            placed.y = element->rect.y - authored.y + shift;
            placed.height = element->rect.height + grow;
        }
        else
            ResolveAxis(element->rect.y - authored.y, element->rect.height, authored.height,
                        area.height, (element->anchors & UI_ANCHOR_TOP) != 0,
                        (element->anchors & UI_ANCHOR_BOTTOM) != 0, tree->needHeight[i],
                        element->maxHeight, &placed.y, &placed.height);
        placed.x += area.x;
        placed.y += area.y;
        out[i] = placed;
    }
}

const UiRect *UiDocumentResolve(UiContext *ui, UiDocument *document, int width, int height)
{
    if (!ui || !document)
        return NULL;
    if (document->resolvedCapacity < document->count || !document->resolved)
    {
        UiRect *grown = realloc(document->resolved, MaxInt(1, (int)document->count) * sizeof(*grown));
        if (!grown)
            return NULL;
        document->resolved = grown;
        document->resolvedCapacity = document->count;
    }
    Tree tree;
    if (!BuildTree(document, &tree))
        return NULL;
    MeasureTree(ui, document, &tree);
    UiRect authoredSurface = {0, 0, document->surfaceWidth, document->surfaceHeight};
    UiRect resolvedSurface = {0, 0, MaxInt(1, width), MaxInt(1, height)};
    ResolveChildren(document, &tree, -1, authoredSurface, resolvedSurface);
    // Containers land before their contents because order[] lists parents first.
    for (int k = 0; k < tree.count; k++)
    {
        int index = tree.order[k];
        const UiElement *element = &document->elements[index];
        if (!IsContainer(element->type) || tree.first[index] < 0)
            continue;
        ResolveChildren(document, &tree, index,
                        UiDocumentContainerContent(ui, element, element->rect),
                        UiDocumentContainerContent(ui, element, document->resolved[index]));
    }
    document->resolvedWidth = resolvedSurface.width;
    document->resolvedHeight = resolvedSurface.height;
    document->resolvedValid = true;
    return document->resolved;
}

void UiDocumentMinimumSize(const UiContext *ui, const UiDocument *document, int *width, int *height)
{
    int smallestWidth = UI_SURFACE_MINIMUM;
    int smallestHeight = UI_SURFACE_MINIMUM;
    Tree tree;
    // The tree lives in the document's scratch; measuring changes nothing a caller can see.
    UiDocument *scratch = (UiDocument *)document;
    if (ui && document && BuildTree(scratch, &tree))
    {
        MeasureTree(ui, document, &tree);
        for (int i = tree.first[tree.count]; i >= 0; i = tree.next[i])
        {
            smallestWidth = MaxInt(smallestWidth, tree.spanWidth[i]);
            smallestHeight = MaxInt(smallestHeight, tree.spanHeight[i]);
        }
    }
    if (width)
        *width = smallestWidth;
    if (height)
        *height = smallestHeight;
}

// ---- Files ----------------------------------------------------------------------------------------
bool UiDocumentSave(const UiDocument *document, const char *path)
{
    if (!document || !path)
        return false;
    CoreAtomicFile atomic;
    FILE *file = CoreAtomicBegin(&atomic, path);
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
        char name[UI_ELEMENT_NAME_CAPACITY];
        CopySingleLine(label, sizeof label, element->label);
        CopyName(name, sizeof name, element->name);
        fprintf(file, "%d %d %d %d %d %.9g %d %u %d %d %d %d %d %d %d %d %s\t%s\n", element->type,
                element->rect.x, element->rect.y, element->rect.width, element->rect.height,
                element->value, element->checked, element->anchors, element->minWidth,
                element->minHeight, (int)element->flow, element->maxWidth, element->maxHeight,
                (int)element->textAlignment.horizontal, (int)element->textAlignment.vertical,
                element->parent, name[0] ? name : "-", label);
    }
    return CoreAtomicCommit(&atomic, true);
}

static bool LoadFailed(UiDocument *loaded, FILE *file, const char *path, int line, const char *why)
{
    TraceLog(LOG_ERROR, "UI document %s:%d: %s", path, line, why);
    UiDocumentFree(loaded);
    fclose(file);
    return false;
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
    UiDocument loaded = {0};
    if (!fgets(line, sizeof line, file) || sscanf(line, "core_ui_document %d", &version) != 1 ||
        version < 1 || version > UI_DOCUMENT_VERSION)
        return LoadFailed(&loaded, file, path, 1, "not a core UI document of a known version");
    // Version 1 files carry no surface line; they described a layout with no defined target size.
    UiDocumentSetSurface(&loaded, 320, 240, UI_SURFACE_PANEL, "");
    int number = 1;
    // Every line must be understood: a document that loads with pieces missing is worse than one
    // that refuses to load, because the next save would make the loss permanent.
    while (fgets(line, sizeof line, file))
    {
        number++;
        if (!strchr(line, '\n') && !feof(file))
            return LoadFailed(&loaded, file, path, number, "line too long");
        if (strspn(line, " \r\n") == strlen(line))
            continue;
        char *tail = strchr(line, '\t');
        if (!tail)
            return LoadFailed(&loaded, file, path, number, "missing the tab before the label");
        *tail++ = '\0';
        tail[strcspn(tail, "\r\n")] = '\0';
        int width = 0;
        int height = 0;
        int style = 0;
        if (!strncmp(line, "surface", 7))
        {
            if (sscanf(line, "surface %d %d %d", &width, &height, &style) != 3)
                return LoadFailed(&loaded, file, path, number, "malformed surface line");
            UiDocumentSetSurface(&loaded, width, height, (UiSurfaceStyle)style, tail);
            continue;
        }
        int type = 0, checked = 0, minWidth = 0, minHeight = 0, flow = UI_FLOW_AUTO;
        int maxWidth = 0, maxHeight = 0, horizontal = 0, vertical = 0, parent = -1, used = 0;
        UiRect rect = {0};
        float value = 0;
        unsigned anchors = 0;
        char name[UI_ELEMENT_NAME_CAPACITY] = "";
        int fields = sscanf(line, "%d %d %d %d %d %f %d %u %d %d %d %d %d %d %d %d %31s %n", &type,
                            &rect.x, &rect.y, &rect.width, &rect.height, &value, &checked, &anchors,
                            &minWidth, &minHeight, &flow, &maxWidth, &maxHeight, &horizontal,
                            &vertical, &parent, name, &used);
        // Each version adds fields; anything between the known counts is a damaged line.
        static const int expected[] = {0, 7, 7, 11, 11, 15, 17};
        if (fields != expected[version] || (version >= 6 && line[used] != '\0') || type < 0 ||
            type >= UI_ELEMENT_COUNT || rect.width <= 0 || rect.height <= 0)
            return LoadFailed(&loaded, file, path, number, "malformed element");
        UiElement *element = UiDocumentAdd(&loaded, (UiElementType)type, rect, tail);
        if (!element)
            return LoadFailed(&loaded, file, path, number, "out of memory");
        element->value = value;
        element->checked = checked != 0;
        element->parent = version >= 6 ? parent : -1;
        if (version >= 6 && strcmp(name, "-"))
            CopyName(element->name, sizeof element->name, name);
        if (fields >= 11)
        {
            element->anchors = anchors & (UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP |
                                          UI_ANCHOR_BOTTOM);
            element->minWidth = MaxInt(0, minWidth);
            element->minHeight = MaxInt(0, minHeight);
            element->flow = flow >= 0 && flow < UI_FLOW_COUNT ? (UiFlow)flow : DefaultFlow(element->type);
        }
        if (fields >= 15)
        {
            element->maxWidth = MaxInt(0, maxWidth);
            element->maxHeight = MaxInt(0, maxHeight);
            element->textAlignment.horizontal = (UiAlign)ClampInt(horizontal, UI_ALIGN_START, UI_ALIGN_END);
            element->textAlignment.vertical = (UiAlign)ClampInt(vertical, UI_ALIGN_START, UI_ALIGN_END);
        }
    }
    if (ferror(file))
        return LoadFailed(&loaded, file, path, number, "read error");
    if (version < 6)
    {
        // Older files kept no parents; they were read off the geometry, which happens once, here.
        for (size_t i = 0; i < loaded.count; i++)
            loaded.elements[i].parent = EnclosingContainer(&loaded, i, loaded.count);
    }
    for (size_t i = 0; i < loaded.count; i++)
    {
        int parent = loaded.elements[i].parent;
        if (parent < -1 || parent >= (int)loaded.count || parent == (int)i ||
            (parent >= 0 && !IsContainer(loaded.elements[parent].type)))
            return LoadFailed(&loaded, file, path, number, "element parent is not a container");
    }
    Tree tree;
    if (!BuildTree(&loaded, &tree))
        return LoadFailed(&loaded, file, path, number, "elements contain each other in a loop");
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
    // The element's own identity, so a press survives the label changing or the array moving.
    if (element->id)
        UiNextId(ui, element->id);
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

// One container's contents, clipped to it and drawn over it, then the same for anything inside them.
static bool DrawBranch(UiContext *ui, UiDocument *document, const Tree *tree, int container,
                       UiRect content, const UiRect *rects, bool interactive)
{
    bool changed = false;
    int slot = container >= 0 ? container : tree->count;
    for (int i = tree->first[slot]; i >= 0; i = tree->next[i])
    {
        UiElement *element = &document->elements[i];
        UiRect placed = rects ? rects[i] : element->rect;
        placed.x += content.x;
        placed.y += content.y;
        bool hit = false;
        // A button already sharing a placed indent keeps its own bevel but adds no second well.
        if (element->type == UI_ELEMENT_BUTTON && UiDocumentInsideIndent(document, (size_t)i))
        {
            UiButtonFlags flags = interactive ? UI_BUTTON_BARE
                                              : (UiButtonFlags)(UI_BUTTON_BARE | UI_BUTTON_NO_INPUT);
            if (element->id)
                UiNextId(ui, element->id);
            hit = UiButtonAligned(ui, placed, element->label, flags, element->textAlignment);
        }
        else
        {
            UiRect authored = element->rect;
            element->rect = (UiRect){placed.x - content.x, placed.y - content.y, placed.width,
                                     placed.height};
            hit = UiDocumentDrawElement(ui, element, (Vector2){(float)content.x, (float)content.y},
                                        interactive);
            element->rect = authored;
        }
        if (hit)
        {
            document->activated = i;
            changed = true;
        }
        if (!IsContainer(element->type) || tree->first[i] < 0)
            continue;
        // Contents belong inside their container, so they are clipped to it and drawn after it.
        // Their coordinates stay relative to the surface, so the origin does not change.
        UiClipState previous = UiPushClip(ui, UiDocumentContainerContent(ui, element, placed));
        changed |= DrawBranch(ui, document, tree, i, content, rects, interactive);
        UiRestoreClip(ui, previous);
    }
    return changed;
}

static bool DrawElementsAt(UiContext *ui, UiDocument *document, UiRect content, const UiRect *rects,
                           bool interactive)
{
    Tree tree;
    if (!BuildTree(document, &tree))
        return false;
    document->activated = -1;
    UiClipState previous = UiPushClip(ui, content);
    bool changed = DrawBranch(ui, document, &tree, -1, content, rects, interactive);
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

// The last element in drawing order that covers the point, ignoring anything its container clips.
static int PickBranch(const UiContext *ui, const UiDocument *document, const Tree *tree,
                      int container, UiRect content, UiRect clip, const UiRect *rects, Vector2 point)
{
    int found = -1;
    int slot = container >= 0 ? container : tree->count;
    for (int i = tree->first[slot]; i >= 0; i = tree->next[i])
    {
        const UiElement *element = &document->elements[i];
        UiRect placed = rects ? rects[i] : element->rect;
        placed.x += content.x;
        placed.y += content.y;
        if (UiPointInRect(point, UiIntersectRect(clip, placed)))
            found = i;
        if (!IsContainer(element->type) || tree->first[i] < 0)
            continue;
        UiRect inside = UiDocumentContainerContent(ui, element, placed);
        int deeper = PickBranch(ui, document, tree, i, content, UiIntersectRect(clip, inside), rects,
                                point);
        if (deeper >= 0)
            found = deeper;
    }
    return found;
}

int UiDocumentElementAt(const UiContext *ui, UiDocument *document, const UiRect *rects, Vector2 point)
{
    Tree tree;
    if (!ui || !document || !BuildTree(document, &tree))
        return -1;
    UiRect everything = {-UI_SURFACE_MAXIMUM, -UI_SURFACE_MAXIMUM, UI_SURFACE_MAXIMUM * 2,
                         UI_SURFACE_MAXIMUM * 2};
    return PickBranch(ui, document, &tree, -1, (UiRect){0, 0, 0, 0}, everything, rects, point);
}
