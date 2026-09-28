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

/* One rule for "is this mine," used both when a snapshot arrives and when deciding which way to
   serialize -- so a listen server's client half and server half can never disagree about one of
   its own objects. Server authority follows localIsServer; owner authority follows localActor,
   and owner zero means the room or the server, never a player. */
static bool LocallyOwned(const CoreNetSync *sync, const CoreNetObject *object)
{
    if (!sync || !object || !object->active || !object->schema)
        return false;
    if (object->schema->authority == CORE_NET_AUTHORITY_SERVER)
        return sync->localIsServer;
    return object->owner != 0 && object->owner == sync->localActor;
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
    /* Bound state belongs to the game -- it is the variable the game reads every frame, not a copy
       -- so the registry never frees it. */
    if (object->ownsState)
        free(object->state);
    free(object->previous);
    *object = (CoreNetObject){0};
}

/* ---- remembered snapshots, so a delta can name the baseline it was built from ---- */

static CoreNetRemembered *HistorySlot(CoreNetSync *sync, uint32_t tick)
{
    return &sync->history[tick % CORE_NET_SNAPSHOT_BACKUP];
}

static const CoreNetRemembered *HistoryFind(const CoreNetSync *sync, uint32_t tick)
{
    const CoreNetRemembered *slot = &sync->history[tick % CORE_NET_SNAPSHOT_BACKUP];
    /* The ring is indexed by tick, so a slot either holds the tick asked for or has been displaced
       by a newer one. Checking the tick is what distinguishes those. */
    return (slot->used && slot->tick == tick) ? slot : NULL;
}

static void FreeRemembered(CoreNetRemembered *slot)
{
    if (slot->objects)
        for (uint16_t i = 0; i < slot->count; i++)
            free(slot->objects[i].state);
    free(slot->objects);
    slot->objects = NULL;
    slot->count = 0;
    slot->used = false;
}

static const void *RememberedState(const CoreNetRemembered *slot, uint32_t id, uint16_t type)
{
    if (!slot)
        return NULL;
    for (uint16_t i = 0; i < slot->count; i++)
        if (slot->objects[i].id == id && slot->objects[i].type == type)
            return slot->objects[i].state;
    return NULL;
}

/* One byte per field, one bit each: did this field change since the baseline. id Tech 3 packs this
   as a bit stream with the index of the last changed field, which is tighter; a byte-aligned mask
   keeps the reader simple and still removes every unchanged field from the packet. */
static size_t MaskBytes(size_t fieldCount)
{
    return (fieldCount + 7u) / 8u;
}

