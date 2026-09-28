/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "object.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---- types ------------------------------------------------------------------------------------
const EngineProperty *EngineTypeProperty(const EngineType *type, const char *name)
{
    for (; type && name; type = type->parent)
        for (int i = 0; i < type->propertyCount; i++)
            if (!strcmp(type->properties[i].name, name))
                return &type->properties[i];
    return NULL;
}

const EngineMethod *EngineTypeMethod(const EngineType *type, const char *name)
{
    for (; type && name; type = type->parent)
        for (int i = 0; i < type->methodCount; i++)
            if (!strcmp(type->methods[i].name, name))
                return &type->methods[i];
    return NULL;
}

const char *EngineTypeSignal(const EngineType *type, const char *name)
{
    for (; type && name; type = type->parent)
        for (int i = 0; i < type->signalCount; i++)
            if (!strcmp(type->signals[i], name))
                return type->signals[i];
    return NULL;
}

bool EngineTypeIs(const EngineType *type, const EngineType *base)
{
    for (; type; type = type->parent)
        if (type == base)
            return true;
    return false;
}

const char *EngineValueTypeName(EngineValueType type)
{
    static const char *const names[] = {"nothing", "bool",    "int",    "float",
                                        "vector2", "vector3", "string", "object"};
    return (unsigned)type < sizeof names / sizeof names[0] ? names[type] : "unknown";
}

bool EngineValueConvert(EngineValue *value, EngineValueType wanted)
{
    if (!value)
        return false;
    if (wanted == ENGINE_NONE || value->type == wanted)
        return true;
    if (wanted == ENGINE_FLOAT && value->type == ENGINE_INT)
    {
        *value = EngineFloat((float)value->as.integer);
        return true;
    }
    if (wanted == ENGINE_INT && value->type == ENGINE_FLOAT && isfinite(value->as.number))
    {
        *value = EngineInt((int)value->as.number);
        return true;
    }
    if (wanted == ENGINE_BOOL && value->type == ENGINE_INT)
    {
        *value = EngineBool(value->as.integer != 0);
        return true;
    }
    return false;
}

static size_t StoredSize(EngineValueType type)
{
    switch (type)
    {
        case ENGINE_BOOL: return sizeof(bool);
        case ENGINE_INT: return sizeof(int);
        case ENGINE_FLOAT: return sizeof(float);
        case ENGINE_VECTOR2: return sizeof(Vector2);
        case ENGINE_VECTOR3: return sizeof(Vector3);
        case ENGINE_OBJECT: return sizeof(EngineObjectId);
        default: return 0; // strings are only stored through a property's own functions
    }
}

bool EnginePropertyGet(const EngineProperty *property, const void *data, EngineValue *out)
{
    if (!property || !data || !out)
        return false;
    if (property->get)
        return property->get(data, out);
    size_t size = StoredSize(property->type);
    if (!size)
        return false;
    *out = (EngineValue){property->type, {0}};
    memcpy(&out->as, (const unsigned char *)data + property->offset, size);
    return true;
}

bool EnginePropertySet(const EngineProperty *property, void *data, const EngineValue *value,
                       const char **error)
{
    const char *unused;
    if (!error)
        error = &unused;
    if (!property || !data || !value)
    {
        *error = "no property or no value";
        return false;
    }
    if (property->flags & ENGINE_PROPERTY_READ_ONLY)
    {
        *error = "that property is read-only";
        return false;
    }
    EngineValue converted = *value;
    if (!EngineValueConvert(&converted, property->type))
    {
        *error = "that value is the wrong type for the property";
        return false;
    }
    if (property->set)
    {
        if (!property->set(data, &converted))
        {
            *error = "the property refused that value";
            return false;
        }
        return true;
    }
    size_t size = StoredSize(property->type);
    if (!size || (property->type == ENGINE_FLOAT && !isfinite(converted.as.number)))
    {
        *error = "that value cannot be stored";
        return false;
    }
    memcpy((unsigned char *)data + property->offset, &converted.as, size);
    return true;
}

