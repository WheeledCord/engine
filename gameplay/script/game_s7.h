/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_SCRIPT_GAME_S7_H
#define GAMEPLAY_SCRIPT_GAME_S7_H

#include "core/store.h"
#include <stdbool.h>
#include <stdint.h>

/* The store's Scheme frontend (docs/developer/store.md §5): things as s7 values, define-kind and its
   handlers, the five rules as errors, and the calls a game makes. It is a second frontend beside
   script_s7.c, with its own interpreter; there is one per process.

   A translation unit that also includes s7.h includes it first: this header then uses s7's own
   declarations of s7_scheme and s7_pointer, and otherwise declares them itself. */
#ifndef S7_H
typedef struct s7_scheme s7_scheme;
typedef struct s7_cell *s7_pointer;
#endif

#define GAME_S7_MAX_ACTIONS 32

/* One tick's input, owned by the runner, which rewrites it before each StoreTick. Action i (in the
   order define-actions declared them) is bit i. */
typedef struct GameInput
{
    uint32_t held, pressed; /* pressed: went down this tick */
    float mouseDx, mouseDy;
} GameInput;

/* A C function Scheme calls: args is the argument list. For a method, its car is the thing. */
typedef s7_pointer (*GameS7Function)(s7_scheme *sc, s7_pointer args);

/** @brief Starts the interpreter on a store: types, calls, then the prelude.
 * @param store Store the game lives in; borrowed until GameS7Close.
 * @param preludePath core/scheme/kinds.scm, resolved with CoreResolvePath.
 * @return True when ready; false when already open, the prelude is missing, or out of memory. */
bool GameS7Open(Store *store, const char *preludePath);

/** @brief Loads a game file into a fresh environment under the rootlet, then freezes it (§5.7).
 *
 * The kinds it declares become live once the whole file has loaded; a file that raises an error is
 * reported through the error sink and its handlers are discarded (kinds already declared stay).
 * @param path Game file, resolved with CoreResolvePath.
 * @return True when the file loaded without an error. */
bool GameS7LoadGame(const char *path);

/** @brief Loads the last game file again into a fresh environment and swaps its handlers in.
 *
 * Kinds keep their ids. A kind whose fields differ from the live declaration (names or types)
 * refuses the reload with a message naming the kind and the field; the old handlers stay.
 * @return True when reloaded; false with the reason reported through the error sink. */
bool GameS7Reload(void);

/** @brief Evaluates REPL text in the game environment (the rootlet before a game is loaded).
 * @param text One or more forms.
 * @param answer Receives the printed value, or the error message; malloced, release with free.
 * May be NULL.
 * @return True when evaluated without an error. */
bool GameS7Eval(const char *text, char **answer);

/** @brief After StoreLoad: spawns again every declared `:local #t` child a loaded thing lacks.
 *
 * A save leaves local things out (docs/developer/store.md §2.3); `(load-game path)` calls this
 * itself, and a runner that calls StoreLoad directly calls it next.
 * @return True when every missing local child was spawned; false with the reason reported. */
bool GameS7RestoreLocalChildren(void);

/** @brief Frees the interpreter; the store is left as it is.
 * @return No value. */
void GameS7Close(void);

/** @brief Returns the interpreter, for modules that register their own calls.
 * @return The interpreter, or NULL when closed. */
s7_scheme *GameS7Scheme(void);

/** @brief Registers a call that may call back into Scheme (s7_define_function), in the rootlet.
 * @param name Scheme name.
 * @param function Implementation.
 * @param required Required arguments.
 * @param optional Optional arguments.
 * @param rest True to accept any number more (keyword arguments need this).
 * @param help Documentation string.
 * @return True when registered; false when closed. */
bool GameS7Define(const char *name, GameS7Function function, int required, int optional, bool rest,
                  const char *help);

/** @brief Registers a call that never calls back into Scheme (s7_define_typed_function).
 * @param name Scheme name.
 * @param function Implementation.
 * @param required Required arguments.
 * @param optional Optional arguments.
 * @param rest True to accept any number more.
 * @param help Documentation string.
 * @param signature s7 signature, or NULL for "any result, any arguments".
 * @return True when registered; false when closed. */
