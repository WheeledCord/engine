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
