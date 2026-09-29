/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Networking on the store (core/store_net.h): a host and two clients in one process over an
// in-memory transport with 150 ms one way and 5% loss on the state channel, plus a client whose
// kinds differ. docs/developer/store.md §9.7.
#include "checks.h"
#include "core/store_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MACHINES 4
#define DELAY 9 /* ticks: 150 ms at 60 a second */

static int failures;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: net: %s\n", what);
        failures++;
    }
}

typedef struct Machine
{
    Store store;
    StoreNet net;
    int player; /* 1 for the host */
    bool live;
    int joined, arrived;
    char ended[512];
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
static uint64_t now, loss;
static bool measuring;
static size_t largestState;

// Field and kind indices, the same in every store because every store declares them in one order.
static StoreKind gameKind, unitKind, partKind;
enum { POSITION, N, MOOD, SEEN, SCORE, TARGET };
static int joinedField, leftField;
static StoreSymbol pokeEvent, orphanedEvent, joinedEvent, leftEvent;

static uint64_t SplitMix(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static StoreValue Int(int32_t i)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_INT;
    v.as.i = i;
    return v;
}

static StoreValue Ref(StoreId id)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_REF;
    v.as.ref = id;
    return v;
}

static StoreValue Vec(float x, float y, float z)
{
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_VEC3;
    v.as.v = (Vector3){x, y, z};
    return v;
}

static int GetInt(Store *s, StoreId id, int field)
{
    StoreValue v;
    return StoreGet(s, id, field, &v) ? v.as.i : -99999;
}

// ---- the transport ----------------------------------------------------------------------------
static int MachineOf(int player)
{
    for (int i = 0; i < MACHINES; i++)
        if (machines[i].player == player)
            return i;
    return -1;
}

static bool Send(void *user, int peer, int channel, bool reliable, const void *data, size_t size)
{
    Machine *m = user;
    int from = (int)(m - machines), to = m->player == 1 ? MachineOf(peer) : 0;
    (void)reliable;
    if (to < 0)
        return false;
    if (channel == 1 && from == 0 && to == 1 && measuring && size > largestState)
        largestState = size;
    if (channel == 1 && SplitMix(&loss) % 100 < 5)
        return true; // lost
    if (packetCount == packetCapacity)
    {
        packetCapacity = packetCapacity ? packetCapacity * 2 : 256;
        packets = realloc(packets, (size_t)packetCapacity * sizeof *packets);
    }
    Packet *p = &packets[packetCount++];
    p->from = from;
    p->to = to;
    p->channel = channel;
    p->due = now + DELAY;
    p->size = size;
    p->data = malloc(size);
    memcpy(p->data, data, size);
    return true;
}

static void Deliver(void)
{
    int count = packetCount;
    for (int i = 0; i < count; i++)
    {
        Packet p = packets[i];
        if (p.due > now || !p.data)
            continue;
        packets[i].data = NULL;
        Machine *to = &machines[p.to];
        if (to->live)
            StoreNetReceive(&to->net, p.to == 0 ? machines[p.from].player : 0, p.channel, p.data,
                            p.size);
        free(p.data);
    }
    int kept = 0;
    for (int i = 0; i < packetCount; i++)
        if (packets[i].data)
            packets[kept++] = packets[i];
    packetCount = kept;
}

static void Step(int ticks)
{
    for (int t = 0; t < ticks; t++)
    {
        now++;
        Deliver();
        for (int i = 0; i < MACHINES; i++)
        {
            Machine *m = &machines[i];
            if (!m->live)
                continue;
            StoreNetBeforeTick(&m->net);
            StoreTick(&m->store, 1.0f / 60.0f);
            StoreNetAfterTick(&m->net);
        }
    }
}

static void OnArrived(void *user, StoreId thing)
{
    (void)thing;
    ((Machine *)user)->arrived++;
}

static void OnJoined(void *user, int player) { ((Machine *)user)->joined = player; }

static void OnEnded(void *user, const char *why)
{
    snprintf(((Machine *)user)->ended, sizeof ((Machine *)user)->ended, "%s", why);
}

// ---- the kinds --------------------------------------------------------------------------------
static bool Handler(Store *s, StoreId self, StoreSymbol event, const StoreValue *args, int count,
                    void *user)
{
    (void)user;
    if (event == joinedEvent && count == 1)
    {
        // The game gives each player a soldier they own, as (on (player-joined p)) does.
        StoreId soldier = StoreSpawn(s, unitKind, args[0].as.i, STORE_NULL, STORE_NO_SYMBOL);
        StoreValue n = Int(1000 + args[0].as.i);
        StoreSetEngine(s, soldier, N, &n);
        StoreSet(s, self, joinedField, &args[0]);
    }
    else if (event == leftEvent && count == 1)
        StoreSet(s, self, leftField, &args[0]);
    else if (event == pokeEvent && count == 1 && args[0].type == STORE_REF)
    {
        StoreValue seen = Int(GetInt(s, args[0].as.ref, SCORE)); // the sender's state, if it came first
        StoreSet(s, self, SEEN, &seen);
    }
    else if (event == orphanedEvent)
    {
        StoreValue seen = Int(-7);
        StoreSet(s, self, SEEN, &seen);
    }
    return true;
}

