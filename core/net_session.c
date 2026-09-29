/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "net_session.h"

#include "enet/enet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTROL_CHANNEL 0
#define STATE_CHANNEL 1
#define UPLOAD_RATE 30

/* Every packet starts with what it is. */
enum
{
    MSG_HELLO = 1,
    MSG_WELCOME,
    MSG_REFUSED,
    MSG_READY,
    MSG_STATE,
    MSG_COMMAND,
    MSG_SNAPSHOT,
    MSG_EVENT
};

static uint32_t GameId(const char *game)
{
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (const unsigned char *c = (const unsigned char *)(game ? game : ""); *c; c++)
        hash = (hash ^ *c) * 16777619u;
    return hash;
}

static bool WriteBytes(CoreNetWriter *writer, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; i++)
        if (!CoreNetWriteU8(writer, bytes[i]))
            return false;
    return true;
}

static int Rate(int configured, int fallback)
{
    return configured > 0 ? configured : fallback;
}

static bool Open(CoreNetSession *session, const CoreNetSessionConfig *config)
{
    CoreNetSessionLeave(session);
    *session = (CoreNetSession){0};
    session->config = *config;
    session->server = CORE_NET_PEER_NONE;
    for (int i = 0; i < CORE_NET_SESSION_ACTORS; i++)
        session->actors[i].peer = CORE_NET_PEER_NONE;
    size_t objects = config->objectCapacity ? config->objectCapacity : 256;
    size_t schemas = config->schemaCapacity ? config->schemaCapacity : 16;
    if (!CoreNetSyncInit(&session->sync, objects, schemas) ||
        (config->registerSchemas && !config->registerSchemas(config->user, &session->sync)))
    {
        CoreNetSyncFree(&session->sync);
        session->status = CORE_NET_SESSION_FAILED;
        return false;
    }
    return true;
}

bool CoreNetSessionHost(CoreNetSession *session, const CoreNetSessionConfig *config, uint16_t port,
                        bool player)
{
    if (!session || !config || !Open(session, config))
        return false;
    if (!CoreNetOpenServer(&session->endpoint, port, CORE_NET_SESSION_ACTORS - 1, 2))
    {
        CoreNetSyncFree(&session->sync);
        session->status = CORE_NET_SESSION_FAILED;
        fprintf(stderr, "network: could not listen on UDP port %u\n", (unsigned int)port);
        return false;
    }
    int tickRate = Rate(config->tickRate, CORE_NET_TICK_RATE);
    CoreNetClockInit(&session->clock, tickRate, Rate(config->sendRate, CORE_NET_SEND_RATE));
    session->isServer = true;
    session->localActor = player ? 1 : 0;
    CoreNetSyncSetLocalActor(&session->sync, session->localActor, true);
    session->status = CORE_NET_SESSION_ACTIVE;
    if (config->started)
        config->started(config->user, &session->sync);
    if (player && config->joined)
        config->joined(config->user, &session->sync, session->localActor);
    fprintf(stderr, "network: serving on UDP port %u\n", (unsigned int)CoreNetPort(&session->endpoint));
    return true;
}

bool CoreNetSessionJoin(CoreNetSession *session, const CoreNetSessionConfig *config,
                        const char *hostname, uint16_t port)
{
    if (!session || !config || !Open(session, config))
        return false;
    if (!CoreNetOpenClient(&session->endpoint, 2))
    {
        CoreNetSyncFree(&session->sync);
        session->status = CORE_NET_SESSION_FAILED;
        return false;
    }
    session->server = CoreNetConnect(&session->endpoint, hostname, port);
    if (session->server == CORE_NET_PEER_NONE)
    {
        CoreNetClose(&session->endpoint);
        CoreNetSyncFree(&session->sync);
        session->status = CORE_NET_SESSION_FAILED;
        return false;
    }
    CoreNetInterpolatorInit(&session->interp, Rate(config->tickRate, CORE_NET_TICK_RATE),
                            Rate(config->sendRate, CORE_NET_SEND_RATE));
    session->status = CORE_NET_SESSION_CONNECTING;
    return true;
}

