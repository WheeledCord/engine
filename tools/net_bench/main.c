/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// What networking on the store (core/store_net.h, docs/developer/store.md §9) costs the host per
// tick: three stores in one process joined by an in-memory transport (150 ms each way, no loss), a
// host with 30 things of a 10-field kind all changing every tick and two clients each moving a
// soldier of its own every tick, 3,600 ticks. Times the host's StoreNetReceive, StoreNetBeforeTick
// and StoreNetAfterTick and prints µs per tick p50/p99; the bytes the host sent and the state hashes
// at the end show that a change to the cost left what goes on the wire alone. Headless. Run it
// pinned to one core, for instance `taskset -c 3 ./build/core/net_bench`.
#define _POSIX_C_SOURCE 199309L
#include "core/store_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICKS 3600
#define THINGS 30
#define DELAY 9 /* ticks: 150 ms at 60 a second */
#define MACHINES 3

typedef struct Machine
{
    Store store;
    StoreNet net;
    int player;
} Machine;

typedef struct Packet
{
    int from, to, channel;
    uint64_t due;
    size_t size;
    unsigned char *data;
} Packet;

static Machine machines[MACHINES];
static Packet *packets;
static int packetCount, packetCapacity;
static uint64_t now, hostBytes;
static StoreKind gameKind, thingKind;
static StoreSymbol joinedEvent, moods[4];

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int Compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// p50 and p99 over ticks, and the mean: two ticks in three send nothing, so the mean is what a
// second of play costs (mean * 60).
static void Report(const char *what, double *micros, int count)
{
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += micros[i];
    qsort(micros, (size_t)count, sizeof *micros, Compare);
    printf("  %-20s p50 %7.2f  p99 %7.2f  mean %7.2f  max %8.2f us\n", what,
           micros[(int)(0.5 * (count - 1) + 0.5)], micros[(int)(0.99 * (count - 1) + 0.5)], sum / count,
           micros[count - 1]);
}

static bool Send(void *user, int peer, int channel, bool reliable, const void *data, size_t size)
{
    Machine *m = user;
    int from = (int)(m - machines), to = from == 0 ? peer - 1 : 0; // player p is machine p - 1
    (void)reliable;
    if (to < 0 || to >= MACHINES)
        return false;
    if (from == 0)
        hostBytes += size;
    if (packetCount == packetCapacity)
    {
        packetCapacity = packetCapacity ? packetCapacity * 2 : 256;
        packets = realloc(packets, (size_t)packetCapacity * sizeof *packets);
    }
    Packet *p = &packets[packetCount++];
    *p = (Packet){from, to, channel, now + DELAY, size, malloc(size)};
    memcpy(p->data, data, size);
    return true;
}

// Delivers what is due to one machine.
static void Deliver(int to)
{
    int kept = 0;
    for (int i = 0; i < packetCount; i++)
    {
        Packet p = packets[i];
        if (p.to != to || p.due > now)
        {
            packets[kept++] = p;
            continue;
        }
        StoreNetReceive(&machines[to].net, to == 0 ? machines[p.from].player : 0, p.channel, p.data,
                        p.size);
        free(p.data);
    }
    packetCount = kept;
}

static bool Handler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                    void *user)
{
    (void)self;
    (void)user;
    if (event == joinedEvent && count == 1) // each player gets a soldier of its own
        StoreSpawn(s, thingKind, args[0].as.i, STORE_NULL, STORE_NO_SYMBOL);
    return true;
}

static void Declare(Store *s)
{
    static const char *names[10] = {"position", "velocity", "yaw", "pitch", "timer",
                                    "health",   "ammo",     "team", "alive", "mood"};
    static const StoreType types[10] = {STORE_VEC3, STORE_VEC3, STORE_FLOAT, STORE_FLOAT, STORE_FLOAT,
                                        STORE_INT,  STORE_INT,  STORE_INT,   STORE_BOOL,  STORE_SYMBOL};
    StoreFieldDecl fields[10];
    memset(fields, 0, sizeof fields);
    for (int f = 0; f < 10; f++)
    {
        fields[f].name = names[f];
        fields[f].type = types[f];
    }
    gameKind = StoreDeclareKind(s, "game", -1, NULL, 0, NULL);
    thingKind = StoreDeclareKind(s, "thing", -1, fields, 10, NULL);
    joinedEvent = StoreIntern(s, "player-joined");
    StoreKindSetHandler(s, gameKind, Handler, NULL);
    StoreKindHandles(s, gameKind, joinedEvent, true);
    const char *moodNames[4] = {"idle", "walk", "run", "dead"};
    for (int i = 0; i < 4; i++)
        moods[i] = StoreIntern(s, moodNames[i]);
}