void CoreNetSyncFree(CoreNetSync *sync)
{
    if (sync)
        for (size_t i = 0; i < CORE_NET_SNAPSHOT_BACKUP; i++)
            FreeRemembered(&sync->history[i]);
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
        schema->stateSize > CORE_NET_STATE_MAX || !schema->fields || !schema->fieldCount ||
        sync->schemaCount == sync->schemaCapacity || Schema(sync, schema->type))
        return false;
    const unsigned int allowedFlags =
        CORE_NET_FIELD_INTERPOLATED | CORE_NET_FIELD_SYNC | CORE_NET_FIELD_SPAWN;
    for (size_t i = 0; i < schema->fieldCount; i++)
    {
        const CoreNetField *field = &schema->fields[i];
        size_t expected = FieldSize(field->type);
        if (!field->name || !expected || field->size != expected || field->offset > schema->stateSize ||
            expected > schema->stateSize - field->offset ||
            (field->flags & ~allowedFlags) ||
            /* SYNC (every time) and SPAWN (once, ever) are contradictory promises about the wire. */
            ((field->flags & CORE_NET_FIELD_SYNC) && (field->flags & CORE_NET_FIELD_SPAWN)) ||
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
        object->ownsState = true;
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

bool CoreNetSyncWriteDelta(const CoreNetSync *sync, uint32_t tick, uint32_t baselineTick,
                           CoreNetWriter *writer);

bool CoreNetSyncWrite(const CoreNetSync *sync, uint32_t tick, CoreNetWriter *writer)
{
    /* A full snapshot is a delta against nothing: every field marked changed. One wire format, so
       the reader never has to know which kind it is holding. */
    return CoreNetSyncWriteDelta(sync, tick, CORE_NET_BASELINE_NONE, writer);
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
    /* Deliberately not requiring the object to be the whole buffer. A caller that packs several
       objects into one packet reads them one after another, and insisting on exact consumption here
       would reject every object but the last -- which is to say, all of them. Whether anything is
       left over is the caller's question, not this one's. */
    memcpy(object->previous, object->state, object->schema->stateSize);
    memcpy(object->state, incoming, object->schema->stateSize);
    object->previousTick = object->currentTick;
    object->currentTick = sync->tick;
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
    uint32_t magic, incomingTick, baselineTick;
    uint16_t count, removed;
    if (!CoreNetReadU32(&scan, &magic) || magic != CORE_NET_SYNC_MAGIC ||
        !CoreNetReadU32(&scan, &incomingTick) || !CoreNetReadU32(&scan, &baselineTick) ||
        !CoreNetReadU16(&scan, &count) || !CoreNetReadU16(&scan, &removed) ||
        count > sync->objectCapacity)
    {
        reader->failed = true;
        return false;
    }
    /* A delta is only readable against the snapshot it was built from. If this receiver no longer
       remembers that tick the packet cannot be reconstructed, and saying so is the only honest
       answer -- the sender will fall back to a full snapshot once its acknowledgement catches up. */
    const CoreNetRemembered *baseline =
        baselineTick == CORE_NET_BASELINE_NONE ? NULL : HistoryFind(sync, baselineTick);
    if (baselineTick != CORE_NET_BASELINE_NONE && !baseline)
    {
        *reader = scan;
        if (tick)
            *tick = sync->tick;
        return true;      /* understood, cannot be applied */
    }
    for (uint16_t r = 0; r < removed; r++)
    {
        uint32_t goneId;
        if (!CoreNetReadU32(&scan, &goneId))
        {
            reader->failed = true;
            return false;
        }
    }
    /* UDP reorders and duplicates. A snapshot older than the one already applied describes a world
       that has since moved on, so applying it would rewind everything visibly. Godot's synchronizer
       rejects stale inbound syncs the same way. Ticks are 32-bit and monotonic -- at 60 a second
       they do not wrap inside any session -- so a plain comparison is enough.

       It is still parsed in full before being dropped: a packet has to be well formed to be called
       understood, and deciding that from the header alone would report a truncated one as fine. */
    bool stale = sync->started && incomingTick <= sync->tick;
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
        incoming[i].ownsState = true;
        if (!incoming[i].state || !incoming[i].previous)
            goto invalid;
        /* Start from what the baseline said this object was, then overwrite only the fields the
           sender marked as changed. A field nobody touched costs nothing on the wire and keeps the
           value it already had. */
        const void *was = RememberedState(baseline, incoming[i].id, schemaType);
        if (was)
            memcpy(incoming[i].state, was, incoming[i].schema->stateSize);
        size_t fieldCount = incoming[i].schema->fieldCount, maskBytes = MaskBytes(fieldCount);
        unsigned char mask[32] = {0};
        if (maskBytes > sizeof mask)
            goto invalid;
        for (size_t b = 0; b < maskBytes; b++)
            if (!CoreNetReadU8(&scan, &mask[b]))
                goto invalid;
        for (size_t f = 0; f < fieldCount; f++)
            if (mask[f / 8u] & (1u << (f % 8u)))
                if (!ReadField(&scan, &incoming[i].schema->fields[f], incoming[i].state))
                    goto invalid;
        CoreNetObject *old = CoreNetSyncFind(sync, incoming[i].id);
        if (old && old->schema == incoming[i].schema)
        {
            memcpy(incoming[i].previous, old->state, incoming[i].schema->stateSize);
            incoming[i].previousTick = old->currentTick;
            incoming[i].hasPrevious = true;
        }
        else
        {
            memcpy(incoming[i].previous, incoming[i].state, incoming[i].schema->stateSize);
            incoming[i].previousTick = incomingTick;
        }
        incoming[i].currentTick = incomingTick;
        incoming[i].active = true;
    }
    if (scan.at != scan.size)
        goto invalid;

    if (stale)
    {
        for (size_t i = 0; i < count; i++)
        {
            free(incoming[i].state);
            free(incoming[i].previous);
        }
        free(incoming);
        *reader = scan;
        if (tick)
            *tick = sync->tick;
        return true;      /* well formed, deliberately not applied */
    }
    sync->started = true;
    sync->tick = incomingTick;
    /* An object whose state the game supplied keeps that storage: the game holds a pointer to it
       and reads it every frame, so the new values are copied in rather than the buffer replaced.
       Swapping the pointer would leave the game reading a variable nobody writes any more. */
    for (size_t i = 0; i < count; i++)
    {
        CoreNetObject *existing = CoreNetSyncFind(sync, incoming[i].id);
        if (!existing || existing->ownsState || existing->schema != incoming[i].schema)
            continue;
        /* Ours to decide: keep what we have and ignore what we were told about it. Server-owned
           objects are ours when this machine runs the server; a player's own body is his. */
        if (LocallyOwned(sync, existing))
        {
            memcpy(incoming[i].state, existing->state, existing->schema->stateSize);
            memcpy(incoming[i].previous, existing->state, existing->schema->stateSize);
        }
        memcpy(existing->previous, existing->state, existing->schema->stateSize);
        memcpy(existing->state, incoming[i].state, existing->schema->stateSize);
        free(incoming[i].state);
        incoming[i].state = existing->state;
        incoming[i].ownsState = false;
        memcpy(incoming[i].previous, existing->previous, existing->schema->stateSize);
        existing->state = NULL;       /* the binding moves across; do not free it below */
        existing->ownsState = true;   /* nothing left to free, and NULL frees safely */
    }
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

bool CoreNetObjectSampleAt(const CoreNetObject *object, double renderTick, void *outState)
{
    if (!object || !object->active || !object->schema || !object->state || !outState)
        return false;
    if (!object->hasPrevious || object->currentTick == object->previousTick)
        return CoreNetObjectSample(object, 1.0f, outState);
    /* Where renderTick falls between the two snapshots IS the blend. Snapshots are not evenly
       spaced in practice -- one arrives late, one is lost, the sender's rate changes -- so the
       spacing has to come from their ticks rather than from an assumed rate. */
    double span = (double)object->currentTick - (double)object->previousTick;
    double alpha = (renderTick - (double)object->previousTick) / span;
    if (alpha < 0.0) alpha = 0.0;
    if (alpha > 1.0) alpha = 1.0;
    return CoreNetObjectSample(object, (float)alpha, outState);
}

bool CoreNetSyncRemember(CoreNetSync *sync, uint32_t tick)
{
    if (!sync || !sync->ready)
        return false;
    CoreNetRemembered *slot = HistorySlot(sync, tick);
    FreeRemembered(slot);
    size_t live = 0;
    for (size_t i = 0; i < sync->objectCapacity; i++)
        if (sync->objects[i].active)
            live++;
    slot->objects = calloc(live ? live : 1, sizeof *slot->objects);
    if (!slot->objects)
        return false;
    uint16_t at = 0;
    for (size_t i = 0; i < sync->objectCapacity; i++)
    {
        const CoreNetObject *object = &sync->objects[i];
        if (!object->active)
            continue;
        slot->objects[at].id = object->id;
        slot->objects[at].type = object->schema->type;
        slot->objects[at].owner = object->owner;
        slot->objects[at].state = malloc(object->schema->stateSize);
        if (!slot->objects[at].state)
        {
            slot->count = at;
            FreeRemembered(slot);
            return false;
        }
        memcpy(slot->objects[at].state, object->state, object->schema->stateSize);
        at++;
    }
    slot->count = at;
    slot->tick = tick;
    slot->serial = sync->dirtySerial;
    slot->used = true;
    return true;
}

/* Has this object anything to say to a receiver holding that baseline? One it has never seen must
   be sent whole. One with a field that goes every time always has something. Otherwise it speaks
   only if the game marked it changed since the baseline was taken -- which is the saving: an object
   nobody touched costs nothing, not even the comparison. */
static bool ObjectSpeaks(const CoreNetObject *object, const CoreNetRemembered *baseline)
{
    if (!baseline || !RememberedState(baseline, object->id, object->schema->type))
        return true;
    /* Without the promise that every change is marked, the only safe answer is to look. */
    if (!(object->schema->flags & CORE_NET_SCHEMA_EXPLICIT_DIRTY))
        return true;
    for (size_t f = 0; f < object->schema->fieldCount; f++)
        if (object->schema->fields[f].flags & CORE_NET_FIELD_SYNC)
            return true;
    return object->dirtySerial > baseline->serial;
}

bool CoreNetSyncWriteDelta(const CoreNetSync *sync, uint32_t tick, uint32_t baselineTick,
                           CoreNetWriter *writer)
{
    if (!sync || !sync->ready || !writer)
        return false;
    const CoreNetRemembered *baseline =
        baselineTick == CORE_NET_BASELINE_NONE ? NULL : HistoryFind(sync, baselineTick);
    /* A baseline the sender no longer remembers cannot be encoded against. Sending in full is
       always correct, and is what a receiver that has fallen behind needs regardless. */
    if (!baseline)
        baselineTick = CORE_NET_BASELINE_NONE;

    size_t live = 0;
    for (size_t i = 0; i < sync->objectCapacity; i++)
        if (sync->objects[i].active && ObjectSpeaks(&sync->objects[i], baseline))
            live++;
    if (live > UINT16_MAX)
        return false;

    /* Objects the baseline had and this snapshot does not: named so the receiver drops them. */
    uint16_t removed = 0;
    if (baseline)
        for (uint16_t i = 0; i < baseline->count; i++)
        {
            const CoreNetObject *still = NULL;
            for (size_t o = 0; o < sync->objectCapacity; o++)
                if (sync->objects[o].active && sync->objects[o].id == baseline->objects[i].id)
                {
                    still = &sync->objects[o];
                    break;
                }
            if (!still)
                removed++;
        }

    if (!CoreNetWriteU32(writer, CORE_NET_SYNC_MAGIC) || !CoreNetWriteU32(writer, tick) ||
        !CoreNetWriteU32(writer, baselineTick) || !CoreNetWriteU16(writer, (uint16_t)live) ||
        !CoreNetWriteU16(writer, removed))
        return false;

    if (baseline)
        for (uint16_t i = 0; i < baseline->count; i++)
        {
            bool still = false;
            for (size_t o = 0; o < sync->objectCapacity && !still; o++)
                still = sync->objects[o].active && sync->objects[o].id == baseline->objects[i].id;
            if (!still && !CoreNetWriteU32(writer, baseline->objects[i].id))
                return false;
        }

    for (size_t i = 0; i < sync->objectCapacity; i++)
    {
        const CoreNetObject *object = &sync->objects[i];
        if (!object->active || !ObjectSpeaks(object, baseline))
            continue;
        const void *was = RememberedState(baseline, object->id, object->schema->type);
        size_t fieldCount = object->schema->fieldCount, bytes = MaskBytes(fieldCount);
        unsigned char mask[32] = {0};
        if (bytes > sizeof mask)
            return false;
        for (size_t f = 0; f < fieldCount; f++)
        {
            const CoreNetField *field = &object->schema->fields[f];
            bool full = !was;      /* the receiver has never seen this object: it needs all of it */
            bool changed = full || memcmp((const unsigned char *)object->state + field->offset,
                                          (const unsigned char *)was + field->offset,
                                          field->size) != 0;
            /* A SPAWN field is decided once and rides only in that first full state. A SYNC field
               goes every time so there is always something to interpolate toward. Everything else
               goes when it changed, and costs nothing when it did not. */
            if (field->flags & CORE_NET_FIELD_SPAWN)
                changed = full;
            else if (field->flags & CORE_NET_FIELD_SYNC)
                changed = true;
            if (changed)
                mask[f / 8u] |= (unsigned char)(1u << (f % 8u));
        }
        if (!CoreNetWriteU32(writer, object->id) ||
            !CoreNetWriteU16(writer, object->schema->type) ||
            !CoreNetWriteU16(writer, object->owner))
            return false;
        for (size_t b = 0; b < bytes; b++)
            if (!CoreNetWriteU8(writer, mask[b]))
                return false;
        for (size_t f = 0; f < fieldCount; f++)
            if (mask[f / 8u] & (1u << (f % 8u)))
                if (!WriteField(writer, &object->schema->fields[f], object->state))
                {
                    writer->failed = true;
                    return false;
                }
    }
    return !writer->failed;
}

bool CoreNetSyncIsMine(const CoreNetSync *sync, const CoreNetObject *object)
{
    return LocallyOwned(sync, object);
}

bool CoreNetSyncDirty(CoreNetSync *sync, uint32_t id)
{
    CoreNetObject *object = CoreNetSyncFind(sync, id);
    if (!object)
        return false;
    /* Counted, not timed: a server never reads a snapshot, so it has no tick to stamp with. One
       mark then survives until every receiver has been told, however far behind the slowest is,
       because each remembered snapshot records where the count stood when it was taken. */
    object->dirtySerial = ++sync->dirtySerial;
    return true;
}

bool CoreNetSyncObserve(CoreNetSync *sync, uint32_t id, CoreNetSerializeFn serialize, void *user)
{
    CoreNetObject *object = CoreNetSyncFind(sync, id);
    if (!object)
        return false;
    object->serialize = serialize;
    object->serializeUser = user;
    return true;
}

size_t CoreNetSyncSerialize(CoreNetSync *sync, uint16_t localActor, double renderTick)
{
    if (!sync || !sync->ready)
        return 0;
    /* The ownership rule reads localActor from the sync, not a parameter, so the read path and
       this path can never disagree; this keeps it current for callers that never call
       CoreNetSyncSetLocalActor and just pass their actor id here every frame. */
    sync->localActor = localActor;
    size_t ran = 0;
    for (size_t i = 0; i < sync->objectCapacity; i++)
    {
        CoreNetObject *object = &sync->objects[i];
        if (!object->active || !object->serialize || !object->schema)
            continue;
        if (LocallyOwned(sync, object))
        {
            /* Ours: the live game is the truth, and the replicated copy is brought up to it. */
            object->serialize(object->serializeUser, object->state, true);
        }
        else
        {
            /* Somebody else's: sample it at the moment being drawn, then hand that to the game.
               Sampling into a scratch copy rather than the object keeps the two snapshots the
               interpolation runs between intact. */
            unsigned char scratch[CORE_NET_STATE_MAX];
            if (object->schema->stateSize > sizeof scratch)
                continue;
            if (!CoreNetObjectSampleAt(object, renderTick, scratch))
                continue;
            object->serialize(object->serializeUser, scratch, false);
        }
        ran++;
    }
    return ran;
}

bool CoreNetCommandWrite(CoreNetWriter *writer, CoreNetCommand command)
{
    return CoreNetWriteU32(writer, command.sequence) && CoreNetWriteU32(writer, command.object) &&
           CoreNetWriteU8(writer, command.op);
}

bool CoreNetCommandRead(CoreNetReader *reader, CoreNetCommand *command)
{
    if (!command)
        return false;
    return CoreNetReadU32(reader, &command->sequence) &&
           CoreNetReadU32(reader, &command->object) && CoreNetReadU8(reader, &command->op);
}

bool CoreNetCommandAccept(uint32_t *lastSequence, uint32_t sequence)
{
    if (!lastSequence)
        return false;
    /* Strictly greater, and nothing cleverer. A sequence at or below the last one executed is a
       repeat, full stop -- trying to also read "a client reconnected and started again" out of the
       number cannot work, because a late packet and a fresh client look identical. Whether a
       connection is new is something the server knows for certain, so it resets the counter on
       connect instead of this guessing. */
    if (sequence <= *lastSequence)
        return false;
    *lastSequence = sequence;
    return true;
}

bool CoreNetSyncBindState(CoreNetSync *sync, uint32_t id, void *state)
{
    CoreNetObject *object = CoreNetSyncFind(sync, id);
    if (!object || !object->schema)
        return false;
    if (!state)
    {
        if (object->ownsState)
            return true;
        object->state = calloc(1, object->schema->stateSize);
        object->ownsState = true;
        return object->state != NULL;
    }
    /* Carry whatever the object already holds into the game's variable, so binding does not lose a
       snapshot that arrived before the game got round to it. */
    if (object->state)
    {
        memcpy(state, object->state, object->schema->stateSize);
        if (object->ownsState)
            free(object->state);
    }
    object->state = state;
    object->ownsState = false;
    return true;
}

void CoreNetSyncSetLocalActor(CoreNetSync *sync, uint16_t localActor, bool isServer)
{
    if (!sync)
        return;
    sync->localActor = localActor;
    sync->localIsServer = isServer;
}
