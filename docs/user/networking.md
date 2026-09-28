# Networking

A game says what is shared and what it means; the engine does the rest. `core/net_session.h` hosts
and joins games, `core/net_sync.h` holds the replicated objects, `core/net_clock.h` keeps the server
tick and the client's interpolation clock, and `core/network.h` is the ENet transport under all of
them.

## A session

```c
static const CoreNetSessionConfig config = {
    .game = "my-game", .version = 3,        /* a joiner that differs is refused, with the reason */
    .registerSchemas = RegisterSchemas,     /* the same schemas on every machine */
    .started = SpawnWorld,                  /* server: objects the server owns */
    .writeWelcome = WriteSeed,              /* server: what a joiner needs to build the world */
    .readWelcome = ReadSeed,                /* joiner: build it, then CoreNetSessionReady */
    .joined = SpawnPlayer,                  /* server: a player is in; spawn what he owns */
    .command = HandleCommand,               /* server: a player asks for something */
    .event = HandleEvent,                   /* player: the server says something happened */
};

CoreNetSession session = {0};
CoreNetSessionHost(&session, &config, 7431, true);          /* host with a player of our own */
/* or */ CoreNetSessionJoin(&session, &config, "10.0.0.2", 7431);

/* every frame */
CoreNetSessionStep(&session, dt, 0);
```

The host is the server. It keeps one registry, its own player is actor 1 in it like anyone else, and
nothing goes through a socket to itself: `CoreNetSessionCommand` on the host runs the server's
command handler in the call, the same handler a joiner's command reaches, and an event addressed to
the host's player is delivered in the call as well. Netcode for GameObjects handles a host's
ServerRpc the same way. A dedicated server is `CoreNetSessionHost(..., false)`: no player of its own.

Joining is three steps, as id Tech 3's connected / primed / active: the joiner connects and says
which engine protocol, game and version it runs; the server refuses a mismatch with a reason
(`session.refusal`, `CoreNetRefusalText`) or welcomes it with an actor number and the game's welcome
data; the joiner builds its world from that and calls `CoreNetSessionReady`, and only then is it
`joined` and sent snapshots. A player who leaves takes the objects he owned with him.

Each frame `CoreNetSessionStep` handles what arrived, runs every replicated object's serialize
callback in the direction ownership says (see below), and sends what is due: the server a delta
snapshot per player at the send rate, a player its own objects at the upload rate.

Commands and events:

- `CoreNetSessionCommand(session, op, object, payload, size)` asks the server to do something and
  returns a sequence number. Every snapshot says the highest sequence handled for that player, and it
  never goes backwards, so `CoreNetSessionCommandDone(session, sequence)` tells a refusal from a slow
  reply. A game that shows the result early -- an item where the player put it -- keeps showing its
  own version until the command is done, then takes the server's.
- `CoreNetSessionEvent(session, actor, op, object, payload, size)` is the server telling one player,
  or `CORE_NET_EVERYONE`, that something happened. Events are reliable.

`CoreNetSessionIsMine(session, object)` answers "does this machine decide it". The server decides
the objects it owns; a player decides the objects it owns; everyone else reads them.

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

The engine does not provide matchmaking, authentication, encryption, NAT traversal, interest
management, prediction, reconciliation, or lag compensation.

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

## Sharing an engine object from Scheme

A property marked `ENGINE_PROPERTY_SHARED` (`core/object.h`) on an engine type is sent to the other
machines in a networked game. A script hosts, joins and shares objects of such a type through the
built-in `"network"` engine type, on top of everything above -- one `CoreNetSession` per `network`
object:

```scheme
;; the game's own type, with two properties marked shared, registered before any network object
;; is made -- see ScriptHostRegisterType for a game's own engine type in C.

(define net (make 'network "my-game" 1))     ; game name and version, like CoreNetSessionConfig
(net 'host! 7431)                             ; or (net 'join! "10.0.0.2" 7431)
(net 'ready!)                                 ; a joiner only, once its status is "welcomed"

(connect! net 'appeared (lambda (obj) (set! remote obj)))  ; a joiner: told about each new object
(connect! net 'joined (lambda (actor) (display actor)))    ; the server: an actor is in

(define mine (make 'unit))
(net 'share! mine)                            ; server only: mine's own storage becomes replicated
(net 'mine? mine)                             ; #t here, #f on every machine that only received it
```

At creation, `network` registers one net schema for every engine type already registered in the
same script host that has at least one shared property (`CoreNetFieldsFromType`), in registration
order -- schema id is that position plus one, name is the type's name. Every machine runs the same
script and so registers the same types in the same order, and the ids agree without a word being
said over the wire.

`status`, `actor`, `server`, `refusal` and `port` are read-only properties, the last for reading back
the UDP port `host!` bound when given zero. `host!`, `join!`, `ready!` and `leave!` wrap
`CoreNetSessionHost`, `CoreNetSessionJoin`, `CoreNetSessionReady` and `CoreNetSessionLeave`.
`share!` is server only: it spawns a replicated object whose state *is* the given object's own
storage (`CoreNetSyncBindState` with `EngineObjectData`) and remembers the pairing; `mine?` says
whether this machine decides a shared or appeared object, the way `CoreNetSessionIsMine` does.

On a joiner, an object a snapshot brings becomes an engine object of the matching type (its schema's
name, looked up in the same host), bound the same way, with the signal `appeared` carrying it; one a
snapshot takes away destroys the engine object it was paired with, silently. Destroying the network
object, or `leave!`, destroys every object it created this way; an object `share!` was given is the
caller's and is never destroyed here. Signals: `joined` and `left` (actor) on the server, `welcomed`
and `appeared` (object) on a joiner.

Commands and events (`CoreNetSessionCommand`/`CoreNetSessionEvent`) are not reachable from Scheme
yet -- a script can host, join and share replicated state, but a one-off request or notification is
still C only.

## What is still missing

There is no client-side prediction of a player's own movement against the server and no
reconciliation: the server accepts the state a player publishes about his own body, checked by the
game's `accept` callback. There is no lag compensation, no interest management (every player is sent
every object), no encryption and no authentication. Scripts can host, join and share replicated
objects (above); commands and events from Scheme are not implemented yet.