bool CoreNetSessionReady(CoreNetSession *session)
{
    if (!session || session->isServer || session->status != CORE_NET_SESSION_WELCOMED)
        return false;
    unsigned char packet[1] = {MSG_READY};
    if (!CoreNetSend(&session->endpoint, session->server, CONTROL_CHANNEL, packet, sizeof packet,
                     true))
        return false;
    session->status = CORE_NET_SESSION_ACTIVE;
    return true;
}

void CoreNetSessionLeave(CoreNetSession *session)
{
    if (!session || session->status == CORE_NET_SESSION_OFF)
        return;
    if (!session->isServer && session->server != CORE_NET_PEER_NONE)
    {
        CoreNetDisconnect(&session->endpoint, session->server, false);
        CoreNetFlush(&session->endpoint);
    }
    CoreNetClose(&session->endpoint);
    CoreNetSyncFree(&session->sync);
    *session = (CoreNetSession){0};
    session->server = CORE_NET_PEER_NONE;
}

static void Fail(CoreNetSession *session, CoreNetRefusal refusal)
{
    session->status = CORE_NET_SESSION_FAILED;
    session->refusal = refusal;
}

/* ---- server ---------------------------------------------------------------------------------- */

static int ActorOfPeer(const CoreNetSession *session, CoreNetPeer peer)
{
    for (int a = 1; a < CORE_NET_SESSION_ACTORS; a++)
        if (session->actors[a].connected && session->actors[a].peer == peer)
            return a;
    return 0;
}

static void Refuse(CoreNetSession *session, CoreNetPeer peer, CoreNetRefusal refusal)
{
    unsigned char packet[2] = {MSG_REFUSED, (unsigned char)refusal};
    CoreNetSend(&session->endpoint, peer, CONTROL_CHANNEL, packet, sizeof packet, true);
    CoreNetDisconnect(&session->endpoint, peer, true);
}

static void Welcome(CoreNetSession *session, int actor, CoreNetReader *reader)
{
    uint32_t protocol = 0, game = 0, version = 0;
    CoreNetPeer peer = session->actors[actor].peer;
    CoreNetRefusal refusal = CORE_NET_REFUSED_NONE;
    if (!CoreNetReadU32(reader, &protocol) || protocol != CORE_NET_PROTOCOL)
        refusal = CORE_NET_REFUSED_PROTOCOL;
    else if (!CoreNetReadU32(reader, &game) || game != GameId(session->config.game))
        refusal = CORE_NET_REFUSED_GAME;
    else if (!CoreNetReadU32(reader, &version) || version != session->config.version)
        refusal = CORE_NET_REFUSED_VERSION;
    if (refusal != CORE_NET_REFUSED_NONE)
    {
        Refuse(session, peer, refusal);
        return;
    }
    unsigned char packet[CORE_NET_SESSION_PACKET];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    CoreNetWriteU8(&writer, MSG_WELCOME);
    CoreNetWriteU16(&writer, (uint16_t)actor);
    if (session->config.writeWelcome)
        session->config.writeWelcome(session->config.user, &writer);
    if (writer.failed)
        return;
    session->actors[actor].welcomed = true;
    CoreNetSend(&session->endpoint, peer, CONTROL_CHANNEL, packet, writer.size, true);
}

