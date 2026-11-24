/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_UI_BROWSER_H
#define CORE_UI_BROWSER_H
#include "ui.h"

/* A folder shown as a grid to pick from, the way Hammer's texture browser shows a whole texture set
   at once: images as thumbnails, anything else by name, a filter box, Up, and Folder... for the
   system's own folder dialog. Thumbnails load a few a frame, so opening a large folder never stalls.

   Caller-owned and immediate-mode like the rest of core/ui: open it on a folder, draw it where it
   should appear each frame, and read `chosen` on the frame the draw returns true. */
#define UI_BROWSER_PATH 512
typedef struct UiFileBrowser
{
    char folder[UI_BROWSER_PATH];
    char filter[64];
    char extensions[64]; /* ".png;.jpg", as raylib's IsFileExtension takes them */
    char chosen[UI_BROWSER_PATH];
    char **names;        /* entries shown: folders first (ending in '/'), then files */
    Texture2D *thumbs;
    bool *tried;
    int count, scroll, contentHeight;
    char listed[UI_BROWSER_PATH]; /* the folder the entries are of */
} UiFileBrowser;

void UiFileBrowserOpen(UiFileBrowser *browser, const char *folder, const char *extensions);
/* Draws the browser filling rect; wheel scrolls it. True on the frame a file is picked. */
bool UiFileBrowserDraw(UiContext *ui, UiFileBrowser *browser, UiRect rect, float wheel);
void UiFileBrowserFree(UiFileBrowser *browser);
#endif