// Every field of a thing, new this tick.
static void Change(Store *s, StoreId id, int tick, int i)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    for (int f = 0; f < 10; f++)
    {
        v.type = StoreFieldAt(s, thingKind, f)->type;
        switch (v.type)
        {
        case STORE_VEC3:
            v.as.v = (Vector3){(float)tick * 0.1f + (float)i, (float)f, (float)i * 0.5f};
            break;
        case STORE_FLOAT:
            v.as.f = (float)tick * 0.25f + (float)(i + f);
            break;
        case STORE_INT:
            v.as.i = tick * 7 + i + f;
            break;
        case STORE_BOOL:
            v.as.b = ((tick + i) & 1) != 0;
            break;
        default:
            v.as.sym = moods[(tick + i + f) & 3];
            break;
        }
        StoreSet(s, id, f, &v);
    }
}

static StoreId Soldier(Store *s, int player)
{
    StoreId ids[64];
    int n = StoreThings(s, thingKind, ids, 64);
    for (int i = 0; i < n && i < 64; i++)
        if (StoreOwner(s, ids[i]) == player)
            return ids[i];
    return STORE_NULL;
}

int main(void)
{
    StoreNetConfig config;
    memset(&config, 0, sizeof config);
    config.send = Send;
    config.game = "net-bench";
    for (int i = 0; i < MACHINES; i++)
    {
        machines[i].player = i + 1;
        if (!StoreInit(&machines[i].store, 100 + (uint64_t)i))
            return 1;
        Declare(&machines[i].store);
        config.user = &machines[i];
        bool ok = i == 0 ? StoreNetHost(&machines[i].net, &machines[i].store, &config)
                         : StoreNetJoin(&machines[i].net, &machines[i].store, &config);
        if (!ok)
            return 1;
        StoreNetInterpolate(&machines[i].net, thingKind, "position");
        StoreNetInterpolate(&machines[i].net, thingKind, "yaw");
    }
    Store *host = &machines[0].store;
    StoreSpawn(host, gameKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreId things[THINGS];
    for (int i = 0; i < THINGS; i++)
        things[i] = StoreSpawn(host, thingKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    for (int p = 2; p <= MACHINES; p++)
        StoreNetPeerConnected(&machines[0].net, p);

    double *receive = malloc(TICKS * sizeof *receive), *before = malloc(TICKS * sizeof *before),
           *after = malloc(TICKS * sizeof *after), *total = malloc(TICKS * sizeof *total);
    if (!receive || !before || !after || !total)
        return 1;
    for (int tick = 0; tick < TICKS; tick++)
    {
        now++;
        for (int i = 0; i < MACHINES; i++)
        {
            Machine *m = &machines[i];
            double t0 = Now();
            Deliver(i);
            double t1 = Now();
            StoreNetBeforeTick(&m->net);
            double t2 = Now();
            if (i == 0)
                for (int k = 0; k < THINGS; k++)
                    Change(host, things[k], tick, k);
            else if (m->net.joined)
                Change(&m->store, Soldier(&m->store, m->player), tick, 100 + i);
            StoreTick(&m->store, 1.0f / 60.0f);
            double t3 = Now();
            StoreNetAfterTick(&m->net);
            double t4 = Now();
            if (i == 0)
            {
                receive[tick] = t1 - t0;
                before[tick] = t2 - t1;
                after[tick] = t4 - t3;
                total[tick] = receive[tick] + before[tick] + after[tick];
            }
        }
    }
    printf("net bench: a host with %d things of 10 fields all changing every tick and two clients each "
           "moving a soldier, %d ticks, %d ticks each way\n",
           THINGS, TICKS, DELAY);
    printf("host per tick:\n");
    Report("receive", receive, TICKS);
    Report("StoreNetBeforeTick", before, TICKS);
    Report("StoreNetAfterTick", after, TICKS);
    Report("total", total, TICKS);
    printf("host sent %llu bytes; state hashes %016llx %016llx %016llx (clients 100 ms behind: they "
           "differ while things move)\n",
           (unsigned long long)hostBytes, (unsigned long long)StoreNetStateHash(&machines[0].net),
           (unsigned long long)StoreNetStateHash(&machines[1].net),
           (unsigned long long)StoreNetStateHash(&machines[2].net));
    for (int i = 0; i < MACHINES; i++)
    {
        StoreNetFree(&machines[i].net);
        StoreFree(&machines[i].store);
    }
    for (int i = 0; i < packetCount; i++)
        free(packets[i].data);
    free(packets);
    free(receive);
    free(before);
    free(after);
    free(total);
    return 0;
}
