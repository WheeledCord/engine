/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_FILE_DIALOG_H
#define CORE_FILE_DIALOG_H
#include <stdbool.h>
#include <stddef.h>

/* The system's own Open, Save and Choose Folder dialogs, for tools: the window every other program
   shows, with its places, recent files and typing a path, rather than a file list drawn in-engine.

   Windows and macOS use their native dialogs; Linux runs zenity or kdialog, whichever the desktop
   has. Each call blocks until the person answers, as those dialogs do everywhere. All come from
   tinyfiledialogs (vendor/tinyfiledialogs, zlib licence).

   `patterns` are like "*.png"; `start` may be a file or a folder ending in '/'. The answer is
   written to out, and false means cancelled, unavailable or too long for out. */
bool CoreOpenFileDialog(char *out, size_t capacity, const char *title, const char *start,
                        const char *const *patterns, int patternCount, const char *description);
bool CoreSaveFileDialog(char *out, size_t capacity, const char *title, const char *start,
                        const char *const *patterns, int patternCount, const char *description);
bool CoreFolderDialog(char *out, size_t capacity, const char *title, const char *start);

/* Answers the next dialog without showing it, for scripted checks: a path, or NULL to cancel. A test
   that never queues an answer never opens a window either way, because nothing asks. */
void CoreFileDialogQueueAnswer(const char *answer);
#endif
