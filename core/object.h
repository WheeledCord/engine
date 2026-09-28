/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_OBJECT_H
#define CORE_OBJECT_H

#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Engine types described once. A type is a C table of properties, methods and signals written
   beside the type itself, the way Godot binds a class in _bind_methods (ClassDB::bind_method,
   ADD_PROPERTY, ADD_SIGNAL) and Source describes an entity with a DATADESC table. Anything that
   needs to reach an object by name -- a script, an inspector, a save -- reads that one table
   instead of keeping its own list of calls.

   Objects live in a caller-owned EngineObjects pool and are named by generational handles, so a
   handle kept past the object's end is refused instead of reaching whatever took its place. */

#define ENGINE_METHOD_ARGUMENTS 6

typedef enum EngineValueType
{
    ENGINE_NONE,
    ENGINE_BOOL,
    ENGINE_INT,
    ENGINE_FLOAT,
    ENGINE_VECTOR2,
    ENGINE_VECTOR3,
    ENGINE_STRING,
    ENGINE_OBJECT
} EngineValueType;

typedef struct EngineObjectId
{
    uint32_t index;
    uint32_t generation;
} EngineObjectId;

#define ENGINE_OBJECT_NULL ((EngineObjectId){UINT32_MAX, 0})

typedef struct EngineValue
{
    EngineValueType type;
    union
    {
        bool boolean;
        int integer;
        float number;
        Vector2 vector2;
        Vector3 vector3;
        const char *string; /* borrowed: valid until the object it came from changes */
        EngineObjectId object;
    } as;
} EngineValue;

/* What else a property is for, beyond being read and written by name. */
#define ENGINE_PROPERTY_READ_ONLY 1u /* scripts may read it but not write it */
#define ENGINE_PROPERTY_SAVE 2u      /* part of what a save records */
#define ENGINE_PROPERTY_SCENE 4u     /* a scene file may set it */
#define ENGINE_PROPERTY_SHARED 8u    /* sent to the other machines in a networked game */

typedef struct EngineProperty
{
    const char *name;     /* as scripts spell it */
    EngineValueType type;
    size_t offset;        /* where it lives in the object, when get and set are NULL */
    unsigned int flags;
    bool (*get)(const void *object, EngineValue *out);
    bool (*set)(void *object, const EngineValue *value);
    const char *help;
} EngineProperty;

/* A property that is simply a member of the object's struct. */
#define ENGINE_FIELD(name, Type, member, kind, flags, help)                                        \
    {name, kind, offsetof(Type, member), flags, NULL, NULL, help}
/* A property worked out by functions rather than stored. */
#define ENGINE_COMPUTED(name, kind, flags, get, set, help) {name, kind, 0, flags, get, set, help}

typedef struct EngineObjects EngineObjects;

/* One method call in flight: the object, its checked arguments, and where the answer goes. */
typedef struct EngineCall
{
    EngineObjects *objects;
    EngineObjectId self;
    void *data;
    const EngineValue *arguments;
    int count;
    EngineValue result;
    const char *error; /* set by a method that fails, to say why */
} EngineCall;

typedef bool (*EngineMethodFn)(EngineCall *call);

typedef struct EngineMethod
{
    const char *name;
    EngineValueType result;
    EngineValueType arguments[ENGINE_METHOD_ARGUMENTS];
    int argumentCount;
    EngineMethodFn call;
    const char *help;
} EngineMethod;

typedef struct EngineType EngineType;
struct EngineType
{
    const char *name;
    const EngineType *parent; /* properties, methods and signals are inherited */
    size_t size;              /* bytes the pool allocates for one object */
    const EngineProperty *properties;
    int propertyCount;
    const EngineMethod *methods;
    int methodCount;
    const char *const *signals;
    int signalCount;
    /* Creation arguments: the first `createRequired` must be given, the rest may be. */
    EngineValueType createArguments[ENGINE_METHOD_ARGUMENTS];
    int createArgumentCount;
    int createRequired;
    bool (*create)(EngineCall *call); /* NULL: the object starts zeroed */
    void (*destroy)(void *data);      /* releases what create acquired */
    void (*step)(EngineObjects *objects, EngineObjectId self, void *data, float dt); /* optional */
    const char *help;
};

typedef void (*EngineSignalFn)(void *user, EngineObjects *objects, EngineObjectId sender,
                               const EngineValue *arguments, int count);
// Frees what a connection was given, once, when the connection ends.
typedef void (*EngineReleaseFn)(void *user);

typedef struct EngineObjectSlot
{
    const EngineType *type;
    void *data;
    uint32_t generation;
    bool live;
    bool owned; /* false for an adopted object, whose storage belongs to someone else */
} EngineObjectSlot;

typedef struct EngineConnection
{
    int id;
    EngineObjectId sender;
    const char *signal; /* interned in the sender's type */
    EngineSignalFn call;
    void *user;
    EngineReleaseFn release;
} EngineConnection;

struct EngineObjects
{
    EngineObjectSlot *slots;
    size_t slotCount, slotCapacity;
    const EngineType **types;
    size_t typeCount, typeCapacity;
    EngineConnection *connections;
    size_t connectionCount, connectionCapacity;
    int nextConnection;
    float alpha; /* how far between simulation steps the current frame draws */
};

