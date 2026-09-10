/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "file.h"
#include "ui_internal.h"
#include "rlgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int MinInt(int a, int b) { return a < b ? a : b; }
static int MaxInt(int a, int b) { return a > b ? a : b; }

static Color RecessedFace(const UiTheme *theme)
{
    return (Color){(unsigned char)(((int)theme->face.r + theme->darkGrey.r) / 2),
                   (unsigned char)(((int)theme->face.g + theme->darkGrey.g) / 2),
                   (unsigned char)(((int)theme->face.b + theme->darkGrey.b) / 2), theme->face.a};
}

static int BorderWidth(UiRect rect, int requested)
{
    int maximum = MinInt(rect.width, rect.height) / 2;
    return requested < maximum ? requested : maximum;
}

static void Fill(UiRect rect, Color color)
{
    if (rect.width > 0 && rect.height > 0)
        DrawRectangle(rect.x, rect.y, rect.width, rect.height, color);
}

static void DrawEdges(UiRect rect, int width, Color topLeft, Color bottomRight)
{
    width = BorderWidth(rect, width);
    for (int i = 0; i < width; i++)
    {
        int w = rect.width - i * 2;
        int h = rect.height - i * 2;
        if (w <= 0 || h <= 0)
            break;
        Fill((UiRect){rect.x + i, rect.y + i, w, 1}, topLeft);
        Fill((UiRect){rect.x + i, rect.y + i, 1, h}, topLeft);
        Fill((UiRect){rect.x + i, rect.y + rect.height - 1 - i, w, 1}, bottomRight);
        Fill((UiRect){rect.x + rect.width - 1 - i, rect.y + i, 1, h}, bottomRight);
    }
}

// Printable ASCII is always baked in, so ordinary text never pays for an atlas rebuild mid-frame.
// The Latin-1 supplement comes with it because the cost is parsing the BDF, not the glyph count:
// 95 glyphs and 191 glyphs both take about 35 ms. Anything else is loaded when text first uses it.
static const int *DefaultCodepoints(int *count)
{
    static int codepoints[95 + 96];
    static int total = 0;
    if (!total)
    {
        for (int c = 32; c <= 126; c++)
            codepoints[total++] = c;
        for (int c = 0xa0; c <= 0xff; c++)
            codepoints[total++] = c;
    }
    *count = total;
    return codepoints;
}

UiTheme UiThemeDefault(void)
{
    UiTheme theme = {0};
    theme.face = (Color){174, 174, 174, 255};
    theme.white = (Color){255, 255, 255, 255};
    theme.darkGrey = (Color){85, 85, 85, 255};
    theme.black = (Color){0, 0, 0, 255};
    theme.fontPath = CORE_UI_DEFAULT_FONT_PATH;
    theme.fontCodepoints = DefaultCodepoints(&theme.fontGlyphCount);
    theme.fontSize = 16;
    theme.textSpacing = 1;
    theme.textOffsetY = -1;
    theme.textShadowOffset = 1;
    theme.frameWidth = 2;
    theme.indentWidth = 1;
    theme.outsetWidth = 1;
    theme.padding = 6;
    theme.containerGap = 3;
    theme.itemHeight = 24;
    theme.titleHeight = 24;
    theme.checkboxSize = 14;
    theme.sliderKnobWidth = 12;
    theme.editorHandleSize = 7;
    theme.menuSlideSpeed = 900.0f;
    return theme;
}

static bool ThemeValid(const UiTheme *theme)
{
    return theme && theme->fontSize > 0 && theme->textSpacing >= 0 && theme->textShadowOffset >= 0 &&
           theme->frameWidth > 0 && theme->indentWidth > 0 && theme->outsetWidth > 0 &&
           theme->padding >= 0 && theme->containerGap >= 0 && theme->itemHeight > 0 &&
           theme->checkboxSize > 0 && theme->sliderKnobWidth > 0 && theme->editorHandleSize > 0 &&
           isfinite(theme->menuSlideSpeed) && theme->menuSlideSpeed > 0;
}

