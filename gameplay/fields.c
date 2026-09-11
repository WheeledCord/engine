/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "fields.h"
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

bool EntityFieldsValid(const EntityField *fields, size_t count, size_t payloadSize)
{
    if (count && !fields)
        return false;
    const size_t sizes[] = {sizeof(float),   sizeof(double), sizeof(int), sizeof(bool), 0,
                            sizeof(Vector2), sizeof(Vector3)};
    for (size_t i = 0; i < count; i++)
    {
        const EntityField *f = fields + i;
        if (!f->name || !*f->name || f->type < ENTITY_FLOAT || f->type > ENTITY_VECTOR3 ||
            !f->size || f->offset > payloadSize || f->size > payloadSize - f->offset ||
            (sizes[f->type] && sizes[f->type] != f->size) ||
            (f->ranged &&
             (!isfinite(f->minimum) || !isfinite(f->maximum) || f->minimum > f->maximum)))
            return false;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(f->name, fields[j].name))
                return false;
    }
    return true;
}

static const char *Space(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return s;
}

bool EntityFieldSet(void *payload, const EntityField *f, const char *text)
{
    if (!payload || !f || !text)
        return false;
    unsigned char *out = (unsigned char *)payload + f->offset;
    if (f->type == ENTITY_STRING)
    {
        size_t n = strlen(text);
        if (n >= f->size)
            return false;
        memcpy(out, text, n + 1);
        return true;
    }
    if (f->type == ENTITY_BOOL)
    {
        bool value;
        if (!strcmp(text, "true") || !strcmp(text, "1"))
            value = true;
        else if (!strcmp(text, "false") || !strcmp(text, "0"))
            value = false;
        else
            return false;
        memcpy(out, &value, sizeof value);
        return true;
    }
    int count = f->type == ENTITY_VECTOR3 ? 3 : f->type == ENTITY_VECTOR2 ? 2 : 1;
    double numbers[3] = {0};
    const char *cursor = Space(text);
    for (int i = 0; i < count; i++)
    {
        char *end;
        errno = 0;
        numbers[i] = strtod(cursor, &end);
        if (end == cursor || errno == ERANGE || !isfinite(numbers[i]) ||
            (f->ranged && (numbers[i] < f->minimum || numbers[i] > f->maximum)))
            return false;
        if (i + 1 < count && !isspace((unsigned char)*end))
            return false;
        cursor = Space(end);
    }
    if (*cursor)
        return false;
    if (f->type == ENTITY_DOUBLE)
        memcpy(out, numbers, sizeof(double));
    else if (f->type == ENTITY_INT)
    {
        if (numbers[0] < INT_MIN || numbers[0] > INT_MAX || trunc(numbers[0]) != numbers[0])
            return false;
        int value = (int)numbers[0];
        memcpy(out, &value, sizeof value);
    }
    else
    {
        float values[3];
        for (int i = 0; i < count; i++)
        {
            if (fabs(numbers[i]) > FLT_MAX)
                return false;
            values[i] = (float)numbers[i];
        }
        if (f->type == ENTITY_VECTOR2)
        {
            Vector2 value = {values[0], values[1]};
            memcpy(out, &value, sizeof value);
        }
        else if (f->type == ENTITY_VECTOR3)
        {
            Vector3 value = {values[0], values[1], values[2]};
            memcpy(out, &value, sizeof value);
        }
        else if (f->type == ENTITY_FLOAT)
            memcpy(out, values, sizeof(float));
        else
            return false;
    }
    return true;
}