static void RemoveActor(CoreNetSession *session, int actor)
{
    bool wasActive = session->actors[actor].active;
    /* Whatever the player owned goes with them, as PUN2 cleans up a leaving player's objects and
       Netcode for GameObjects destroys a disconnecting client's player object. */
    for (size_t i = 0; i < session->sync.objectCapacity; i++)
    {
        const CoreNetObject *object = &session->sync.objects[i];
        if (object->active && object->owner == (uint16_t)actor)
            CoreNetSyncDespawn(&session->sync, object->id);
    }
    session->actors[actor] = (CoreNetSessionActor){.peer = CORE_NET_PEER_NONE};
    if (wasActive && session->config.left)
        session->config.left(session->config.user, (uint16_t)actor);
    fprintf(stderr, "network: player %d left\n", actor);
}

static void ReadUpload(CoreNetSession *session, int actor, CoreNetReader *reader)
{
    uint32_t ack = CORE_NET_BASELINE_NONE;
    if (!CoreNetReadU32(reader, &ack))
        return;
    if (ack > session->actors[actor].acked) /* never backwards: packets arrive out of order */
        session->actors[actor].acked = ack;
    /* Stamp what arrives with the server's tick, so the host can draw it interpolated between the
       last two uploads the way a client draws snapshots. */
    session->sync.tick = session->clock.tick;
    while (reader->at < reader->size && !reader->failed)
    {
        CoreNetReader peek = *reader;
        uint32_t magic = 0, id = 0;
        if (!CoreNetReadU32(&peek, &magic) || !CoreNetReadU32(&peek, &id))
            break;
        CoreNetObject *object = CoreNetSyncFind(&session->sync, id);
        bool first = object && !object->hasPrevious;
        if (!CoreNetSyncReadObject(&session->sync, reader, (uint16_t)actor, false))
            break;
        /* A first upload has nothing before it to blend from; without this it blends from zero. */
        if (first)
        {
            memcpy(object->previous, object->state, object->schema->stateSize);
            object->previousTick = object->currentTick;
        }
        if (session->config.accept && !session->config.accept(session->config.user, object))
        {
            memcpy(object->state, object->previous, object->schema->stateSize);
            fprintf(stderr, "network: player %d sent an unacceptable state for object %u\n", actor,
                    (unsigned int)id);
        }
    }
}

static void RunCommand(CoreNetSession *session, int actor, CoreNetReader *reader)
{
    CoreNetCommand command;
    if (!CoreNetCommandRead(reader, &command) ||
        !CoreNetCommandAccept(&session->actors[actor].lastCommand, command.sequence))
        return;
    if (session->config.command)
        session->config.command(session->config.user, (uint16_t)actor, &command, reader);
}

static void ServerHandle(CoreNetSession *session, const CoreNetEvent *event)
{
    int actor = ActorOfPeer(session, event->peer);
    if (event->type == CORE_NET_EVENT_CONNECTED)
    {
        for (int a = session->localActor + 1; a < CORE_NET_SESSION_ACTORS; a++)
            if (!session->actors[a].connected)
            {
                session->actors[a] = (CoreNetSessionActor){.peer = event->peer, .connected = true};
                return;
            }
        Refuse(session, event->peer, CORE_NET_REFUSED_FULL);
        return;
    }
    if (!actor)
        return;
    if (event->type == CORE_NET_EVENT_DISCONNECTED)
    {
        RemoveActor(session, actor);
        return;
    }
    if (event->type != CORE_NET_EVENT_RECEIVED || !event->size)
        return;
    CoreNetReader reader = CoreNetReaderBegin(event->data, event->size);
    uint8_t kind = 0;
    CoreNetReadU8(&reader, &kind);
    CoreNetSessionActor *who = &session->actors[actor];
    if (kind == MSG_HELLO && !who->welcomed)
        Welcome(session, actor, &reader);
    else if (kind == MSG_READY && who->welcomed && !who->active)
    {
        /* Joining is id Tech 3's three steps (server.h CS_CONNECTED / CS_PRIMED / CS_ACTIVE): a
           connection, a welcome that says what the world is, and only once the joiner has built it,
           snapshots. */
        who->active = true;
        who->acked = CORE_NET_BASELINE_NONE;
        who->lastCommand = 0;
        if (session->config.joined)
            session->config.joined(session->config.user, &session->sync, (uint16_t)actor);
        fprintf(stderr, "network: player %d joined\n", actor);
    }
    else if (kind == MSG_STATE && who->active)
        ReadUpload(session, actor, &reader);
    else if (kind == MSG_COMMAND && who->active)
        RunCommand(session, actor, &reader);
}

