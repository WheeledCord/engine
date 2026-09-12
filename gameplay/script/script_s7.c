/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "script_s7.h"

#include "core/file.h"
#include "s7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

/* The Scheme frontend. Everything it knows about the engine comes from the binding table: the
   trampolines below are written by the same declaration file, and registering them is one loop.
   There is no engine call named anywhere in this file. */

typedef struct ScriptS7
{
    s7_scheme *scheme;
    ScriptHost *host;
    ScriptLanguage language;
} ScriptS7;

static ScriptS7 state;

// A row's place in the table, so a trampoline finds its own declaration without searching by name.
#define SCRIPT_BINDING(id, name, result, help, types) SCRIPT_INDEX_##id,
enum
{
#include "script_api.def"
    SCRIPT_INDEX_COUNT
};
#undef SCRIPT_BINDING

// ---- values -----------------------------------------------------------------------------------
static s7_pointer ToScheme(s7_scheme *sc, ScriptValue value)
{
    switch (value.type)
    {
        case SCRIPT_BOOL:
            return s7_make_boolean(sc, value.as.boolean);
        case SCRIPT_INT:
            return s7_make_integer(sc, value.as.integer);
        case SCRIPT_ENTITY:
            return s7_make_integer(sc, value.as.entity);
        case SCRIPT_FLOAT:
            return s7_make_real(sc, value.as.number);
        case SCRIPT_VECTOR2:
            return s7_list(sc, 2, s7_make_real(sc, value.as.vector2.x),
                           s7_make_real(sc, value.as.vector2.y));
        case SCRIPT_VECTOR3:
            return s7_list(sc, 3, s7_make_real(sc, value.as.vector3.x),
                           s7_make_real(sc, value.as.vector3.y),
                           s7_make_real(sc, value.as.vector3.z));
        case SCRIPT_STRING:
            return s7_make_string(sc, value.as.string ? value.as.string : "");
        default:
            return s7_unspecified(sc);
    }
}

static bool Number(s7_scheme *sc, s7_pointer p, double *out)
{
    (void)sc;
    if (s7_is_integer(p))
        *out = (double)s7_integer(p);
    else if (s7_is_real(p))
        *out = s7_real(p);
    else
        return false;
    return true;
}

// A vector value is a list or a vector of numbers, whichever the script finds natural to write.
static bool Numbers(s7_scheme *sc, s7_pointer p, double *out, int wanted)
{
    if (s7_is_vector(p) && s7_vector_length(p) == wanted)
    {
        for (int i = 0; i < wanted; i++)
            if (!Number(sc, s7_vector_ref(sc, p, i), &out[i]))
                return false;
        return true;
    }
    if (!s7_is_list(sc, p) || s7_list_length(sc, p) != wanted)
        return false;
    for (int i = 0; i < wanted; i++)
        if (!Number(sc, s7_list_ref(sc, p, i), &out[i]))
            return false;
    return true;
}

static bool FromScheme(s7_scheme *sc, s7_pointer p, ScriptType wanted, ScriptValue *out)
{
    double numbers[3] = {0, 0, 0};
    switch (wanted)
    {
        case SCRIPT_BOOL:
            *out = ScriptBool(s7_boolean(sc, p));
            return true;
        case SCRIPT_INT:
            if (!Number(sc, p, numbers))
                return false;
            *out = ScriptInt((int)numbers[0]);
            return true;
        case SCRIPT_ENTITY:
            if (!Number(sc, p, numbers))
                return false;
            *out = ScriptHandle((int)numbers[0]);
            return true;
        case SCRIPT_FLOAT:
            if (!Number(sc, p, numbers))
                return false;
            *out = ScriptFloat((float)numbers[0]);
            return true;
        case SCRIPT_VECTOR2:
            if (!Numbers(sc, p, numbers, 2))
                return false;
            *out = ScriptVector2((Vector2){(float)numbers[0], (float)numbers[1]});
            return true;
        case SCRIPT_VECTOR3:
            if (!Numbers(sc, p, numbers, 3))
                return false;
            *out = ScriptVector3(
                (Vector3){(float)numbers[0], (float)numbers[1], (float)numbers[2]});
            return true;
        case SCRIPT_STRING:
            if (!s7_is_string(p))
                return false;
            *out = ScriptString(s7_string(p));
            return true;
        default:
            *out = ScriptNone();
            return true;
    }
}

// ---- one dispatcher, one trampoline per row ---------------------------------------------------
static s7_pointer Dispatch(s7_scheme *sc, int index, s7_pointer args)
{
    const ScriptBinding *table = ScriptBindings(NULL);
    const ScriptBinding *binding = &table[index];
    ScriptValue values[8];
    int count = (int)s7_list_length(sc, args);
    if (count > (int)(sizeof values / sizeof values[0]))
        return s7_error(sc, s7_make_symbol(sc, "engine-error"),
                        s7_list(sc, 1, s7_make_string(sc, "too many arguments")));
    for (int i = 0; i < count && i < binding->argumentCount; i++)
        if (!FromScheme(sc, s7_list_ref(sc, args, i), binding->arguments[i], &values[i]))
            return s7_wrong_type_arg_error(sc, binding->name, i + 1, s7_list_ref(sc, args, i),
                                           ScriptTypeName(binding->arguments[i]));
    ScriptValue result = ScriptNone();
    const char *message = "wrong arguments";
    if (!ScriptInvoke(state.host, binding, values, count, &result, &message))
        return s7_error(sc, s7_make_symbol(sc, "engine-error"),
                        s7_list(sc, 1, s7_make_string(sc, message)));
    return ToScheme(sc, result);
}

