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
/* The server, as an actor. Objects it owns carry this, and the machine running the server passes it
   as its own actor id, so "is this mine" is one comparison for players and the server alike. */
#define CORE_NET_SERVER_ACTOR 0u
#define CORE_NET_FIELD_INTERPOLATED 1u
/* When a field is sent. Godot spells these spawn/sync/watch; Netcode for GameObjects splits the
   same idea into WriteField and WriteDelta; Mirror into OnSerialize(initialState) and its dirty
   bitmask. All three agree on the distinction, so it is spelled here too.

   WATCH is the default and the cheapest: a field that has not changed since the receiver's baseline
   is not sent at all. SYNC goes every time regardless, which is what a position being interpolated
   wants -- a steady stream to interpolate along, even when it briefly stops moving. SPAWN goes only
   in an object's first, full state: a thing that is decided once and never changes. */
#define CORE_NET_FIELD_SYNC  2u
#define CORE_NET_FIELD_SPAWN 4u
/* How many sent snapshots each side remembers, so a delta can name one of them as its baseline.
   id Tech 3 keeps 32 (PACKET_BACKUP) and refuses a baseline older than PACKET_BACKUP - 3, which at
   20 snapshots a second is well over a second of loss before a full snapshot is forced. */
#define CORE_NET_SNAPSHOT_BACKUP 32
#define CORE_NET_BASELINE_NONE 0u

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
/* Sent every time, not only when it changed: the steady stream an interpolated value needs. */
#define CORE_NET_FIELD_STREAM(State, member, kind)                                                \
    {.name = #member,                                                                             \
     .type = kind,                                                                                \
     .offset = offsetof(State, member),                                                           \
     .size = sizeof(((State *)0)->member),                                                        \
     .flags = CORE_NET_FIELD_INTERPOLATED | CORE_NET_FIELD_SYNC}
/* Decided once, when the object first appears, and never sent again. */
#define CORE_NET_FIELD_ONCE(State, member, kind)                                                  \
    {.name = #member,                                                                             \
     .type = kind,                                                                                \
     .offset = offsetof(State, member),                                                           \
     .size = sizeof(((State *)0)->member),                                                        \
     .flags = CORE_NET_FIELD_SPAWN}
#define CORE_NET_FIELD_LERP(State, member, kind)                                                  \
    {.name = #member,                                                                             \
     .type = kind,                                                                                \
     .offset = offsetof(State, member),                                                           \
     .size = sizeof(((State *)0)->member),                                                        \
     .flags = CORE_NET_FIELD_INTERPOLATED}

/* Trust the game to say when one of these changed, instead of comparing every field on every send.
   It is the cheaper answer and it is what Mirror, RobustToolbox and SS14 all do -- but all three
   generate the mark into the setter, so it cannot be forgotten. Nothing here can generate it, and a
   forgotten mark is a field that silently stops replicating: a worse fault than the work saved.
   So it is asked for, per schema, by a game that has made every mutation go through one place. */
#define CORE_NET_SCHEMA_EXPLICIT_DIRTY 1u
/* The largest stateSize CoreNetSyncSerialize's scratch buffer can sample a remote object into. */
#define CORE_NET_STATE_MAX 512

typedef struct CoreNetSchema
{
    uint16_t type;
    const char *name;
    size_t stateSize;
    const CoreNetField *fields;
    size_t fieldCount;
    CoreNetAuthority authority;
    unsigned int flags;
} CoreNetSchema;

/* One function per replicated thing, called in both directions -- the registry decides which.
   Writing means this machine owns the object and the callback copies the live game state into the
   replicated one; reading means somebody else owns it and the callback copies the replicated state
   back out into the live game. PUN2's OnPhotonSerializeView works this way for the same reason: two
   separate publish and apply functions drift apart the moment somebody edits one of them, and the
   compiler cannot tell you. */
typedef void (*CoreNetSerializeFn)(void *user, void *state, bool writing);

typedef struct CoreNetObject
{
    uint32_t id;
    uint16_t owner;
    const CoreNetSchema *schema;
    void *state;
    void *previous;
    /* The server ticks the two buffered states were sent at. A client renders in the past and needs
       to know how far apart in time the pair it is interpolating between actually are; a fixed
       alpha assumes a snapshot rate, and is wrong the moment one is late, early or lost. */
    uint32_t previousTick, currentTick;
    uint64_t dirtySerial; /* the registry's change counter when the game last marked this */
    CoreNetSerializeFn serialize;
    void *serializeUser;
    bool ownsState;   /* false when the game supplied the storage: then it is the game's variable */
    bool active, hasPrevious;
} CoreNetObject;

/* One remembered snapshot: what every object looked like at a tick that was sent. Both sides keep a
   ring of these -- the sender to delta against, the receiver to rebuild from -- and a delta names
   which one by tick, so a packet lost in between costs nothing as long as some remembered snapshot
   still matches. */