static inline EngineValue EngineNone(void) { return (EngineValue){ENGINE_NONE, {0}}; }
static inline EngineValue EngineBool(bool v) { EngineValue e = {ENGINE_BOOL, {0}}; e.as.boolean = v; return e; }
static inline EngineValue EngineInt(int v) { EngineValue e = {ENGINE_INT, {0}}; e.as.integer = v; return e; }
static inline EngineValue EngineFloat(float v) { EngineValue e = {ENGINE_FLOAT, {0}}; e.as.number = v; return e; }
static inline EngineValue EngineVector2(Vector2 v) { EngineValue e = {ENGINE_VECTOR2, {0}}; e.as.vector2 = v; return e; }
static inline EngineValue EngineVector3(Vector3 v) { EngineValue e = {ENGINE_VECTOR3, {0}}; e.as.vector3 = v; return e; }
static inline EngineValue EngineString(const char *v) { EngineValue e = {ENGINE_STRING, {0}}; e.as.string = v ? v : ""; return e; }
static inline EngineValue EngineObject(EngineObjectId v) { EngineValue e = {ENGINE_OBJECT, {0}}; e.as.object = v; return e; }
static inline bool EngineObjectIdIsNull(EngineObjectId id) { return id.index == UINT32_MAX; }

/** @brief Finds a property on a type or any type it inherits from.
 * @param type Type to search; NULL answers NULL.
 * @param name Property name as scripts spell it.
 * @return The borrowed property description, or NULL when the type has none of that name. */
const EngineProperty *EngineTypeProperty(const EngineType *type, const char *name);

/** @brief Finds a method on a type or any type it inherits from.
 * @param type Type to search; NULL answers NULL.
 * @param name Method name as scripts spell it.
 * @return The borrowed method description, or NULL when the type has none of that name. */
const EngineMethod *EngineTypeMethod(const EngineType *type, const char *name);

/** @brief Finds a signal a type or any type it inherits from can emit.
 * @param type Type to search; NULL answers NULL.
 * @param name Signal name.
 * @return The type's own interned spelling of the name, or NULL when it has no such signal. */
const char *EngineTypeSignal(const EngineType *type, const char *name);

/** @brief Says whether a type is another or inherits from it.
 * @param type Type to test; NULL answers false.
 * @param base Type it may be or inherit from.
 * @return True when type is base or one of its descendants. */
bool EngineTypeIs(const EngineType *type, const EngineType *base);

/** @brief Converts a value to the type a property or argument wants, where that is lossless enough.
 *
 * Integers and floats convert to each other and a number converts to a bool; nothing else does.
 * @param value Value to convert in place.
 * @param wanted Type wanted; ENGINE_NONE accepts anything.
 * @return True when value now has the wanted type. */
bool EngineValueConvert(EngineValue *value, EngineValueType wanted);

/** @brief Names a value type the way an error message should.
 * @param type Value type.
 * @return A static lowercase name such as "float" or "vector2". */
const char *EngineValueTypeName(EngineValueType type);

/** @brief Reads a property from an object's storage.
 * @param property Property description.
 * @param data The object's storage.
 * @param out Receives the value.
 * @return True when the value was read. */
bool EnginePropertyGet(const EngineProperty *property, const void *data, EngineValue *out);

/** @brief Writes a property into an object's storage, converting the value first.
 * @param property Property description.
 * @param data The object's storage.
 * @param value Value to write; strings are only written through a property's own setter.
 * @param error Receives a static reason on failure; may be NULL.
 * @return True when the value was accepted. */
bool EnginePropertySet(const EngineProperty *property, void *data, const EngineValue *value,
                       const char **error);

/** @brief Calls a method on an object's storage after checking the arguments against it.
 * @param call Call record: objects, self, data, arguments and count filled in by the caller.
 * @param method Method description.
 * @return True when the method ran; on false call->error says why. */
bool EngineMethodInvoke(EngineCall *call, const EngineMethod *method);

/** @brief Prepares an empty object pool.
 * @param objects Caller-owned pool.
 * @return True on success. */
bool EngineObjectsInit(EngineObjects *objects);

/** @brief Destroys every object the pool owns, releases every connection and the pool's storage.
 * @param objects Pool; NULL is accepted.
 * @return No value. Adopted objects are forgotten, not destroyed. */
void EngineObjectsFree(EngineObjects *objects);

/** @brief Makes a type creatable by name from this pool.
 * @param objects Pool.
 * @param type Borrowed type description that must outlive the pool.
 * @return True when added, or already present; false when another type has the same name. */
bool EngineObjectsRegisterType(EngineObjects *objects, const EngineType *type);

/** @brief Finds a registered type by name.
 * @param objects Pool.
 * @param name Type name.
 * @return The borrowed type, or NULL. */
const EngineType *EngineObjectsTypeNamed(const EngineObjects *objects, const char *name);

