/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// sclinux.h wants the byte order macros, which strict C99 hides.
#define _DEFAULT_SOURCE
#include "script_pawn.h"

#include "amx.h"
#include "amxaux.h"
int AMXAPI amx_FloatInit(AMX *amx); // from the vendored float module, which has no header
#include "core/file.h"
#include <stdio.h>
#include <string.h>

// Pawn carries a float in a cell as its bits. amx_ctof and amx_ftoc do that by punning a pointer,
// which this build refuses; the conversion is the same, without the aliasing.
typedef char PawnCellIsTheSizeOfAFloat[sizeof(cell) == sizeof(float) ? 1 : -1];
static float CellToFloat(cell value)
{
    float number;
    memcpy(&number, &value, sizeof number);
    return number;
}
static cell FloatToCell(float number)
{
    cell value;
    memcpy(&value, &number, sizeof value);
    return value;
}

/* The Pawn frontend. Like the Scheme one, it names no engine function: the trampolines come from
   the same declaration file and registering them is one loop. What differs is only what Pawn can
   say — its identifiers have no dashes or question marks, and a native returns a single cell — so
   this file holds the rules for spelling a row's name and for carrying values in and out. */

typedef struct ScriptPawn
{
    AMX amx;
    void *program;
    ScriptHost *host;
    ScriptLanguage language;
    AMX_NATIVE_INFO natives[128];
    char names[128][48];
    bool loaded;
} ScriptPawn;

static ScriptPawn state;

#define SCRIPT_BINDING(id, name, result, help, types) SCRIPT_INDEX_##id,
enum
{
#include "script_api.def"
    SCRIPT_INDEX_COUNT
};
#undef SCRIPT_BINDING

// Pawn identifiers carry neither dashes nor the marks Scheme puts on questions and changes, so a
// row's name is spelled the way Pawn can say it: move-world! is move_world, alive? is alive.
void ScriptPawnName(const char *name, char *out, size_t capacity)
{
    size_t at = 0;
    for (const char *c = name; *c && at + 1 < capacity; c++)
    {
        if (*c == '!' || *c == '?')
            continue;
        out[at++] = *c == '-' ? '_' : *c;
    }
    out[at] = '\0';
}

// How many cells a value of this type takes: Pawn has no compound values, so a vector is its
// components side by side.
static int Cells(ScriptType type)
{
    if (type == SCRIPT_VECTOR2)
        return 2;
    if (type == SCRIPT_VECTOR3)
        return 3;
    return 1;
}

static cell Dispatch(AMX *amx, int index, const cell *params)
{
    const ScriptBinding *binding = &ScriptBindings(NULL)[index];
    ScriptValue values[8];
    char strings[4][256];
    int used = 0;
    int at = 1; // params[0] is the byte count
    int given = (int)(params[0] / (cell)sizeof(cell));
    for (int i = 0; i < binding->argumentCount; i++)
    {
        ScriptType type = binding->arguments[i];
        if (at + Cells(type) - 1 > given)
        {
            amx_RaiseError(amx, AMX_ERR_NATIVE);
            return 0;
        }
        switch (type)
        {
            case SCRIPT_FLOAT:
                values[i] = ScriptFloat(CellToFloat(params[at]));
                break;
            case SCRIPT_BOOL:
                values[i] = ScriptBool(params[at] != 0);
                break;
            case SCRIPT_ENTITY:
                values[i] = ScriptHandle((int)params[at]);
                break;
            case SCRIPT_VECTOR2:
                values[i] = ScriptVector2(
                    (Vector2){CellToFloat(params[at]), CellToFloat(params[at + 1])});
                break;
            case SCRIPT_VECTOR3:
                values[i] = ScriptVector3((Vector3){CellToFloat(params[at]), CellToFloat(params[at + 1]),
                                                    CellToFloat(params[at + 2])});
                break;
            case SCRIPT_STRING:
            {
                cell *address = amx_Address(amx, params[at]);
                int length = 0;
                if (!address || used == 4)
                {
                    amx_RaiseError(amx, AMX_ERR_NATIVE);
                    return 0;
                }
                amx_StrLen(address, &length);
                if (length >= (int)sizeof strings[0])
                    length = (int)sizeof strings[0] - 1;
                amx_GetString(strings[used], address, 0, (size_t)length + 1);
                values[i] = ScriptString(strings[used++]);
                break;
            }
            default:
                values[i] = ScriptInt((int)params[at]);
                break;
        }
        at += Cells(type);
    }
    ScriptValue result = ScriptNone();
    const char *message = "wrong arguments";
    if (!ScriptInvoke(state.host, binding, values, binding->argumentCount, &result, &message))
    {
        TraceLog(LOG_ERROR, "Script: %s", message);
        amx_RaiseError(amx, AMX_ERR_NATIVE);
        return 0;
    }
    switch (result.type)
    {
        case SCRIPT_FLOAT:
            return FloatToCell(result.as.number);
        case SCRIPT_BOOL:
            return result.as.boolean ? 1 : 0;
        case SCRIPT_ENTITY:
            return result.as.entity;
        case SCRIPT_VECTOR2:
        case SCRIPT_VECTOR3:
        {
            // A native answers with one cell, so a vector comes back through the reference
            // arguments the script passes after the others.
            int count = Cells(result.type);
            const float *components = result.type == SCRIPT_VECTOR2
                                          ? (const float *)&result.as.vector2
                                          : (const float *)&result.as.vector3;
            for (int i = 0; i < count; i++)
            {
                if (at + i > given)
                    return 0;
                cell *slot = amx_Address(amx, params[at + i]);
                if (!slot)
                    return 0;
                *slot = FloatToCell(components[i]);
            }
            return 1;
        }
        case SCRIPT_NONE:
            return 0;
        default:
            return result.as.integer;
    }
}

