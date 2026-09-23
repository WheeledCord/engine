# Networking

`core/network.h` supplies a small message transport over vendored ENet. The engine owns connection
management, peer handles, channels, reliable delivery, and sequenced unreliable delivery.
`core/net_sync.h` optionally adds declarative object state replication. A project declares what its
state means and remains responsible for its game rules.

Open a server and poll it without blocking the game loop:

```c
CoreNetEndpoint server = {0};
if (!CoreNetOpenServer(&server, 27960, 8, 2))
    return false;

CoreNetEvent event;
while (CoreNetPoll(&server, 0, &event))
{
    if (event.type == CORE_NET_EVENT_RECEIVED)
        ReadProjectMessage(event.peer, event.data, event.size);
    CoreNetEventFree(&event);
}
```

A client opens its endpoint separately and receives a connection event asynchronously:

```c
CoreNetEndpoint client = {0};
if (!CoreNetOpenClient(&client, 2))
    return false;
CoreNetPeer server = CoreNetConnect(&client, "127.0.0.1", 27960);
```

Use reliable delivery for messages that must arrive, such as joining, spawning, inventory changes,
or chat. Use unreliable delivery for frequent state snapshots that replace older snapshots. Keeping
these message classes on separate channels prevents their ordering from interfering with each other.

Every event produced by `CoreNetPoll` must be passed to `CoreNetEventFree`, including connection and
error events. Received packet storage remains valid until that call. Peer handles are local to one
endpoint and become invalid on disconnect. Close each successfully opened endpoint with
`CoreNetClose`.

Do not transmit C structs directly: padding, byte order, and revisions make them an unstable wire
format. Give the project protocol a magic value and version, encode fields explicitly, validate every
length and finite numeric value, and reject unknown messages. Keep snapshots comfortably below the
path MTU where practical even though ENet can fragment larger messages.

## Replicated objects

Declare fields once when state needs PUN-style automatic synchronization. Both endpoints register
the same stable schema identifier; the registry handles field encoding, object identities,
spawn/despawn in complete snapshots, and interpolation:

```c
typedef struct PlayerState {
    Vector3 position;
    float yaw;
    int32_t health;
} PlayerState;

static const CoreNetField playerFields[] = {
    CORE_NET_FIELD_LERP(PlayerState, position, CORE_NET_VECTOR3),
    CORE_NET_FIELD_LERP(PlayerState, yaw, CORE_NET_F32),
    CORE_NET_FIELD(PlayerState, health, CORE_NET_I32),
};
static const CoreNetSchema playerSchema = {
    1, "player", sizeof(PlayerState), playerFields,
    sizeof playerFields / sizeof playerFields[0], CORE_NET_AUTHORITY_OWNER
};
```

The server creates stable objects with `CoreNetSyncSpawn`. An owning client serializes only its own
object with `CoreNetObjectWrite`; the server applies it with `CoreNetSyncReadObject`, which checks the
sender's actor number against the object's owner. The server then sends `CoreNetSyncWrite` snapshots
to every client. A client applies a snapshot transactionally with `CoreNetSyncRead` and samples
marked fields with `CoreNetObjectSample`. Look objects up again after every successful full snapshot,
because applying it replaces the registry's object storage.

This deliberately separates input/state ownership from state authority. An owner may submit the
fields a project permits, but an authoritative server remains able to write every object and should
validate project-specific limits before rebroadcasting. Use reliable project messages for durable
events such as inventory transfers. Use unreliable object updates and full snapshots for frequent
state that will soon be replaced.

For an action game, prefer an authoritative server. Clients send sampled inputs; the server advances
the accepted simulation and emits snapshots at a fixed rate. Render remote entities from a short
snapshot history so packet timing does not become visible motion jitter. Prediction and reconciliation
are project policy because they depend on the project's movement and collision rules.

The package does not provide matchmaking, authentication, encryption, NAT traversal, RPCs, delta
compression, interest management, prediction, reconciliation, or lag compensation.

## Two rates, and drawing in the past

