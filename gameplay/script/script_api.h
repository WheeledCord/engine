/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_API_H
#define GAMEPLAY_SCRIPT_API_H
#include "core/object.h"
#include "gameplay/entity.h"
#include <stdint.h>

/* The script-facing API, described once as data. Every language frontend is a loop over this table:
   it registers each row in whatever way its language wants, converts arguments into ScriptValue and
   the result back. Nothing here knows about a particular language, and a new call is one row in
   script_api.def rather than a change in each frontend. */

/* Values a script and the engine pass each other are the engine's own (core/object.h), so an
   object's properties, a method's arguments and a table row's all mean the same thing. */
typedef EngineValueType ScriptType;
typedef EngineValue ScriptValue;
#define SCRIPT_NONE ENGINE_NONE
#define SCRIPT_BOOL ENGINE_BOOL
#define SCRIPT_INT ENGINE_INT
#define SCRIPT_FLOAT ENGINE_FLOAT
#define SCRIPT_VECTOR2 ENGINE_VECTOR2
#define SCRIPT_VECTOR3 ENGINE_VECTOR3
#define SCRIPT_STRING ENGINE_STRING
#define SCRIPT_OBJECT ENGINE_OBJECT /* an engine object: a scripted entity, a camera, a timer... */

typedef struct ScriptHost ScriptHost;
typedef ScriptValue (*ScriptCall)(ScriptHost *host, const ScriptValue *arguments);

typedef struct ScriptBinding
{
    const char *name; // as scripts say it
    ScriptType result;
    const ScriptType *arguments;
    int argumentCount;
    ScriptCall call;
    const char *help;
} ScriptBinding;

/* The engine's own rows. Frontends walk the whole table through ScriptBindingAt, which is these
   followed by whatever a game added. */
const ScriptBinding *ScriptBindings(int *count);

/* A game adds calls of its own at runtime: they are checked, spelled and registered exactly like
   the engine's, in every language. Add them before opening a frontend, because a frontend resolves
   every native a script names when the script is loaded.

   SCRIPT_CALL writes the C function and the row it is described by together, so the two cannot
   drift, and leaves a ScriptBinding named <id>_binding to hand to ScriptAddBinding:

       SCRIPT_CALL(grapple, "grapple!", SCRIPT_NONE, "fire a grapple at a point",
                   (SCRIPT_NONE, SCRIPT_VECTOR2))
       {
           Grapple(host, a[0].as.vector2);
           return ScriptNone();
       }
       ...
       ScriptAddBinding(&host, &grapple_binding); */
/* A row that takes no arguments never reads `a`, and a row that ignores the host never reads that.
   Marking both here means no one writing a row has to know, and a project built the way the engine
   builds itself -- warnings as errors -- is not punished for declaring a call with no arguments. */
#if defined(__GNUC__) || defined(__clang__)
#define SCRIPT_MAYBE_UNUSED __attribute__((unused))
#else
#define SCRIPT_MAYBE_UNUSED
#endif
#define SCRIPT_UNWRAP(...) {__VA_ARGS__}
#define SCRIPT_CALL(id, name, result, help, types)                                                 \
    static ScriptValue Script_##id(ScriptHost *host, const ScriptValue *a);                        \
    static const ScriptType id##_types[] = SCRIPT_UNWRAP types;                                    \
    static const ScriptBinding id##_binding = {                                                    \
        name, result, id##_types + 1,                                                              \
        (int)(sizeof id##_types / sizeof(ScriptType)) - 1, Script_##id, help};                      \
    static ScriptValue Script_##id(ScriptHost *host SCRIPT_MAYBE_UNUSED,                           \
                                   const ScriptValue *a SCRIPT_MAYBE_UNUSED)

// The row and everything it points at must outlive the host. Returns false when there is no room.
bool ScriptAddBinding(ScriptHost *host, const ScriptBinding *binding);
// Every call a script may make: the engine's rows, then the game's.
int ScriptBindingCount(const ScriptHost *host);
const ScriptBinding *ScriptBindingAt(const ScriptHost *host, int index);
const ScriptBinding *ScriptBindingNamed(const char *name);
const char *ScriptTypeName(ScriptType type);

/* Checks arity and argument types against the declaration before calling, so no frontend has to
   trust what a script passed. On failure nothing is called and message says what was wrong. */
bool ScriptInvoke(ScriptHost *host, const ScriptBinding *binding, const ScriptValue *arguments,
                  int count, ScriptValue *result, const char **message);

static inline ScriptValue ScriptNone(void) { return EngineNone(); }
static inline ScriptValue ScriptBool(bool v) { return EngineBool(v); }
static inline ScriptValue ScriptInt(int v) { return EngineInt(v); }
static inline ScriptValue ScriptFloat(float v) { return EngineFloat(v); }
static inline ScriptValue ScriptVector2(Vector2 v) { return EngineVector2(v); }
static inline ScriptValue ScriptVector3(Vector3 v) { return EngineVector3(v); }
static inline ScriptValue ScriptString(const char *v) { return EngineString(v); }
static inline ScriptValue ScriptObject(EngineObjectId v) { return EngineObject(v); }
#endif