static StoreFieldDecl Field(const char *name, StoreType type)
{
    StoreFieldDecl d;
    memset(&d, 0, sizeof d);
    d.name = name;
    d.type = type;
    return d;
}

static void Declare(Store *s, bool differ)
{
    StoreFieldDecl game[] = {Field("joined", STORE_INT), Field("left", STORE_INT)};
    StoreFieldDecl unit[] = {Field("position", STORE_VEC3), Field("n", STORE_INT),
                             Field("mood", STORE_SYMBOL),   Field("seen", STORE_INT),
                             Field("score", STORE_INT),     Field("target", STORE_REF),
                             Field("extra", STORE_INT)};
    StoreFieldDecl part[] = {Field("level", STORE_INT)};
    gameKind = StoreDeclareKind(s, "game", -1, game, 2, NULL);
    unitKind = StoreDeclareKind(s, "unit", -1, unit, differ ? 7 : 6, NULL);
    partKind = StoreDeclareKind(s, "part", -1, part, 1, NULL);
    if (differ)
        StoreDeclareKind(s, "medkit", -1, NULL, 0, NULL);
    joinedField = StoreFieldIndex(s, gameKind, "joined");
    leftField = StoreFieldIndex(s, gameKind, "left");
    pokeEvent = StoreIntern(s, "poke");
    orphanedEvent = StoreIntern(s, "orphaned");
    joinedEvent = StoreIntern(s, "player-joined");
    leftEvent = StoreIntern(s, "player-left");
    StoreKindSetHandler(s, gameKind, Handler, NULL);
    StoreKindSetHandler(s, unitKind, Handler, NULL);
    StoreKindHandles(s, gameKind, joinedEvent, true);
    StoreKindHandles(s, gameKind, leftEvent, true);
    StoreKindHandles(s, unitKind, pokeEvent, true);
    StoreKindHandles(s, unitKind, orphanedEvent, true);
}

static StoreId Unit(Store *s, int n)
{
    StoreId things[1024];
    int count = StoreThings(s, unitKind, things, 1024);
    for (int i = 0; i < count; i++)
        if (GetInt(s, things[i], N) == n)
            return things[i];
    return STORE_NULL;
}

static bool Near(Store *s, StoreId id, float x, float y, float z)
{
    StoreValue v;
    return StoreGet(s, id, POSITION, &v) && v.as.v.x == x && v.as.v.y == y && v.as.v.z == z;
}

// On every machine but those skipped: unit n hangs under unit parentN (or is a root for 0), owned
// by owner.
static bool Placed(int n, int parentN, int owner, int skip)
{
    for (int i = 0; i < 3; i++)
    {
        if (i == skip)
            continue;
        Store *s = &machines[i].store;
        StoreId id = Unit(s, n), parent = StoreParent(s, id);
        if (!StoreAlive(s, id) || StoreOwner(s, id) != owner)
            return false;
        if (parentN ? GetInt(s, parent, N) != parentN || !StoreIsGuest(s, id)
                    : parent.index != UINT32_MAX)
            return false;
    }
    return true;
}

