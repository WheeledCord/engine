/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "iso_map_spawn.h"
#include <stdio.h>
#include <string.h>

#define MAX_FIELDS 32

/* Splits "a=1 b=\"two words\"" in place into key/value pairs, appending to properties. */
static int ParseFields(char *text, EntityProperty *properties, int count)
{
    char *p = text;
    while (*p && count < MAX_FIELDS)
    {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *key = p;
        while (*p && *p != '=' && *p != ' ')
            p++;
        if (*p != '=')
        {
            while (*p && *p != ' ')
                p++;
            continue; /* a word with no value says nothing */
        }
        *p++ = 0;
        char *value = p;
        if (*p == '"')
        {
            value = ++p;
            while (*p && *p != '"')
                p++;
        }
        else
            while (*p && *p != ' ')
                p++;
        if (*p)
            *p++ = 0;
        /* A later key replaces an earlier one: the object's own over its kind's. */
        int at = 0;
        while (at < count && strcmp(properties[at].key, key))
            at++;
        properties[at] = (EntityProperty){key, value};
        if (at == count)
            count++;
    }
    return count;
}

static bool TakesPosition(const EntityClass *type)
{
    for (size_t i = 0; i < type->fieldCount; i++)
        if (!strcmp(type->fields[i].name, "position"))
            return true;
    return type->KeyValue != NULL;
}

int IsoMapSpawnObjects(GameplayWorld *world, const IsoMap *map, const IsoTileset *tileset)
{
    int spawned = 0;
    for (int i = 0; world && map && tileset && i < map->objectCount; i++)
    {
        const IsoMapObject *object = &map->objects[i];
        if (object->kind < 0 || object->kind >= tileset->objectCount)
            continue;
        const IsoObjectKind *kind = &tileset->objects[object->kind];
        const EntityClass *type = EntityClassFind(world, kind->classname);
        if (!type)
        {
            TraceLog(LOG_WARNING, "ISO MAP: object %d is a %s, and no entity class %s is registered", i, kind->name,
                     kind->classname);
            continue;
        }
        EntityProperty properties[MAX_FIELDS];
        char hex[32], position[64], fields[2][ISO_MAP_PATH];
        snprintf(hex, sizeof hex, "%d %d", object->hex.x, object->hex.y);
        int count = 0;
        properties[count++] = (EntityProperty){"hex", hex};
        if (TakesPosition(type))
        {
            Vector2 at = IsoHexToScreen(object->hex);
            snprintf(position, sizeof position, "%g %g", (double)at.x, (double)at.y);
            properties[count++] = (EntityProperty){"position", position};
        }
        snprintf(fields[0], sizeof fields[0], "%s", kind->fields);
        snprintf(fields[1], sizeof fields[1], "%s", object->fields);
        count = ParseFields(fields[0], properties, count);
        count = ParseFields(fields[1], properties, count);
        if (EntityAlive(world, EntitySpawnWith(world, kind->classname, properties, (size_t)count)))
            spawned++;
        else
            TraceLog(LOG_WARNING, "ISO MAP: object %d (%s at %d,%d) did not spawn", i, kind->name, object->hex.x,
                     object->hex.y);
    }
    return spawned;
}
