/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_FIELDS_H
#define GAMEPLAY_FIELDS_H
#include "core/math.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum EntityFieldType
{
    ENTITY_FLOAT,
    ENTITY_DOUBLE,
    ENTITY_INT,
    ENTITY_BOOL,
    ENTITY_STRING,
    ENTITY_VECTOR2,
    ENTITY_VECTOR3
} EntityFieldType;

typedef struct EntityField
{
    const char *name;
    EntityFieldType type;
    size_t offset, size;
    bool ranged; // Numeric limits also apply to each vector component.
    double minimum, maximum;
} EntityField;

#define ENTITY_FIELD(Type, member, kind)                                                           \
    {.name = #member,                                                                              \
     .type = kind,                                                                                 \
     .offset = offsetof(Type, member),                                                             \
     .size = sizeof(((Type *)0)->member)}

bool EntityFieldsValid(const EntityField *fields, size_t count, size_t payloadSize);
/* Validates fully before writing. Strings never truncate; numeric input must be finite. */
bool EntityFieldSet(void *payload, const EntityField *field, const char *value);
#endif