static void SendSnapshots(CoreNetSession *session)
{
    uint32_t tick = session->clock.tick;
    for (int a = 1; a < CORE_NET_SESSION_ACTORS; a++)
    {
        CoreNetSessionActor *who = &session->actors[a];
        if (!who->active || who->peer == CORE_NET_PEER_NONE)
            continue;
        /* ENet fragments a message larger than one datagram, so this is a ceiling on the world, not
           on a packet. */
        static unsigned char packet[64 * 1024];
        CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
        /* Which of this player's commands have been handled, as RobustToolbox puts
           LastProcessedInput in every state: without it a refusal and a slow reply look the same. */
        if (CoreNetWriteU8(&writer, MSG_SNAPSHOT) && CoreNetWriteU32(&writer, who->lastCommand) &&
            CoreNetSyncWriteDelta(&session->sync, tick, who->acked, &writer))
            CoreNetSend(&session->endpoint, who->peer, STATE_CHANNEL, packet, writer.size, false);
    }
    CoreNetSyncRemember(&session->sync, tick);
}

static void ServerStep(CoreNetSession *session, double dt, uint32_t waitMs)
{
    CoreNetEvent event;
    while (CoreNetPoll(&session->endpoint, waitMs, &event))
    {
        waitMs = 0;
        ServerHandle(session, &event);
        CoreNetEventFree(&event);
    }
    int ticks = CoreNetClockAdvance(&session->clock, dt, 16);
    for (int i = 0; i < ticks; i++)
        CoreNetClockTicked(&session->clock);
    CoreNetSyncSetLocalActor(&session->sync, session->localActor, true);
    CoreNetSyncSerialize(&session->sync, session->localActor, CoreNetSessionRenderTick(session));
    if (CoreNetClockShouldSend(&session->clock))
        SendSnapshots(session);
}

/* ---- client ---------------------------------------------------------------------------------- */

static void SendHello(CoreNetSession *session)
{
    unsigned char packet[16];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    if (CoreNetWriteU8(&writer, MSG_HELLO) && CoreNetWriteU32(&writer, CORE_NET_PROTOCOL) &&
        CoreNetWriteU32(&writer, GameId(session->config.game)) &&
        CoreNetWriteU32(&writer, session->config.version))
        CoreNetSend(&session->endpoint, session->server, CONTROL_CHANNEL, packet, writer.size, true);
}

/* Every object id active right now, so a caller can tell what a coming CoreNetSyncRead adds or
   removes. CoreNetSyncRead replaces the registry's object storage wholesale on every applied read
   (net_sync.h), so nothing about the current set survives that call except what was captured here
   beforehand -- the same reason CoreNetSyncBindState's caller-owned buffer has to be handed back in
   rather than found again afterwards. */
static uint32_t *ActiveIds(const CoreNetSync *sync, size_t *count)
{
    *count = 0;
    uint32_t *ids = malloc(sync->objectCapacity * sizeof *ids);
    if (!ids)
        return NULL;
    for (size_t i = 0; i < sync->objectCapacity; i++)
        if (sync->objects[i].active)
            ids[(*count)++] = sync->objects[i].id;
    return ids;
}

static bool IdWas(const uint32_t *ids, size_t count, uint32_t id)
{
    for (size_t i = 0; i < count; i++)
        if (ids[i] == id)
            return true;
    return false;
}

