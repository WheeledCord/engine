/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_S7_H
#define GAMEPLAY_SCRIPT_S7_H
#include "script.h"

/* The Scheme frontend: it registers the binding table and dispatches entity callbacks into named
   Scheme functions. One interpreter per program. */
bool ScriptS7Open(ScriptHost *host);
bool ScriptS7Load(ScriptHost *host, const char *path);
// Evaluates one expression and, when answer is given, hands back a printable result to free.
bool ScriptS7Eval(const char *text, char **answer);
// Evaluates whatever has been typed since the last call, against the world as it stands.
void ScriptS7Repl(ScriptHost *host);
void ScriptS7Close(void);
#endif
