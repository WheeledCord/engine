/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "ui_browser.h"
#include "file_dialog.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CELL 92
#define THUMB 64

static void Clear(UiFileBrowser *b)
{
    for (int i = 0; i < b->count; i++)
    {
        free(b->names[i]);
        if (b->thumbs[i].id)
            UnloadTexture(b->thumbs[i]);
    }
    free(b->names);
    free(b->thumbs);
    free(b->tried);
    b->names = NULL;
    b->thumbs = NULL;
    b->tried = NULL;
    b->count = 0;
}
void UiFileBrowserFree(UiFileBrowser *b)
{
    Clear(b);
    b->listed[0] = 0;
}
void UiFileBrowserOpen(UiFileBrowser *b, const char *folder, const char *extensions)
{
    snprintf(b->folder, sizeof b->folder, "%s", folder && folder[0] ? folder : ".");
    snprintf(b->extensions, sizeof b->extensions, "%s", extensions ? extensions : "");
    b->chosen[0] = 0;
    b->scroll = 0;
    b->listed[0] = 0; /* read on the next draw */
}

static bool Matches(const char *name, const char *filter)
{
    if (!filter[0])
        return true;
    for (const char *start = name; *start; start++)
    {
        size_t i = 0;
        while (filter[i] && start[i] && tolower((unsigned char)start[i]) == tolower((unsigned char)filter[i]))
            i++;
        if (!filter[i])
            return true;
    }
    return false;
}
static int ByName(const void *a, const void *b)
{
    const char *l = *(char *const *)a, *r = *(char *const *)b;
    bool ld = l[strlen(l) - 1] == '/', rd = r[strlen(r) - 1] == '/';
    if (ld != rd)
        return ld ? -1 : 1;
    return strcmp(l, r);
}
static void List(UiFileBrowser *b)
{
    Clear(b);
    snprintf(b->listed, sizeof b->listed, "%s", b->folder);
    if (!DirectoryExists(b->folder))
        return;
    FilePathList all = LoadDirectoryFiles(b->folder);
    b->names = calloc(all.count + 1, sizeof *b->names);
    b->thumbs = calloc(all.count + 1, sizeof *b->thumbs);
    b->tried = calloc(all.count + 1, sizeof *b->tried);
    for (unsigned i = 0; b->names && b->thumbs && b->tried && i < all.count; i++)
    {
        const char *name = GetFileName(all.paths[i]);
        if (name[0] == '.')
            continue;
        bool folder = DirectoryExists(all.paths[i]);
        if (!folder && b->extensions[0] && !IsFileExtension(name, b->extensions))
            continue;
        size_t length = strlen(name);
        char *copy = malloc(length + 2);
        if (!copy)
            continue;
        memcpy(copy, name, length + 1);
        if (folder)
            copy[length] = '/', copy[length + 1] = 0;
        b->names[b->count++] = copy;
    }
    UnloadDirectoryFiles(all);
    if (b->count)
        qsort(b->names, (size_t)b->count, sizeof *b->names, ByName);
}
static bool IsImage(const char *name)
{
    return IsFileExtension(name, ".png;.jpg;.jpeg;.bmp;.tga;.gif;.qoi");
}
/* A few a frame: a folder of hundreds of textures fills in over a second instead of freezing. */
static void LoadSomeThumbnails(UiFileBrowser *b, int first, int last)
{
    int budget = 4;
    for (int i = first; i < last && i < b->count && budget > 0; i++)
    {
        if (b->tried[i] || !IsImage(b->names[i]))
            continue;
        b->tried[i] = true;
        budget--;
        char path[UI_BROWSER_PATH * 2];
        snprintf(path, sizeof path, "%s/%s", b->folder, b->names[i]);
        Image image = LoadImage(path);
        if (!image.data)
            continue;
        float scale = (float)THUMB / (float)(image.width > image.height ? image.width : image.height);
        if (scale < 1)
            ImageResize(&image, image.width * scale < 1 ? 1 : (int)(image.width * scale),
                        image.height * scale < 1 ? 1 : (int)(image.height * scale));
        b->thumbs[i] = LoadTextureFromImage(image);
        UnloadImage(image);
    }
}