static bool ResolveThemeFont(UiTheme *theme, bool *owned)
{
    *owned = false;
    if (theme->font.texture.id)
    {
        SetTextureFilter(theme->font.texture, TEXTURE_FILTER_POINT);
        return true;
    }
    char fontbuf[1024];
    const char *resolvedFont = theme->fontPath ? CoreResolvePath(theme->fontPath, fontbuf, sizeof fontbuf) : NULL;
    if (!resolvedFont || !FileExists(resolvedFont))
    {
        // Report where it actually looked, which is rarely the path that was asked for.
        TraceLog(LOG_ERROR, "UI: Bitmap font file not found: %s (looked for %s)",
                 theme->fontPath ? theme->fontPath : "(null)",
                 resolvedFont ? resolvedFont : "(path too long)");
        return false;
    }

    // A caller's own list is honoured, but printable ASCII is added to it: a rebuild triggered by
    // ordinary text would be a visible hitch in a game loop.
    int *requested = NULL;
    int requestedCount = theme->fontGlyphCount;
    if (theme->fontCodepoints && requestedCount > 0)
    {
        requested = malloc((size_t)(requestedCount + 95) * sizeof(*requested));
        if (!requested)
            return false;
        memcpy(requested, theme->fontCodepoints, (size_t)requestedCount * sizeof(*requested));
        for (int c = 32; c <= 126; c++)
        {
            bool present = false;
            for (int i = 0; i < requestedCount && !present; i++)
                present = requested[i] == c;
            if (!present)
                requested[requestedCount++] = c;
        }
    }
    Font font = LoadFontEx(resolvedFont, theme->fontSize,
                           requested ? requested : (int *)theme->fontCodepoints, requestedCount);
    free(requested);
    Font fallback = GetFontDefault();
    if (!IsFontValid(font) || font.texture.id == fallback.texture.id)
    {
        TraceLog(LOG_ERROR, "UI: Failed to load bitmap font: %s", theme->fontPath);
        return false;
    }

    // A fixed-cell BDF is baseline-positioned. UI rectangles use the cell's top-left.
    int reference = -1;
    bool fixedCell = true;
    for (int i = 0; fixedCell && i < font.glyphCount; i++)
    {
        const GlyphInfo *glyph = &font.glyphs[i];
        // raylib rebuilds even missing glyph images with calloc(0), which may return non-null.
        // Empty rectangles have no bearing on the font's cell alignment.
        if (glyph->image.width <= 0 || glyph->image.height <= 0)
            continue;
        if (reference < 0)
            reference = i;
        fixedCell = glyph->image.height == font.baseSize &&
                    glyph->offsetX == font.glyphs[reference].offsetX &&
                    glyph->offsetY == font.glyphs[reference].offsetY;
    }
    if (fixedCell && reference >= 0)
        for (int i = 0; i < font.glyphCount; i++)
        {
            font.glyphs[i].offsetX = 0;
            font.glyphs[i].offsetY = 0;
        }

    SetTextureFilter(font.texture, TEXTURE_FILTER_POINT);
    theme->font = font;
    *owned = true;
    return true;
}

static void EnsureTextGlyphs(UiContext *ui, const char *text)
{
    UiState *state = ui->state;
    if (!state->ownsFont)
        return; // A borrowed atlas remains the project's responsibility.
    int previousCount = state->fontGlyphCount;
    for (const char *c = text; *c;)
    {
        int bytes = 0;
        int codepoint = GetCodepointNext(c, &bytes);
        c += bytes;
        Font font = ui->theme.font;
        if (codepoint < 32 || (codepoint >= 127 && codepoint < 160) ||
            font.glyphs[GetGlyphIndex(font, codepoint)].value == codepoint)
            continue;
        bool requested = false;
        for (int i = 0; i < state->fontGlyphCount; i++)
            if (state->fontCodepoints[i] == codepoint)
                requested = true;
        if (requested)
            continue;
        int count = state->fontCodepoints ? state->fontGlyphCount : font.glyphCount;
        int *grown = realloc(state->fontCodepoints, (size_t)(count + 1) * sizeof(*grown));
        if (!grown)
            break;
        if (!state->fontCodepoints)
            for (int i = 0; i < count; i++)
                grown[i] = font.glyphs[i].value;
        state->fontCodepoints = grown;
        state->fontCodepoints[count] = codepoint;
        state->fontGlyphCount = count + 1;
    }
    if (state->fontGlyphCount == previousCount)
        return;
    UiTheme expanded = ui->theme;
    expanded.font = (Font){0};
    expanded.fontCodepoints = state->fontCodepoints;
    expanded.fontGlyphCount = state->fontGlyphCount;
    bool owned = false;
    if (!ResolveThemeFont(&expanded, &owned))
        return;
    // Earlier widgets in this frame may still reference the old atlas in raylib's draw batch.
    rlDrawRenderBatchActive();
    UnloadFont(ui->theme.font);
    ui->theme.font = expanded.font;
}

bool UiInit(UiContext *ui, UiTheme theme)
{
    if (!ui || !ThemeValid(&theme))
        return false;
    *ui = (UiContext){0};
    bool ownsFont = false;
    if (!ResolveThemeFont(&theme, &ownsFont))
        return false;
    ui->state = calloc(1, sizeof(*ui->state));
    if (!ui->state)
    {
        if (ownsFont)
            UnloadFont(theme.font);
        return false;
    }
    ui->theme = theme;
    ui->state->ownsFont = ownsFont;
    return true;
}