A server does not send a snapshot because a packet arrived. It simulates on a fixed tick and sends
world snapshots at its own, lower rate. Source runs 66 ticks a second and sends about 20 snapshots,
and never sends more snapshots than it has simulated ticks. Broadcasting on arrival instead ties the
world's cadence to whoever happens to be talking, and the traffic grows with the square of the
player count.

`core/net_clock.h` is those two rates:

```c
CoreNetClock clock;
CoreNetClockInit(&clock, CORE_NET_TICK_RATE, CORE_NET_SEND_RATE);   /* 60 and 20 */

int due = CoreNetClockAdvance(&clock, dt, 16);   /* banked, not rounded */
for (int i = 0; i < due; i++) {
    CoreNetClockTicked(&clock);
    /* simulate one fixed step */
}
if (CoreNetClockShouldSend(&clock))
    CoreNetSyncWrite(&sync, clock.tick, &writer);
```

The ceiling on `CoreNetClockAdvance` matters: without it a long stall demands the whole backlog on
the next frame, which takes longer still, and the simulation never catches up. Time beyond the
ceiling is discarded instead.

A client cannot draw the newest snapshot the moment it arrives, because it has nothing to
interpolate toward and the next one may be late or lost. It draws the world slightly in the past
instead, far enough back that both snapshots bracketing the moment being drawn have arrived. Two
snapshot intervals is the usual answer -- one snapshot can be lost outright and there is still
something ahead -- and it is where Source's default 100ms lands at 20 snapshots a second.

```c
CoreNetInterpolator interp;
CoreNetInterpolatorInit(&interp, CORE_NET_TICK_RATE, CORE_NET_SEND_RATE);

/* as each snapshot arrives */
CoreNetInterpolatorSnapshot(&interp, tick);
/* each frame */
CoreNetInterpolatorAdvance(&interp, dt);
CoreNetObjectSampleAt(object, CoreNetInterpolatorRenderTick(&interp), &state);
```

`CoreNetObjectSampleAt` takes a moment, not a blend factor. Where that moment falls between the two
snapshots an object holds is the blend -- the same calculation id Tech 3 makes in `CG_CalcEntityLerp`
as `(cg.time - cg.snap->serverTime) / (cg.nextSnap->serverTime - cg.snap->serverTime)`. Passing a
blend factor directly assumes snapshots are evenly spaced, which is wrong the moment one is late,
early, or lost.

Snapshots older than the one already applied are read, checked, and then deliberately not applied.
UDP reorders, and rewinding the world to a state it has already passed is worse than dropping the
packet.

## Sending only what changed

A world that is mostly still should cost almost nothing to send. Each side remembers the last
`CORE_NET_SNAPSHOT_BACKUP` (32) snapshots, and a snapshot is written as the difference from one of
them:

```c
/* receiver, after applying a snapshot */
CoreNetSyncRemember(&sync, tick);
/* and it tells the sender which tick it reached */

/* sender, once per send interval, per receiver */
CoreNetSyncWriteDelta(&sync, tick, ackedByThatReceiver, &writer);
CoreNetSyncRemember(&sync, tick);
```

Only objects present are written, and within each only the fields that differ from the baseline; a
field nobody touched costs nothing and keeps the value it had. Objects the baseline held and this
snapshot does not are named so the receiver drops them.

Two rules keep this safe. A baseline the *sender* no longer remembers falls back to a full snapshot,
which is always readable. A baseline the *receiver* does not remember means the packet cannot be
reconstructed, so it is read, understood, and deliberately not applied -- the sender's next
acknowledgement will be old enough to force a full snapshot. Neither case loses the world.

Each receiver is sent its own snapshot, against its own acknowledgement. Two players who have missed
different packets cannot share a baseline, which is why id Tech 3 keeps its backup ring per client.
`CoreNetSyncWrite` is simply a delta against no baseline.

## What is still missing

There is no client-side prediction of the local player, so a player moves at the speed of a round
trip; no reconciliation, no numbered input commands, and no server movement authority -- the server
accepts the position a client claims, checked only for sanity. There is no lag compensation, no
interest management, no protocol version handshake, and no encryption. See NETWORKING.md at the
repository root for the full list and what each one blocks.
