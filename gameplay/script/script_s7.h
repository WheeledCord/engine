/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_S7_H
#define GAMEPLAY_SCRIPT_S7_H
#include "script.h"

/* The Scheme frontend: it registers the binding table, dispatches entity callbacks into named
   Scheme functions, and makes every engine object a Scheme value. One interpreter per program.

   An object is applicable: (obj 'name) reads a property or calls a method with no arguments,
   (obj 'name args...) calls a method, and (set! (obj 'name) value) writes a property. Beside the
   table, the frontend defines (make 'type args...), (free! obj), (alive? obj), (object? x),
   (object-type obj), (connect! obj 'signal procedure), (disconnect! id), (properties obj) and
   (methods obj). */
bool ScriptS7Open(ScriptHost *host);
bool ScriptS7Load(ScriptHost *host, const char *path);
// Evaluates one expression and, when answer is given, hands back a printable result to free.
bool ScriptS7Eval(const char *text, char **answer);
// Evaluates whatever has been typed since the last call, against the world as it stands.
void ScriptS7Repl(ScriptHost *host);
void ScriptS7Close(void);
/* Gives an object a global name in Scheme, so a game can hand scripts something of its own that it
   adopted into host->objects -- the player, a level, a HUD. */
bool ScriptS7DefineObject(const char *name, EngineObjectId object);
#endif