typedef struct CoreNetRemembered
{
    uint32_t tick;
    uint64_t serial;      /* the change counter when this snapshot was taken */
    uint16_t count;
    bool used;
    struct
    {
        uint32_t id;
        uint16_t type, owner;
        void *state;
    } *objects;
} CoreNetRemembered;

typedef struct CoreNetSync
{
    const CoreNetSchema **schemas;
    size_t schemaCount, schemaCapacity;
    CoreNetObject *objects;
    size_t objectCount, objectCapacity;
    CoreNetRemembered history[CORE_NET_SNAPSHOT_BACKUP];
    uint32_t nextId;
    /* The tick of the newest snapshot applied. Objects are stamped with it so a client can tell how
       far apart in server time the two states it holds for each of them are. */
    uint32_t tick;
    uint64_t dirtySerial;  /* rises on every mark; a snapshot records where it stood */
    uint16_t localActor;   /* which player this machine is */
    bool localIsServer;    /* and whether it also runs the server */
    bool started;     /* a snapshot has been applied, so tick is meaningful */
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

/* A discrete thing a client asks the server to do -- pick this up, put that down -- as opposed to
   the continuous state it publishes about its own body. id Tech 3 splits the same two ways: a
   usercmd every frame for input, and reliable sequenced client commands for one-off actions the
   server executes (SV_ExecuteClientCommand). A client cannot simply write the world, so without a
   path like this it cannot affect it at all: it changes its own copy and the next snapshot puts it
   back where the server still thinks it is. */
typedef struct CoreNetCommand
{
    uint32_t sequence;   /* rises per client; lets the server ignore a repeat */
    uint32_t object;     /* what it is about */
    uint8_t op;          /* what to do, defined by the game */
} CoreNetCommand;

/** @brief Writes a command header; the game's own payload follows it.
 * @param writer Destination.
 * @param command Sequence, target object and operation.
 * @return True when it was written. */
bool CoreNetCommandWrite(CoreNetWriter *writer, CoreNetCommand command);

/** @brief Reads a command header written by CoreNetCommandWrite.
 * @param reader Source, left positioned at the game's payload.
 * @param command Receives the header.
 * @return True when a whole header was read. */
bool CoreNetCommandRead(CoreNetReader *reader, CoreNetCommand *command);

/** @brief Decides whether a command is new, and records it if so.
 *
 * Commands are sent reliably, but a server that has seen a sequence must not act on it twice --
 * picking a thing up twice is not the same as picking it up once. Only a strictly higher sequence
 * is new. Reset the counter when a client connects: a late packet and a reconnected client that
 * restarted its numbering are indistinguishable here, and the server knows which it has.
 * @param lastSequence The highest sequence executed for this client so far; updated on acceptance.
 * @param sequence The incoming command's sequence.
 * @return True when the command has not been executed yet. */
bool CoreNetCommandAccept(uint32_t *lastSequence, uint32_t sequence);

/** @brief Says whether this machine is the one that decides an object's state.
 * @param sync Registry holding this machine's localActor and localIsServer.
 * @param object Borrowed object; NULL or inactive answers false.
 * @return True when the object is owned here, so this machine writes it and everyone else reads. */
bool CoreNetSyncIsMine(const CoreNetSync *sync, const CoreNetObject *object);

/** @brief Points a replicated object at the game's own variable, so no copy is needed.
 *
 * The replicated state then IS the state the game reads and writes every frame: an arriving
 * snapshot is written straight into it, and what is sent is read straight out of it. Mirror's
 * SyncVar, Netcode for GameObjects' NetworkVariable and Godot's replicated properties all work this
 * way, and none of them asks a game to write code that copies between two versions of the same
 * thing -- because the moment two copies exist, they can disagree.
 *
 * The buffer must be at least the schema's stateSize, must outlive the object, and belongs to the
 * caller: the registry never frees it, and keeps using it across snapshots.
 * @param sync Registry holding the object.
 * @param id The object's identity.
 * @param state The game's own variable; NULL hands ownership back to the registry.
 * @return True when the object exists and the binding was made. */
bool CoreNetSyncBindState(CoreNetSync *sync, uint32_t id, void *state);

/** @brief Attaches the function that moves state between an object and the live game.
 *
 * One callback serves both directions, so the two can never disagree about the field order or the
 * meaning of a value. Attach it once after spawning; it is called by CoreNetSyncSerialize.
 * @param sync Registry holding the object.
 * @param id The object's identity.
 * @param serialize Callback, or NULL to detach.
 * @param user Passed back to the callback untouched; the registry does not own it.
 * @return True when the object exists and the callback was attached. */
bool CoreNetSyncObserve(CoreNetSync *sync, uint32_t id, CoreNetSerializeFn serialize, void *user);

/** @brief Says that an object has changed, so the next snapshot carries it.
 *
 * Call it where the change is made. Comparing every field of every object against a baseline on
 * every send finds the same answer at the cost of doing the work for everything that did not move;
 * RobustToolbox raises EntityDirtied at the write site and keeps a per-tick set for this reason
 * (PvsSystem.Dirty.cs), and Mirror sets a dirty bit in the SyncVar setter.
 *
 * An object never marked dirty is still sent when a receiver has not seen it at all, and fields
 * declared CORE_NET_FIELD_SYNC are still sent every time -- those are promises about the wire, not
 * about change. Marks are counted rather than timed, so this is meaningful on a server, which
 * never reads a snapshot and so has no tick of its own.
 * @param sync Registry holding the object.
 * @param id The object's identity.
 * @return True when the object exists and was marked. */
bool CoreNetSyncDirty(CoreNetSync *sync, uint32_t id);

/** @brief Tells a registry who this machine is, so it can refuse to be told its own state.
 *
 * An arriving snapshot must never overwrite an object this machine has authority over: it decides
 * that object, and what came back is its own word, older. On a listen server the same variable is
 * both the simulation's and the client's, and without this it is written twice a cycle -- once
 * live, once by its own echo -- which reads as jitter. Every one of Godot, Netcode for GameObjects
 * and Mirror checks authority on receive for the same reason.
 * @param sync Registry to configure.
 * A listen server is both at once -- a player, and the machine deciding the world -- so it says so
 * with both arguments rather than having to choose.
 * @param localActor Which player this machine is; zero when it is only a server.
 * @param isServer Whether this machine also runs the server, and so decides server-owned objects.
 * @return Nothing. */
void CoreNetSyncSetLocalActor(CoreNetSync *sync, uint16_t localActor, bool isServer);

/** @brief Runs every attached callback in the direction ownership implies.
 *
 * Call once a frame. Objects owned here are written from the live game; the rest are read into it.
 * This is the whole of what a game has to do per frame to keep replicated state and live state
 * together -- there is no separate publish and apply pass to keep in step.
 * @param sync Registry to walk.
 * @param localActor This machine's actor id.
 * @param renderTick Server tick to sample incoming objects at, from CoreNetInterpolatorRenderTick;
 * objects owned here ignore it.
 * @return How many callbacks ran. */
size_t CoreNetSyncSerialize(CoreNetSync *sync, uint16_t localActor, double renderTick);

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

/** @brief Remembers the current state of every object as the snapshot for a tick.
 *
 * Call on the sender immediately after writing a snapshot, and on the receiver immediately after
 * applying one. Both sides then hold the same set of past snapshots, and a delta can name one of
 * them as the baseline it was built against. The oldest remembered snapshot is displaced.
 * @param sync Registry to record.
 * @param tick The tick this snapshot represents.
 * @return True when it was recorded, false on an unusable argument or an allocation failure, in
 * which case the sender must fall back to a full snapshot. */
bool CoreNetSyncRemember(CoreNetSync *sync, uint32_t tick);

/** @brief Writes a snapshot as the difference from a remembered one.
 *
 * Only objects that changed are written, and within them only the fields that changed, so a world
 * that is mostly still costs almost nothing to send. Objects that vanished are named so the
 * receiver can drop them.
 *
 * baselineTick must be a tick the receiver also remembers -- in practice the newest one it has
 * acknowledged. If it is not in this registry's history, the snapshot is written in full instead,
 * which is always correct and is what a receiver that has fallen too far behind needs anyway.
 * @param sync Borrowed registry, already recorded up to the previous tick.
 * @param tick The tick being sent.
 * @param baselineTick The remembered tick to encode against, or CORE_NET_BASELINE_NONE for a full
 * snapshot.
 * @param writer Destination.
 * @return True when a complete snapshot was written. */
bool CoreNetSyncWriteDelta(const CoreNetSync *sync, uint32_t tick, uint32_t baselineTick,
                           CoreNetWriter *writer);

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

/** @brief Samples a replicated object at a point in server time, interpolating the pair it lies
 * between.
 *
 * The client renders the world slightly in the past so that the two snapshots bracketing the moment
 * it is drawing have both arrived; this reads that moment. renderTick is in server ticks and may be
 * fractional. Where it falls between the object's two buffered snapshots decides the blend, so a
 * snapshot that arrived late, early, or not at all changes the blend rather than being assumed
 * evenly spaced -- which is what sampling by a fixed alpha assumes and gets wrong.
 *
 * Fields flagged CORE_NET_FIELD_INTERPOLATED are blended; everything else is taken from the newer
 * snapshot. A renderTick outside the pair is clamped to it, so falling behind holds the oldest
 * state rather than extrapolating into a guess.
 * @param object Borrowed replicated object.
 * @param renderTick Server tick to sample at, fractional; typically now minus an interpolation
 * delay of two or more snapshot intervals.
 * @param outState Caller-owned buffer of the schema's state size.
 * @return True when a state was written, false when an argument is missing or unusable. */
bool CoreNetObjectSampleAt(const CoreNetObject *object, double renderTick, void *outState);

#endif
