/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_STORE_NET_ENET_H
#define CORE_STORE_NET_ENET_H

#include "network.h"
#include "store_net.h"

/* A store_net session over ENet (docs/developer/store.md §9.5b), so the runner and a C game use the
   same socket code. The link opens the endpoint, supplies store_net's send, maps transport peers (on
   a client the host is peer 0; on the host a connecting client takes the lowest free player id from
   2, and that is its peer number), retries a join for 5 s, and counts ENet's own wire totals. The
   caller ticks link.net itself: StoreNetLinkPoll -> StoreNetBeforeTick -> StoreTick ->
   StoreNetAfterTick -> StoreNetLinkFlush. */

#define STORE_NET_LINK_CHANNELS 3       /* 0 reliable, 1 unreliable sequenced (state), 2 effects */
#define STORE_NET_LINK_JOIN_SECONDS 5.0 /* a client with no welcome by then has ended */
#define STORE_NET_LINK_LERPS 32         /* fields a link can register for interpolation */

/* Sees every packet received, before store_net does, and the host's transport events as the channels
   core/replay.h names: REPLAY_PEER_CONNECTED and REPLAY_PEER_LEFT, with peer the player id and no
   bytes. */
typedef void (*StoreNetLinkTap)(void *user, int peer, int channel, const void *data, size_t size);

/* Caller-owned. Read net and the wire totals; the rest is the link's. */
typedef struct StoreNetLink
{
    StoreNet net;                    /* the session: tick it, read its players, hash it */
    uint64_t wireSent, wireReceived; /* ENet's totals, headers, acknowledgements and resends included */
    Store *store;
    StoreNetConfig config; /* the caller's; store_net is given the link's own callbacks, which chain */
    CoreNetEndpoint endpoint;
    CoreNetPeer hostPeer;                      /* a client's ENet peer for the host */
    CoreNetPeer peerOf[STORE_NET_PLAYERS + 1]; /* the host's ENet peer of each player */
    int playerOf[STORE_NET_PLAYERS];           /* the player of each of the host's ENet peers */
    bool open, hosting, connected;
    char address[256];
    uint16_t port;
    double started;
    char why[256]; /* why the session ended; empty while live or joining */
    StoreNetLinkTap tap;
    void *tapUser;
    struct
    {
        StoreKind kind;
        int field;
        bool angle;
    } lerps[STORE_NET_LINK_LERPS];
    int lerpCount;
} StoreNetLink;

/** @brief Hosts a session on a UDP port: opens the endpoint and runs StoreNetHost.
 * @param link Caller-owned; its previous contents are not released.
 * @param store The store to network, with its other hooks installed first (StoreNetHost).
 * @param config The session's callbacks and game name; send is ignored (the link sends).
 * @param port UDP port; 0 lets the system pick one.
 * @return True when hosting; false with StoreNetLinkEnded saying why. Close the link either way. */
bool StoreNetLinkHost(StoreNetLink *link, Store *store, const StoreNetConfig *config, uint16_t port);

/** @brief Joins a session: opens a client endpoint and starts connecting.
 *
 * StoreNetJoin says hello once ENet has connected; until then link.net is empty and not joined. A
 * connection refused before it is made is tried again until 5 s have passed; with no welcome by
 * then, the session has ended.
 * @param link Caller-owned; its previous contents are not released.
 * @param store The store to network.
 * @param config The session's callbacks and game name; send is ignored (the link sends).
 * @param address Host name or numeric address of the host; copied.
 * @param port The host's UDP port.
 * @return True when connecting; false with StoreNetLinkEnded saying why. Close the link either way. */
bool StoreNetLinkJoin(StoreNetLink *link, Store *store, const StoreNetConfig *config, const char *address,
                      uint16_t port);

/** @brief Registers a field for interpolation (StoreNetInterpolate), also for a session not yet joined.
 * @param link Link, after StoreNetLinkHost or StoreNetLinkJoin.
 * @param kind Kind.
 * @param field Name of a shared FLOAT or VEC3 field of kind.
 * @return True when registered; false for an unknown kind or field, another type, or too many. */
bool StoreNetLinkInterpolate(StoreNetLink *link, StoreKind kind, const char *field);

/** @brief Registers an angle field for shortest-arc interpolation (StoreNetInterpolateAngle), also for a
 * session not yet joined.
 * @param link Link, after StoreNetLinkHost or StoreNetLinkJoin.
 * @param kind Kind.
 * @param field Name of a shared FLOAT or VEC3 field of kind, in radians.
 * @return True when registered; false for an unknown kind or field, another type, or too many. */
bool StoreNetLinkInterpolateAngle(StoreNetLink *link, StoreKind kind, const char *field);

/** @brief Hands every ENet event since the last poll to store_net (and to the tap).
 * @param link Link.
 * @param timeoutMs Milliseconds to wait for the first event; 0 does not wait.
 * @return No value. */
void StoreNetLinkPoll(StoreNetLink *link, uint32_t timeoutMs);

/** @brief Sends what store_net queued this tick now.
 * @param link Link.
 * @return No value. */
void StoreNetLinkFlush(StoreNetLink *link);

/** @brief Says whether the session has ended, and why.
 * @param link Link.
 * @return NULL while live or joining; else why (refused, no welcome in 5 s, the host left, or why the
 * link could not open). */
const char *StoreNetLinkEnded(const StoreNetLink *link);

/** @brief Sets the function that sees every received packet and transport event (for a recording).
 * @param link Link, after StoreNetLinkHost or StoreNetLinkJoin (they clear it).
 * @param tap The function, or NULL for none.
 * @param user Passed to tap.
 * @return No value. */
void StoreNetLinkSetTap(StoreNetLink *link, StoreNetLinkTap tap, void *user);

/** @brief Ends the session: disconnects every peer cleanly, closes the endpoint, frees link.net.
 * @param link Link; one that never opened is accepted.
 * @param lingerSeconds A host first waits up to this long for its clients to leave; 0 does not wait.
 * @return No value. The wire totals stay readable. */
void StoreNetLinkClose(StoreNetLink *link, double lingerSeconds);

#endif