// Checks and converts arguments against a declared list, into a scratch copy the callee may read.
static bool CheckArguments(const EngineValueType *wanted, int declared, int required,
                           const EngineValue *given, int count, EngineValue *checked,
                           const char **error)
{
    if (count < required || count > declared)
    {
        *error = "wrong number of arguments";
        return false;
    }
    for (int i = 0; i < count; i++)
    {
        checked[i] = given[i];
        if (!EngineValueConvert(&checked[i], wanted[i]))
        {
            *error = "an argument is the wrong type";
            return false;
        }
    }
    return true;
}

bool EngineMethodInvoke(EngineCall *call, const EngineMethod *method)
{
    if (!call || !method || !method->call)
    {
        if (call)
            call->error = "no such method";
        return false;
    }
    EngineValue checked[ENGINE_METHOD_ARGUMENTS];
    const char *error = NULL;
    if (!CheckArguments(method->arguments, method->argumentCount, method->argumentCount,
                        call->arguments, call->count, checked, &error))
    {
        call->error = error;
        return false;
    }
    call->arguments = checked;
    call->result = EngineNone();
    call->error = NULL;
    bool ok = method->call(call);
    if (ok && method->result != ENGINE_NONE && !EngineValueConvert(&call->result, method->result))
    {
        call->error = "the method answered with the wrong type";
        ok = false;
    }
    if (!ok && !call->error)
        call->error = "the method failed";
    call->arguments = NULL;
    return ok;
}

