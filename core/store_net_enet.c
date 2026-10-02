/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#define _DEFAULT_SOURCE /* clock_gettime under -std=c99 */
#include "store_net_enet.h"

#include "replay.h"

#include "enet/enet.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* store_net over ENet (docs/developer/store.md §9.5b): what gameplay/game.c did for itself, so a C
   game gets the same. */

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// ENet's own byte counts since the last look, added to the link's totals; ENet's are then zeroed.
static void Sample(StoreNetLink *link)
{
    ENetHost *host = (ENetHost *)link->endpoint.host;
    if (!host)
        return;
    link->wireSent += host->totalSentData;
    link->wireReceived += host->totalReceivedData;
    host->totalSentData = 0;
    host->totalReceivedData = 0;
}

static void Tap(StoreNetLink *link, int peer, int channel, const void *data, size_t size)
{
    if (link->tap)
        link->tap(link->tapUser, peer, channel, data, size);
}

// ---- store_net's callbacks: the link sends, and passes the rest on to the caller's ------------------
static bool LinkSend(void *user, int peer, int channel, bool reliable, const void *data, size_t size)
{
    StoreNetLink *link = user;
    CoreNetPeer to = CORE_NET_PEER_NONE;
    if (link->hosting && peer >= 2 && peer <= STORE_NET_PLAYERS)
        to = link->peerOf[peer];
    else if (!link->hosting && peer == 0)
        to = link->hostPeer;
    return to != CORE_NET_PEER_NONE && CoreNetSend(&link->endpoint, to, (uint8_t)channel, data, size, reliable);
}

static void LinkArrived(void *user, StoreId thing)
{
    StoreNetLink *link = user;
    if (link->config.onArrived)
        link->config.onArrived(link->config.user, thing);
}

static void LinkEffect(void *user, const char *name, const char *what, Vector3 at)
{
    StoreNetLink *link = user;
    if (link->config.onEffect)
        link->config.onEffect(link->config.user, name, what, at);
}

static void LinkJoined(void *user, int player)
{
    StoreNetLink *link = user;
    if (link->config.onJoined)
        link->config.onJoined(link->config.user, player);
}

static void LinkEnded(void *user, const char *why)
{
    StoreNetLink *link = user;
    if (!link->why[0])
        snprintf(link->why, sizeof link->why, "%s", why && why[0] ? why : "the session ended");
    if (link->config.onEnded)
        link->config.onEnded(link->config.user, why);
}

static StoreNetConfig Wrapped(StoreNetLink *link)
{
    StoreNetConfig c = link->config;
    c.user = link;
    c.send = LinkSend;
    c.onArrived = LinkArrived;
    c.onEffect = LinkEffect;
    c.onJoined = LinkJoined;
    c.onEnded = LinkEnded;
    return c;
}

// ---- opening ------------------------------------------------------------------------------------
static bool Begin(StoreNetLink *link, Store *store, const StoreNetConfig *config)
{
    if (!link)
        return false;
    memset(link, 0, sizeof *link);
    link->hostPeer = CORE_NET_PEER_NONE;
    for (int p = 0; p <= STORE_NET_PLAYERS; p++)
        link->peerOf[p] = CORE_NET_PEER_NONE;
    if (!store || !config)
    {
        snprintf(link->why, sizeof link->why, "no store or no configuration");
        return false;
    }
    link->store = store;
    link->config = *config;
    link->started = Now();
    return true;
}

bool StoreNetLinkHost(StoreNetLink *link, Store *store, const StoreNetConfig *config, uint16_t port)
{
    if (!Begin(link, store, config))
        return false;
    if (!CoreNetOpenServer(&link->endpoint, port, STORE_NET_PLAYERS - 1, STORE_NET_LINK_CHANNELS))
        return snprintf(link->why, sizeof link->why, "could not open UDP port %d", port), false;
    StoreNetConfig c = Wrapped(link);
    if (!StoreNetHost(&link->net, store, &c))
    {
        CoreNetClose(&link->endpoint);
        return snprintf(link->why, sizeof link->why, "the store could not start networking"), false;
    }
    link->open = link->hosting = true;
    link->port = port;
    return true;
}