#define SCRIPT_BINDING(id, name, result, help, types)                                              \
    static s7_pointer S7_##id(s7_scheme *sc, s7_pointer args)                                      \
    {                                                                                              \
        return Dispatch(sc, SCRIPT_INDEX_##id, args);                                              \
    }
#include "script_api.def"
#undef SCRIPT_BINDING

// define-entity is sugar over the class-* rows, not an engine call of its own: the shape of a
// declaration is a language's business, and every frontend spells it its own way.
// One form, because that is what s7_eval_c_string evaluates.
static const char *prelude =
    "(begin\n"
    "(define (define-entity name fields events)\n"
    "  (class-new name)\n"
    "  (for-each (lambda (field) (class-field name (car field) (cadr field))) fields)\n"
    "  (for-each (lambda (event) (class-on name (car event) (cadr event))) events)\n"
    "  (class-done name))\n"
    "(define (vec x y) (list x y))\n"
    "(define (vec-x v) (car v))\n"
    "(define (vec-y v) (cadr v))\n"
    "(define (vec* v k) (list (* (car v) k) (* (cadr v) k)))\n"
    "(define (vec+ a b) (list (+ (car a) (car b)) (+ (cadr a) (cadr b))))\n"
    ")\n";

// A script that raises is reported and carried on from, rather than taking the program with it.
static bool raised;
static s7_pointer OnError(s7_scheme *sc, s7_pointer args)
{
    raised = true;
    char *text = s7_object_to_c_string(sc, args);
    TraceLog(LOG_ERROR, "Script: %s", text ? text : "error");
    free(text);
    return s7_nil(sc);
}

static bool CallFunction(void *user, const char *function)
{
    ScriptS7 *s7 = user;
    s7_pointer fn = s7_name_to_value(s7->scheme, function);
    if (!s7_is_procedure(fn))
    {
        TraceLog(LOG_ERROR, "Script: %s is not defined", function);
        return false;
    }
    raised = false;
    // A callback takes no arguments, so it is already the thunk catch wants.
    s7_call_with_catch(s7->scheme, s7_t(s7->scheme), fn,
                       s7_name_to_value(s7->scheme, "engine-on-error"));
    return !raised;
}

bool ScriptS7Open(ScriptHost *host)
{
    if (!host || state.scheme)
        return false;
    state.scheme = s7_init();
    if (!state.scheme)
        return false;
    state.host = host;
    state.language = (ScriptLanguage){"s7", &state, CallFunction};
    int count = 0;
    const ScriptBinding *table = ScriptBindings(&count);
    // The whole frontend: every row, registered the way Scheme wants it.
    static const s7_function trampolines[] = {
#define SCRIPT_BINDING(id, name, result, help, types) S7_##id,
#include "script_api.def"
#undef SCRIPT_BINDING
    };
    for (int i = 0; i < count; i++)
        s7_define_function(state.scheme, table[i].name, trampolines[i], table[i].argumentCount, 0,
                           false, table[i].help);
    s7_define_function(state.scheme, "engine-on-error", OnError, 0, 0, true,
                       "reports a script error and lets the engine carry on");
    s7_eval_c_string(state.scheme, prelude);
    ScriptHostUseLanguage(host, &state.language);
    return true;
}

bool ScriptS7Load(ScriptHost *host, const char *path)
{
    if (!state.scheme || !path)
        return false;
    (void)host;
    char resolved[512];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    if (!actual || !s7_load(state.scheme, actual))
    {
        TraceLog(LOG_ERROR, "Script: could not load %s", path);
        return false;
    }
    return true;
}

bool ScriptS7Eval(const char *text, char **answer)
{
    if (!state.scheme || !text)
        return false;
    raised = false;
    // Caught, so a mistake at the REPL reports itself instead of unwinding the engine, and
    // evaluated in the rootlet, so what it defines is defined for everyone rather than inside the
    // lambda the catch needs.
    char wrapped[1024];
    if ((size_t)snprintf(wrapped, sizeof wrapped,
                         "(catch #t (lambda () (eval (quote %s) (rootlet))) engine-on-error)",
                         text) >= sizeof wrapped)
        return false;
    s7_pointer result =
        s7_eval_c_string_with_environment(state.scheme, wrapped, s7_rootlet(state.scheme));
    if (answer)
        *answer = s7_object_to_c_string(state.scheme, result);
    return !raised;
}

// A REPL against the world as it runs: whatever has been typed is evaluated between frames, so a
// definition or a spawn takes effect in the world already on screen.
void ScriptS7Repl(ScriptHost *host)
{
    (void)host;
    if (!state.scheme)
        return;
    for (;;)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        struct timeval nothing = {0, 0};
        if (select(STDIN_FILENO + 1, &readable, NULL, NULL, &nothing) <= 0)
            return;
        char line[512];
        if (!fgets(line, sizeof line, stdin))
            return;
        if (strspn(line, " \t\r\n") == strlen(line))
            continue;
        char *answer = NULL;
        ScriptS7Eval(line, &answer);
        printf("%s\n> ", answer ? answer : "");
        fflush(stdout);
        free(answer);
    }
}

void ScriptS7Close(void)
{
    if (state.scheme)
        s7_free(state.scheme);
    state = (ScriptS7){0};
}
