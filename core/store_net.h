/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_STORE_NET_H
#define CORE_STORE_NET_H

#include "store.h"

/* Networking for the things in a store (docs/developer/store.md §9, proposal B3). The host is machine
   0: it owns the world (owner 0) and plays as player 1, unless it is dedicated and plays no one.
   Clients are players 2-16, one machine each, and talk only to the host, which relays. Each machine
   sends the things it owns as deltas against the last state the receiver acknowledged; messages to
   things owned elsewhere travel reliably with their sender's state. Other machines' things are shown
   100 ms behind, every field of one thing from one tick, and their messages are delivered there.

   It opens no socket: packets leave through the config's send callback and arrive through
   StoreNetReceive, so the runner puts ENet under it and the checks an in-memory queue. Transport
   peers are numbered by the caller: on a client the host is peer 0; on the host a client's peer
   number is its player id. Channels: 0 reliable (hello, welcome, refusal, messages with their state,
   states over 1,200 bytes), 1 unreliable and sequenced (state), 2 unreliable (effects). */

#define STORE_NET_PROTOCOL 3u  /* the store's wire protocol; a client speaking another is refused */
#define STORE_NET_PLAYERS 16   /* players 1 (the host) to 16 */

typedef struct StoreNetConfig
{
    void *user;
    /* Sends one packet to a transport peer on a channel; reliable is true for channel 0. */
    bool (*send)(void *user, int peer, int channel, bool reliable, const void *data, size_t size);
    void (*onArrived)(void *user, StoreId thing);            /* a thing appeared from the network */
    void (*onEffect)(void *user, const char *name, const char *what, Vector3 at);
    void (*onJoined)(void *user, int player);                /* client: welcome received */
    void (*onEnded)(void *user, const char *why);            /* refused, dropped, host left */
    const char *game; /* the game's name; a client of another game is refused. Kept, not copied */
    bool dedicated;   /* host only: it plays no one; local owners {0}; clients are players 2 and up */
} StoreNetConfig;

typedef struct StoreNetData StoreNetData; /* private to store_net.c */

/* Caller-owned; prepare with StoreNetHost or StoreNetJoin, release with StoreNetFree. Read the
   members, never write them. */
typedef struct StoreNet
{
    Store *store;
    StoreNetConfig config;
    StoreHooks chained; /* the store's hooks from before, which the net's own call on */
    bool host;          /* this machine hosts */
    bool joined;        /* in a session: always on the host, after the welcome on a client */
    bool ended;         /* the session is over: refused, or the host left */
    int player;         /* this machine's player: 1 on the host (0 dedicated), 0 on a client until
                           welcomed */
    uint32_t players;   /* bit p set for each player in the session */
    StoreNetData *data; /* id maps, captures, peers, message queues */
} StoreNet;

/** @brief Hosts a session over a store: this machine becomes machine 0 and runs owners 0 and 1.
 *
 * Sets the store's local owners to {0, 1} ({0} with config->dedicated) and installs the net's store
 * hooks in front of the ones the store has (install those first; StoreNetFree puts them back).
 * Every live thing the store holds is replicated from the next capture on.
 * @param net Caller-owned; its previous contents are not released.
 * @param store Store with its kinds declared, at a fixed address while net is in use.
 * @param config Callbacks and game name, copied; send is required.
 * @return True when hosting; false for a missing argument, a kind with more than 255 fields, or
 * out of memory, with net safe to StoreNetFree. */
bool StoreNetHost(StoreNet *net, Store *store, const StoreNetConfig *config);

/** @brief Joins a session: sends the hello (protocol, game, kinds) to the host, peer 0.
 *
 * Nothing replicates until the welcome arrives. On the welcome the store's world is emptied, its
 * tick count set to the host's, its local owners set to {the player}, and onJoined called; the
 * joining runner does not spawn its own `game`. A refusal ends the session through onEnded with a
 * message naming what differs.
 * @param net Caller-owned; its previous contents are not released.
 * @param store Store with its kinds declared, at a fixed address while net is in use.
 * @param config Callbacks and game name, copied; send is required.
 * @return True when the hello was sent; false otherwise, with net safe to StoreNetFree. */
bool StoreNetJoin(StoreNet *net, Store *store, const StoreNetConfig *config);

/** @brief Tells the host a transport peer connected; it joins once its hello is accepted.
 * @param net Hosting net.
 * @param peer The peer's number, which becomes its player id (2 to 16).
 * @return No value. */
void StoreNetPeerConnected(StoreNet *net, int peer);