bool StoreNetLinkJoin(StoreNetLink *link, Store *store, const StoreNetConfig *config, const char *address,
                      uint16_t port)
{
    if (!Begin(link, store, config))
        return false;
    snprintf(link->address, sizeof link->address, "%s", address ? address : "");
    link->port = port;
    if (!CoreNetOpenClient(&link->endpoint, STORE_NET_LINK_CHANNELS))
        return snprintf(link->why, sizeof link->why, "could not open a UDP socket"), false;
    link->hostPeer = CoreNetConnect(&link->endpoint, link->address, port);
    if (link->hostPeer == CORE_NET_PEER_NONE)
    {
        CoreNetClose(&link->endpoint);
        return snprintf(link->why, sizeof link->why, "could not resolve %.200s", link->address), false;
    }
    link->open = true; // link->net stays empty until ENet connects and StoreNetJoin says hello
    return true;
}

static bool AddLerp(StoreNetLink *link, StoreKind kind, const char *field, bool angle)
{
    if (!link || !link->open || link->lerpCount == STORE_NET_LINK_LERPS)
        return false;
    int f = StoreFieldIndex(link->store, kind, field);
    const StoreFieldDecl *d = StoreFieldAt(link->store, kind, f);
    if (!d || (d->flags & STORE_LOCAL) || (d->type != STORE_FLOAT && d->type != STORE_VEC3))
        return false;
    if (link->net.data && !(angle ? StoreNetInterpolateAngle(&link->net, kind, field)
                                  : StoreNetInterpolate(&link->net, kind, field)))
        return false;
    link->lerps[link->lerpCount].kind = kind;
    link->lerps[link->lerpCount].field = f;
    link->lerps[link->lerpCount++].angle = angle;
    return true;
}

bool StoreNetLinkInterpolate(StoreNetLink *link, StoreKind kind, const char *field)
{
    return AddLerp(link, kind, field, false);
}

bool StoreNetLinkInterpolateAngle(StoreNetLink *link, StoreKind kind, const char *field)
{
    return AddLerp(link, kind, field, true);
}

// ---- polling --------------------------------------------------------------------------------------
// A client's connection to the host is up: store_net says hello, and takes the registered fields.
static void Connected(StoreNetLink *link)
{
    link->connected = true;
    StoreNetConfig c = Wrapped(link);
    if (!StoreNetJoin(&link->net, link->store, &c))
    {
        snprintf(link->why, sizeof link->why, "could not say hello to the host");
        return;
    }
    for (int i = 0; i < link->lerpCount; i++)
    {
        const char *name = StoreFieldAt(link->store, link->lerps[i].kind, link->lerps[i].field)->name;
        if (link->lerps[i].angle)
            StoreNetInterpolateAngle(&link->net, link->lerps[i].kind, name);
        else
            StoreNetInterpolate(&link->net, link->lerps[i].kind, name);
    }
}

static void HostEvent(StoreNetLink *link, const CoreNetEvent *e)
{
    int player = 0;
    if (e->peer >= STORE_NET_PLAYERS)
        return;
    if (e->type == CORE_NET_EVENT_CONNECTED)
    {
        // A client's player id is the lowest free one from 2 (store_net.h: its peer number).
        for (int p = 2; p <= STORE_NET_PLAYERS && !player; p++)
            if (link->peerOf[p] == CORE_NET_PEER_NONE)
                player = p;
        if (!player)
        {
            CoreNetDisconnect(&link->endpoint, e->peer, false);
            return;
        }
        link->peerOf[player] = e->peer;
        link->playerOf[e->peer] = player;
        Tap(link, player, REPLAY_PEER_CONNECTED, NULL, 0);
        StoreNetPeerConnected(&link->net, player);
    }
    else if (e->type == CORE_NET_EVENT_DISCONNECTED && link->playerOf[e->peer])
    {
        player = link->playerOf[e->peer];
        link->playerOf[e->peer] = 0;
        link->peerOf[player] = CORE_NET_PEER_NONE;
        Tap(link, player, REPLAY_PEER_LEFT, NULL, 0);
        StoreNetPeerLeft(&link->net, player);
    }
    else if (e->type == CORE_NET_EVENT_RECEIVED && link->playerOf[e->peer])
    {
        Tap(link, link->playerOf[e->peer], e->channel, e->data, e->size);
        StoreNetReceive(&link->net, link->playerOf[e->peer], e->channel, e->data, e->size);
    }
}