#define SCRIPT_BINDING(id, name, result, help, types)                                              \
    static cell AMX_NATIVE_CALL Pawn_##id(AMX *amx, const cell *params)                            \
    {                                                                                              \
        return Dispatch(amx, SCRIPT_INDEX_##id, params);                                           \
    }
#include "script_api.def"
#undef SCRIPT_BINDING

static bool CallFunction(void *user, const char *function)
{
    ScriptPawn *pawn = user;
    int index = 0;
    if (amx_FindPublic(&pawn->amx, function, &index) != AMX_ERR_NONE)
    {
        TraceLog(LOG_ERROR, "Script: public %s is not in the script", function);
        return false;
    }
    int error = amx_Exec(&pawn->amx, NULL, index);
    if (error != AMX_ERR_NONE)
    {
        TraceLog(LOG_ERROR, "Script: %s failed (%s)", function, aux_StrError(error));
        return false;
    }
    return true;
}

bool ScriptPawnOpen(ScriptHost *host, const char *path)
{
    if (!host || !path || state.loaded)
        return false;
    char resolved[512];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    int error = actual ? aux_LoadProgram(&state.amx, (char *)actual, NULL) : AMX_ERR_NOTFOUND;
    if (error != AMX_ERR_NONE)
    {
        TraceLog(LOG_ERROR, "Script: could not load %s (%s)", path, aux_StrError(error));
        return false;
    }
    state.host = host;
    state.language = (ScriptLanguage){"pawn", &state, CallFunction};
    int count = 0;
    const ScriptBinding *table = ScriptBindings(&count);
    if (count >= (int)(sizeof state.natives / sizeof state.natives[0]))
        return false;
    // The whole frontend: every row, registered the way Pawn wants it.
    for (int i = 0; i < count; i++)
    {
        ScriptPawnName(table[i].name, state.names[i], sizeof state.names[i]);
        state.natives[i].name = state.names[i];
        state.natives[i].func = NULL;
    }
    static const AMX_NATIVE trampolines[] = {
#define SCRIPT_BINDING(id, name, result, help, types) Pawn_##id,
#include "script_api.def"
#undef SCRIPT_BINDING
    };
    for (int i = 0; i < count; i++)
        state.natives[i].func = trampolines[i];
    state.natives[count].name = NULL;
    state.natives[count].func = NULL;
    // Pawn's arithmetic on Float: values is itself a set of natives, and comes with the VM.
    amx_FloatInit(&state.amx);
    if (amx_Register(&state.amx, state.natives, -1) != AMX_ERR_NONE)
    {
        TraceLog(LOG_ERROR, "Script: %s asks for a native the engine does not have", path);
        aux_FreeProgram(&state.amx);
        return false;
    }
    ScriptHostUseLanguage(host, &state.language);
    state.loaded = true;
    // main() is where a Pawn script declares its classes, so it runs before any scene is read.
    int result = amx_Exec(&state.amx, NULL, AMX_EXEC_MAIN);
    if (result != AMX_ERR_NONE && result != AMX_ERR_INDEX)
    {
        TraceLog(LOG_ERROR, "Script: %s failed in main (%s)", path, aux_StrError(result));
        return false;
    }
    return true;
}

void ScriptPawnClose(void)
{
    if (state.loaded)
        aux_FreeProgram(&state.amx);
    state = (ScriptPawn){0};
}

/* Pawn needs every native declared before a script can call one. The declarations are written from
   the same table, so they cannot fall behind it: a row added to script_api.def turns up here on the
   next build. */
bool ScriptPawnWriteInclude(const char *path)
{
    FILE *file = fopen(path, "wb");
    if (!file)
        return false;
    fprintf(file, "/* Written from the engine's binding table. Do not edit: it is regenerated by\n"
                  "   the build, and anything changed here would be lost and out of step. */\n"
                  "#if defined _engine_included\n  #endinput\n#endif\n"
                  "#define _engine_included\n\n#include <float>\n\n");
    int count = 0;
    const ScriptBinding *table = ScriptBindings(&count);
    for (int i = 0; i < count; i++)
    {
        char name[48];
        ScriptPawnName(table[i].name, name, sizeof name);
        const char *tag = table[i].result == SCRIPT_FLOAT ? "Float:" : "";
        fprintf(file, "// %s\nnative %s%s(", table[i].help, tag, name);
        const char *separator = "";
        for (int argument = 0; argument < table[i].argumentCount; argument++)
        {
            ScriptType type = table[i].arguments[argument];
            static const char *axes[] = {"x", "y", "z"};
            for (int cell = 0; cell < Cells(type); cell++)
            {
                if (type == SCRIPT_VECTOR2 || type == SCRIPT_VECTOR3)
                    fprintf(file, "%sFloat:a%d%s", separator, argument, axes[cell]);
                else if (type == SCRIPT_FLOAT)
                    fprintf(file, "%sFloat:a%d", separator, argument);
                else if (type == SCRIPT_STRING)
                    fprintf(file, "%sconst a%d[]", separator, argument);
                else
                    fprintf(file, "%sa%d", separator, argument);
                separator = ", ";
            }
        }
        // A vector answer comes back through references, because a native returns one cell.
        if (table[i].result == SCRIPT_VECTOR2 || table[i].result == SCRIPT_VECTOR3)
        {
            static const char *axes[] = {"x", "y", "z"};
            for (int cell = 0; cell < Cells(table[i].result); cell++)
            {
                fprintf(file, "%s&Float:out%s", separator, axes[cell]);
                separator = ", ";
            }
        }
        fprintf(file, ");\n\n");
    }
    bool ok = !ferror(file);
    return fclose(file) == 0 && ok;
}