void UiFree(UiContext *ui)
{
    if (!ui)
        return;
    if (ui->state)
    {
        if (ui->state->ownsFont)
            UnloadFont(ui->theme.font);
        free(ui->state->fontCodepoints);
        free(ui->state->menuPath);
        free(ui->state->menuRects);
        free(ui->state->menuDrawRects);
        free(ui->state->menuSelections);
        free(ui->state);
    }
    *ui = (UiContext){0};
}

bool UiSetTheme(UiContext *ui, UiTheme theme)
{
    if (!ui || !ui->state || !ThemeValid(&theme))
        return false;

    bool ownsFont = false;
    if (theme.font.texture.id == ui->theme.font.texture.id)
        ownsFont = ui->state->ownsFont;
    else if (!ResolveThemeFont(&theme, &ownsFont))
        return false;

    if (ui->theme.font.texture.id != theme.font.texture.id)
    {
        if (ui->state->ownsFont)
        {
            rlDrawRenderBatchActive();
            UnloadFont(ui->theme.font);
        }
        free(ui->state->fontCodepoints);
        ui->state->fontCodepoints = NULL;
        ui->state->fontGlyphCount = 0;
    }
    ui->theme = theme;
    ui->state->ownsFont = ownsFont;
    return true;
}

void UiBeginFrame(UiContext *ui, const EngineInput *input, UiRect screen)
{
    if (!ui || !ui->state || !input)
        return;
    ui->state->input = *input;
    ui->state->screen = screen;
    ui->state->frameDt = GetFrameTime();
    ui->state->clip = (UiClipState){0};
    ui->state->mouseConsumed = ui->state->menuRoot != NULL || ui->state->activeId != 0;
    ui->state->focusClaimed = false;
}

void UiEndFrame(UiContext *ui)
{
    if (!ui || !ui->state)
        return;
    if (ui->state->clip.active)
    {
        EndScissorMode();
        ui->state->clip = (UiClipState){0};
    }
    if (!ui->state->input.mouseDown[MOUSE_BUTTON_LEFT])
        ui->state->activeId = 0;
    // Focus is dropped only once every field has had its say, so the last one drawn cannot steal it.
    if (ui->state->input.mousePressed[MOUSE_BUTTON_LEFT] && !ui->state->focusClaimed)
        ui->state->focusedId = 0;
}

bool UiConsumesMouse(const UiContext *ui)
{
    return ui && ui->state && ui->state->mouseConsumed;
}

UiRect UiRectInset(UiRect rect, int amount)
{
    if (amount < 0)
        amount = 0;
    rect.x += amount;
    rect.y += amount;
    rect.width = MaxInt(0, rect.width - amount * 2);
    rect.height = MaxInt(0, rect.height - amount * 2);
    return rect;
}

void UiDrawFrame(UiContext *ui, UiRect rect)
{
    if (!ui || !ui->state)
        return;
    Fill(rect, ui->theme.face);
    int width = BorderWidth(rect, ui->theme.frameWidth);
    if (width <= 0)
        return;
    Fill((UiRect){rect.x, rect.y, rect.width, width}, ui->theme.white);
    Fill((UiRect){rect.x, rect.y, width, rect.height}, ui->theme.white);
    Fill((UiRect){rect.x, rect.y + rect.height - 1, rect.width, 1}, ui->theme.black);
    Fill((UiRect){rect.x + rect.width - 1, rect.y, 1, rect.height}, ui->theme.black);
    if (width > 1)
    {
        Fill((UiRect){rect.x + 1, rect.y + rect.height - width, rect.width - 2, width - 1},
             ui->theme.darkGrey);
        Fill((UiRect){rect.x + rect.width - width, rect.y + 1, width - 1, rect.height - 2},
             ui->theme.darkGrey);
    }
}

void UiDrawIndent(UiContext *ui, UiRect rect)
{
    if (!ui || !ui->state)
        return;
    Fill(rect, ui->theme.face);
    DrawEdges(rect, ui->theme.indentWidth, ui->theme.black, ui->theme.white);
}

void UiDrawOutset(UiContext *ui, UiRect rect)
{
    if (!ui || !ui->state)
        return;
    Fill(rect, ui->theme.face);
    DrawEdges(rect, ui->theme.outsetWidth, ui->theme.white, ui->theme.black);
}

