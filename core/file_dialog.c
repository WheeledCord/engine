/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "file_dialog.h"
#include "tinyfiledialogs.h"
#include <stdio.h>
#include <string.h>

static bool queued, queuedCancel;
static char queuedAnswer[1024];

void CoreFileDialogQueueAnswer(const char *answer)
{
    queued = true;
    queuedCancel = answer == NULL;
    snprintf(queuedAnswer, sizeof queuedAnswer, "%s", answer ? answer : "");
}

static bool Answer(char *out, size_t capacity, const char *answer)
{
    if (!answer || !answer[0] || strlen(answer) >= capacity)
        return false;
    memcpy(out, answer, strlen(answer) + 1);
    return true;
}
static bool Queued(char *out, size_t capacity, bool *result)
{
    if (!queued)
        return false;
    queued = false;
    *result = !queuedCancel && Answer(out, capacity, queuedAnswer);
    return true;
}

bool CoreOpenFileDialog(char *out, size_t capacity, const char *title, const char *start,
                        const char *const *patterns, int patternCount, const char *description)
{
    bool result = false;
    if (Queued(out, capacity, &result))
        return result;
    return Answer(out, capacity,
                  tinyfd_openFileDialog(title, start ? start : "", patternCount, patterns, description, 0));
}
bool CoreSaveFileDialog(char *out, size_t capacity, const char *title, const char *start,
                        const char *const *patterns, int patternCount, const char *description)
{
    bool result = false;
    if (Queued(out, capacity, &result))
        return result;
    return Answer(out, capacity, tinyfd_saveFileDialog(title, start ? start : "", patternCount, patterns, description));
}
bool CoreFolderDialog(char *out, size_t capacity, const char *title, const char *start)
{
    bool result = false;
    if (Queued(out, capacity, &result))
        return result;
    return Answer(out, capacity, tinyfd_selectFolderDialog(title, start ? start : ""));
}
