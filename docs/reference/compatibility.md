# Compatibility and deliberate limits

Trench Engine builds on raylib and targets OpenGL 2.1 with GLSL 120. A project declares required GPU features; unsupported declared features fail at startup rather than selecting a fallback renderer.

The documented build target is Linux with X11 and OpenGL development packages. The engine does not yet provide physics, collision response or a transform hierarchy; games that need them currently supply their own. `core/collision2d.h` is a spatial-hash query helper, not a physics system.

Audio buses are volume/mute groups for engine-owned cached sounds and music, not a DSP graph.
Debug primitives visualize geometry a game supplies; they are not collision or query facilities.
Sprite presentation remains immediate drawing and does not batch sprites.

The networking package provides ENet transport plus an optional full-snapshot replicated-object
registry. Projects still choose schemas, spawn policy, simulation authority, input validation,
prediction, reconciliation, matchmaking, authentication, encryption, and NAT traversal. Replication
does not implement delta compression, RPCs, interest management, lag compensation, or persistence.
The current documented platform remains Linux; the vendored ENet transport is portable, but the
engine build has not yet enabled or verified its Windows backend.
