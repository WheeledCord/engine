/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_NET_SESSION_H
#define CORE_NET_SESSION_H

#include "net_clock.h"
#include "net_sync.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A multiplayer session: hosting, joining, the handshake, and the per-frame work of keeping the
   replicated registry and the live game together. The registry (net_sync) says what is shared; this
   says who is in the game and moves it between them.

   The machine that hosts IS the server. It keeps one registry, its own player is an actor in it like
   any other, and what that player asks the server to do runs as a direct call rather than a round
   trip through a socket to itself. Netcode for GameObjects does the same: a ServerRpc sent on the
   host is handled in place (ngo NetworkBehaviour.cs:121-139), and Mirror's host talks to itself
   through an in-memory LocalConnectionToServer. A dedicated server is the same thing with no player
   of its own. */

/* The engine's own wire protocol. A peer speaking another one is refused at the door rather than
   misreading every packet after it. */
#define CORE_NET_PROTOCOL 1u
#define CORE_NET_SESSION_ACTORS 16
#define CORE_NET_EVERYONE UINT16_MAX
#define CORE_NET_CONNECT_TIMEOUT 5.0
#define CORE_NET_SESSION_PACKET 1400

typedef enum CoreNetSessionStatus
{
    CORE_NET_SESSION_OFF,        /* not networked */
    CORE_NET_SESSION_CONNECTING, /* a join is out; nothing has answered yet */
    CORE_NET_SESSION_WELCOMED,   /* the server let us in and said what the world is; build it, then Ready */
    CORE_NET_SESSION_ACTIVE,     /* in the game: hosting, or joined and ready */
    CORE_NET_SESSION_FAILED      /* refused, dropped, or never answered; see refusal */
} CoreNetSessionStatus;

typedef enum CoreNetRefusal
{
    CORE_NET_REFUSED_NONE,
    CORE_NET_REFUSED_PROTOCOL, /* a different engine wire protocol */
    CORE_NET_REFUSED_GAME,     /* a different game */
    CORE_NET_REFUSED_VERSION,  /* the same game at a different version */
    CORE_NET_REFUSED_FULL,     /* no free player slot */
    CORE_NET_REFUSED_TIMEOUT,  /* nothing answered */
    CORE_NET_REFUSED_DROPPED   /* the connection was lost */
} CoreNetRefusal;

/* What the game tells the session. Every callback is optional. */
typedef struct CoreNetSessionConfig
{
    /* Two builds may play together only when both agree on these. OpenRA refuses a joiner whose mod
       or version differs (OpenRA Server.cs ValidateClient); a mismatch there is an error message,
       not a desync ten minutes in. */
    const char *game;
    uint32_t version;
    size_t objectCapacity, schemaCapacity;
    int tickRate, sendRate, uploadRate; /* zero picks CORE_NET_TICK_RATE, CORE_NET_SEND_RATE, 30 */
    void *user;
    /* Register the game's schemas. Called once when the session's registry is made. */
    bool (*registerSchemas)(void *user, CoreNetSync *sync);
    /* Server: spawn the objects the server owns from the start (the world, a garrison). */
    void (*started)(void *user, CoreNetSync *sync);
    /* Server: what a joiner needs to build the same world before any snapshot, such as a seed. */
    void (*writeWelcome)(void *user, CoreNetWriter *writer);
    /* Client: read it back. False refuses the welcome and fails the join. */
    bool (*readWelcome)(void *user, CoreNetReader *reader);
    /* Server: an actor is in the game (for the host's own player, as soon as hosting starts). The
       game spawns what that player owns here. */
    void (*joined)(void *user, CoreNetSync *sync, uint16_t actor);
    /* Server: an actor has gone. Everything it owned has already been removed. */
    void (*left)(void *user, uint16_t actor);
    /* Server: accept or reject an owner's upload of its own object, after it is read. False puts the
       object back as it was. */
    bool (*accept)(void *user, const CoreNetObject *object);
    /* Server: a player asks for something. The payload follows the header in reader. */
    void (*command)(void *user, uint16_t actor, const CoreNetCommand *command, CoreNetReader *reader);
    /* A player (including the host's own): the server says something happened. */
    void (*event)(void *user, uint8_t op, uint32_t object, CoreNetReader *reader);
} CoreNetSessionConfig;

typedef struct CoreNetSessionActor
{
    CoreNetPeer peer;
    uint32_t acked;       /* newest snapshot tick this actor said it holds */
    uint32_t lastCommand; /* highest command sequence handled for it */
    bool connected, welcomed, active;
} CoreNetSessionActor;

