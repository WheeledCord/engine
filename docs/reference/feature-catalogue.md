# Feature catalogue

This is the complete map of supported engine services. The linked header is the public contract; the
linked guide explains authoring where one exists. A service listed as **project-owned** is deliberately
not an engine feature.

| Area | Service | Public header / guide |
| --- | --- | --- |
| Runtime | Window, fixed/variable loop, callback lifecycle, UI input routing | `core/engine.h`; [application loop](../user/application-loop.md) |
| Runtime | Timing snapshot and opt-in debug overlay, text and transient primitives | `core/diagnostics.h` |
| Audio | Owned sound/music cache, data-root paths and named volume groups | `core/audio.h` |
| Runtime | GPU capability contract | `core/capabilities.h` |
| Input | Raw input, actions, axes, vectors | `core/input.h` |
| Files | Data-root resolution, checked reads, atomic writes | `core/file.h` |
| Rendering | GLSL 120 shader tables and frame uniforms | `core/shader.h`, `core/frame_uniforms.h` |
| Rendering | Colour/depth render targets | `core/render_target.h` |
| Rendering | Model instances and shader overrides | `core/world_draw.h` |
| Geometry | Explicit mesh construction | `core/mesh_builder.h` |
| Animation | Skeleton poses, clips, blending, aim, GPU skinning | `core/skeleton.h`, `core/animation.h`, `core/playback.h` |
| Sprites | Grid atlases, animation, ground anchors | `core/sprite_sheet.h` |
| Sprites | Anchored scale/tint/flip/rotation presentation and deterministic draw ordering | `core/sprite_sheet.h` |
| Isometric | Trimetic tile/hex conversion, ordering, neighbours | `core/iso_grid.h` |
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
| Scripting | Shared Scheme/Pawn binding table and scripted entities | `gameplay/script/script.h`; [scripting guide](../user/scripting.md) |

## Project-owned facilities

Physics, collision response, terrain rules, map size, content formats/conversion, game rules, and
application-specific state remain project-owned. The optional collision package supplies query indexing,
not a physics model. See [compatibility and limits](compatibility.md).

## Reference coverage

The XML reference begins with the application and entity-world contracts, whose lifetime rules are the
most consequential. Add XML files service by service as their public API is maintained; do not claim a
service is fully reference-covered until all exported symbols and public types in its header are
documented.