static void ReadSnapshot(CoreNetSession *session, CoreNetReader *reader)
{
    uint32_t applied = 0, tick = 0;
    if (!CoreNetReadU32(reader, &applied))
        return;
    /* Snapshots arrive out of order; an older one must not un-handle a command. */
    if (applied > session->commandApplied)
        session->commandApplied = applied;

    bool watching = session->config.appeared || session->config.vanished;
    size_t beforeCount = 0;
    uint32_t *before = watching ? ActiveIds(&session->sync, &beforeCount) : NULL;

    if (CoreNetSyncRead(&session->sync, reader, &tick) && tick != session->tick)
    {
        session->tick = tick;
        CoreNetSyncRemember(&session->sync, tick);
        CoreNetInterpolatorSnapshot(&session->interp, tick);

        if (before)
        {
            if (session->config.vanished)
                for (size_t i = 0; i < beforeCount; i++)
                    if (!CoreNetSyncFind(&session->sync, before[i]))
                        session->config.vanished(session->config.user, before[i]);
            if (session->config.appeared)
                for (size_t i = 0; i < session->sync.objectCapacity; i++)
                {
                    CoreNetObject *object = &session->sync.objects[i];
                    if (object->active && !IdWas(before, beforeCount, object->id))
                        session->config.appeared(session->config.user, &session->sync, object);
                }
        }
    }
    free(before);
}

static void ClientHandle(CoreNetSession *session, const CoreNetEvent *event)
{
    if (event->type == CORE_NET_EVENT_CONNECTED)
    {
        SendHello(session);
        return;
    }
    if (event->type == CORE_NET_EVENT_DISCONNECTED || event->type == CORE_NET_EVENT_ERROR)
    {
        if (session->status != CORE_NET_SESSION_FAILED)
            Fail(session, CORE_NET_REFUSED_DROPPED);
        return;
    }
    if (event->type != CORE_NET_EVENT_RECEIVED || !event->size)
        return;
    CoreNetReader reader = CoreNetReaderBegin(event->data, event->size);
    uint8_t kind = 0;
    CoreNetReadU8(&reader, &kind);
    if (kind == MSG_REFUSED)
    {
        uint8_t why = CORE_NET_REFUSED_DROPPED;
        CoreNetReadU8(&reader, &why);
        Fail(session, (CoreNetRefusal)why);
    }
    else if (kind == MSG_WELCOME && session->status == CORE_NET_SESSION_CONNECTING)
    {
        uint16_t actor = 0;
        if (!CoreNetReadU16(&reader, &actor) || !actor ||
            (session->config.readWelcome &&
             !session->config.readWelcome(session->config.user, &reader)))
        {
            Fail(session, CORE_NET_REFUSED_GAME);
            return;
        }
        session->localActor = actor;
        CoreNetSyncSetLocalActor(&session->sync, actor, false);
        session->status = CORE_NET_SESSION_WELCOMED;
    }
    else if (kind == MSG_SNAPSHOT && session->status == CORE_NET_SESSION_ACTIVE)
        ReadSnapshot(session, &reader);
    else if (kind == MSG_EVENT && session->status == CORE_NET_SESSION_ACTIVE)
    {
        uint8_t op = 0;
        uint32_t object = 0;
        if (CoreNetReadU8(&reader, &op) && CoreNetReadU32(&reader, &object) &&
            session->config.event)
            session->config.event(session->config.user, op, object, &reader);
    }
}

/* Only what this player owns goes up: a client that could write anything else could write
   anything. The server checks ownership again on arrival. */
static void Upload(CoreNetSession *session)
{
    static unsigned char packet[16 * 1024];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    if (!CoreNetWriteU8(&writer, MSG_STATE) || !CoreNetWriteU32(&writer, session->tick))
        return;
    for (size_t i = 0; i < session->sync.objectCapacity; i++)
    {
        const CoreNetObject *object = &session->sync.objects[i];
        if (!object->active || object->owner != session->localActor ||
            object->schema->authority != CORE_NET_AUTHORITY_OWNER)
            continue;
        if (!CoreNetObjectWrite(object, &writer))
            return;
    }
    /* Sent even when nothing is owned: the acknowledgement alone keeps the server's deltas small. */
    CoreNetSend(&session->endpoint, session->server, STATE_CHANNEL, packet, writer.size, false);
}

