# Compatibility and deliberate limits

Trench Engine builds on raylib and targets OpenGL 2.1 with GLSL 120. A project declares required GPU features; unsupported declared features fail at startup rather than selecting a fallback renderer.

The documented build target is Linux with X11 and OpenGL development packages. Physics, collision response, terrain policy, spatial indexing, custom asset conversion, and a mandatory transform model are intentionally project-owned rather than engine facilities.
