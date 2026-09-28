/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "script_network.h"

#include <stdlib.h>
#include <string.h>

/* One schema per shared engine type, built once at creation from CoreNetFieldsFromType. Kept
   heap-allocated for the network object's lifetime, because CoreNetSyncRegister keeps only a
   borrowed pointer to what it is given (net_sync.h). */
#define SCRIPT_NETWORK_FIELDS_MAX 32
typedef struct ScriptNetworkSchema
{
    CoreNetSchema schema;
    CoreNetField fields[SCRIPT_NETWORK_FIELDS_MAX];
} ScriptNetworkSchema;

/* A replicated object and the engine object it stands for. created is true only for one this
   network object made itself -- a joiner's answer to appeared -- which is what leave! and destroy
   are allowed to end; an object share! was given is the caller's and is never destroyed here. */
typedef struct ScriptNetworkPairing
{
    uint32_t netId;
    EngineObjectId object;
    bool created;
} ScriptNetworkPairing;
#define SCRIPT_NETWORK_PAIRING_CAPACITY 256
#define SCRIPT_NETWORK_NAME_CAPACITY 64

typedef struct ScriptNetwork
{
    CoreNetSession session;
    EngineObjects *objects; // the pool this object and everything it shares or creates live in
    EngineObjectId self;
    char gameName[SCRIPT_NETWORK_NAME_CAPACITY];
    uint32_t version;
    ScriptNetworkSchema *schemas;
    size_t schemaCount;
    ScriptNetworkPairing pairings[SCRIPT_NETWORK_PAIRING_CAPACITY];
    size_t pairingCount;
} ScriptNetwork;

static bool SameObjectId(EngineObjectId a, EngineObjectId b)
{
    return a.index == b.index && a.generation == b.generation;
}

static bool AddPairing(ScriptNetwork *net, uint32_t netId, EngineObjectId object, bool created)
{
    if (net->pairingCount >= SCRIPT_NETWORK_PAIRING_CAPACITY)
        return false;
    net->pairings[net->pairingCount++] = (ScriptNetworkPairing){netId, object, created};
    return true;
}

// Ends every pairing this network object made itself; one share! was given is left alone.
static void ClearPairings(ScriptNetwork *net)
{
    for (size_t i = 0; i < net->pairingCount; i++)
        if (net->pairings[i].created)
            EngineObjectDestroy(net->objects, net->pairings[i].object);
    net->pairingCount = 0;
}

static uint16_t SchemaIdForTypeName(const CoreNetSync *sync, const char *name)
{
    for (size_t i = 0; i < sync->schemaCount; i++)
        if (!strcmp(sync->schemas[i]->name, name))
            return sync->schemas[i]->type;
    return 0;
}

// A plain flag scan, independent of whether CoreNetFieldsFromType could actually place the
// property: the two must be told apart, so a type with nothing shared is quietly skipped while one
// whose shared property cannot go on the wire fails creation instead of being skipped just the same.
static bool TypeHasSharedProperty(const EngineType *type)
{
    for (; type; type = type->parent)
        for (int i = 0; i < type->propertyCount; i++)
            if (type->properties[i].flags & ENGINE_PROPERTY_SHARED)
                return true;
    return false;
}

// ---- session callbacks --------------------------------------------------------------------------
static bool NetworkRegisterSchemas(void *user, CoreNetSync *sync)
{
    ScriptNetwork *net = user;
    for (size_t i = 0; i < net->schemaCount; i++)
        if (!CoreNetSyncRegister(sync, &net->schemas[i].schema))
            return false;
    return true;
}

// A joiner's only welcome payload is the fact of being welcomed; the moment to say so is here,
// the one place CoreNetSessionJoin's caller learns of it before status flips to WELCOMED.
static bool NetworkReadWelcome(void *user, CoreNetReader *reader)
{
    (void)reader;
    ScriptNetwork *net = user;
    EngineObjectEmit(net->objects, net->self, "welcomed", NULL, 0);
    return true;
}