// ---- the pool ----------------------------------------------------------------------------------
static bool Grow(void **items, size_t *capacity, size_t count, size_t size)
{
    if (count < *capacity)
        return true;
    size_t next = *capacity ? *capacity * 2 : 16;
    void *grown = realloc(*items, next * size);
    if (!grown)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

bool EngineObjectsInit(EngineObjects *objects)
{
    if (!objects)
        return false;
    *objects = (EngineObjects){0};
    objects->nextConnection = 1;
    objects->alpha = 1.0f;
    return true;
}

static EngineObjectSlot *Live(const EngineObjects *objects, EngineObjectId id)
{
    if (!objects || id.index >= objects->slotCount)
        return NULL;
    EngineObjectSlot *slot = &objects->slots[id.index];
    return slot->live && slot->generation == id.generation ? slot : NULL;
}

static bool SameId(EngineObjectId a, EngineObjectId b)
{
    return a.index == b.index && a.generation == b.generation;
}

static void ReleaseConnection(EngineObjects *objects, size_t index)
{
    EngineConnection ended = objects->connections[index];
    memmove(&objects->connections[index], &objects->connections[index + 1],
            (objects->connectionCount - index - 1) * sizeof *objects->connections);
    objects->connectionCount--;
    if (ended.release)
        ended.release(ended.user);
}

void EngineObjectsFree(EngineObjects *objects)
{
    if (!objects)
        return;
    while (objects->connectionCount)
        ReleaseConnection(objects, objects->connectionCount - 1);
    for (size_t i = 0; i < objects->slotCount; i++)
    {
        EngineObjectSlot *slot = &objects->slots[i];
        if (slot->live && slot->owned)
        {
            if (slot->type->destroy)
                slot->type->destroy(slot->data);
            free(slot->data);
        }
    }
    free(objects->slots);
    free(objects->types);
    free(objects->connections);
    *objects = (EngineObjects){0};
}

bool EngineObjectsRegisterType(EngineObjects *objects, const EngineType *type)
{
    if (!objects || !type || !type->name)
        return false;
    const EngineType *existing = EngineObjectsTypeNamed(objects, type->name);
    if (existing)
        return existing == type;
    if (!Grow((void **)&objects->types, &objects->typeCapacity, objects->typeCount,
              sizeof *objects->types))
        return false;
    objects->types[objects->typeCount++] = type;
    return true;
}

const EngineType *EngineObjectsTypeNamed(const EngineObjects *objects, const char *name)
{
    if (!objects || !name)
        return NULL;
    for (size_t i = 0; i < objects->typeCount; i++)
        if (!strcmp(objects->types[i]->name, name))
            return objects->types[i];
    return NULL;
}

// A free slot, reusing ended ones first so handles stay small; its generation is already bumped.
static EngineObjectId Claim(EngineObjects *objects)
{
    for (size_t i = 0; i < objects->slotCount; i++)
        if (!objects->slots[i].live)
            return (EngineObjectId){(uint32_t)i, objects->slots[i].generation};
    if (objects->slotCount >= UINT32_MAX - 1 ||
        !Grow((void **)&objects->slots, &objects->slotCapacity, objects->slotCount,
              sizeof *objects->slots))
        return ENGINE_OBJECT_NULL;
    objects->slots[objects->slotCount] = (EngineObjectSlot){0};
    return (EngineObjectId){(uint32_t)objects->slotCount++, 0};
}

EngineObjectId EngineObjectCreate(EngineObjects *objects, const EngineType *type,
                                  const EngineValue *arguments, int count, const char **error)
{
    const char *unused;
    if (!error)
        error = &unused;
    if (!objects || !type || !type->size)
    {
        *error = "no type to create";
        return ENGINE_OBJECT_NULL;
    }
    EngineValue checked[ENGINE_METHOD_ARGUMENTS];
    if (!CheckArguments(type->createArguments, type->createArgumentCount, type->createRequired,
                        arguments, count, checked, error))
        return ENGINE_OBJECT_NULL;
    EngineObjectId id = Claim(objects);
    if (EngineObjectIdIsNull(id))
    {
        *error = "out of memory";
        return ENGINE_OBJECT_NULL;
    }
    void *data = calloc(1, type->size);
    if (!data)
    {
        *error = "out of memory";
        return ENGINE_OBJECT_NULL;
    }
    EngineObjectSlot *slot = &objects->slots[id.index];
    *slot = (EngineObjectSlot){type, data, id.generation, true, true};
    if (type->create)
    {
        EngineCall call = {objects, id, data, checked, count, EngineNone(), NULL};
        if (!type->create(&call))
        {
            *error = call.error ? call.error : "the object could not be created";
            free(data);
            *slot = (EngineObjectSlot){.generation = id.generation + 1};
            return ENGINE_OBJECT_NULL;
        }
    }
    return id;
}

EngineObjectId EngineObjectAdopt(EngineObjects *objects, const EngineType *type, void *data)
{
    if (!objects || !type || !data)
        return ENGINE_OBJECT_NULL;
    EngineObjectId id = Claim(objects);
    if (!EngineObjectIdIsNull(id))
        objects->slots[id.index] = (EngineObjectSlot){type, data, id.generation, true, false};
    return id;
}

bool EngineObjectDestroy(EngineObjects *objects, EngineObjectId id)
{
    EngineObjectSlot *slot = Live(objects, id);
    if (!slot)
        return false;
    // Stale first, so a handler or destructor that looks the object up finds it already gone.
    EngineObjectSlot ended = *slot;
    slot->live = false;
    slot->generation++;
    slot->data = NULL;
    for (size_t i = objects->connectionCount; i-- > 0;)
        if (i < objects->connectionCount && SameId(objects->connections[i].sender, id))
            ReleaseConnection(objects, i);
    if (ended.owned)
    {
        if (ended.type->destroy)
            ended.type->destroy(ended.data);
        free(ended.data);
    }
    return true;
}

bool EngineObjectAlive(const EngineObjects *objects, EngineObjectId id)
{
    return Live(objects, id) != NULL;
}

const EngineType *EngineObjectTypeOf(const EngineObjects *objects, EngineObjectId id)
{
    EngineObjectSlot *slot = Live(objects, id);
    return slot ? slot->type : NULL;
}

void *EngineObjectData(const EngineObjects *objects, EngineObjectId id, const EngineType *wanted)
{
    EngineObjectSlot *slot = Live(objects, id);
    if (!slot || (wanted && !EngineTypeIs(slot->type, wanted)))
        return NULL;
    return slot->data;
}

bool EngineObjectGet(EngineObjects *objects, EngineObjectId id, const char *name, EngineValue *out,
                     const char **error)
{
    const char *unused;
    if (!error)
        error = &unused;
    EngineObjectSlot *slot = Live(objects, id);
    if (!slot)
    {
        *error = "that object no longer exists";
        return false;
    }
    const EngineProperty *property = EngineTypeProperty(slot->type, name);
    if (!property)
    {
        *error = "that object has no property of that name";
        return false;
    }
    if (!EnginePropertyGet(property, slot->data, out))
    {
        *error = "that property could not be read";
        return false;
    }
    return true;
}

bool EngineObjectSet(EngineObjects *objects, EngineObjectId id, const char *name,
                     const EngineValue *value, const char **error)
{
    const char *unused;
    if (!error)
        error = &unused;
    EngineObjectSlot *slot = Live(objects, id);
    if (!slot)
    {
        *error = "that object no longer exists";
        return false;
    }
    const EngineProperty *property = EngineTypeProperty(slot->type, name);
    if (!property)
    {
        *error = "that object has no property of that name";
        return false;
    }
    return EnginePropertySet(property, slot->data, value, error);
}

bool EngineObjectCall(EngineObjects *objects, EngineObjectId id, const char *name,
                      const EngineValue *arguments, int count, EngineValue *result,
                      const char **error)
{
    const char *unused;
    if (!error)
        error = &unused;
    EngineObjectSlot *slot = Live(objects, id);
    if (!slot)
    {
        *error = "that object no longer exists";
        return false;
    }
    const EngineMethod *method = EngineTypeMethod(slot->type, name);
    if (!method)
    {
        *error = "that object has no method of that name";
        return false;
    }
    EngineCall call = {objects, id, slot->data, arguments, count, EngineNone(), NULL};
    bool ok = EngineMethodInvoke(&call, method);
    if (!ok)
        *error = call.error;
    else if (result)
        *result = call.result;
    return ok;
}

void *EngineCallObject(EngineCall *call, int argument, const EngineType *wanted)
{
    if (!call || !call->arguments || argument < 0 || argument >= call->count ||
        call->arguments[argument].type != ENGINE_OBJECT)
        return NULL;
    return EngineObjectData(call->objects, call->arguments[argument].as.object, wanted);
}

// ---- signals ------------------------------------------------------------------------------------
int EngineObjectConnect(EngineObjects *objects, EngineObjectId sender, const char *signal,
                        EngineSignalFn call, void *user, EngineReleaseFn release)
{
    EngineObjectSlot *slot = Live(objects, sender);
    const char *interned = slot ? EngineTypeSignal(slot->type, signal) : NULL;
    if (!interned || !call ||
        !Grow((void **)&objects->connections, &objects->connectionCapacity,
              objects->connectionCount, sizeof *objects->connections))
    {
        if (release)
            release(user);
        return 0;
    }
    int id = objects->nextConnection++;
    objects->connections[objects->connectionCount++] =
        (EngineConnection){id, sender, interned, call, user, release};
    return id;
}

bool EngineObjectDisconnect(EngineObjects *objects, int connection)
{
    if (!objects)
        return false;
    for (size_t i = 0; i < objects->connectionCount; i++)
        if (objects->connections[i].id == connection)
        {
            ReleaseConnection(objects, i);
            return true;
        }
    return false;
}

int EngineObjectEmit(EngineObjects *objects, EngineObjectId sender, const char *signal,
                     const EngineValue *arguments, int count)
{
    EngineObjectSlot *slot = Live(objects, sender);
    const char *interned = slot ? EngineTypeSignal(slot->type, signal) : NULL;
    if (!interned)
        return -1;
    /* Handlers may connect, disconnect or destroy, so the list is walked by connection id rather
       than by position: only connections that existed when the emission began are called, each
       at most once, and a connection ended by an earlier handler is skipped. */
    int last = objects->nextConnection - 1, delivered = 0, after = 0;
    for (;;)
    {
        if (!EngineObjectAlive(objects, sender))
            break;
        const EngineConnection *next = NULL;
        for (size_t i = 0; i < objects->connectionCount; i++)
        {
            const EngineConnection *c = &objects->connections[i];
            if (c->id > after && c->id <= last && c->signal == interned && SameId(c->sender, sender) &&
                (!next || c->id < next->id))
                next = c;
        }
        if (!next)
            break;
        EngineConnection fire = *next;
        after = fire.id;
        fire.call(fire.user, objects, sender, arguments, count);
        delivered++;
    }
    return delivered;
}

void EngineObjectsStep(EngineObjects *objects, float dt)
{
    if (!objects)
        return;
    size_t count = objects->slotCount; // objects created by a step wait for the next one
    for (size_t i = 0; i < count; i++)
    {
        EngineObjectSlot *slot = &objects->slots[i];
        if (slot->live && slot->type->step)
            slot->type->step(objects, (EngineObjectId){(uint32_t)i, slot->generation}, slot->data, dt);
    }
}
