/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "net_sync.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static size_t FieldSize(CoreNetFieldType type)
{
    switch (type)
    {
        case CORE_NET_U8: return sizeof(uint8_t);
        case CORE_NET_BOOL: return sizeof(bool);
        case CORE_NET_I32: return sizeof(int32_t);
        case CORE_NET_U32: return sizeof(uint32_t);
        case CORE_NET_F32: return sizeof(float);
        case CORE_NET_VECTOR2: return sizeof(Vector2);
        case CORE_NET_VECTOR3: return sizeof(Vector3);
    }
    return 0;
}

static const CoreNetSchema *Schema(const CoreNetSync *sync, uint16_t type)
{
    if (!sync)
        return NULL;
    for (size_t i = 0; i < sync->schemaCount; i++)
        if (sync->schemas[i]->type == type)
            return sync->schemas[i];
    return NULL;
}

bool CoreNetSyncInit(CoreNetSync *sync, size_t objectCapacity, size_t schemaCapacity)
{
    if (!sync || sync->ready || !objectCapacity || objectCapacity > UINT16_MAX || !schemaCapacity)
        return false;
    CoreNetObject *objects = calloc(objectCapacity, sizeof *objects);
    const CoreNetSchema **schemas = calloc(schemaCapacity, sizeof *schemas);
    if (!objects || !schemas)
    {
        free(objects);
        free(schemas);
        return false;
    }
    *sync = (CoreNetSync){.schemas = schemas,
                         .schemaCapacity = schemaCapacity,
                         .objects = objects,
                         .objectCapacity = objectCapacity,
                         .nextId = 1,
                         .ready = true};
    return true;
}

static void FreeObject(CoreNetObject *object)
{
    if (!object)
        return;
    free(object->state);
    free(object->previous);
    *object = (CoreNetObject){0};
}

void CoreNetSyncFree(CoreNetSync *sync)
{
    if (!sync)
        return;
    if (sync->objects)
        for (size_t i = 0; i < sync->objectCapacity; i++)
            FreeObject(&sync->objects[i]);
    free(sync->objects);
    free(sync->schemas);
    *sync = (CoreNetSync){0};
}

bool CoreNetSyncRegister(CoreNetSync *sync, const CoreNetSchema *schema)
{
    if (!sync || !sync->ready || !schema || !schema->type || !schema->name || !schema->stateSize ||
        !schema->fields || !schema->fieldCount || sync->schemaCount == sync->schemaCapacity ||
        Schema(sync, schema->type))
        return false;
    for (size_t i = 0; i < schema->fieldCount; i++)
    {
        const CoreNetField *field = &schema->fields[i];
        size_t expected = FieldSize(field->type);
        if (!field->name || !expected || field->size != expected || field->offset > schema->stateSize ||
            expected > schema->stateSize - field->offset ||
            (field->flags & ~CORE_NET_FIELD_INTERPOLATED) ||
            ((field->flags & CORE_NET_FIELD_INTERPOLATED) && field->type != CORE_NET_F32 &&
             field->type != CORE_NET_VECTOR2 && field->type != CORE_NET_VECTOR3))
            return false;
    }
    sync->schemas[sync->schemaCount++] = schema;
    return true;
}

CoreNetObject *CoreNetSyncFind(CoreNetSync *sync, uint32_t id)
{
    if (!sync || !sync->ready || !id)
        return NULL;
    for (size_t i = 0; i < sync->objectCapacity; i++)
        if (sync->objects[i].active && sync->objects[i].id == id)
            return &sync->objects[i];
    return NULL;
}

CoreNetObject *CoreNetSyncSpawn(CoreNetSync *sync, uint32_t id, uint16_t schemaType, uint16_t owner)
{
    const CoreNetSchema *schema = Schema(sync, schemaType);
    if (!sync || !sync->ready || !id || !schema || sync->objectCount == sync->objectCapacity ||
        CoreNetSyncFind(sync, id))
        return NULL;
    for (size_t i = 0; i < sync->objectCapacity; i++)
    {
        CoreNetObject *object = &sync->objects[i];
        if (object->active)
            continue;
        object->state = calloc(1, schema->stateSize);
        object->previous = calloc(1, schema->stateSize);
        if (!object->state || !object->previous)
        {
            FreeObject(object);
            return NULL;
        }
        object->id = id;
        object->owner = owner;
        object->schema = schema;
        object->active = true;
        sync->objectCount++;
        if (id >= sync->nextId)
            sync->nextId = id == UINT32_MAX ? 0 : id + 1;
        return object;
    }
    return NULL;
}