static void NetworkJoined(void *user, CoreNetSync *sync, uint16_t actor)
{
    (void)sync;
    ScriptNetwork *net = user;
    EngineObjectEmit(net->objects, net->self, "joined", (EngineValue[]){EngineInt(actor)}, 1);
}

static void NetworkLeft(void *user, uint16_t actor)
{
    ScriptNetwork *net = user;
    EngineObjectEmit(net->objects, net->self, "left", (EngineValue[]){EngineInt(actor)}, 1);
}

// Joiner only: a snapshot brought an object nobody here had a pairing for yet.
static void NetworkAppeared(void *user, CoreNetSync *sync, CoreNetObject *object)
{
    ScriptNetwork *net = user;
    const EngineType *type = EngineObjectsTypeNamed(net->objects, object->schema->name);
    if (!type)
        return; // no matching type registered here: nothing sensible to make of it
    const char *error = NULL;
    EngineObjectId id = EngineObjectCreate(net->objects, type, NULL, 0, &error);
    if (EngineObjectIdIsNull(id))
        return;
    void *data = EngineObjectData(net->objects, id, type);
    // CoreNetSyncBindState copies the state that already arrived into data as part of binding.
    if (!data || !CoreNetSyncBindState(sync, object->id, data) || !AddPairing(net, object->id, id, true))
    {
        EngineObjectDestroy(net->objects, id);
        return;
    }
    EngineObjectEmit(net->objects, net->self, "appeared", (EngineValue[]){EngineObject(id)}, 1);
}

// Joiner only: a snapshot took an object away. Whatever it was paired with goes with it.
static void NetworkVanished(void *user, uint32_t netId)
{
    ScriptNetwork *net = user;
    for (size_t i = 0; i < net->pairingCount; i++)
        if (net->pairings[i].netId == netId)
        {
            EngineObjectId object = net->pairings[i].object;
            bool created = net->pairings[i].created;
            net->pairings[i] = net->pairings[--net->pairingCount];
            if (created)
                EngineObjectDestroy(net->objects, object);
            return;
        }
}

static CoreNetSessionConfig NetworkConfig(ScriptNetwork *net)
{
    return (CoreNetSessionConfig){
        .game = net->gameName,
        .version = net->version,
        .schemaCapacity = net->schemaCount ? net->schemaCount : 1,
        .user = net,
        .registerSchemas = NetworkRegisterSchemas,
        .readWelcome = NetworkReadWelcome,
        .joined = NetworkJoined,
        .left = NetworkLeft,
        .appeared = NetworkAppeared,
        .vanished = NetworkVanished,
    };
}

// ---- as an engine type ---------------------------------------------------------------------------
static bool NetworkCreate(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    net->objects = call->objects;
    net->self = call->self;
    const char *name = call->arguments[0].as.string;
    if (!name || strlen(name) >= sizeof net->gameName)
    {
        call->error = "the game name is too long";
        return false;
    }
    strcpy(net->gameName, name);
    net->version = (uint32_t)call->arguments[1].as.integer;

    // One schema per registered type with at least one shared property, in registration order.
    size_t capacity = net->objects->typeCount;
    net->schemas = capacity ? calloc(capacity, sizeof *net->schemas) : NULL;
    if (capacity && !net->schemas)
    {
        call->error = "out of memory";
        return false;
    }
    for (size_t i = 0; i < net->objects->typeCount; i++)
    {
        const EngineType *type = net->objects->types[i];
        if (!TypeHasSharedProperty(type))
            continue;
        ScriptNetworkSchema *entry = &net->schemas[net->schemaCount];
        size_t written = CoreNetFieldsFromType(type, entry->fields, SCRIPT_NETWORK_FIELDS_MAX);
        if (!written)
        {
            // A property was marked shared but cannot go on the wire (or there were too many to
            // fit): said loudly here rather than quietly leaving that type unreplicated.
            call->error = "a registered type marks a property shared that cannot go on the wire";
            free(net->schemas);
            net->schemas = NULL;
            return false;
        }
        entry->schema = (CoreNetSchema){
            .type = (uint16_t)(net->schemaCount + 1),
            .name = type->name,
            .stateSize = type->size,
            .fields = entry->fields,
            .fieldCount = written,
            .authority = CORE_NET_AUTHORITY_SERVER,
        };
        net->schemaCount++;
    }
    return true;
}

