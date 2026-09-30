# Feature catalogue

This is the complete map of supported engine services. The linked header is the public contract; the
linked guide explains authoring where one exists.

| Area | Service | Public header / guide |
| --- | --- | --- |
| Runtime | Window, fixed/variable loop, callback lifecycle, UI input routing | `core/engine.h`; [application loop](../user/application-loop.md) |
| Runtime | Timing snapshot and opt-in debug overlay, text and transient primitives | `core/diagnostics.h` |
| Audio | Owned sound/music cache, data-root paths, named volume groups, positional sound with a listener, falloff and pan, overlapping voices, and held voices that move, loop and stop | `core/audio.h`; [audio](../user/diagnostics-audio-sprites.md) |
| Runtime | GPU capability contract | `core/capabilities.h` |
| Input | Raw input, actions, axes, vectors, and focus-safe relative pointer capture | `core/input.h`; [mouse capture](../user/mouse-capture.md) |
| Files | Data-root resolution, checked reads, atomic writes | `core/file.h` |
| Files | Versioned named-field save files with migration | `core/save.h` |
| Input | Named actions with saved, rebindable bindings | `core/input_map.h` |
| Networking | ENet-backed client/server endpoints, peer events, reliable messages and replaceable snapshots | `core/network.h`; [networking](../user/networking.md) |
| Networking | The store's things shared between a host and up to 15 clients: ownership by the tree, delta state against acknowledged baselines, messages delivered with their sender's state, interpolation 100 ms behind, joining checked kind by kind | `core/store_net.h`; [networking](../user/networking.md) |
| Networking | store_net over ENet: hosting, joining with retry, peer numbering, flushing, wire byte totals | `core/store_net_enet.h`; [networking](../user/networking.md) |
| Networking | Fixed server tick, a separate snapshot rate, and the client clock that draws the world in the past | `core/net_clock.h`; [networking](../user/networking.md) |
| Rendering | GLSL 120 shader tables, `#include` expansion, and frame uniforms | `core/shader.h`, `core/frame_uniforms.h` |
| Rendering | Colour/depth render targets | `core/render_target.h` |
| Rendering | Model instances and shader overrides | `core/world_draw.h` |
| Geometry | Explicit mesh construction | `core/mesh_builder.h` |
| Textures | Data-root loading with explicit mipmap, filtering, wrapping, and ownership | `core/texture.h` |
| Textures | Bake a material shader into a mesh's UV layout, with seam dilation and mip-safe background fill | `core/uv_bake.h`; [texture baking](../user/texture-baking.md) |
| Textures | Surface maps: colour plus height in one texture, normal derived from it | `core/shaders/surface_map.glsl`; [texture baking](../user/texture-baking.md) |
| Animation | Skeleton poses, clips, blending, aim, GPU skinning | `core/skeleton.h`, `core/animation.h`, `core/playback.h` |
| Animation | Load a binary `.rig` file (bones, rest matrices, per-vertex weights, pose clips) into skeleton/animation data an Actor can play | `core/rig_file.h` |
| Sprites | Grid atlases, animation, ground anchors | `core/sprite_sheet.h` |
| Sprites | Anchored scale/tint/flip/rotation presentation and deterministic draw ordering | `core/sprite_sheet.h` |
| Isometric | Trimetric tile/hex conversion, ordering, neighbours | `core/iso_grid.h` |
| Camera | FPS camera and viewmodel helpers | `core/fps_camera.h`, `core/viewmodel.h` |
| Camera | Optional 2D follow, bounds, interpolation, and world/screen conversion | `core/camera2d.h`; [2D guide](../user/2d-space.md) |
| Transforms | 2D/3D movement, conversion, bounded turns | `core/transform.h` |
| Collision | Optional circles/AABBs, layer/mask filtering, overlap and sweep queries, spatial hash | `core/collision2d.h`; [2D guide](../user/2d-space.md) |
| UI | Immediate widgets, text input, clipping, menus, panels | `core/ui.h`, `core/ui_menu.h`, `core/ui_containers.h` |
| UI authoring | Layout/document load-save-resolve and editing helpers | `core/ui_document.h`, `core/ui_editor.h`, `core/ui_layout.h` |
| World | The store: kinds with typed fields, things in a tree with generational handles, the ordered tick, the presentation frame, the replay rules, snapshots, a world hash and saves | `core/store.h`; [store design](../developer/store.md) |
| World | Built-in 3D kinds: nodes, models, sockets, cameras, lights, characters with collide-and-slide, solids, areas, tilemaps with paths and ray casts, sounds | `core/world3d.h`; [running a Scheme game](../user/kinds.md) |
| Rendering | The draw path: sorted, culled, statically batched draws of a frame's models | `core/draw_path.h` |
| Runtime | Recording and replaying a session's input, commands and packets | `core/replay.h` |
| Scripting | Scheme on the store: `define-kind`, handlers, the rules as errors, the calls a game makes | `gameplay/script/game_s7.h`; [running a Scheme game](../user/kinds.md) |
| Runner | `trench run <dir>`: a Scheme project windowed or headless, with co-op, recording, replay, a bot and hashes | `gameplay/game.h`; [running a Scheme game](../user/kinds.md) |

## Not provided yet

Physics and collision response are not engine systems yet; games that need them currently supply
their own. The collision package supplies query indexing, not a physics model; world3d's characters
collide and slide against boxes, which is not a physics model either.
See [compatibility and limits](compatibility.md).

## Reference coverage

The XML reference begins with the application contract, whose lifetime rules are the most
consequential. Add XML files service by service as their public API is maintained; do not claim a
service is fully reference-covered until all exported symbols and public types in its header are
documented.
