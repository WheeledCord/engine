/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Writes the reference for every engine object type a script can make, from the type tables
   themselves, so the reference cannot drift from what the engine actually has. */
#include "gameplay/script/script.h"
#include <stdio.h>
#include <string.h>

static const char *Flags(unsigned int flags)
{
    static char text[64];
    text[0] = '\0';
    if (flags & ENGINE_PROPERTY_READ_ONLY)
        strcat(text, "read-only ");
    if (flags & ENGINE_PROPERTY_SCENE)
        strcat(text, "scene ");
    if (flags & ENGINE_PROPERTY_SAVE)
        strcat(text, "saved ");
    size_t length = strlen(text);
    if (length)
        text[length - 1] = '\0';
    return text;
}

static void Arguments(FILE *out, const EngineValueType *types, int count, int required)
{
    for (int i = 0; i < count; i++)
        fprintf(out, " %s%s%s", i >= required ? "[" : "", EngineValueTypeName(types[i]),
                i >= required ? "]" : "");
}

static void Describe(FILE *out, const EngineType *type)
{
    const char *help = type->help ? type->help : "";
    fprintf(out, "## %s\n\n%c%s.", type->name, help[0] >= 'a' && help[0] <= 'z' ? help[0] - 32 : help[0],
            help[0] ? help + 1 : "");
    if (type->parent)
        fprintf(out, " Everything a %s has, it has too.", type->parent->name);
    fprintf(out, "\n\n");
    if (type->size && (type->create || type->createArgumentCount))
    {
        fprintf(out, "Made with `(make '%s", type->name);
        Arguments(out, type->createArguments, type->createArgumentCount, type->createRequired);
        fprintf(out, ")`.\n\n");
    }
    if (type->propertyCount)
    {
        fprintf(out, "| Property | Type | | |\n| --- | --- | --- | --- |\n");
        for (int i = 0; i < type->propertyCount; i++)
        {
            const EngineProperty *p = &type->properties[i];
            fprintf(out, "| `%s` | %s | %s | %s |\n", p->name, EngineValueTypeName(p->type),
                    Flags(p->flags), p->help ? p->help : "");
        }
        fprintf(out, "\n");
    }
    if (type->methodCount)
    {
        fprintf(out, "| Method | Answers | |\n| --- | --- | --- |\n");
        for (int i = 0; i < type->methodCount; i++)
        {
            const EngineMethod *m = &type->methods[i];
            fprintf(out, "| `(obj '%s", m->name);
            Arguments(out, m->arguments, m->argumentCount, m->argumentCount);
            fprintf(out, ")` | %s | %s |\n", EngineValueTypeName(m->result), m->help ? m->help : "");
        }
        fprintf(out, "\n");
    }
    for (int i = 0; i < type->signalCount; i++)
        fprintf(out, "%s `%s`", i ? "," : "Signals:", type->signals[i]);
    if (type->signalCount)
        fprintf(out, ". Connect with `(connect! obj '%s procedure)`.\n\n", type->signals[0]);
}

int main(int argc, char **argv)
{
    FILE *out = argc > 1 ? fopen(argv[1], "w") : stdout;
    GameplayWorld world = {0};
    ScriptHost host;
    if (!out || !GameplayWorldInit(&world, (GameplayWorldConfig){1, 0.1}) ||
        !ScriptHostInit(&host, &world))
        return 1;
    fprintf(out, "# Engine objects a script can use\n\n"
                 "Written from the engine's type tables by `tools/describe_types`; do not edit.\n\n"
                 "`(obj 'property)` reads, `(set! (obj 'property) value)` writes, `(obj 'method args...)`\n"
                 "calls, and `(free! obj)` ends an object. `(properties obj)` and `(methods obj)` list what\n"
                 "one has.\n\n");
    Describe(out, &ScriptEntityType);
    for (size_t i = 0; i < host.objects.typeCount; i++)
        Describe(out, host.objects.types[i]);
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
    if (out != stdout)
        fclose(out);
    return 0;
}