static void NetworkDestroy(void *data)
{
    ScriptNetwork *net = data;
    ClearPairings(net);
    CoreNetSessionLeave(&net->session);
    free(net->schemas);
}

static void NetworkStep(EngineObjects *objects, EngineObjectId self, void *data, float dt)
{
    (void)objects;
    (void)self;
    CoreNetSessionStep(&((ScriptNetwork *)data)->session, dt, 0);
}

static bool NetworkHost(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    int port = call->arguments[0].as.integer;
    CoreNetSessionConfig config = NetworkConfig(net);
    call->result = EngineBool(port >= 0 && port <= UINT16_MAX &&
                              CoreNetSessionHost(&net->session, &config, (uint16_t)port, true));
    return true;
}

static bool NetworkJoin(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    int port = call->arguments[1].as.integer;
    CoreNetSessionConfig config = NetworkConfig(net);
    call->result = EngineBool(port >= 0 && port <= UINT16_MAX &&
                              CoreNetSessionJoin(&net->session, &config, call->arguments[0].as.string,
                                                 (uint16_t)port));
    return true;
}

static bool NetworkReady(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    call->result = EngineBool(CoreNetSessionReady(&net->session));
    return true;
}

static bool NetworkLeave(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    ClearPairings(net);
    CoreNetSessionLeave(&net->session);
    return true;
}

static bool NetworkShare(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    EngineObjectId target = call->arguments[0].as.object;
    const EngineType *type = EngineObjectTypeOf(call->objects, target);
    void *data = type ? EngineObjectData(call->objects, target, type) : NULL;
    if (!net->session.isServer || net->session.status == CORE_NET_SESSION_OFF || !data)
    {
        call->result = EngineBool(false);
        return true;
    }
    uint16_t schemaId = SchemaIdForTypeName(&net->session.sync, type->name);
    CoreNetObject *netObject =
        schemaId ? CoreNetSyncSpawnNext(&net->session.sync, schemaId, CORE_NET_SERVER_ACTOR) : NULL;
    if (!netObject)
    {
        call->result = EngineBool(false);
        return true;
    }
    // CoreNetSyncBindState copies the freshly spawned (zeroed) state into data as part of binding;
    // put back what the script had already set before anyone reads it off the wire.
    unsigned char before[CORE_NET_STATE_MAX];
    memcpy(before, data, type->size);
    bool bound = CoreNetSyncBindState(&net->session.sync, netObject->id, data);
    if (bound)
        memcpy(data, before, type->size);
    if (!bound || !AddPairing(net, netObject->id, target, false))
    {
        CoreNetSyncDespawn(&net->session.sync, netObject->id);
        call->result = EngineBool(false);
        return true;
    }
    call->result = EngineBool(true);
    return true;
}

static bool NetworkMine(EngineCall *call)
{
    ScriptNetwork *net = call->data;
    EngineObjectId target = call->arguments[0].as.object;
    for (size_t i = 0; i < net->pairingCount; i++)
        if (SameObjectId(net->pairings[i].object, target))
        {
            call->result =
                EngineBool(CoreNetSessionIsMine(&net->session, CoreNetSyncFind(&net->session.sync,
                                                                               net->pairings[i].netId)));
            return true;
        }
    call->result = EngineBool(false);
    return true;
}