CoreNetObject *CoreNetSyncSpawnNext(CoreNetSync *sync, uint16_t schemaType, uint16_t owner)
{
    if (!sync || !sync->nextId)
        return NULL;
    uint32_t id = sync->nextId;
    while (CoreNetSyncFind(sync, id))
    {
        if (id == UINT32_MAX)
            return NULL;
        id++;
    }
    return CoreNetSyncSpawn(sync, id, schemaType, owner);
}

bool CoreNetSyncDespawn(CoreNetSync *sync, uint32_t id)
{
    CoreNetObject *object = CoreNetSyncFind(sync, id);
    if (!object)
        return false;
    FreeObject(object);
    sync->objectCount--;
    return true;
}

bool CoreNetObjectCanWrite(const CoreNetObject *object, uint16_t actor, bool isServer)
{
    if (!object || !object->active || !object->schema)
        return false;
    return isServer ||
           (object->schema->authority == CORE_NET_AUTHORITY_OWNER && object->owner &&
            object->owner == actor);
}

static bool WriteField(CoreNetWriter *writer, const CoreNetField *field, const void *state)
{
    const unsigned char *at = (const unsigned char *)state + field->offset;
    switch (field->type)
    {
        case CORE_NET_U8: return CoreNetWriteU8(writer, *(const uint8_t *)at);
        case CORE_NET_BOOL: return CoreNetWriteU8(writer, *(const bool *)at ? 1 : 0);
        case CORE_NET_I32:
        {
            int32_t value;
            memcpy(&value, at, sizeof value);
            return CoreNetWriteU32(writer, (uint32_t)value);
        }
        case CORE_NET_U32:
        {
            uint32_t value;
            memcpy(&value, at, sizeof value);
            return CoreNetWriteU32(writer, value);
        }
        case CORE_NET_F32:
        {
            float value;
            memcpy(&value, at, sizeof value);
            return isfinite(value) && CoreNetWriteF32(writer, value);
        }
        case CORE_NET_VECTOR2:
        {
            Vector2 value;
            memcpy(&value, at, sizeof value);
            return isfinite(value.x) && isfinite(value.y) && CoreNetWriteF32(writer, value.x) &&
                   CoreNetWriteF32(writer, value.y);
        }
        case CORE_NET_VECTOR3:
        {
            Vector3 value;
            memcpy(&value, at, sizeof value);
            return isfinite(value.x) && isfinite(value.y) && isfinite(value.z) &&
                   CoreNetWriteF32(writer, value.x) && CoreNetWriteF32(writer, value.y) &&
                   CoreNetWriteF32(writer, value.z);
        }
    }
    return false;
}

bool CoreNetSyncWrite(const CoreNetSync *sync, uint32_t tick, CoreNetWriter *writer)
{
    if (!sync || !sync->ready || !writer || sync->objectCount > UINT16_MAX ||
        !CoreNetWriteU32(writer, CORE_NET_SYNC_MAGIC) || !CoreNetWriteU32(writer, tick) ||
        !CoreNetWriteU16(writer, (uint16_t)sync->objectCount))
        return false;
    for (size_t i = 0; i < sync->objectCapacity; i++)
    {
        const CoreNetObject *object = &sync->objects[i];
        if (!object->active)
            continue;
        if (!CoreNetWriteU32(writer, object->id) ||
            !CoreNetWriteU16(writer, object->schema->type) ||
            !CoreNetWriteU16(writer, object->owner))
            return false;
        for (size_t f = 0; f < object->schema->fieldCount; f++)
            if (!WriteField(writer, &object->schema->fields[f], object->state))
            {
                writer->failed = true;
                return false;
            }
    }
    return !writer->failed;
}

