/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_NET_SYNC_H
#define CORE_NET_SYNC_H

#include "network.h"
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CORE_NET_SYNC_MAGIC 0x544e5331u /* TNS1 */
#define CORE_NET_OBJECT_MAGIC 0x544e4f31u /* TNO1 */
#define CORE_NET_OBJECT_NONE 0u
#define CORE_NET_FIELD_INTERPOLATED 1u

typedef enum CoreNetFieldType
{
    CORE_NET_U8,
    CORE_NET_BOOL,
    CORE_NET_I32,
    CORE_NET_U32,
    CORE_NET_F32,
    CORE_NET_VECTOR2,
    CORE_NET_VECTOR3
} CoreNetFieldType;

typedef enum CoreNetAuthority
{
    CORE_NET_AUTHORITY_SERVER,
    CORE_NET_AUTHORITY_OWNER
} CoreNetAuthority;

typedef struct CoreNetField
{
    const char *name;
    CoreNetFieldType type;
    size_t offset, size;
    unsigned int flags;
} CoreNetField;

#define CORE_NET_FIELD(State, member, kind)                                                       \
    {.name = #member,                                                                             \
     .type = kind,                                                                                \
     .offset = offsetof(State, member),                                                           \
     .size = sizeof(((State *)0)->member),                                                        \
     .flags = 0}
#define CORE_NET_FIELD_LERP(State, member, kind)                                                  \
    {.name = #member,                                                                             \
     .type = kind,                                                                                \
     .offset = offsetof(State, member),                                                           \
     .size = sizeof(((State *)0)->member),                                                        \
     .flags = CORE_NET_FIELD_INTERPOLATED}

typedef struct CoreNetSchema
{
    uint16_t type;
    const char *name;
    size_t stateSize;
    const CoreNetField *fields;
    size_t fieldCount;
    CoreNetAuthority authority;
} CoreNetSchema;

typedef struct CoreNetObject
{
    uint32_t id;
    uint16_t owner;
    const CoreNetSchema *schema;
    void *state;
    void *previous;
    bool active, hasPrevious;
} CoreNetObject;

typedef struct CoreNetSync
{
    const CoreNetSchema **schemas;
    size_t schemaCount, schemaCapacity;
    CoreNetObject *objects;
    size_t objectCount, objectCapacity;
    uint32_t nextId;
    bool ready;
} CoreNetSync;

/** @brief Allocates an empty replicated-object registry.
 * @param sync Zeroed caller-owned registry.
 * @param objectCapacity Maximum simultaneously replicated objects; must fit in 16 bits.
 * @param schemaCapacity Maximum registered object schemas.
 * @return True on success; false leaves sync safe to free. */
bool CoreNetSyncInit(CoreNetSync *sync, size_t objectCapacity, size_t schemaCapacity);

/** @brief Releases every replicated state buffer and registry allocation.
 * @param sync Registry to release; NULL is accepted.
 * @return No value. Schema and returned state pointers become invalid. */
void CoreNetSyncFree(CoreNetSync *sync);

/** @brief Registers a stable schema shared by server and clients.
 * @param sync Initialized registry.
 * @param schema Borrowed schema and field array that must outlive sync.
 * @return True when valid and unique; false for unsupported fields, invalid offsets, or duplicate IDs. */
bool CoreNetSyncRegister(CoreNetSync *sync, const CoreNetSchema *schema);

/** @brief Creates a zeroed replicated object with a caller-selected stable identity.
 * @param sync Initialized registry containing schemaType.
 * @param id Nonzero identity unique for the session.
 * @param schemaType Registered schema identifier.
 * @param owner Input/state owner actor number, or zero for a room/server object.
 * @return Borrowed object, or NULL on invalid input, duplicate ID, allocation failure, or capacity. */
CoreNetObject *CoreNetSyncSpawn(CoreNetSync *sync, uint32_t id, uint16_t schemaType, uint16_t owner);

/** @brief Creates an object using the registry's monotonically increasing identity.
 * @param sync Initialized registry.
 * @param schemaType Registered schema identifier.
 * @param owner Input/state owner actor number, or zero for a room/server object.
 * @return Borrowed object, or NULL on invalid input, allocation failure, exhausted IDs, or capacity. */
CoreNetObject *CoreNetSyncSpawnNext(CoreNetSync *sync, uint16_t schemaType, uint16_t owner);

/** @brief Removes one replicated object.
 * @param sync Initialized registry.
 * @param id Object identity.
 * @return True when an active object was removed. */
bool CoreNetSyncDespawn(CoreNetSync *sync, uint32_t id);

/** @brief Finds a replicated object by stable identity.
 * @param sync Initialized registry.
 * @param id Object identity.
 * @return Borrowed object, or NULL when absent. A successful full snapshot read invalidates it. */
CoreNetObject *CoreNetSyncFind(CoreNetSync *sync, uint32_t id);

/** @brief Tests whether an actor may author an object's state.
 * @param object Replicated object.
 * @param actor Actor number attempting the write.
 * @param isServer True when the writer is the authoritative server.
 * @return True for the server, or for the object's owner under owner-authority schemas. */
bool CoreNetObjectCanWrite(const CoreNetObject *object, uint16_t actor, bool isServer);

/** @brief Encodes one object's current state for submission to an authority.
 * @param object Active replicated object.
 * @param writer Destination message writer.
 * @return True on success; false marks writer failed for invalid state or insufficient capacity. */
bool CoreNetObjectWrite(const CoreNetObject *object, CoreNetWriter *writer);

/** @brief Validates and applies one object update transactionally.
 * @param sync Registry already containing the identified object.
 * @param reader Complete object-update message.
 * @param actor Actor number that sent the update.
 * @param isServer True only when the sender is the authoritative server.
 * @return True when identity, schema, ownership, fields, and message length were valid. */
bool CoreNetSyncReadObject(CoreNetSync *sync, CoreNetReader *reader, uint16_t actor, bool isServer);

/** @brief Encodes a complete current-state snapshot.
 * @param sync Initialized registry.
 * @param tick Project simulation tick carried with the snapshot.
 * @param writer Destination message writer.
 * @return True on success; false marks writer failed when capacity is insufficient. */
bool CoreNetSyncWrite(const CoreNetSync *sync, uint32_t tick, CoreNetWriter *writer);

/** @brief Applies a complete snapshot transactionally, spawning and removing objects as needed.
 * @param sync Initialized registry with every referenced schema registered.
 * @param reader Snapshot source; malformed input is rejected without changing sync.
 * @param tick Receives the snapshot tick; may be NULL.
 * @return True when the complete snapshot was valid and applied. Existing object pointers are then
 * invalid and must be found again by identity. */
bool CoreNetSyncRead(CoreNetSync *sync, CoreNetReader *reader, uint32_t *tick);

/** @brief Produces render state between an object's previous and latest snapshots.
 * @param object Replicated object.
 * @param alpha Interpolation fraction clamped to zero through one.
 * @param outState Caller storage at least schema stateSize bytes long.
 * @return True on success. Only fields marked CORE_NET_FIELD_INTERPOLATED are blended. */
bool CoreNetObjectSample(const CoreNetObject *object, float alpha, void *outState);

#endif