void UiDrawTextRaw(UiContext *ui, int x, int y, const char *text, Color color)
{
    if (!ui || !ui->state || !text)
        return;
    EnsureTextGlyphs(ui, text);
    DrawTextEx(ui->theme.font, text, (Vector2){(float)x, (float)(y + ui->theme.textOffsetY)},
               (float)ui->theme.fontSize,
               (float)ui->theme.textSpacing, color);
}

void UiDrawShadowText(UiContext *ui, int x, int y, const char *text, Color color)
{
    if (!ui || !ui->state || !text)
        return;
    int offset = ui->theme.textShadowOffset;
    if (offset)
        UiDrawTextRaw(ui, x + offset, y + offset, text, ui->theme.black);
    UiDrawTextRaw(ui, x, y, text, color);
}

int UiTextWidth(UiContext *ui, const char *text)
{
    if (!ui || !ui->state || !text)
        return 0;
    EnsureTextGlyphs(ui, text);
    return (int)ceilf(MeasureTextEx(ui->theme.font, text, (float)ui->theme.fontSize,
                                   (float)ui->theme.textSpacing)
                          .x);
}

bool UiPointInRect(Vector2 point, UiRect rect)
{
    return rect.width > 0 && rect.height > 0 && point.x >= rect.x && point.y >= rect.y &&
           point.x < rect.x + rect.width && point.y < rect.y + rect.height;
}

UiRect UiIntersectRect(UiRect a, UiRect b)
{
    int left = MaxInt(a.x, b.x);
    int top = MaxInt(a.y, b.y);
    int right = MinInt(a.x + a.width, b.x + b.width);
    int bottom = MinInt(a.y + a.height, b.y + b.height);
    return (UiRect){left, top, MaxInt(0, right - left), MaxInt(0, bottom - top)};
}