/** @brief Allocates and creates an object of a type.
 * @param objects Pool.
 * @param type Type to create; it need not be registered.
 * @param arguments Creation arguments, checked against the type's.
 * @param count Number of arguments.
 * @param error Receives a static reason on failure; may be NULL.
 * @return The new object's handle, or ENGINE_OBJECT_NULL. */
EngineObjectId EngineObjectCreate(EngineObjects *objects, const EngineType *type,
                                  const EngineValue *arguments, int count, const char **error);

/** @brief Gives storage the caller owns a handle, so it can be reached like any other object.
 * @param objects Pool.
 * @param type Type describing the storage.
 * @param data Caller-owned storage that must outlive the handle, or be released with
 * EngineObjectDestroy first.
 * @return The handle, or ENGINE_OBJECT_NULL. */
EngineObjectId EngineObjectAdopt(EngineObjects *objects, const EngineType *type, void *data);

/** @brief Ends an object: disconnects its signals, destroys what it owns, and makes its handle stale.
 * @param objects Pool.
 * @param id Handle.
 * @return True when a live object was ended. */
bool EngineObjectDestroy(EngineObjects *objects, EngineObjectId id);

/** @brief Says whether a handle still names a live object.
 * @param objects Pool.
 * @param id Handle.
 * @return True for a live object. */
bool EngineObjectAlive(const EngineObjects *objects, EngineObjectId id);

/** @brief Returns an object's type.
 * @param objects Pool.
 * @param id Handle.
 * @return The type, or NULL for a stale handle. */
const EngineType *EngineObjectTypeOf(const EngineObjects *objects, EngineObjectId id);

/** @brief Returns an object's storage when it is of a wanted type.
 * @param objects Pool.
 * @param id Handle.
 * @param wanted Type the caller expects, or an ancestor of it; NULL accepts any.
 * @return The storage, or NULL for a stale handle or a different type. */
void *EngineObjectData(const EngineObjects *objects, EngineObjectId id, const EngineType *wanted);

/** @brief Reads a property of an object by name.
 * @param objects Pool.
 * @param id Handle.
 * @param name Property name.
 * @param out Receives the value.
 * @param error Receives a static reason on failure; may be NULL.
 * @return True when read. */
bool EngineObjectGet(EngineObjects *objects, EngineObjectId id, const char *name, EngineValue *out,
                     const char **error);

/** @brief Writes a property of an object by name.
 * @param objects Pool.
 * @param id Handle.
 * @param name Property name.
 * @param value Value to write, converted to the property's type.
 * @param error Receives a static reason on failure; may be NULL.
 * @return True when written. */
bool EngineObjectSet(EngineObjects *objects, EngineObjectId id, const char *name,
                     const EngineValue *value, const char **error);

/** @brief Calls a method of an object by name.
 * @param objects Pool.
 * @param id Handle.
 * @param name Method name.
 * @param arguments Arguments, checked and converted against the method's.
 * @param count Number of arguments.
 * @param result Receives the method's answer; may be NULL.
 * @param error Receives a static reason on failure; may be NULL.
 * @return True when the method ran. */
bool EngineObjectCall(EngineObjects *objects, EngineObjectId id, const char *name,
                      const EngineValue *arguments, int count, EngineValue *result,
                      const char **error);

/** @brief Resolves an object argument of a method call to its storage.
 * @param call The call in flight.
 * @param argument Argument index.
 * @param wanted Type expected; NULL accepts any.
 * @return The storage, or NULL when the argument is not a live object of that type. */
void *EngineCallObject(EngineCall *call, int argument, const EngineType *wanted);

/** @brief Asks to be told when an object emits a signal.
 * @param objects Pool.
 * @param sender Object that emits.
 * @param signal Signal name, which the sender's type must declare.
 * @param call Function to call on each emission.
 * @param user Passed back to call and release.
 * @param release Called once when the connection ends, to free user; may be NULL.
 * @return A positive connection id, or zero on failure, when release has already been called. */
int EngineObjectConnect(EngineObjects *objects, EngineObjectId sender, const char *signal,
                        EngineSignalFn call, void *user, EngineReleaseFn release);

/** @brief Ends one connection.
 * @param objects Pool.
 * @param connection Id from EngineObjectConnect.
 * @return True when a connection was ended. */
bool EngineObjectDisconnect(EngineObjects *objects, int connection);

/** @brief Tells everything connected to a signal that it happened, in the order they connected.
 *
 * A handler may destroy objects, connect or disconnect; connections made during an emission are
 * not called by it.
 * @param objects Pool.
 * @param sender Emitting object.
 * @param signal Signal name, which the sender's type must declare.
 * @param arguments Values handed to each handler.
 * @param count Number of values.
 * @return How many handlers were called, or -1 when the signal is not the sender's. */
int EngineObjectEmit(EngineObjects *objects, EngineObjectId sender, const char *signal,
                     const EngineValue *arguments, int count);

/** @brief Advances every object whose type has a step, once.
 * @param objects Pool.
 * @param dt Seconds to advance.
 * @return No value. Objects created during the step are first stepped next time. */
void EngineObjectsStep(EngineObjects *objects, float dt);

#endif