static bool ReadField(CoreNetReader *reader, const CoreNetField *field, void *state)
{
    unsigned char *at = state ? (unsigned char *)state + field->offset : NULL;
    uint8_t byte;
    uint32_t integer;
    float values[3];
    switch (field->type)
    {
        case CORE_NET_U8:
            if (!CoreNetReadU8(reader, &byte)) return false;
            if (at) memcpy(at, &byte, sizeof byte);
            return true;
        case CORE_NET_BOOL:
        {
            if (!CoreNetReadU8(reader, &byte) || byte > 1) return false;
            bool value = byte != 0;
            if (at) memcpy(at, &value, sizeof value);
            return true;
        }
        case CORE_NET_I32:
            if (!CoreNetReadU32(reader, &integer)) return false;
            if (at)
            {
                int32_t value = (int32_t)integer;
                memcpy(at, &value, sizeof value);
            }
            return true;
        case CORE_NET_U32:
            if (!CoreNetReadU32(reader, &integer)) return false;
            if (at) memcpy(at, &integer, sizeof integer);
            return true;
        case CORE_NET_F32:
            if (!CoreNetReadF32(reader, &values[0]) || !isfinite(values[0])) return false;
            if (at) memcpy(at, &values[0], sizeof(float));
            return true;
        case CORE_NET_VECTOR2:
        {
            if (!CoreNetReadF32(reader, &values[0]) || !CoreNetReadF32(reader, &values[1]) ||
                !isfinite(values[0]) || !isfinite(values[1])) return false;
            Vector2 value = {values[0], values[1]};
            if (at) memcpy(at, &value, sizeof value);
            return true;
        }
        case CORE_NET_VECTOR3:
        {
            if (!CoreNetReadF32(reader, &values[0]) || !CoreNetReadF32(reader, &values[1]) ||
                !CoreNetReadF32(reader, &values[2]) || !isfinite(values[0]) ||
                !isfinite(values[1]) || !isfinite(values[2])) return false;
            Vector3 value = {values[0], values[1], values[2]};
            if (at) memcpy(at, &value, sizeof value);
            return true;
        }
    }
    return false;
}

bool CoreNetObjectWrite(const CoreNetObject *object, CoreNetWriter *writer)
{
    if (!object || !object->active || !object->schema || !object->state || !writer ||
        !CoreNetWriteU32(writer, CORE_NET_OBJECT_MAGIC) ||
        !CoreNetWriteU32(writer, object->id) ||
        !CoreNetWriteU16(writer, object->schema->type) ||
        !CoreNetWriteU16(writer, object->owner))
        return false;
    for (size_t i = 0; i < object->schema->fieldCount; i++)
        if (!WriteField(writer, &object->schema->fields[i], object->state))
        {
            writer->failed = true;
            return false;
        }
    return !writer->failed;
}

bool CoreNetSyncReadObject(CoreNetSync *sync, CoreNetReader *reader, uint16_t actor, bool isServer)
{
    if (!sync || !sync->ready || !reader || reader->failed)
        return false;
    CoreNetReader scan = *reader;
    uint32_t magic, id;
    uint16_t schemaType, owner;
    if (!CoreNetReadU32(&scan, &magic) || magic != CORE_NET_OBJECT_MAGIC ||
        !CoreNetReadU32(&scan, &id) || !CoreNetReadU16(&scan, &schemaType) ||
        !CoreNetReadU16(&scan, &owner))
        goto invalid;

    CoreNetObject *object = CoreNetSyncFind(sync, id);
    if (!object || object->schema->type != schemaType || object->owner != owner ||
        !CoreNetObjectCanWrite(object, actor, isServer))
        goto invalid;

    void *incoming = calloc(1, object->schema->stateSize);
    if (!incoming)
        return false;
    for (size_t i = 0; i < object->schema->fieldCount; i++)
        if (!ReadField(&scan, &object->schema->fields[i], incoming))
        {
            free(incoming);
            goto invalid;
        }
    if (scan.at != scan.size)
    {
        free(incoming);
        goto invalid;
    }

    memcpy(object->previous, object->state, object->schema->stateSize);
    memcpy(object->state, incoming, object->schema->stateSize);
    object->hasPrevious = true;
    free(incoming);
    *reader = scan;
    return true;

invalid:
    reader->failed = true;
    return false;
}

static void FreeTemporary(CoreNetObject *objects, size_t count)
{
    if (!objects)
        return;
    for (size_t i = 0; i < count; i++)
        FreeObject(&objects[i]);
    free(objects);
}

