/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_DOCUMENT_H
#define CORE_UI_DOCUMENT_H

#include "ui.h"

#include <stddef.h>

#define UI_ELEMENT_LABEL_CAPACITY 64
#define UI_DOCUMENT_TITLE_CAPACITY 64

typedef enum UiElementType
{
    UI_ELEMENT_BUTTON,
    UI_ELEMENT_SLIDER,
    UI_ELEMENT_CHECKBOX,
    UI_ELEMENT_LABEL,
    UI_ELEMENT_INDENT,
    UI_ELEMENT_WINDOW,
    UI_ELEMENT_COUNT
} UiElementType;

// How the layout is framed where it is used. Element coordinates are always content-relative.
typedef enum UiSurfaceStyle
{
    UI_SURFACE_SCREEN, // no chrome: drawn straight onto whatever is behind it
    UI_SURFACE_PANEL,  // raised face with a frame
    UI_SURFACE_WINDOW, // frame plus a title bar
    UI_SURFACE_STYLE_COUNT
} UiSurfaceStyle;

// Which edges of the containing region an element keeps its distance from when that region grows.
// Both edges of an axis pinned means the element stretches; neither means it floats with the middle.
typedef enum UiAnchor
{
    UI_ANCHOR_LEFT = 1 << 0,
    UI_ANCHOR_RIGHT = 1 << 1,
    UI_ANCHOR_TOP = 1 << 2,
    UI_ANCHOR_BOTTOM = 1 << 3
} UiAnchor;

// How a container divides itself among its children. Dividing overrides their anchors and leaves
// them flush, because the contents of an indent have no padding and no gaps: they share it.
typedef enum UiFlow
{
    UI_FLOW_AUTO,   // divide along whichever axis the contents were laid out in
    UI_FLOW_ROW,    // divide left to right
    UI_FLOW_COLUMN, // divide top to bottom
    UI_FLOW_FREE,   // leave the contents exactly where they were placed
    UI_FLOW_COUNT
} UiFlow;

typedef struct UiElement
{
    UiElementType type;
    UiRect rect; // as authored, against the authored surface size
    char label[UI_ELEMENT_LABEL_CAPACITY];
    float value;
    bool checked;
    unsigned anchors;
    int minWidth;
    int minHeight;
    int maxWidth;  // 0: no limit
    int maxHeight; // 0: no limit
    UiFlow flow; // containers only
    UiTextAlignment textAlignment; // buttons and labels
} UiElement;

typedef struct UiDocument
{
    UiElement *elements;
    size_t count;
    size_t capacity;
    int surfaceWidth; // authored content size, excluding chrome
    int surfaceHeight;
    UiSurfaceStyle style;
    char title[UI_DOCUMENT_TITLE_CAPACITY];
    UiRect *resolved; // scratch: where the elements land at the last resolved size
    size_t resolvedCapacity;
    int resolvedWidth;
    int resolvedHeight;
    bool resolvedValid;
} UiDocument;

// Releases any existing contents, then applies the surface description.
void UiDocumentInit(UiDocument *document, int surfaceWidth, int surfaceHeight, UiSurfaceStyle style,
                    const char *title);
void UiDocumentSetSurface(UiDocument *document, int surfaceWidth, int surfaceHeight,
                          UiSurfaceStyle style, const char *title);
void UiDocumentFree(UiDocument *document);
bool UiDocumentCopy(UiDocument *destination, const UiDocument *source);

UiElement *UiDocumentAdd(UiDocument *document, UiElementType type, UiRect rect, const char *label);
bool UiDocumentRemove(UiDocument *document, size_t index);
bool UiDocumentSwap(UiDocument *document, size_t a, size_t b);

bool UiDocumentSave(const UiDocument *document, const char *path);
bool UiDocumentLoad(UiDocument *document, const char *path);

// origin is the top-left of the whole surface, chrome included.
UiRect UiDocumentOuterRect(const UiContext *ui, const UiDocument *document, Vector2 origin);
UiRect UiDocumentContentRect(const UiContext *ui, const UiDocument *document, Vector2 origin);
UiRect UiDocumentOuterRectSized(const UiContext *ui, const UiDocument *document, Vector2 origin,
                                int width, int height);
UiRect UiDocumentContentRectSized(const UiContext *ui, const UiDocument *document, Vector2 origin,
                                  int width, int height);

// The region a container hands to its contents: an indent's bezel, a window's frame and title bar.
UiRect UiDocumentContainerContent(const UiContext *ui, const UiElement *element, UiRect rect);
// Innermost placed container enclosing the element, or -1 when it sits on the surface itself.
int UiDocumentContainerOf(const UiDocument *document, size_t index);
// True when a placed indent already encloses this element, so it must not add a second one.
bool UiDocumentInsideIndent(const UiDocument *document, size_t index);

// Places every element for a content size other than the authored one. Authored rects are never
// modified; the returned array is owned by the document and valid until the next resolve.
const UiRect *UiDocumentResolve(UiContext *ui, UiDocument *document, int width, int height);
// The smallest content size that keeps every element at or above its minimum. Conservative.
void UiDocumentMinimumSize(const UiContext *ui, const UiDocument *document, int *width,
                           int *height);

bool UiDocumentDrawElement(UiContext *ui, UiElement *element, Vector2 origin, bool interactive);
// Chrome and elements are separable so an editor can draw its own guides between the two.
void UiDocumentDrawSurface(UiContext *ui, const UiDocument *document, Vector2 origin);
void UiDocumentDrawSurfaceSized(UiContext *ui, const UiDocument *document, Vector2 origin, int width,
                                int height);
bool UiDocumentDrawElements(UiContext *ui, UiDocument *document, Vector2 origin, bool interactive);
bool UiDocumentDraw(UiContext *ui, UiDocument *document, Vector2 origin, bool interactive);
// Draws the layout resolved to the given content size.
bool UiDocumentDrawSized(UiContext *ui, UiDocument *document, Vector2 origin, int width, int height,
                         bool interactive);

#endif