static void ClientStep(CoreNetSession *session, double dt, uint32_t waitMs)
{
    CoreNetEvent event;
    while (session->status != CORE_NET_SESSION_OFF && CoreNetPoll(&session->endpoint, waitMs, &event))
    {
        waitMs = 0;
        ClientHandle(session, &event);
        CoreNetEventFree(&event);
    }
    /* A wrong address answers nothing at all, so an attempt has to end by itself. */
    if (session->status == CORE_NET_SESSION_CONNECTING)
    {
        session->connectingFor += dt;
        if (session->connectingFor >= CORE_NET_CONNECT_TIMEOUT)
            Fail(session, CORE_NET_REFUSED_TIMEOUT);
    }
    if (session->status != CORE_NET_SESSION_ACTIVE)
        return;
    CoreNetInterpolatorAdvance(&session->interp, dt);
    CoreNetSyncSetLocalActor(&session->sync, session->localActor, false);
    CoreNetSyncSerialize(&session->sync, session->localActor, CoreNetSessionRenderTick(session));
    session->uploadAccumulator += dt;
    double interval = 1.0 / Rate(session->config.uploadRate, UPLOAD_RATE);
    if (session->uploadAccumulator >= interval)
    {
        session->uploadAccumulator = 0.0;
        Upload(session);
    }
}

/* ENet counts bytes per host in 32 bits and never clears them. Each step takes what it counted into
   the session's 64-bit totals and zeroes its counters, so they cannot wrap. The rates are bytes per
   second over the last whole second of stepped time. */
static void SampleTraffic(CoreNetSession *session, double dt)
{
    ENetHost *host = (ENetHost *)session->endpoint.host;
    if (!host)
        return;
    uint32_t sent = host->totalSentData, received = host->totalReceivedData;
    host->totalSentData = 0;
    host->totalReceivedData = 0;
    session->bytesSent += sent;
    session->bytesReceived += received;
    session->rateSent += sent;
    session->rateReceived += received;
    if (dt > 0)
        session->rateClock += dt;
    if (session->rateClock >= 1.0)
    {
        session->sendRate = (double)session->rateSent / session->rateClock;
        session->receiveRate = (double)session->rateReceived / session->rateClock;
        session->rateClock = 0.0;
        session->rateSent = session->rateReceived = 0;
    }
}

void CoreNetSessionStep(CoreNetSession *session, double dt, uint32_t waitMs)
{
    if (!session || session->status == CORE_NET_SESSION_OFF || !session->endpoint.host)
        return;
    if (session->isServer)
        ServerStep(session, dt, waitMs);
    else
        ClientStep(session, dt, waitMs);
    SampleTraffic(session, dt);
}

/* ---- both -------------------------------------------------------------------------------------- */

uint32_t CoreNetSessionCommand(CoreNetSession *session, uint8_t op, uint32_t object,
                               const void *payload, size_t size)
{
    if (!session || session->status != CORE_NET_SESSION_ACTIVE || !session->localActor ||
        (size && !payload))
        return 0;
    CoreNetCommand command = {++session->commandSequence, object, op};
    if (session->isServer)
    {
        /* The host is the server: its player's request is handled here and now, by the same
           handler a remote player's goes through. */
        CoreNetReader reader = CoreNetReaderBegin(payload, size);
        session->actors[session->localActor].lastCommand = command.sequence;
        if (session->config.command)
            session->config.command(session->config.user, session->localActor, &command, &reader);
        session->commandApplied = command.sequence;
        return command.sequence;
    }
    unsigned char packet[CORE_NET_SESSION_PACKET];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    if (!CoreNetWriteU8(&writer, MSG_COMMAND) || !CoreNetCommandWrite(&writer, command) ||
        !WriteBytes(&writer, payload, size) ||
        !CoreNetSend(&session->endpoint, session->server, CONTROL_CHANNEL, packet, writer.size, true))
        return 0;
    return command.sequence;
}

