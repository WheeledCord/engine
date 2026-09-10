# Core, stage one

Modules are ordinary C translation units, built into `build/core/libcore.a`. Public headers expose raylib-native types. Core includes only its own files, raylib and system headers; projects provide policy and content.

- `engine`: window lifetime, accumulator loop, project callback signatures and input buffering.
- `capabilities`: required GPU/depth support and limit logging; no fallback renderer.
- `file`, `shader`: checked disk reads and table-driven GLSL 120 shader loading.
- `frame_uniforms`: caller-declared names/types/counts, cached locations, per-frame value binding. Missing uniforms are skipped.
- `render_target`: colour/depth FBO construction and destruction.
- `world_draw`: model-instance drawing with optional shader override across material slots.
- `mesh_builder`: explicit position/normal/UV emission, quad helpers and directional mapping math.
- `skeleton`: hierarchy, local/global pose conversion, posed-pivot rotations, chain collapse and raymath skin matrices.
- `animation`: per-instance playback, explicit FPS, frame sampling, quaternion pose blending, aim and GPU pose upload.
- `fps_camera`: movement smoothing, accepted-distance gait, footfall events, breath/bob and viewmodel sway. Input mappings and collision/terrain policy belong to callers.
- `viewmodel`: one scoped projection/depth function and camera-space transform helper.
- `ui`: integer-pixel bevel primitives, immediate widgets, single-line text editing with caret,
  selection and system clipboard, nested clipped indent regions
  and an externally loaded bitmap font. A normal button creates its compact indent automatically;
  `UiButtonBare` is the explicit escape hatch for flush button stacks already inside one indent.
- `ui_layout`: standard-gap row and column cursors, equal cells and conventional grouped indents.
- `ui_document`: a saved layout — a sized, styled surface plus its elements — with versioned
  plain-text load/save, drawn by editor and runtime through one call so it looks the same in both.
  Chrome and elements are separable for editors that draw between them. Elements carry anchors,
  minimum sizes and an optional container flow; `UiDocumentResolve` places them for any other content
  size in one integer pass, leaving the authored rectangles alone, and `UiDocumentMinimumSize`
  reports how small the layout may go.
- `ui_editor`: topmost rectangle picking, eight-handle move/resize, selection handles, and snapping
  to a grid, to other rectangles' edges and centres, to the conventional gap and to whole rows,
  reporting the guide lines it matched. Snap distances, grid pitch and what counts as conventional
  are the caller's policy.
- `ui_menu`: right-click menus with screen-clamped cascading submenus and visible chain movement when
  space is made at a screen edge.
- `ui_containers`: retained draggable panels and fixed square tiles placed in dock columns.

Panels and menus both use `UiDrawWindow`, so the 2px frame, title bar, clipping and content origin
cannot drift. Menu roots use an explicit menu title or the clicked context name. Submenus use the
label of the button that opened them. Menu button stacks use one grouped indent with no gaps between
buttons; submenu buttons are click toggles and draw their new state on the release frame.

The project owns its config and defines `Init`, `FrameInput`, `Update`, `Draw`, `Shutdown` callbacks. `EngineRun` owns the loop. `Draw` runs inside `BeginDrawing`/`EndDrawing`; it receives interpolation alpha, never simulation time advancement. Shutdown also handles partially completed initialization.

`gameplay/` sits above core and may include its headers; project code may include either layer. Core's
dependency guard rejects actual compiler dependencies on gameplay, projects, or legacy root headers.

With `fixed_dt>0`, Update runs zero or more times and alpha is the remaining accumulator fraction. With `fixed_dt=0`, Update runs once with elapsed time and alpha is 1. `max_frame_dt=0` disables time clamping; a positive value explicitly caps catch-up time and logs when applied.

Input is sampled once per rendered frame. Mouse deltas/wheel accumulate until an update consumes them. Press/release edges are OR-latched through frames with no update, delivered on the first subsequent Update, then cleared; later updates see held states with no repeated edges or deltas. Multiple presses before any update coalesce into one edge. `FrameInput` sees raw frame input for window/cursor controls, not simulation events.

UI receives the raw `FrameInput` snapshot once during drawing, so interaction is independent of the
number of fixed updates. Nested UI regions intersect their integer clip rectangles and explicitly restore
the parent scissor because raylib's `EndScissorMode` disables clipping rather than stacking it.
The default theme reads GNU Unifont's BDF from `core/fonts/`, starts with ASCII and Latin-1, and loads
additional glyphs on demand. `UiTextWidth` ensures glyphs are loaded before measuring UI text.
The raylib build enables its existing BDF loader with a compiler flag; vendored source is unchanged.
The font is not embedded or copied into project output. Projects can set `UiTheme.fontPath`, request a
different codepoint list, or pass a loaded `Font` whose lifetime they own.

Ownership: shader tables own loaded shaders; the uniform registry copies names but borrows shaders. MB transfers buffers to raylib when uploaded. Actor borrows its model, clips and aim-joint list, and owns its pose state. Call `ActorUploadPose` immediately before drawing each instance when models are shared. Model loading remains raylib's responsibility; no custom formats or asset conversion are introduced.

`make -f Makefile.core` always runs `tools/check_core_dependencies.py` on compiler-produced dependency lists for core sources and headers before compilation/linking. Relative, absolute and transitive includes escaping the allowed roots fail the build. The executable project includes only core headers and system C headers. This is build enforcement, not a sandbox against deliberately disabling the build rules.

Run instructions and verification live in `projects/core_test/README.md`.