/** @brief Tells the net a transport peer went away. Call between ticks.
 *
 * On the host, for a player who had joined: every guest under a thing that player spawned is
 * detached at its world transform (the store's orphan hook) and sent `orphaned`; every root the
 * player spawned is removed, declared children with it; then the `game` root is sent
 * `player-left` with the player id. On a client, the host leaving ends the session (onEnded).
 * @param net Net.
 * @param peer The transport peer.
 * @return No value. */
void StoreNetPeerLeft(StoreNet *net, int peer);

/** @brief Hands the net a packet that arrived. Call between ticks.
 *
 * Hellos, welcomes, refusals and effects are handled here; states are stored and messages queued
 * for StoreNetBeforeTick. A malformed packet is ignored.
 * @param net Net.
 * @param peer The transport peer it came from.
 * @param channel The channel it came on.
 * @param data Packet bytes, read during the call only.
 * @param size Byte count.
 * @return No value. */
void StoreNetReceive(StoreNet *net, int peer, int channel, const void *data, size_t size);

/** @brief Writes other machines' things as they stood 100 ms ago; call just before StoreTick
 * (B2.2 step 1).
 *
 * First, a thing that a newly arrived state makes this machine's own takes that state's header and
 * fields at once. Then, per sender, renderTick = its newest tick received + ticks since that arrived
 * - 6, and every thing from it is written as it stood at renderTick: fields registered with
 * StoreNetInterpolate blended between the states either side, every other field, the header,
 * spawns and removes from the earlier. Things new here are created raw (no declared children, no
 * `start`; onArrived follows); a thing already here is written only when its owner here is not
 * local (a client) or is the sending player (the host). A thing this machine let go of by its own
 * write is left alone in states whose sender had not yet seen it let go. Last, each message whose
 * sender tick renderTick has reached is sent with StoreSend, forwarded to its owner's machine, or
 * bounced once to the host.
 * @param net Net.
 * @return No value. */
void StoreNetBeforeTick(StoreNet *net);

/** @brief Sends what this tick produced; call just after StoreTick.
 *
 * Every third tick (StoreTickCount % 3 == 0) it captures the world and sends each peer an
 * unreliable delta against the state that peer last acknowledged. In any tick that queued messages
 * for a peer, that peer's delta goes now and reliably, with the messages.
 * @param net Net.
 * @return No value. */
void StoreNetAfterTick(StoreNet *net);

/** @brief Sends an effect played by gameplay code to every other machine; the host relays.
 * @param net Net in a session.
 * @param name What kind of effect (`play-sound`, `burst`), at most 255 bytes.
 * @param what The sound or preset, at most 255 bytes.
 * @param at Where.
 * @return True when sent; false outside a session or in a presentation handler (those stay local). */
bool StoreNetEffect(StoreNet *net, const char *name, const char *what, Vector3 at);

/** @brief Blends a field of other machines' things between states (B3.5).
 *
 * The field of things of kind, or of a kind derived from it, is set each tick between the two
 * received states either side of renderTick; across a parent change it takes the earlier state's
 * value. Unregistered fields step with the earlier state.
 * @param net Net, after StoreNetHost or StoreNetJoin.
 * @param kind Kind.
 * @param field Name of a shared FLOAT or VEC3 field of kind.
 * @return True when registered; false for an unknown kind or field, a local field, or another type. */
bool StoreNetInterpolate(StoreNet *net, StoreKind kind, const char *field);

/** @brief Returns this machine's player id.
 * @param net Net.
 * @return 1 on the host, 0 on a dedicated host, the welcome's id on a client, 0 before it. */
int StoreNetPlayer(const StoreNet *net);

/** @brief Lists the players in the session, ascending.
 * @param net Net.
 * @param out Receives up to max ids; may be NULL to count.
 * @param max Capacity of out.
 * @return The number of players (which may exceed max). */
int StoreNetPlayers(const StoreNet *net, int *out, int max);

/** @brief Hashes the replicated world as every machine should agree on it.
 *
 * 64-bit FNV-1a over each live, non-local thing with a network id, in network id order: its id,
 * kind name, parent's network id, child name, guest flag, owner, spawner and shared fields, with
 * REFs as network ids (0 for a removed thing) and SYMBOLs as names. Equal on every machine once the
 * game has been still. Limit: a SET of REFs or a MAP keyed by REF is kept sorted by local handle,
 * which differs between machines, so such a field can hash differently while holding the same
 * things.
 * @param net Net.
 * @return The hash. */
uint64_t StoreNetStateHash(const StoreNet *net);

/** @brief Releases the net and puts the store's own hooks back; call before StoreFree.
 * @param net Net; NULL is accepted.
 * @return No value. The store keeps its things and local owners. */
void StoreNetFree(StoreNet *net);

#endif
