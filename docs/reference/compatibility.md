# Compatibility and deliberate limits

Trench Engine builds on raylib and targets OpenGL 2.1 with GLSL 120. A project declares required GPU features; unsupported declared features fail at startup rather than selecting a fallback renderer.

The documented build target is Linux with X11 and OpenGL development packages. Physics, collision response, terrain policy, custom asset conversion, and a mandatory transform model are intentionally project-owned rather than engine facilities. `core/collision2d.h` is an optional spatial-hash query helper, not a mandatory index or a physics system.

Audio buses are volume/mute groups for engine-owned cached sounds and music, not a DSP graph.
Debug primitives visualize geometry a game supplies; they are not collision or query facilities.
Sprite presentation remains immediate drawing and does not batch sprites.