static void ClientEvent(StoreNetLink *link, const CoreNetEvent *e)
{
    if (e->peer != link->hostPeer)
        return;
    if (e->type == CORE_NET_EVENT_CONNECTED)
        Connected(link);
    else if (e->type == CORE_NET_EVENT_DISCONNECTED)
    {
        if (!link->connected && Now() - link->started < STORE_NET_LINK_JOIN_SECONDS)
            link->hostPeer = CoreNetConnect(&link->endpoint, link->address, link->port); // again
        else if (link->connected)
        {
            link->connected = false;
            StoreNetPeerLeft(&link->net, 0); // ends the session: the host left
            if (!link->why[0])
                snprintf(link->why, sizeof link->why, "the host closed the connection");
        }
    }
    else if (e->type == CORE_NET_EVENT_RECEIVED)
    {
        Tap(link, 0, e->channel, e->data, e->size);
        StoreNetReceive(&link->net, 0, e->channel, e->data, e->size);
    }
}

void StoreNetLinkPoll(StoreNetLink *link, uint32_t timeoutMs)
{
    if (!link || !link->open || !link->endpoint.host)
        return;
    CoreNetEvent e;
    for (uint32_t wait = timeoutMs; CoreNetPoll(&link->endpoint, wait, &e); wait = 0)
    {
        bool error = e.type == CORE_NET_EVENT_ERROR;
        if (!error && link->hosting)
            HostEvent(link, &e);
        else if (!error)
            ClientEvent(link, &e);
        CoreNetEventFree(&e);
        if (error)
            break;
    }
    Sample(link);
    if (!link->hosting && !link->net.joined && !link->why[0] &&
        Now() - link->started > STORE_NET_LINK_JOIN_SECONDS)
        snprintf(link->why, sizeof link->why, "no welcome from %.200s:%d within %.0f s", link->address,
                 link->port, STORE_NET_LINK_JOIN_SECONDS);
}

void StoreNetLinkFlush(StoreNetLink *link)
{
    if (!link || !link->open)
        return;
    CoreNetFlush(&link->endpoint);
    Sample(link);
}

const char *StoreNetLinkEnded(const StoreNetLink *link)
{
    if (!link)
        return "no link";
    return link->why[0] ? link->why : NULL;
}

void StoreNetLinkSetTap(StoreNetLink *link, StoreNetLinkTap tap, void *user)
{
    if (!link)
        return;
    link->tap = tap;
    link->tapUser = user;
}

void StoreNetLinkClose(StoreNetLink *link, double lingerSeconds)
{
    if (!link)
        return;
    if (link->endpoint.host)
    {
        CoreNetEvent e;
        int connected = 0;
        for (int p = 2; link->hosting && p <= STORE_NET_PLAYERS; p++)
            connected += link->peerOf[p] != CORE_NET_PEER_NONE;
        // A host that finished first gives its clients a moment to finish theirs and leave.
        for (double end = Now() + lingerSeconds; connected > 0 && Now() < end;)
            if (CoreNetPoll(&link->endpoint, 10, &e))
            {
                if (e.type == CORE_NET_EVENT_DISCONNECTED && e.peer < STORE_NET_PLAYERS && link->playerOf[e.peer])
                {
                    link->peerOf[link->playerOf[e.peer]] = CORE_NET_PEER_NONE;
                    link->playerOf[e.peer] = 0;
                    connected--;
                }
                CoreNetEventFree(&e);
            }
        int waiting = 0;
        for (int p = 2; link->hosting && p <= STORE_NET_PLAYERS; p++)
            if (link->peerOf[p] != CORE_NET_PEER_NONE)
                CoreNetDisconnect(&link->endpoint, link->peerOf[p], false), waiting++;
        if (!link->hosting && link->hostPeer != CORE_NET_PEER_NONE && link->connected)
            CoreNetDisconnect(&link->endpoint, link->hostPeer, false), waiting++;
        for (double end = Now() + 1.0; waiting > 0 && Now() < end;)
            if (CoreNetPoll(&link->endpoint, 10, &e))
            {
                waiting -= e.type == CORE_NET_EVENT_DISCONNECTED;
                CoreNetEventFree(&e);
            }
        Sample(link);
        CoreNetClose(&link->endpoint);
    }
    StoreNetFree(&link->net);
    link->open = link->hosting = link->connected = false;
}
