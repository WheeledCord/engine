/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_API_H
#define GAMEPLAY_SCRIPT_API_H
#include "gameplay/entity.h"

/* The script-facing API, described once as data. Every language frontend is a loop over this table:
   it registers each row in whatever way its language wants, converts arguments into ScriptValue and
   the result back. Nothing here knows about a particular language, and a new call is one row in
   script_api.def rather than a change in each frontend. */

typedef enum ScriptType
{
    SCRIPT_NONE,
    SCRIPT_BOOL,
    SCRIPT_INT,
    SCRIPT_FLOAT,
    SCRIPT_VECTOR2,
    SCRIPT_VECTOR3,
    SCRIPT_STRING,
    SCRIPT_ENTITY // an entity handle, as the script sees it: see ScriptEntityId
} ScriptType;

typedef struct ScriptValue
{
    ScriptType type;
    union
    {
        bool boolean;
        int integer;
        float number;
        Vector2 vector2;
        Vector3 vector3;
        const char *string; // borrowed for the length of the call
        int entity;
    } as;
} ScriptValue;

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
   the engine's, in every language and in the generated Pawn declarations. Add them before opening a
   frontend, because Pawn resolves every native a script names when the script is loaded.

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
#define SCRIPT_UNWRAP(...) {__VA_ARGS__}
#define SCRIPT_CALL(id, name, result, help, types)                                                 \
    static ScriptValue Script_##id(ScriptHost *host, const ScriptValue *a);                        \
    static const ScriptType id##_types[] = SCRIPT_UNWRAP types;                                    \
    static const ScriptBinding id##_binding = {                                                    \
        name, result, id##_types + 1,                                                              \
        (int)(sizeof id##_types / sizeof(ScriptType)) - 1, Script_##id, help};                      \
    static ScriptValue Script_##id(ScriptHost *host, const ScriptValue *a)

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

static inline ScriptValue ScriptNone(void) { return (ScriptValue){SCRIPT_NONE, {0}}; }
static inline ScriptValue ScriptBool(bool v)
{
    ScriptValue value = {SCRIPT_BOOL, {0}};
    value.as.boolean = v;
    return value;
}
static inline ScriptValue ScriptInt(int v)
{
    ScriptValue value = {SCRIPT_INT, {0}};
    value.as.integer = v;
    return value;
}
static inline ScriptValue ScriptFloat(float v)
{
    ScriptValue value = {SCRIPT_FLOAT, {0}};
    value.as.number = v;
    return value;
}
static inline ScriptValue ScriptVector2(Vector2 v)
{
    ScriptValue value = {SCRIPT_VECTOR2, {0}};
    value.as.vector2 = v;
    return value;
}
static inline ScriptValue ScriptVector3(Vector3 v)
{
    ScriptValue value = {SCRIPT_VECTOR3, {0}};
    value.as.vector3 = v;
    return value;
}
static inline ScriptValue ScriptString(const char *v)
{
    ScriptValue value = {SCRIPT_STRING, {0}};
    value.as.string = v ? v : "";
    return value;
}
static inline ScriptValue ScriptHandle(int v)
{
    ScriptValue value = {SCRIPT_ENTITY, {0}};
    value.as.entity = v;
    return value;
}
#endif
