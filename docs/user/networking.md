# Networking

Games network on the store (`core/store.h`): the things a machine owns are sent to the others, and
messages to things owned elsewhere travel with them. A Scheme game gets this from the runner with no
code of its own (`trench run mygame --host 7777`, `--join ADDRESS:7777`, and `player-joined` in
`game`; see [Running a Scheme game](kinds.md#co-op)). A C game uses the same code directly:
`core/store_net.h` is the session, `core/store_net_enet.h` puts it on ENet, and Trenchfoot is built
this way. The design and its rules are [the store design](../developer/store.md) §9.

## The model

The host is machine 0: it owns the world (owner 0) and plays as player 1, unless it is dedicated and
plays no one. Clients are players 2 to 16, one machine each, and talk only to the host, which relays.
A thing is owned by whoever owns the root of the tree it hangs from, so picking something up
(attaching it under a player's thing) hands it to that player, and putting it down hands it back.

Each machine sends the things it owns as deltas against the last state the receiver acknowledged
(Quake 3's scheme), every third tick, unreliably unless the packet is over 1,200 bytes. Other
machines' things are shown 100 ms behind, every field of one thing from one tick, with registered
FLOAT and VEC3 fields blended between ticks. A message to a thing owned elsewhere travels reliably in
the same packet as its sender's state and is delivered when that tick is shown. Joining checks the
protocol, the game's name and every kind's declaration; when a player leaves, the roots they owned
are removed and anything of someone else's hanging under them is orphaned to the host.

## A session in C

```c
static Store store;         /* kinds declared, the game's hooks installed */
static StoreNetLink link;   /* caller-owned */

StoreNetConfig config = {.game = "my-game", .onJoined = Joined, .onEnded = Ended, .user = &game};
StoreNetLinkHost(&link, &store, &config, 7777);            /* host, as player 1 */
/* or */ StoreNetLinkJoin(&link, &store, &config, "10.0.0.2", 7777);
StoreNetLinkInterpolate(&link, soldierKind, "position");  /* blended between ticks */

/* every fixed tick */
StoreNetLinkPoll(&link, 0);
if (link.net.joined && !StoreNetLinkEnded(&link)) {
    StoreNetBeforeTick(&link.net);
    StoreTick(&store, 1.0f / 60.0f);
    StoreNetAfterTick(&link.net);
}
StoreNetLinkFlush(&link);

/* at the end */
StoreNetLinkClose(&link, 0.0);
```

`StoreNetPlayer(&link.net)` is this machine's player (1 on the host; a client's once welcomed) and
`StoreNetPlayers` lists everyone in the session. `StoreNetLinkEnded` says why a session ended: a
refusal, no welcome within 5 s, or the host leaving. `link.wireSent` and `link.wireReceived` are
ENet's own totals, headers and resends included. `StoreNetStateHash` hashes what a machine holds of
the shared world, so two machines can be compared.

`core/store_net.h` itself opens no socket: packets leave through the config's `send` callback and
arrive through `StoreNetReceive`, which is how the regression checks run a host and clients over an
in-memory queue with delay and loss.

## Two rates, and drawing in the past

`core/net_clock.h` keeps a fixed simulation tick with a lower send rate, and the interpolation clock
a client draws by. A server does not send because a packet arrived: it simulates on a fixed tick and
sends at its own, lower rate. Source runs 66 ticks a second and sends about 20 snapshots, and never
sends more snapshots than it has simulated ticks.

```c
CoreNetClock clock;
CoreNetClockInit(&clock, CORE_NET_TICK_RATE, CORE_NET_SEND_RATE);   /* 60 and 20 */

int due = CoreNetClockAdvance(&clock, dt, 16);   /* banked, not rounded */
for (int i = 0; i < due; i++) {
    CoreNetClockTicked(&clock);
    /* simulate one fixed step */
}
if (CoreNetClockShouldSend(&clock))
    /* send what is due */;
```

The ceiling on `CoreNetClockAdvance` matters: without it a long stall demands the whole backlog on
the next frame, which takes longer still, and the simulation never catches up. Time beyond the
ceiling is discarded instead.

A client cannot draw the newest state the moment it arrives, because it has nothing to interpolate
toward and the next one may be late or lost. It draws the world slightly in the past instead, far
enough back that both states bracketing the moment being drawn have arrived. Two send intervals is
the usual answer -- one packet can be lost outright and there is still something ahead -- and it is
where Source's default 100 ms lands at 20 snapshots a second.

```c
CoreNetInterpolator interp;
CoreNetInterpolatorInit(&interp, CORE_NET_TICK_RATE, CORE_NET_SEND_RATE);

/* as each snapshot arrives */
CoreNetInterpolatorSnapshot(&interp, tick);
/* each frame */
CoreNetInterpolatorAdvance(&interp, dt);
double drawn = CoreNetInterpolatorRenderTick(&interp);   /* the tick to draw the world at */
```

The drawn moment is a tick, not a blend factor: where it falls between the two states held is the
blend, the calculation id Tech 3 makes in `CG_CalcEntityLerp`. A blend factor passed directly assumes
states arrive evenly spaced, which is wrong the moment one is late, early or lost. A snapshot older
than the newest one seen cannot drag the clock backwards, and drift is closed by retiming playback a
little at a time rather than by moving the drawn moment. `CoreNetClockNow()` is a monotonic clock in
seconds.

## The transport

`core/network.h` is the ENet transport under `store_net_enet`: an endpoint that hosts or connects,
numbered peers, channels (reliable or not), and a fixed-width big-endian writer and reader for
building packets. A game rarely needs it directly.

## What is not there

No client-side prediction of a player's own movement and no reconciliation: a machine is the
authority for what it owns. No lag compensation, no interest management (every machine is sent every
shared thing), no encryption, no authentication, no matchmaking and no NAT traversal.
