# Core, stage one

Modules are ordinary C translation units, built into `build/core/libcore.a`. Public headers expose raylib-native types. Core includes only its own files, raylib and system headers; projects provide policy and content.

- `engine`: window lifetime, accumulator loop, project callback signatures and input buffering.
- `transform`: optional local/world movement, rotation, coordinate conversion and bounded turning for
  2D/3D plain transforms. [Usage and raymath equivalents](TRANSFORMS.md).
- `capabilities`: enforces exactly what the project declared in `CoreRequirements` and nothing else.
  A project that declares nothing boots with no probe and no shader compiled; declaring
  `gpuSkinning`, `renderTargets` or `sampleableDepth` is what makes core demand and test them.
  Anything declared and unavailable is fatal — there is no fallback renderer.
- `file`, `shader`: checked disk reads and table-driven GLSL 120 shader loading. `CoreSetDataRoot`
  names where the engine's own files live so a binary started from another working directory still
  finds them; `CoreResolvePath` leaves absolute paths and files present in the working directory
  alone, so a project can override an engine file by shipping its own.
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
additional glyphs on demand. Printable ASCII is added to whatever list a project supplies, because a
rebuild triggered by ordinary text would be a visible hitch; Latin-1 rides along because the ~35 ms
cost is parsing the BDF rather than the glyph count. `UiTextWidth` ensures glyphs are loaded before measuring UI text.
The raylib build enables its existing BDF loader with a compiler flag; vendored source is unchanged.
The font is not embedded or copied into project output. Projects can set `UiTheme.fontPath`, request a
different codepoint list, or pass a loaded `Font` whose lifetime they own.

`EngineRun` sets the data root from `EngineConfig.engine_path`, or the executable's directory when
that is null, walking up until the engine's own `core/` data is in reach — an executable in a build
directory sits several levels below it. Shader, font and layout loads resolve through it; writes
never redirect, since the caller is naming where the file should go.

Ownership: shader tables own loaded shaders; the uniform registry copies names but borrows shaders. MB transfers buffers to raylib when uploaded. Actor borrows its model, clips and aim-joint list, and owns its pose state. Call `ActorUploadPose` immediately before drawing each instance when models are shared. Model loading remains raylib's responsibility; no custom formats or asset conversion are introduced.

`make -f Makefile.core` always runs `tools/check_core_dependencies.py` on compiler-produced dependency lists for core sources and headers before compilation/linking. Relative, absolute and transitive includes escaping the allowed roots fail the build. The executable project includes only core headers and system C headers. This is build enforcement, not a sandbox against deliberately disabling the build rules.

Run instructions and verification live in `projects/core_test/README.md`.

## Application authoring

`EngineConfigDefault()` supplies a 960×540 window, 60 FPS, a 1/60 fixed step and a 0.25 second catch-up
clamp. These are explicit defaults: overriding fixed_dt with zero still means variable-only, targetFps
zero means uncapped, and max_frame_dt zero means no clamp. No camera, UI or GPU feature is assumed.
All EngineProject callbacks are now optional; absent Update keeps running, absent Draw leaves the
application clear colour. Invalid configuration logs a reason before returning failure.

`EngineApplicationDefault()` combines config, callbacks, context and clear colour. Run it with
`EngineRunApplication`, or define `EngineApplicationMain(argc, argv)` and opt into `core/entry.c`.
That entry file is separate from libcore; custom main functions remain supported. Returning a descriptor
does not extend the lifetime of its pointers: use static state or otherwise retain it for the run.
The reusable Makefile declaration is `$(eval $(call core_project,name,core,entry))`; omit entry when
supplying main, and use gameplay in the second argument only when linking that optional layer.

A complete core-only moving rectangle needs no entity registration or initialization callback:

```c
#include "core/engine.h"
static float x;
static bool Update(void *context, double dt, const EngineInput *input)
{
    (void)context;
    if (input->down[KEY_D]) x += 100 * (float)dt;
    return !input->pressed[KEY_ESCAPE];
}
static void Draw(void *context, float alpha)
{
    (void)context; (void)alpha;
    DrawRectangle((int)x, 100, 24, 24, WHITE);
}
EngineApplication EngineApplicationMain(int argc, char **argv)
{
    (void)argc; (void)argv;
    EngineApplication app = EngineApplicationDefault();
    app.config.fixed_dt = 0;
    app.callbacks.Update = Update;
    app.callbacks.Draw = Draw;
    return app;
}
```

## Input actions and UI routing

`core/input.h` retains EngineInput's raw keys, mouse and text. InputAction borrows a binding array;
InputActionRead supplies held/pressed/released state, InputAxis combines two actions, and InputVector
limits digital diagonals to length one. Bindings currently support keyboard and mouse buttons; changing
the binding array changes the mapping without changing behaviour code. No allocation or name lookup
occurs during queries. Alternate bindings do not retrigger an action already held by another binding.
As with raw EngineInput, multiple transitions within a pending interval coalesce; event ordering is not
preserved. A press and release before the next update are both reported.

For UI-aware applications, supply `app.ui` and `app.BuildUi`. The runner starts a deferred UI frame,
calls BuildUi exactly once, finishes interaction, filters gameplay input, performs updates and world
drawing, then renders the recorded UI. A zero-initialized UI gets the default theme automatically; the
runner frees only a UI it initialized itself, after project Shutdown. A project may initialize its own
UI in Init and then owns its cleanup. FrameInput receives raw input; Update receives routed input.
Additional modal routing can be supplied by CaptureInput, combined with the UI's capture flags.

The UI consumes keyboard input when a text field owned focus during this frame or a menu is open,
and mouse input according to the existing UI hit/capture rules. The focus-release frame remains
captured, so a click used to dismiss a field does not also activate gameplay. The engine filters both
current and accumulated input. Captured edges/deltas are discarded, not replayed after dismissal.
Held keys resume after capture ends. With zero updates, uncaptured edges/deltas accumulate; with
multiple updates, they drain after the first update while held state remains available to all updates.

Existing immediate UI calls still work inside Draw. That path does not automatically route input before
simulation; use BuildUi for same-frame routing. Widgets and container callbacks retain their APIs.
Drawing commands copy text and reuse buffer capacity; glyphs resolve before layout and playback uses
the current atlas, so font expansion cannot leave stale texture references. Keep the theme/font stable
between building and rendering a deferred frame. Clip rectangles are stored per command and nested
indents retain the existing intersection semantics. No offscreen texture, shader or new GPU requirement
is introduced. For custom images/rendered views, use UiDrawCustom with a draw-only callback and data
that survives until rendering; direct raylib drawing inside BuildUi would execute too early.