bool GameS7DefineTyped(const char *name, GameS7Function function, int required, int optional,
                       bool rest, const char *help, s7_pointer signature);

/** @brief Adds a method every thing answers: `(thing 'name args...)` calls function with
 * `(thing args...)`. Fields and declared children of that name win over it.
 * @param name Method name.
 * @param function Implementation; it must not call back into Scheme.
 * @return True when added; false when closed or 32 methods are registered already. */
bool GameS7DefineMethod(const char *name, GameS7Function function);

/** @brief Makes the Scheme value for a thing: the same object for the same live handle.
 * @param sc Interpreter.
 * @param id Thing; STORE_NULL gives #f.
 * @return The thing, or #f. */
s7_pointer GameS7Thing(s7_scheme *sc, StoreId id);

/** @brief Reads a thing's handle out of a Scheme value, live or removed.
 * @param p Value.
 * @param out Receives the handle.
 * @return True when p is a thing. */
bool GameS7ToId(s7_pointer p, StoreId *out);

/** @brief Converts a Scheme value to a store value, choosing the type from the value (§5.1).
 * @param sc Interpreter.
 * @param p Integer, real, boolean, symbol, string (63 bytes at most), vec3 or thing.
 * @param out Receives the value.
 * @param why On failure, what p was ("a procedure", "a hash table", ...) for the rule 4 message.
 * @return True when converted. */
bool GameS7ToValue(s7_scheme *sc, s7_pointer p, StoreValue *out, const char **why);

/** @brief Converts a store value to Scheme: a removed thing is #f, STORE_NONE is #f, and a
 * collection type (as -changed carries) is #t.
 * @param sc Interpreter.
 * @param value Value.
 * @return The Scheme value. */
s7_pointer GameS7FromValue(s7_scheme *sc, const StoreValue *value);

/** @brief Makes a vec3 (a float-vector of 3).
 * @param sc Interpreter.
 * @param v Vector.
 * @return The float-vector. */
s7_pointer GameS7Vec3(s7_scheme *sc, Vector3 v);

/** @brief Reads a vec3: a float-vector or vector of three numbers.
 * @param p Value.
 * @param out Receives the vector.
 * @return True when p is one. */
bool GameS7ToVec3(s7_pointer p, Vector3 *out);

/** @brief Finds a keyword argument (`:name value`) after the positional ones.
 * @param sc Interpreter.
 * @param args Argument list.
 * @param name Keyword without the colon.
 * @return The value, or NULL when absent. */
s7_pointer GameS7KeywordArg(s7_scheme *sc, s7_pointer args, const char *name);

/** @brief Raises a Scheme error carrying a message, reported like any script error. Does not
 * return to the caller.
 * @param sc Interpreter.
 * @param message Message, copied.
 * @return Nothing useful: the error unwinds. Written `return GameS7Error(sc, "...")`. */
s7_pointer GameS7Error(s7_scheme *sc, const char *message);

/** @brief Gives the tick's input to held?, pressed?, input-vector and mouse-motion.
 * @param input Borrowed until replaced; NULL means no input.
 * @return No value. */
void GameS7SetInput(const GameInput *input);

/** @brief Counts the actions define-actions declared.
 * @return 0 to GAME_S7_MAX_ACTIONS. */
int GameS7ActionCount(void);

/** @brief Names an action.
 * @param action Action index, which is also its bit in GameInput.
 * @return The name, or NULL out of range. Valid until the next define-actions. */
const char *GameS7ActionName(int action);

/** @brief Gives an action's default key, as the game wrote it ("W", "Space", "Mouse1").
 * @param action Action index.
 * @return The key name, or NULL out of range. Valid until the next define-actions. */
const char *GameS7ActionKey(int action);

/** @brief Receives every reported error line (handler errors as `<kind> #<index> <event>: <message>
 * (<file>:<line>)`, at most once per kind and event per second; load and reload errors), besides
 * TraceLog. For tests and tools.
 * @param sink Function, or NULL to stop.
 * @return No value. */
void GameS7SetErrorSink(void (*sink)(const char *message));

#endif
