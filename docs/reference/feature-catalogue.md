# Feature catalogue

This is the complete map of supported engine services. The linked header is the public contract; the
linked guide explains authoring where one exists.

| Area | Service | Public header / guide |
| --- | --- | --- |
| Runtime | Window, fixed/variable loop, callback lifecycle, UI input routing | `core/engine.h`; [application loop](../user/application-loop.md) |
| Runtime | Timing snapshot and opt-in debug overlay, text and transient primitives | `core/diagnostics.h` |
| Audio | Owned sound/music cache, data-root paths and named volume groups | `core/audio.h` |
| Runtime | GPU capability contract | `core/capabilities.h` |
| Input | Raw input, actions, axes, vectors, and focus-safe relative pointer capture | `core/input.h`; [mouse capture](../user/mouse-capture.md) |
| Files | Data-root resolution, checked reads, atomic writes | `core/file.h` |
| Files | Versioned named-field save files with migration | `core/save.h` |
| Input | Named actions with saved, rebindable bindings | `core/input_map.h` |
| Networking | ENet-backed client/server endpoints, peer events, reliable messages and replaceable snapshots | `core/network.h`; [networking](../user/networking.md) |
| Networking | Declarative replicated-object schemas, stable identities, ownership checks, full snapshots, stale-snapshot rejection, and interpolation against server time | `core/net_sync.h`; [networking](../user/networking.md) |
| Networking | Fixed server tick, a separate snapshot rate, and the client clock that draws the world in the past | `core/net_clock.h`; [networking](../user/networking.md) |
| Rendering | GLSL 120 shader tables, `#include` expansion, and frame uniforms | `core/shader.h`, `core/frame_uniforms.h` |
| Rendering | Colour/depth render targets | `core/render_target.h` |
| Rendering | Model instances and shader overrides | `core/world_draw.h` |
| Geometry | Explicit mesh construction | `core/mesh_builder.h` |
| Textures | Data-root loading with explicit mipmap, filtering, wrapping, and ownership | `core/texture.h` |
| Textures | Bake a material shader into a mesh's UV layout, with seam dilation and mip-safe background fill | `core/uv_bake.h`; [texture baking](../user/texture-baking.md) |
| Textures | Surface maps: colour plus height in one texture, normal derived from it | `core/shaders/surface_map.glsl`; [texture baking](../user/texture-baking.md) |
| Animation | Skeleton poses, clips, blending, aim, GPU skinning | `core/skeleton.h`, `core/animation.h`, `core/playback.h` |
| Sprites | Grid atlases, animation, ground anchors | `core/sprite_sheet.h` |
| Sprites | Anchored scale/tint/flip/rotation presentation and deterministic draw ordering | `core/sprite_sheet.h` |
| Isometric | Trimetric tile/hex conversion, ordering, neighbours | `core/iso_grid.h` |
| Camera | FPS camera and viewmodel helpers | `core/fps_camera.h`, `core/viewmodel.h` |
| Camera | Optional 2D follow, bounds, interpolation, and world/screen conversion | `core/camera2d.h`; [2D guide](../user/2d-space.md) |
| Transforms | 2D/3D movement, conversion, bounded turns | `core/transform.h` |
| Collision | Optional circles/AABBs, layer/mask filtering, overlap and sweep queries, spatial hash | `core/collision2d.h`; [2D guide](../user/2d-space.md) |
| UI | Immediate widgets, text input, clipping, menus, panels | `core/ui.h`, `core/ui_menu.h`, `core/ui_containers.h` |
| UI authoring | Layout/document load-save-resolve and editing helpers | `core/ui_document.h`, `core/ui_editor.h`, `core/ui_layout.h` |
| Gameplay | Generational entity world and lifecycle | `gameplay/entity.h`; [gameplay guide](../user/gameplay.md) |
| Gameplay | Typed entity fields and scene files | `gameplay/fields.h`, `gameplay/scene.h` |
| Gameplay | Ordered update/draw system registry | `gameplay/systems.h` |
| Gameplay | Entity-count diagnostic bridge | `gameplay/debug.h` |
| Gameplay | Core application adapter | `gameplay/runtime.h` |
| Gameplay | A* routing and movement over the isometric hex grid | `gameplay/iso_move.h` |
| Scripting | Scheme binding table and scripted entities | `gameplay/script/script.h`; [scripting guide](../user/scripting.md) |

## Not provided yet

Physics, collision response and a transform hierarchy are not engine systems yet; games that need
them currently supply their own. The collision package supplies query indexing, not a physics model.
See [compatibility and limits](compatibility.md).

## Reference coverage

The XML reference begins with the application and entity-world contracts, whose lifetime rules are the
most consequential. Add XML files service by service as their public API is maintained; do not claim a
service is fully reference-covered until all exported symbols and public types in its header are
documented.