typedef struct CoreNetSession
{
    CoreNetSessionConfig config;
    CoreNetSessionStatus status;
    CoreNetRefusal refusal;
    CoreNetEndpoint endpoint;
    CoreNetSync sync;
    CoreNetClock clock;          /* server: the tick snapshots are sent at */
    CoreNetInterpolator interp;  /* client: where in server time to draw */
    CoreNetSessionActor actors[CORE_NET_SESSION_ACTORS];
    CoreNetPeer server;
    uint16_t localActor;         /* zero on a dedicated server */
    bool isServer;
    uint32_t tick;               /* client: newest snapshot applied */
    uint32_t commandSequence;    /* client: the last command sent */
    uint32_t commandApplied;     /* the highest of ours the server has handled; never goes back */
    double connectingFor, uploadAccumulator;
} CoreNetSession;

/** @brief Opens a server on this machine.
 * @param session Zeroed or left caller storage; any previous session is left first.
 * @param config Game description and callbacks, copied.
 * @param port UDP port to listen on; zero lets the system pick.
 * @param player True to host with a player of our own (actor 1); false for a dedicated server.
 * @return True when listening; false leaves status FAILED. */
bool CoreNetSessionHost(CoreNetSession *session, const CoreNetSessionConfig *config, uint16_t port,
                        bool player);

/** @brief Starts joining a server. Whether it answers arrives later, in status.
 * @param session Zeroed or left caller storage; any previous session is left first.
 * @param config Game description and callbacks, copied.
 * @param hostname Server name or address.
 * @param port Server UDP port.
 * @return True when the attempt is out; false leaves status FAILED. */
bool CoreNetSessionJoin(CoreNetSession *session, const CoreNetSessionConfig *config,
                        const char *hostname, uint16_t port);

/** @brief Says the world from the welcome is built, so snapshots may start.
 * @param session A session in CORE_NET_SESSION_WELCOMED.
 * @return True when the server was told. */
bool CoreNetSessionReady(CoreNetSession *session);

/** @brief Closes the session and frees its registry.
 * @param session Session to leave; an unused one is accepted.
 * @return No value. Status is OFF afterwards. */
void CoreNetSessionLeave(CoreNetSession *session);

/** @brief Does one frame of networking: handles what arrived, runs every object's serialize
 * callback in the direction ownership says, and sends what is due.
 * @param session Session to step; OFF does nothing.
 * @param dt Seconds since the last step.
 * @param waitMs Milliseconds to wait for the first packet; zero never blocks (a game's frame).
 * @return No value. */
void CoreNetSessionStep(CoreNetSession *session, double dt, uint32_t waitMs);

/** @brief Asks the server to do something. On a host it runs now, in this call.
 * @param session An ACTIVE session with a player of its own.
 * @param op What to do, defined by the game.
 * @param object What it is about.
 * @param payload The game's arguments, or NULL.
 * @param size Payload bytes.
 * @return The command's sequence, for CoreNetSessionCommandDone, or zero when it was not sent. */
uint32_t CoreNetSessionCommand(CoreNetSession *session, uint8_t op, uint32_t object,
                               const void *payload, size_t size);

/** @brief Says whether the server has handled a command yet, so a result can be read.
 * @param session Session that sent it.
 * @param sequence What CoreNetSessionCommand returned.
 * @return True once the server has handled it (accepted or refused). */
bool CoreNetSessionCommandDone(const CoreNetSession *session, uint32_t sequence);

/** @brief Tells players something happened. Server only; the host's own player hears it directly.
 * @param session A hosting session.
 * @param actor One actor, or CORE_NET_EVERYONE.
 * @param op What happened, defined by the game.
 * @param object What it is about.
 * @param payload The game's details, or NULL.
 * @param size Payload bytes.
 * @return True when it was sent or delivered. */
bool CoreNetSessionEvent(CoreNetSession *session, uint16_t actor, uint8_t op, uint32_t object,
                         const void *payload, size_t size);

/** @brief Says whether this machine decides an object: it owns it, or it is the server and the
 * object is the server's.
 * @param session Session holding the object.
 * @param object Borrowed object; NULL answers false.
 * @return True when this machine writes it and everyone else reads it. */
bool CoreNetSessionIsMine(const CoreNetSession *session, const CoreNetObject *object);

/** @brief The server tick this machine is drawing remote objects at.
 * @param session Session to ask.
 * @return A fractional server tick, behind the newest by the interpolation delay. */
double CoreNetSessionRenderTick(const CoreNetSession *session);

/** @brief Says whether an actor is in the game right now.
 * @param session Session to ask.
 * @param actor Actor id.
 * @return True for the host's own player and for joined, ready remote players; the server knows
 * every actor, a client only itself. */
bool CoreNetSessionActorActive(const CoreNetSession *session, uint16_t actor);

/** @brief Plain words for a refusal, for a menu to show.
 * @param refusal A refusal reason.
 * @return A static string. */
const char *CoreNetRefusalText(CoreNetRefusal refusal);

#endif
