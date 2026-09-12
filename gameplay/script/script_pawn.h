/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_PAWN_H
#define GAMEPLAY_SCRIPT_PAWN_H
#include "script.h"

/* The Pawn frontend: it registers the binding table as natives and dispatches entity callbacks into
   public functions. One script per program; its main() declares the classes. */
bool ScriptPawnOpen(ScriptHost *host, const char *compiledPath);
void ScriptPawnClose(void);
// How a row's name is spelled where dashes and question marks are not allowed in an identifier.
void ScriptPawnName(const char *name, char *out, size_t capacity);
// Writes the native declarations a Pawn script includes, from the same table.
bool ScriptPawnWriteInclude(const char *path);
#endif