uint64_t UiPointerId(const void *pointer, uint64_t salt)
{
    uintptr_t value = (uintptr_t)pointer;
    uint64_t hash = 1469598103934665603ULL ^ salt;
    for (size_t i = 0; i < sizeof(value); i++)
    {
        hash ^= (value >> (i * 8)) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

uint64_t UiWidgetId(const char *label, UiRect rect, uint64_t salt)
{
    uint64_t hash = 1469598103934665603ULL ^ salt;
    const unsigned char *byte = (const unsigned char *)(label ? label : "");
    while (*byte)
    {
        hash ^= *byte++;
        hash *= 1099511628211ULL;
    }
    const int values[] = {rect.x, rect.y, rect.width, rect.height};
    for (size_t i = 0; i < sizeof(values); i++)
    {
        hash ^= ((const unsigned char *)values)[i];
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

bool UiInputAllowed(const UiContext *ui)
{
    return ui && ui->state && !ui->state->menuRoot;
}

void UiMarkMouse(UiContext *ui)
{
    if (ui && ui->state)
        ui->state->mouseConsumed = true;
}

UiClipState UiPushClip(UiContext *ui, UiRect rect)
{
    UiClipState previous = {0};
    if (!ui || !ui->state)
        return previous;
    previous = ui->state->clip;
    if (previous.active)
        rect = UiIntersectRect(previous.rect, rect);
    ui->state->clip = (UiClipState){true, rect};
    BeginScissorMode(rect.x, rect.y, rect.width, rect.height);
    return previous;
}

void UiRestoreClip(UiContext *ui, UiClipState previous)
{
    if (!ui || !ui->state)
        return;
    EndScissorMode();
    ui->state->clip = previous;
    if (previous.active)
        BeginScissorMode(previous.rect.x, previous.rect.y, previous.rect.width, previous.rect.height);
}

static int AlignOffset(int available, int size, UiAlign alignment)
{
    if (alignment == UI_ALIGN_END)
        return available - size;
    return alignment == UI_ALIGN_CENTER ? (available - size) / 2 : 0;
}

static void AlignedText(UiContext *ui, UiRect rect, const char *text, int offset, Color color,
                        UiTextAlignment alignment)
{
    int width = UiTextWidth(ui, text);
    int x = rect.x + AlignOffset(rect.width, width, alignment.horizontal) + offset;
    int y = rect.y + AlignOffset(rect.height, ui->theme.fontSize, alignment.vertical) + offset;
    UiDrawTextRaw(ui, x, y, text, color);
}

bool UiButtonControl(UiContext *ui, UiRect rect, const char *label, uint64_t id,
                     bool enabled, bool interactive, bool forcedDown, bool allowWhileMenuOpen,
                     bool toggleVisual, UiTextAlignment alignment)
{
    if (!ui || !ui->state || !label)
        return false;
    UiState *state = ui->state;
    bool hot = enabled && UiPointInRect(state->input.mousePosition, rect);
    if (hot && interactive)
        UiMarkMouse(ui);
    bool inputAllowed = allowWhileMenuOpen || UiInputAllowed(ui);
    if (interactive && inputAllowed && hot && state->input.mousePressed[MOUSE_BUTTON_LEFT] &&
        !state->activeId)
        state->activeId = id;
    bool held = state->activeId == id && state->input.mouseDown[MOUSE_BUTTON_LEFT];
    bool clicked = state->activeId == id && hot && state->input.mouseReleased[MOUSE_BUTTON_LEFT];
    Color text = enabled ? ui->theme.black : ui->theme.darkGrey;
    bool down = (toggleVisual && clicked) ? !forcedDown : (held || forcedDown);
    // The control's own 1 px bevel: lit top-left at rest, inverted while held, face grey either way.
    if (down)
        UiDrawIndent(ui, rect);
    else
        UiDrawOutset(ui, rect);
    UiRect inside = UiRectInset(rect, down ? ui->theme.indentWidth : ui->theme.outsetWidth);
    UiRect textRect = inside;
    textRect.x += ui->theme.padding;
    textRect.width = MaxInt(0, textRect.width - ui->theme.padding * 2);
    UiClipState previous = UiPushClip(ui, inside);
    AlignedText(ui, textRect, label, down ? 1 : 0, text, alignment);
    UiRestoreClip(ui, previous);
    return clicked;
}

bool UiButton(UiContext *ui, UiRect rect, const char *label)
{
    return UiButtonEx(ui, rect, label, UI_BUTTON_DEFAULT);
}

bool UiButtonEx(UiContext *ui, UiRect rect, const char *label, UiButtonFlags flags)
{
    return UiButtonAligned(ui, rect, label, flags,
                           (UiTextAlignment){UI_ALIGN_CENTER, UI_ALIGN_CENTER});
}

bool UiButtonAligned(UiContext *ui, UiRect rect, const char *label, UiButtonFlags flags,
                     UiTextAlignment alignment)
{
    if (!ui || !ui->state || !label)
        return false;
    // A control belongs in an indent, so a plain button brings its own tight one. UI_BUTTON_BARE is
    // the explicit override for a button already sharing a group's indent.
    UiRect button = rect;
    if (!(flags & UI_BUTTON_BARE))
    {
        UiDrawIndent(ui, rect);
        button = UiRectInset(rect, ui->theme.indentWidth);
    }
    return UiButtonControl(ui, button, label, UiWidgetId(label, rect, 0x425554544f4eULL),
                           !(flags & UI_BUTTON_DISABLED),
                           !(flags & (UI_BUTTON_DISABLED | UI_BUTTON_NO_INPUT)),
                           flags & UI_BUTTON_DOWN, false,
                           flags & UI_BUTTON_TOGGLE_VISUAL, alignment);
}

bool UiButtonBare(UiContext *ui, UiRect rect, const char *label)
{
    return UiButtonEx(ui, rect, label, UI_BUTTON_BARE);
}

bool UiToggleButton(UiContext *ui, UiRect rect, const char *label, bool *value)
{
    if (!value)
        return false;
    UiButtonFlags flags = UI_BUTTON_TOGGLE_VISUAL;
    if (*value)
        flags = (UiButtonFlags)(flags | UI_BUTTON_DOWN);
    bool changed = UiButtonEx(ui, rect, label, flags);
    if (changed)
        *value = !*value;
    return changed;
}

void UiDrawTitleBar(UiContext *ui, UiRect rect, const char *title)
{
    if (!ui || !ui->state || rect.width <= 0 || rect.height <= 0)
        return;
    DrawRectangle(rect.x, rect.y, rect.width, rect.height, ui->theme.darkGrey);
    DrawRectangle(rect.x, rect.y + rect.height - 1, rect.width, 1, ui->theme.black);
    if (title)
        UiDrawShadowText(ui, rect.x + ui->theme.padding,
                         rect.y + (rect.height - ui->theme.fontSize) / 2,
                         title, ui->theme.white);
}

bool UiSlider(UiContext *ui, UiRect rect, float *value, float minimum, float maximum)
{
    return UiSliderEx(ui, rect, value, minimum, maximum, true);
}

bool UiSliderEx(UiContext *ui, UiRect rect, float *value, float minimum, float maximum,
                bool interactive)
{
    if (!ui || !ui->state || !value || !isfinite(minimum) || !isfinite(maximum) || maximum <= minimum)
        return false;
    UiState *state = ui->state;
    uint64_t id = UiPointerId(value, 0x534c49444552ULL);
    bool hot = interactive && UiPointInRect(state->input.mousePosition, rect);
    if (hot || (interactive && state->activeId == id))
        UiMarkMouse(ui);
    if (interactive && UiInputAllowed(ui) && hot && state->input.mousePressed[MOUSE_BUTTON_LEFT] &&
        !state->activeId)
        state->activeId = id;
    UiRect well = UiRectInset(rect, ui->theme.indentWidth);
    bool changed = false;
    if (interactive && state->activeId == id && state->input.mouseDown[MOUSE_BUTTON_LEFT])
    {
        int span = MaxInt(1, well.width - ui->theme.sliderKnobWidth);
        float fraction =
            (state->input.mousePosition.x - well.x - ui->theme.sliderKnobWidth * 0.5f) / span;
        if (fraction < 0)
            fraction = 0;
        if (fraction > 1)
            fraction = 1;
        float next = minimum + fraction * (maximum - minimum);
        changed = next != *value;
        *value = next;
    }
    UiDrawIndent(ui, rect);
    if (well.width > 0 && well.height > 0)
        DrawRectangle(well.x, well.y, well.width, well.height, RecessedFace(&ui->theme));
    float fraction = (*value - minimum) / (maximum - minimum);
    if (fraction < 0)
        fraction = 0;
    if (fraction > 1)
        fraction = 1;
    int knobX = well.x + (int)lroundf(fraction * (well.width - ui->theme.sliderKnobWidth));
    UiDrawOutset(ui, (UiRect){knobX, well.y, ui->theme.sliderKnobWidth, well.height});
    return changed;
}

bool UiCheckbox(UiContext *ui, UiRect rect, const char *label, bool *value)
{
    return UiCheckboxEx(ui, rect, label, value, true);
}

bool UiCheckboxEx(UiContext *ui, UiRect rect, const char *label, bool *value, bool interactive)
{
    if (!ui || !ui->state || !label || !value)
        return false;
    UiState *state = ui->state;
    uint64_t id = UiPointerId(value, 0x434845434bULL);
    bool hot = interactive && UiPointInRect(state->input.mousePosition, rect);
    if (hot)
        UiMarkMouse(ui);
    if (interactive && UiInputAllowed(ui) && hot && state->input.mousePressed[MOUSE_BUTTON_LEFT] &&
        !state->activeId)
        state->activeId = id;
    bool changed = interactive && state->activeId == id && hot &&
                   state->input.mouseReleased[MOUSE_BUTTON_LEFT];
    if (changed)
        *value = !*value;
    int size = MinInt(ui->theme.checkboxSize, rect.height);
    UiRect box = {rect.x, rect.y + (rect.height - size) / 2, size, size};
    UiDrawIndent(ui, box);
    if (*value)
    {
        int inset = ui->theme.indentWidth + 3;
        Fill(UiRectInset(box, inset), ui->theme.black);
    }
    UiDrawTextRaw(ui, box.x + box.width + ui->theme.padding,
                  rect.y + (rect.height - ui->theme.fontSize) / 2, label, ui->theme.black);
    return changed;
}

// Single-line editing over a caller-owned buffer: caret, selection, clipboard, mouse. Offsets are
// byte offsets that always land on a codepoint boundary.
static int FieldStepBack(const char *text, int index)
{
    if (index <= 0)
        return 0;
    index--;
    while (index > 0 && ((unsigned char)text[index] & 0xc0U) == 0x80U)
        index--;
    return index;
}

static int FieldStepForward(const char *text, int index)
{
    int length = (int)strlen(text);
    if (index >= length)
        return length;
    index++;
    while (index < length && ((unsigned char)text[index] & 0xc0U) == 0x80U)
        index++;
    return index;
}

// Measuring a prefix without a copy: the terminator is put back immediately.
static int FieldPrefixWidth(UiContext *ui, char *text, int index)
{
    char held = text[index];
    text[index] = '\0';
    int width = UiTextWidth(ui, text);
    text[index] = held;
    return width;
}

static int FieldIndexAt(UiContext *ui, char *text, int x)
{
    int length = (int)strlen(text);
    int best = 0;
    int bestDistance = -1;
    for (int index = 0; index <= length; index = FieldStepForward(text, index))
    {
        int distance = FieldPrefixWidth(ui, text, index) - x;
        if (distance < 0)
            distance = -distance;
        if (bestDistance < 0 || distance < bestDistance)
        {
            bestDistance = distance;
            best = index;
        }
        if (index == length)
            break;
    }
    return best;
}

static void FieldRemove(char *text, int from, int to)
{
    int length = (int)strlen(text);
    if (from >= to || from < 0 || to > length)
        return;
    memmove(text + from, text + to, (size_t)(length - to) + 1);
}

static bool FieldInsert(char *text, size_t capacity, int at, const char *insert, int bytes)
{
    int length = (int)strlen(text);
    if (bytes <= 0 || (size_t)(length + bytes) >= capacity)
        return false;
    memmove(text + at + bytes, text + at, (size_t)(length - at) + 1);
    memcpy(text + at, insert, (size_t)bytes);
    return true;
}

bool UiTextField(UiContext *ui, UiRect rect, char *text, size_t capacity)
{
    if (!ui || !ui->state || !text || capacity == 0)
        return false;
    EnsureTextGlyphs(ui, text);
    UiState *state = ui->state;
    uint64_t id = UiPointerId(text, 0x544558544649454cULL);
    bool hot = UiPointInRect(state->input.mousePosition, rect);
    if (hot)
        UiMarkMouse(ui);
    // Padding is horizontal breathing room only: insetting the height as well would clip the tops
    // of the glyphs, which costs an accent even where it leaves ASCII looking fine.
    UiRect content = {rect.x + ui->theme.indentWidth + ui->theme.padding,
                      rect.y + ui->theme.indentWidth,
                      MaxInt(0, rect.width - (ui->theme.indentWidth + ui->theme.padding) * 2),
                      MaxInt(0, rect.height - ui->theme.indentWidth * 2)};
    int length = (int)strlen(text);

    if (UiInputAllowed(ui) && hot && state->input.mousePressed[MOUSE_BUTTON_LEFT])
    {
        if (state->focusedId != id)
            state->scroll = 0;
        state->focusedId = id;
        state->focusClaimed = true;
        state->caret = FieldIndexAt(ui, text,
                                   (int)state->input.mousePosition.x - content.x + state->scroll);
        state->selection = state->caret;
        state->selecting = true;
    }
    if (!state->input.mouseDown[MOUSE_BUTTON_LEFT])
        state->selecting = false;
    bool focused = state->focusedId == id;
    if (focused && state->selecting && state->input.mouseDown[MOUSE_BUTTON_LEFT])
        state->caret = FieldIndexAt(ui, text,
                                   (int)state->input.mousePosition.x - content.x + state->scroll);

    bool changed = false;
    if (focused)
    {
        bool control = state->input.down[KEY_LEFT_CONTROL] || state->input.down[KEY_RIGHT_CONTROL];
        bool shift = state->input.down[KEY_LEFT_SHIFT] || state->input.down[KEY_RIGHT_SHIFT];
        state->caret = state->caret < 0 ? 0 : (state->caret > length ? length : state->caret);
        state->selection =
            state->selection < 0 ? 0 : (state->selection > length ? length : state->selection);
        int from = state->caret < state->selection ? state->caret : state->selection;
        int to = state->caret < state->selection ? state->selection : state->caret;

        if (control && state->input.pressed[KEY_A])
        {
            state->selection = 0;
            state->caret = length;
        }
        else if (control && (state->input.pressed[KEY_C] || state->input.pressed[KEY_X]) && to > from)
        {
            char held = text[to];
            text[to] = '\0';
            SetClipboardText(text + from);
            text[to] = held;
            if (state->input.pressed[KEY_X])
            {
                FieldRemove(text, from, to);
                state->caret = state->selection = from;
                changed = true;
            }
        }
        else if (control && state->input.pressed[KEY_V])
        {
            const char *paste = GetClipboardText();
            if (paste)
            {
                changed = to > from;
                FieldRemove(text, from, to);
                state->caret = state->selection = from;
                for (const char *c = paste; *c;)
                {
                    int bytes = 0;
                    int codepoint = GetCodepointNext(c, &bytes);
                    const char *character = c;
                    c += bytes;
                    if (codepoint < 32 || (codepoint >= 127 && codepoint < 160))
                        continue;
                    // Insert whole characters, never a partial UTF-8 sequence at the buffer limit.
                    if (!FieldInsert(text, capacity, state->caret, character, bytes))
                        break;
                    state->caret += bytes;
                    changed = true;
                }
                state->selection = state->caret;
                EnsureTextGlyphs(ui, text);
            }
        }
        else if (state->input.pressed[KEY_LEFT] || state->input.pressed[KEY_RIGHT])
        {
            bool forward = state->input.pressed[KEY_RIGHT];
            if (!shift && to > from)
                state->caret = forward ? to : from;
            else
                state->caret = forward ? FieldStepForward(text, state->caret)
                                       : FieldStepBack(text, state->caret);
            if (!shift)
                state->selection = state->caret;
        }
        else if (state->input.pressed[KEY_HOME] || state->input.pressed[KEY_END])
        {
            state->caret = state->input.pressed[KEY_END] ? length : 0;
            if (!shift)
                state->selection = state->caret;
        }
        else if (state->input.pressed[KEY_BACKSPACE] || state->input.pressed[KEY_DELETE])
        {
            if (to > from)
                FieldRemove(text, from, to);
            else if (state->input.pressed[KEY_BACKSPACE] && state->caret > 0)
            {
                from = FieldStepBack(text, state->caret);
                FieldRemove(text, from, state->caret);
            }
            else if (state->input.pressed[KEY_DELETE] && state->caret < length)
                FieldRemove(text, state->caret, FieldStepForward(text, state->caret));
            state->caret = state->selection = from;
            changed = true;
        }

        for (int i = 0; i < state->input.textCount; i++)
        {
            int codepoint = state->input.text[i];
            if (codepoint < 32 || codepoint == 127 || control)
                continue;
            int bytes = 0;
            const char *encoded = CodepointToUTF8(codepoint, &bytes);
            from = state->caret < state->selection ? state->caret : state->selection;
            to = state->caret < state->selection ? state->selection : state->caret;
            if (to > from)
            {
                FieldRemove(text, from, to);
                state->caret = state->selection = from;
            }
            if (FieldInsert(text, capacity, state->caret, encoded, bytes))
            {
                state->caret += bytes;
                state->selection = state->caret;
                changed = true;
            }
        }
        if (state->input.pressed[KEY_ENTER] || state->input.pressed[KEY_ESCAPE])
        {
            state->focusedId = 0;
            focused = false;
        }
    }

    UiDrawIndent(ui, rect);
    UiClipState previous = UiPushClip(ui, content);
    int y = rect.y + (rect.height - ui->theme.fontSize) / 2;
    if (focused)
    {
        // Keep the caret inside the visible run of the field.
        int caretX = FieldPrefixWidth(ui, text, state->caret);
        if (caretX - state->scroll > content.width - 1)
            state->scroll = caretX - content.width + 1;
        if (caretX - state->scroll < 0)
            state->scroll = caretX;
        int total = UiTextWidth(ui, text);
        if (state->scroll > total - content.width)
            state->scroll = total - content.width;
        if (state->scroll < 0)
            state->scroll = 0;

        int from = state->caret < state->selection ? state->caret : state->selection;
        int to = state->caret < state->selection ? state->selection : state->caret;
        if (to > from)
        {
            int left = content.x + FieldPrefixWidth(ui, text, from) - state->scroll;
            int right = content.x + FieldPrefixWidth(ui, text, to) - state->scroll;
            DrawRectangle(left, y + ui->theme.textOffsetY, right - left, ui->theme.fontSize,
                          ui->theme.darkGrey);
        }
        UiDrawTextRaw(ui, content.x - state->scroll, y, text, ui->theme.black);
        if (((int)(GetTime() * 2.0) & 1) == 0)
            DrawRectangle(content.x + caretX - state->scroll, y + ui->theme.textOffsetY, 1,
                          ui->theme.fontSize, ui->theme.black);
    }
    else
        UiDrawTextRaw(ui, content.x, y, text, ui->theme.black);
    UiRestoreClip(ui, previous);
    return changed;
}

bool UiTextFieldActive(const UiContext *ui)
{
    return ui && ui->state && ui->state->focusedId != 0;
}

bool UiTextFieldFocused(const UiContext *ui, const char *text)
{
    return ui && ui->state && text &&
           ui->state->focusedId == UiPointerId(text, 0x544558544649454cULL);
}

void UiLabel(UiContext *ui, UiRect rect, const char *text)
{
    UiLabelAligned(ui, rect, text, (UiTextAlignment){UI_ALIGN_START, UI_ALIGN_CENTER});
}

void UiLabelAligned(UiContext *ui, UiRect rect, const char *text, UiTextAlignment alignment)
{
    if (!ui || !ui->state || !text)
        return;
    UiClipState previous = UiPushClip(ui, rect);
    AlignedText(ui, rect, text, 0, ui->theme.black, alignment);
    UiRestoreClip(ui, previous);
}

void UiIndent(UiContext *ui, UiRect rect, UiContentFn draw, void *user)
{
    if (!ui || !ui->state)
        return;
    UiDrawIndent(ui, rect);
    if (UiPointInRect(ui->state->input.mousePosition, rect))
        UiMarkMouse(ui);
    UiRect content = UiRectInset(rect, ui->theme.indentWidth);
    UiClipState previous = UiPushClip(ui, content);
    if (draw && content.width > 0 && content.height > 0)
        draw(ui, content, user);
    UiRestoreClip(ui, previous);
}