bool CoreNetSyncRead(CoreNetSync *sync, CoreNetReader *reader, uint32_t *tick)
{
    if (!sync || !sync->ready || !reader || reader->failed)
        return false;
    CoreNetReader scan = *reader;
    uint32_t magic, incomingTick;
    uint16_t count;
    if (!CoreNetReadU32(&scan, &magic) || magic != CORE_NET_SYNC_MAGIC ||
        !CoreNetReadU32(&scan, &incomingTick) || !CoreNetReadU16(&scan, &count) ||
        count > sync->objectCapacity)
    {
        reader->failed = true;
        return false;
    }
    CoreNetObject *incoming = calloc(count ? count : 1, sizeof *incoming);
    if (!incoming)
        return false;
    for (size_t i = 0; i < count; i++)
    {
        uint16_t schemaType;
        if (!CoreNetReadU32(&scan, &incoming[i].id) || !incoming[i].id ||
            !CoreNetReadU16(&scan, &schemaType) || !CoreNetReadU16(&scan, &incoming[i].owner))
            goto invalid;
        incoming[i].schema = Schema(sync, schemaType);
        if (!incoming[i].schema)
            goto invalid;
        for (size_t previous = 0; previous < i; previous++)
            if (incoming[previous].id == incoming[i].id)
                goto invalid;
        incoming[i].state = calloc(1, incoming[i].schema->stateSize);
        incoming[i].previous = calloc(1, incoming[i].schema->stateSize);
        if (!incoming[i].state || !incoming[i].previous)
            goto invalid;
        for (size_t f = 0; f < incoming[i].schema->fieldCount; f++)
            if (!ReadField(&scan, &incoming[i].schema->fields[f], incoming[i].state))
                goto invalid;
        CoreNetObject *old = CoreNetSyncFind(sync, incoming[i].id);
        if (old && old->schema == incoming[i].schema)
        {
            memcpy(incoming[i].previous, old->state, incoming[i].schema->stateSize);
            incoming[i].hasPrevious = true;
        }
        else
            memcpy(incoming[i].previous, incoming[i].state, incoming[i].schema->stateSize);
        incoming[i].active = true;
    }
    if (scan.at != scan.size)
        goto invalid;

    for (size_t i = 0; i < sync->objectCapacity; i++)
        FreeObject(&sync->objects[i]);
    memset(sync->objects, 0, sync->objectCapacity * sizeof *sync->objects);
    sync->objectCount = count;
    sync->nextId = 1;
    for (size_t i = 0; i < count; i++)
    {
        sync->objects[i] = incoming[i];
        incoming[i] = (CoreNetObject){0};
        if (sync->objects[i].id >= sync->nextId)
            sync->nextId = sync->objects[i].id == UINT32_MAX ? 0 : sync->objects[i].id + 1;
    }
    free(incoming);
    *reader = scan;
    if (tick)
        *tick = incomingTick;
    return true;

invalid:
    FreeTemporary(incoming, count);
    reader->failed = true;
    return false;
}

bool CoreNetObjectSample(const CoreNetObject *object, float alpha, void *outState)
{
    if (!object || !object->active || !object->schema || !object->state || !outState)
        return false;
    memcpy(outState, object->state, object->schema->stateSize);
    if (!object->hasPrevious)
        return true;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    for (size_t i = 0; i < object->schema->fieldCount; i++)
    {
        const CoreNetField *field = &object->schema->fields[i];
        if (!(field->flags & CORE_NET_FIELD_INTERPOLATED))
            continue;
        const unsigned char *before = (const unsigned char *)object->previous + field->offset;
        const unsigned char *after = (const unsigned char *)object->state + field->offset;
        unsigned char *out = (unsigned char *)outState + field->offset;
        if (field->type == CORE_NET_F32)
        {
            float a, b, value;
            memcpy(&a, before, sizeof a); memcpy(&b, after, sizeof b);
            value = a + (b - a) * alpha;
            memcpy(out, &value, sizeof value);
        }
        else if (field->type == CORE_NET_VECTOR2)
        {
            Vector2 a, b, value;
            memcpy(&a, before, sizeof a); memcpy(&b, after, sizeof b);
            value = (Vector2){a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha};
            memcpy(out, &value, sizeof value);
        }
        else if (field->type == CORE_NET_VECTOR3)
        {
            Vector3 a, b, value;
            memcpy(&a, before, sizeof a); memcpy(&b, after, sizeof b);
            value = (Vector3){a.x + (b.x - a.x) * alpha, a.y + (b.y - a.y) * alpha,
                              a.z + (b.z - a.z) * alpha};
            memcpy(out, &value, sizeof value);
        }
    }
    return true;
}