static const char *StatusName(CoreNetSessionStatus status)
{
    switch (status)
    {
        case CORE_NET_SESSION_OFF: return "off";
        case CORE_NET_SESSION_CONNECTING: return "connecting";
        case CORE_NET_SESSION_WELCOMED: return "welcomed";
        case CORE_NET_SESSION_ACTIVE: return "active";
        case CORE_NET_SESSION_FAILED: return "failed";
    }
    return "off";
}
static bool NetworkStatus(const void *object, EngineValue *out)
{
    *out = EngineString(StatusName(((const ScriptNetwork *)object)->session.status));
    return true;
}
static bool NetworkActor(const void *object, EngineValue *out)
{
    *out = EngineInt(((const ScriptNetwork *)object)->session.localActor);
    return true;
}
static bool NetworkIsServer(const void *object, EngineValue *out)
{
    *out = EngineBool(((const ScriptNetwork *)object)->session.isServer);
    return true;
}
static bool NetworkRefusal(const void *object, EngineValue *out)
{
    *out = EngineString(CoreNetRefusalText(((const ScriptNetwork *)object)->session.refusal));
    return true;
}
static bool NetworkPort(const void *object, EngineValue *out)
{
    *out = EngineInt((int)CoreNetPort(&((const ScriptNetwork *)object)->session.endpoint));
    return true;
}

static const EngineProperty networkProperties[] = {
    ENGINE_COMPUTED("status", ENGINE_STRING, ENGINE_PROPERTY_READ_ONLY, NetworkStatus, NULL,
                    "off, connecting, welcomed, active or failed"),
    ENGINE_COMPUTED("actor", ENGINE_INT, ENGINE_PROPERTY_READ_ONLY, NetworkActor, NULL,
                    "which player this machine is; zero on a dedicated server"),
    ENGINE_COMPUTED("server", ENGINE_BOOL, ENGINE_PROPERTY_READ_ONLY, NetworkIsServer, NULL,
                    "whether this machine is the one hosting"),
    ENGINE_COMPUTED("refusal", ENGINE_STRING, ENGINE_PROPERTY_READ_ONLY, NetworkRefusal, NULL,
                    "why joining failed, once status is failed"),
    ENGINE_COMPUTED("port", ENGINE_INT, ENGINE_PROPERTY_READ_ONLY, NetworkPort, NULL,
                    "the UDP port bound while hosting, useful when host! was given zero"),
};
static const EngineMethod networkMethods[] = {
    {"host!", ENGINE_BOOL, {ENGINE_INT}, 1, NetworkHost,
     "listen for joiners on a port; zero lets the system pick one (see the port property)"},
    {"join!", ENGINE_BOOL, {ENGINE_STRING, ENGINE_INT}, 2, NetworkJoin,
     "start joining a server at an address and port"},
    {"ready!", ENGINE_BOOL, {ENGINE_NONE}, 0, NetworkReady,
     "say the world from the welcome is built, so snapshots may start"},
    {"leave!", ENGINE_NONE, {ENGINE_NONE}, 0, NetworkLeave,
     "close the session; objects appeared made are destroyed"},
    {"share!", ENGINE_BOOL, {ENGINE_OBJECT}, 1, NetworkShare,
     "server only: replicate an object, whose storage becomes the replicated state"},
    {"mine?", ENGINE_BOOL, {ENGINE_OBJECT}, 1, NetworkMine,
     "whether this machine decides a shared or appeared object"},
};
static const char *const networkSignals[] = {"joined", "left", "welcomed", "appeared"};

const EngineType ScriptNetworkType = {
    .name = "network",
    .size = sizeof(ScriptNetwork),
    .properties = networkProperties,
    .propertyCount = sizeof networkProperties / sizeof networkProperties[0],
    .methods = networkMethods,
    .methodCount = sizeof networkMethods / sizeof networkMethods[0],
    .signals = networkSignals,
    .signalCount = sizeof networkSignals / sizeof networkSignals[0],
    .createArguments = {ENGINE_STRING, ENGINE_INT},
    .createArgumentCount = 2,
    .createRequired = 2,
    .create = NetworkCreate,
    .destroy = NetworkDestroy,
    .step = NetworkStep,
    .help = "a networked game session that shares engine objects marked ENGINE_PROPERTY_SHARED",
};