bool UiFileBrowserDraw(UiContext *ui, UiFileBrowser *b, UiRect rect, float wheel)
{
    if (!ui || !b)
        return false;
    if (strcmp(b->listed, b->folder))
        List(b);
    const UiTheme *theme = &ui->theme;
    int row = theme->itemHeight, gap = theme->containerGap;
    UiRect top = {rect.x, rect.y, rect.width, row};
    int buttons = 64;
    UiNextId(ui, (uint64_t)(uintptr_t)b * 8 + 1);
    if (UiButton(ui, (UiRect){top.x, top.y, 40, row}, "Up"))
    {
        char parent[UI_BROWSER_PATH];
        snprintf(parent, sizeof parent, "%s", GetPrevDirectoryPath(b->folder));
        snprintf(b->folder, sizeof b->folder, "%s", parent);
        b->scroll = 0;
    }
    UiTextField(ui, (UiRect){top.x + 40 + gap, top.y, top.width - 40 - buttons * 1 - gap * 2, row}, b->folder,
                sizeof b->folder);
    UiNextId(ui, (uint64_t)(uintptr_t)b * 8 + 2);
    if (UiButton(ui, (UiRect){top.x + top.width - buttons, top.y, buttons, row}, "Folder"))
    {
        char picked[UI_BROWSER_PATH];
        if (CoreFolderDialog(picked, sizeof picked, "Choose a folder", b->folder))
        {
            snprintf(b->folder, sizeof b->folder, "%s", picked);
            b->scroll = 0;
        }
    }
    UiRect search = {rect.x, rect.y + row + gap, rect.width, row};
    UiLabel(ui, (UiRect){search.x, search.y, 52, row}, "Filter");
    UiTextField(ui, (UiRect){search.x + 52, search.y, search.width - 52, row}, b->filter, sizeof b->filter);
    UiRect grid = {rect.x, search.y + row + gap, rect.width, rect.height - (row + gap) * 2};
    UiDrawIndent(ui, grid);
    UiRect inner = UiRectInset(grid, theme->indentWidth + 2);
    int columns = inner.width / (CELL + gap) > 0 ? inner.width / (CELL + gap) : 1;
    int shown = 0;
    for (int i = 0; i < b->count; i++)
        shown += Matches(b->names[i], b->filter);
    int rows = (shown + columns - 1) / columns;
    b->contentHeight = rows * (CELL + gap);
    Vector2 mouse = GetMousePosition();
    if (wheel != 0 && mouse.x >= grid.x && mouse.y >= grid.y && mouse.x < grid.x + grid.width &&
        mouse.y < grid.y + grid.height)
        b->scroll -= (int)(wheel * (CELL + gap));
    int most = b->contentHeight - inner.height;
    b->scroll = b->scroll > most ? most : b->scroll;
    b->scroll = b->scroll < 0 ? 0 : b->scroll;
    int firstRow = b->scroll / (CELL + gap), lastRow = (b->scroll + inner.height) / (CELL + gap) + 1;
    BeginScissorMode(inner.x, inner.y, inner.width, inner.height);
    bool picked = false;
    int at = 0, firstVisible = -1, lastVisible = 0;
    for (int i = 0; i < b->count; i++)
    {
        if (!Matches(b->names[i], b->filter))
            continue;
        int r = at / columns, c = at % columns;
        at++;
        if (r < firstRow || r > lastRow)
            continue;
        if (firstVisible < 0)
            firstVisible = i;
        lastVisible = i + 1;
        UiRect cell = {inner.x + c * (CELL + gap), inner.y + r * (CELL + gap) - b->scroll, CELL, CELL};
        bool folder = b->names[i][strlen(b->names[i]) - 1] == '/';
        UiNextId(ui, (uint64_t)(uintptr_t)b->names[i]);
        if (UiButtonEx(ui, cell, "", UI_BUTTON_TOGGLE_VISUAL))
        {
            char path[UI_BROWSER_PATH * 2];
            snprintf(path, sizeof path, "%s/%s", b->folder, b->names[i]);
            size_t length = strlen(path) - (folder ? 1 : 0);
            if (length < sizeof b->folder)
            {
                char *into = folder ? b->folder : b->chosen;
                memcpy(into, path, length);
                into[length] = 0;
                b->scroll = folder ? 0 : b->scroll;
                picked = !folder;
            }
        }
        if (b->thumbs && b->thumbs[i].id)
        {
            Texture2D t = b->thumbs[i];
            DrawTexture(t, cell.x + (CELL - t.width) / 2, cell.y + 4 + (THUMB - t.height) / 2, WHITE);
        }
        else if (folder)
            UiLabel(ui, (UiRect){cell.x + 4, cell.y + 20, CELL - 8, row}, "[folder]");
        UiLabel(ui, (UiRect){cell.x + 3, cell.y + CELL - row, CELL - 6, row}, b->names[i]);
    }
    EndScissorMode();
    if (firstVisible >= 0)
        LoadSomeThumbnails(b, firstVisible, lastVisible);
    if (!b->count)
        UiLabel(ui, (UiRect){inner.x + 4, inner.y + 4, inner.width - 8, row},
                DirectoryExists(b->folder) ? "Nothing here matches" : "No such folder");
    /* Folder changes are picked up on the next draw, so a click inside this one stays valid. */
    return picked;
}
