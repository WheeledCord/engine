/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "scene.h"

#include "core/file.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Token returns NULL both at the end and on a bad token, so it says which through this flag.
typedef struct SceneReader
{
    const char *cursor;
    const char *end;
    int line;
    bool failed;
} SceneReader;

static void SkipSpace(SceneReader *reader)
{
    for (;;)
    {
        while (reader->cursor < reader->end && isspace((unsigned char)*reader->cursor))
        {
            if (*reader->cursor++ == '\n')
                reader->line++;
        }
        if (reader->cursor < reader->end && *reader->cursor == '#')
        {
            while (reader->cursor < reader->end && *reader->cursor != '\n')
                reader->cursor++;
            continue;
        }
        if (reader->cursor + 1 < reader->end && reader->cursor[0] == '/' && reader->cursor[1] == '/')
        {
            reader->cursor += 2;
            while (reader->cursor < reader->end && *reader->cursor != '\n')
                reader->cursor++;
            continue;
        }
        return;
    }
}

static char *Token(SceneReader *reader)
{
    SkipSpace(reader);
    if (reader->cursor >= reader->end)
        return NULL;
    if (*reader->cursor == '{' || *reader->cursor == '}')
    {
        char *token = malloc(2);
        if (token)
        {
            token[0] = *reader->cursor++;
            token[1] = 0;
        }
        return token;
    }
    const char *start = reader->cursor;
    bool quoted = *reader->cursor == '"';
    if (quoted)
        start = ++reader->cursor;
    size_t capacity = 32, length = 0;
    bool closed = false;
    char *token = malloc(capacity);
    if (!token)
    {
        reader->failed = true;
        return NULL;
    }
    while (reader->cursor < reader->end)
    {
        char character = *reader->cursor++;
        if (quoted)
        {
            if (character == '"')
            {
                closed = true;
                break;
            }
            if (character == '\\' && reader->cursor < reader->end)
            {
                character = *reader->cursor++;
                if (character == 'n')
                    character = '\n';
            }
        }
        else if (isspace((unsigned char)character) || character == '{' || character == '}')
        {
            reader->cursor--;
            break;
        }
        if (length + 1 >= capacity)
        {
            capacity *= 2;
            char *grown = realloc(token, capacity);
            if (!grown)
            {
                reader->failed = true;
                free(token);
                return NULL;
            }
            token = grown;
        }
        token[length++] = character;
    }
    if (quoted && !closed)
    {
        reader->failed = true;
        free(token);
        return NULL;
    }
    (void)start;
    token[length] = 0;
    return token;
}

static bool Expect(SceneReader *reader, const char *expected)
{
    char *token = Token(reader);
    bool matches = token && !strcmp(token, expected);
    free(token);
    return matches;
}

bool GameplaySceneLoad(GameplayWorld *world, const char *path, bool replaceWorld)
{
    if (!world || !path) return false;
    char *text = CoreReadFile(path);
    if (!text)
        return false;
    if (replaceWorld)
        GameplayWorldClear(world);
    SceneReader reader = {text, text + strlen(text), 1, false};
    bool ok = true;
    for (;;)
    {
        char *kind = Token(&reader);
        if (!kind)
        {
            if (reader.failed)
            {
                fprintf(stderr, "%s:%d: unterminated or unreadable token\n", path, reader.line);
                ok = false;
            }
            break;
        }
        if (strcmp(kind, "entity"))
        {
            fprintf(stderr, "%s:%d: expected 'entity'\n", path, reader.line);
            free(kind);
            ok = false;
            break;
        }
        free(kind);
        char *classname = Token(&reader);
        if (!classname || !Expect(&reader, "{"))
        {
            fprintf(stderr, "%s:%d: entity requires classname and '{'\n", path, reader.line);
            free(classname);
            ok = false;
            break;
        }
        EntityProperty *properties = NULL;
        size_t count = 0;
        for (;;)
        {
            char *key = Token(&reader);
            if (!key)
            {
                fprintf(stderr, "%s:%d: unterminated entity\n", path, reader.line);
                ok = false;
                break;
            }
            if (!strcmp(key, "}"))
            {
                free(key);
                break;
            }
            char *value = Token(&reader);
            EntityProperty *grown = value ? realloc(properties, (count + 1) * sizeof(*properties)) : NULL;
            if (!grown)
            {
                fprintf(stderr, "%s:%d: invalid key/value for %s\n", path, reader.line, key);
                free(key);
                free(value);
                ok = false;
                break;
            }
            properties = grown;
            properties[count++] = (EntityProperty){key, value};
        }
        if (ok)
        {
            EntityHandle entity = EntitySpawnWith(world, classname, properties, count);
            ok = EntityAlive(world, entity);
            if (!ok) fprintf(stderr, "%s:%d: could not spawn %s (class, properties, Spawn or capacity)\n",
                             path, reader.line, classname);
        }
        for (size_t i = 0; i < count; i++)
        {
            free((char *)properties[i].key);
            free((char *)properties[i].value);
        }
        free(properties);
        free(classname);
        if (!ok)
            break;
    }
    CoreFreeFile(text);
    return ok;
}

static bool Quote(FILE *file, const char *text)
{
    if (fputc('"', file) == EOF)
        return false;
    for (; *text; text++)
    {
        if (*text == '"' || *text == '\\')
            if (fputc('\\', file) == EOF)
                return false;
        if (*text == '\n')
        {
            if (fputs("\\n", file) == EOF)
                return false;
        }
        else if (fputc(*text, file) == EOF)
            return false;
    }
    return fputc('"', file) != EOF;
}

bool GameplaySceneWrite(const GameplayWorld *world, const char *path)
{
    if (!world || !path)
        return false;
    CoreAtomicFile atomic;
    FILE *file = CoreAtomicBegin(&atomic, path);
    if (!file)
        return false;
    bool ok = true;
    for (size_t i = 0; ok && i < world->maxEntities; i++)
    {
        const GameplayEntity *slot = &world->entities[i];
        if (!slot->alive)
            continue;
        ok =
            fputs("entity ", file) != EOF && Quote(file, slot->type->classname) && fputs(" {\n", file) != EOF;
        for (size_t kv = 0; ok && kv < slot->keyValueCount; kv++)
            ok = fputs("  ", file) != EOF && Quote(file, slot->keyValues[kv].key) &&
                 fputc(' ', file) != EOF && Quote(file, slot->keyValues[kv].value) &&
                 fputc('\n', file) != EOF;
        ok = ok && fputs("}\n\n", file) != EOF;
    }
    return CoreAtomicCommit(&atomic, ok);
}