bool CoreNetSessionCommandDone(const CoreNetSession *session, uint32_t sequence)
{
    return session && sequence && session->commandApplied >= sequence;
}

bool CoreNetSessionEvent(CoreNetSession *session, uint16_t actor, uint8_t op, uint32_t object,
                         const void *payload, size_t size)
{
    if (!session || !session->isServer || session->status != CORE_NET_SESSION_ACTIVE ||
        (size && !payload))
        return false;
    bool delivered = false;
    if (session->localActor && (actor == CORE_NET_EVERYONE || actor == session->localActor))
    {
        CoreNetReader reader = CoreNetReaderBegin(payload, size);
        if (session->config.event)
            session->config.event(session->config.user, op, object, &reader);
        delivered = true;
    }
    unsigned char packet[CORE_NET_SESSION_PACKET];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    if (!CoreNetWriteU8(&writer, MSG_EVENT) || !CoreNetWriteU8(&writer, op) ||
        !CoreNetWriteU32(&writer, object) || !WriteBytes(&writer, payload, size))
        return delivered;
    for (int a = 1; a < CORE_NET_SESSION_ACTORS; a++)
    {
        const CoreNetSessionActor *who = &session->actors[a];
        if (!who->active || who->peer == CORE_NET_PEER_NONE ||
            (actor != CORE_NET_EVERYONE && actor != (uint16_t)a))
            continue;
        delivered |= CoreNetSend(&session->endpoint, who->peer, CONTROL_CHANNEL, packet, writer.size,
                                 true);
    }
    return delivered;
}

bool CoreNetSessionIsMine(const CoreNetSession *session, const CoreNetObject *object)
{
    return session && session->status != CORE_NET_SESSION_OFF &&
           CoreNetSyncIsMine(&session->sync, object);
}

double CoreNetSessionRenderTick(const CoreNetSession *session)
{
    if (!session)
        return 0.0;
    if (!session->isServer)
        return CoreNetInterpolatorRenderTick(&session->interp);
    /* The server has uploads stamped with the tick they arrived at, one upload interval apart; drawing
       one interval behind now always has the pair it needs. */
    const CoreNetClock *clock = &session->clock;
    double now = (double)clock->tick +
                 (clock->tickInterval > 0.0 ? clock->tickAccumulator / clock->tickInterval : 0.0);
    double delay = (double)Rate(session->config.tickRate, CORE_NET_TICK_RATE) /
                   (double)Rate(session->config.uploadRate, UPLOAD_RATE);
    return now - delay;
}

bool CoreNetSessionActorActive(const CoreNetSession *session, uint16_t actor)
{
    if (!session || !actor || actor >= CORE_NET_SESSION_ACTORS ||
        session->status != CORE_NET_SESSION_ACTIVE)
        return false;
    if (actor == session->localActor)
        return true;
    return session->isServer && session->actors[actor].active;
}

const char *CoreNetRefusalText(CoreNetRefusal refusal)
{
    switch (refusal)
    {
    case CORE_NET_REFUSED_NONE: return "";
    case CORE_NET_REFUSED_PROTOCOL: return "the server runs a different engine version";
    case CORE_NET_REFUSED_GAME: return "the server is running a different game";
    case CORE_NET_REFUSED_VERSION: return "the server runs a different version of this game";
    case CORE_NET_REFUSED_FULL: return "the server is full";
    case CORE_NET_REFUSED_TIMEOUT: return "no server answered";
    case CORE_NET_REFUSED_DROPPED: return "the connection was lost";
    }
    return "refused";
}