// ---- the session ------------------------------------------------------------------------------
int NetChecks(void)
{
    failures = 0;
    now = 0;
    loss = 0x5EED5EEDull;
    memset(machines, 0, sizeof machines);
    for (int i = 0; i < MACHINES; i++)
    {
        Machine *m = &machines[i];
        StoreInit(&m->store, 100 + (uint64_t)i);
        Declare(&m->store, i == 3);
        m->player = i + 1;
        m->live = true;
    }
    Store *host = &machines[0].store, *a = &machines[1].store, *b = &machines[2].store;
    StoreNetConfig config;
    memset(&config, 0, sizeof config);
    config.send = Send;
    config.onArrived = OnArrived;
    config.onJoined = OnJoined;
    config.onEnded = OnEnded;
    config.game = "net-check";
    config.user = &machines[0];
    Expect(StoreNetHost(&machines[0].net, host, &config), "a host starts");
    // A world of 300 units, one with a declared child and one pointing at another.
    StoreId game = StoreSpawn(host, gameKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreSymbol calm = StoreIntern(host, "calm"), arm = StoreIntern(host, "arm");
    StoreId units[300];
    for (int i = 0; i < 300; i++)
    {
        units[i] = StoreSpawn(host, unitKind, 0, STORE_NULL, STORE_NO_SYMBOL);
        StoreValue n = Int(i), mood = {STORE_SYMBOL, {.sym = calm}}, at = Vec((float)i, 0, 0);
        StoreSet(host, units[i], N, &n);
        StoreSet(host, units[i], MOOD, &mood);
        StoreSet(host, units[i], POSITION, &at);
    }
    StoreId part = StoreSpawn(host, partKind, 0, units[0], arm);
    StoreValue level = Int(3), target = Ref(units[2]);
    StoreSet(host, part, 0, &level);
    StoreSet(host, units[1], TARGET, &target);
    // A client with something of its own before it joins: the host's world replaces it.
    StoreSpawn(a, unitKind, 0, STORE_NULL, STORE_NO_SYMBOL);
    for (int i = 1; i < MACHINES; i++)
    {
        config.user = &machines[i];
        Expect(StoreNetJoin(&machines[i].net, &machines[i].store, &config), "a client sends its hello");
    }
    for (int i = 0; i < 3; i++)
        StoreNetInterpolate(&machines[i].net, unitKind, "position");
    Step(90);

    // Joining, and the kinds check.
    int players[8];
    Expect(machines[1].joined == 2 && machines[2].joined == 3 &&
               StoreNetPlayers(&machines[0].net, players, 8) == 3 && players[2] == 3 &&
               StoreNetPlayer(&machines[1].net) == 2,
           "two clients are welcomed as players 2 and 3");
    Expect(machines[3].net.ended && strstr(machines[3].ended, "unit (fields)") &&
               strstr(machines[3].ended, "medkit (missing on the host)") && !machines[3].joined &&
               StoreNetPlayers(&machines[0].net, NULL, 0) == 3,
           "a client whose kinds differ is refused with the kinds named");
    Expect(GetInt(host, game, joinedField) == 3, "the game hears player-joined");

    // Spawns reach both clients: the units, the declared child, the reference, the soldiers.
    for (int i = 1; i <= 2; i++)
    {
        Store *s = &machines[i].store;
        StoreId five = Unit(s, 5), zero = Unit(s, 0), one = Unit(s, 1);
        StoreValue v;
        Expect(StoreThings(s, unitKind, NULL, 0) == 302 && machines[i].arrived == 304,
               "every unit and soldier arrives, and the client's own is gone");
        Expect(StoreAlive(s, five) && Near(s, five, 5, 0, 0) && StoreGet(s, five, MOOD, &v) &&
                   !strcmp(StoreSymbolName(s, v.as.sym), "calm") && StoreOwner(s, five) == 0,
               "a unit arrives with its fields and owner");
        StoreId child = StoreChildNamed(s, zero, StoreIntern(s, "arm"));
        Expect(StoreAlive(s, child) && GetInt(s, child, 0) == 3 && !StoreIsGuest(s, child),
               "a declared child arrives under its parent with its name");
        Expect(StoreGet(s, one, TARGET, &v) && GetInt(s, v.as.ref, N) == 2,
               "a reference arrives as the same thing");
        Expect(StoreOwner(s, Unit(s, 1002)) == 2 && StoreOwner(s, Unit(s, 1003)) == 3,
               "each player's soldier is theirs on every machine");
    }

    // Updates and removes reach both clients.
    StoreSymbol angry = StoreIntern(host, "angry");
    StoreValue mood = {STORE_SYMBOL, {.sym = angry}}, moved = Vec(50, 1, 0);
    StoreSet(host, units[5], MOOD, &mood);
    StoreSet(host, units[5], POSITION, &moved);
    StoreRemove(host, units[7]);
    StoreRemove(host, units[0]);
    Step(40);
    for (int i = 1; i <= 2; i++)
    {
        Store *s = &machines[i].store;
        StoreValue v;
        Expect(StoreGet(s, Unit(s, 5), MOOD, &v) && !strcmp(StoreSymbolName(s, v.as.sym), "angry") &&
                   Near(s, Unit(s, 5), 50, 1, 0),
               "an update arrives, the held-back position settling on the newest");
        Expect(!StoreAlive(s, Unit(s, 7)) && !StoreAlive(s, Unit(s, 0)) &&
                   StoreThings(s, partKind, NULL, 0) == 0,
               "a remove arrives, with the declared child");
    }

    // Bytes per state packet: 300 things, one field changing.
    measuring = true;
    for (int t = 0; t < 30; t++)
    {
        StoreValue n = Int(10 + 1000 * (t + 1));
        StoreSet(host, units[10], SCORE, &n);
        Step(1);
    }
    measuring = false;
    printf("net: bytes per state packet at 300 things, one field changing: %zu\n", largestState);
    Expect(largestState > 0 && largestState < 1024, "a state packet with one change is under 1 KB");

    // A client's message reaches a host-owned thing after the state it carries, and back.
    StoreId soldierA = Unit(a, 1002);
    StoreValue score = Int(77), from = Ref(soldierA);
    StoreSet(a, soldierA, SCORE, &score);
    StoreCommand(a, Unit(a, 20), pokeEvent, &from, 1);
    score = Int(55);
    from = Ref(units[21]);
    StoreSet(host, units[21], SCORE, &score);
    StoreSend(host, Unit(host, 1002), pokeEvent, &from, 1);
    Step(40);
    Expect(GetInt(host, units[20], SEEN) == 77,
           "a client's message reaches a host-owned thing after its carried state");
    Expect(GetInt(a, soldierA, SEEN) == 55 && GetInt(host, Unit(host, 1002), SEEN) == 55,
           "a host's message reaches a client-owned thing after its carried state");

    // An attach by the host moves ownership to the client; the client's detach moves it back.
    StoreAttach(host, units[30], Unit(host, 1002));
    Step(40);
    Expect(Placed(30, 1002, 2, -1), "an attach under a client's soldier makes the client its owner");
    score = Int(99);
    StoreSet(a, Unit(a, 30), SCORE, &score);
    Step(40);
    Expect(GetInt(host, units[30], SCORE) == 99 && GetInt(b, Unit(b, 30), SCORE) == 99,
           "the new owner's writes reach the host and the other client");
    StoreDetach(a, Unit(a, 30));
    Step(40);
    Expect(Placed(30, 0, 0, -1), "the owner's detach gives it back to the host everywhere");

    // Still for 60 ticks: every machine agrees.
    Step(60);
    uint64_t hashes[3];
    for (int i = 0; i < 3; i++)
        hashes[i] = StoreNetStateHash(&machines[i].net);
    printf("net: state hashes after 60 still ticks: %016llx %016llx %016llx\n",
           (unsigned long long)hashes[0], (unsigned long long)hashes[1], (unsigned long long)hashes[2]);
    Expect(hashes[0] == hashes[1] && hashes[1] == hashes[2], "the state hash is equal on all three");

    // A client's thing moving at one unit a tick, as the other client sees it through the host. The
    // two hops take 2 * DELAY ticks on the wire; beyond that B may trail by one send interval plus
    // the 100 ms (6 ticks) of interpolation, not by the host's own 100 ms as well.
    float lag = 0;
    for (int t = 0; t < 120; t++)
    {
        StoreValue at = Vec((float)t, 0, 0);
        StoreSet(a, soldierA, POSITION, &at);
        Step(1);
        StoreValue seen;
        if (t >= 60 && StoreGet(b, Unit(b, 1002), POSITION, &seen) && (float)t - seen.as.v.x > lag)
            lag = (float)t - seen.as.v.x;
    }
    Step(60);
    printf("net: client-to-client lag of a moving thing: %.1f ticks, %d of them on the wire\n",
           (double)lag, 2 * DELAY);
    Expect(lag > 2 * DELAY && lag <= 2 * DELAY + 3 + 6,
           "a client's moving thing reaches the other client one send interval plus 100 ms behind");

    // A leaver's roots are removed and the guests under them orphaned.
    StoreAttach(host, units[40], Unit(host, 1003));
    StoreId mine = StoreSpawn(b, unitKind, 3, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue n = Int(3333);
    StoreSet(b, mine, N, &n);
    Step(40);
    Expect(StoreAlive(host, Unit(host, 3333)) && Placed(40, 1003, 3, -1),
           "the leaver's own thing and the guest it holds are everywhere");
    machines[2].live = false;
    StoreNetPeerLeft(&machines[0].net, 3);
    Expect(!StoreAlive(host, Unit(host, 1003)) && !StoreAlive(host, Unit(host, 3333)) &&
               StoreOwner(host, units[40]) == 0 && StoreParent(host, units[40]).index == UINT32_MAX &&
               StoreNetPlayers(&machines[0].net, NULL, 0) == 2,
           "the leaver's roots are removed and its guest is detached to the host");
    Step(40);
    Expect(GetInt(host, units[40], SEEN) == -7 && GetInt(host, game, leftField) == 3,
           "the guest hears orphaned, then the game player-left");
    Expect(!StoreAlive(a, Unit(a, 1003)) && !StoreAlive(a, Unit(a, 3333)) && Placed(40, 0, 0, 2),
           "the other client sees the leaver's things go and the guest dropped");

    for (int i = 0; i < MACHINES; i++)
    {
        StoreNetFree(&machines[i].net);
        StoreFree(&machines[i].store);
    }
    for (int i = 0; i < packetCount; i++)
        free(packets[i].data);
    free(packets);
    packets = NULL;
    packetCount = packetCapacity = 0;
    return failures;
}
