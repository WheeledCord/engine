# Core

Modules are ordinary C translation units, built into `build/core/libcore.a`. Public headers expose raylib-native types. Core includes only its own files, raylib and system headers.

- `engine`: window lifetime, accumulator loop, project callback signatures and input buffering.
- `object`: engine types described once — properties, methods and signals in a table beside the
  type — and a caller-owned pool of objects named by generational handles, with signal connections.
  Camera, audio, collision world, network clocks, texture and timer each declare their type in
  their own file.
- `timer`: a countdown object that emits `timeout`, then starts again unless it is one-shot.
- `node`: a `node3d` object with a local position, rotation and scale and an optional parent, whose
  world transform is its parent's world transform times its own, as in Godot's Node3D.
- `network`, `net_clock`, `net_sync`, `net_session`: multiplayer. `network` is the ENet transport;
  `net_clock` the server tick and the client's interpolation clock; `net_sync` the registry of
  replicated objects, whose schemas mark which fields are shared, with delta snapshots and ownership;
  `net_session` hosting and joining around it — the host is the server, with one registry, and its own
  player's commands run in place; a joiner is checked for engine protocol, game and version, welcomed
  with what it needs to build the world, and sent snapshots once ready. Commands go up to the
  server, events come down to players. See `docs/user/networking.md`.
- `waypoints`: a fixed-capacity point graph -- up to 256 points, 8 links each -- with shortest-path
  queries by summed straight-line edge distance (A*, as Godot's AStar3D). A project builds its own
  graph over it; `next-hop` and `path` answer the route between two of its points.
- `capabilities`: enforces exactly what the project declared in `CoreRequirements` and nothing else.
  A project that declares nothing boots with no probe and no shader compiled; declaring
  `gpuSkinning`, `renderTargets` or `sampleableDepth` is what makes core demand and test them.
  Anything declared and unavailable is fatal — there is no fallback renderer.
- `file`, `shader`: checked disk reads, atomic saves and table-driven GLSL 120 shader loading.
  `CoreAtomicBegin`/`CoreAtomicCommit` write beside the destination and rename over it once the
  bytes are on disk, so a save that fails leaves the previous file untouched. `CoreSetDataRoot`
  names where the engine's own files live so a binary started from another working directory still
  finds them; `CoreResolvePath` leaves absolute paths and files present in the working directory
  alone, so a project can override an engine file by shipping its own.
- `frame_uniforms`: caller-declared names/types/counts, cached locations, per-frame value binding.
  Missing uniforms are skipped. Textures take units of their own above the ones raylib uses for
  material maps and its batch, so a bound frame texture stays bound across every world draw; a card
  without the units fails the declaration instead of sampling something else.
- `render_target`: colour/depth FBO construction and destruction.
- `world_draw`: model-instance drawing with optional shader override across material slots.
- `mesh_builder`: explicit position/normal/UV emission, quad helpers and directional mapping math.
- `skeleton`: hierarchy, local/global pose conversion, posed-pivot rotations, chain collapse and raymath skin matrices.
- `animation`: per-instance playback, explicit FPS, frame sampling, quaternion pose blending, aim and GPU pose upload.
- `playback`: where a clip has got to after a given time -- frame, and the pair either side of it
  for blending. Skeletal animation and sprite sheets both ask it, so neither of them owns the rule
  and the two cannot answer differently.
- `sprite_sheet`: a grid atlas played frame by frame, its plain-text sidecar, and the ground anchor
  saying where in a cell its subject's feet are, so a drawer never has to guess at the offset. Any
  producer will do: baked out of 3D, hand-drawn, whatever fills the grid. `SpriteAnim` keeps the
  clock, as Actor does for skeletons. Loading finds each frame's opaque rectangle, and draws cover
  only that where the result is pixel-identical (`SpriteSheetCanTrim`: zoom 1 or 2, unrotated), so a
  cell's transparent margin costs no fill.
- `particles`: a fixed-capacity pool of physically simulated billboards -- age, gravity, drag,
  curl-noise turbulence, an optional pull toward an anchor and a rotation spin-up -- with a `step`
  hook run after each particle moves and a `look` hook that can replace a particle's drawn size,
  colour, up vector or rotation. A game owns what a `kind` means, its textures and its presets; the
  pool only ages, pushes and draws them. As the `"particles"` engine type, a script sets a
  template's life/size/gravity/drag and calls `emit!`/`clear!`; drawing is not reachable from
  scripts.
- `iso_grid`: the two grids Fallout 1/2 lay over the same ground -- a square tile grid for the floor
  and a hex grid twice as fine for everything that moves -- in their own trimetric projection, which
  leans rather than mirroring. Screen conversion both ways, hex neighbours, distance and facing,
  the cell outlines that tile exactly, and the far-to-near order an isometric scene must be painted
  in. No camera and no map size: those belong to a project.
- `iso_map`: a map on those two grids, as Fallout's mapper and RPG Maker lay one out: a floor
  tile per tile, path tiles per hex over it (their outline worked out per pixel from the hexes
  around, a smooth wandering edge rather than a hexagonal one), solid blocks stacked on tiles,
  connected walls one per tile (cardinal neighbours as in OpenRA's wall connector; adjustable
  thickness and placement, with filled regions). Walls reuse the top and side surfaces of an
  ordinary block sheet as clipped textured polygons; no complete-mask sprite sets are needed.
  Styles referring to an existing block share its atlas. Legacy 16-frame wall sheets still work
  when width is zero. Wall topology is cached and navigation conservatively intersects whole
  hex cells with the wall footprint. Roofs sit over tiles (a building's roof found by flood fill,
  as Fallout hides one; a pitched roof autotiles its slopes and ridge), props have hex footprints,
  and objects are left for a game to spawn. A tileset names the kinds; maps store them by name. The ground is
  composited once into point-filtered chunk textures (512 texels, 256 where the device's texture
  limit is under 1024) and refreshed only where edited; `IsoFog` is the map's edge fog, built the same
  way from the bounds alone (clear a given number of tiles in, dithered by `SpriteDitherDensity`) and
  drawn around its exactly computed clear middle; blocks come as
  two slices (as do walls) sorted on their own hexes so characters beside them sort correctly. `Tools/map-editor`
  edits these.
- `sprite_bake`: the pixel-art bake -- a toon pass and a world-normal pass through the same camera,
  Floyd-Steinberg over the bands between light and shadow, more toward a wall's foot when ground
  shade is on (a height pass says how high each pixel is) -- plus planar cast shadows and a live draw
  with the same shaders, texture clean-up (median denoise, blur, flatten, brightness, median-cut
  colours) and median-cut palette reduction. `Tools/sprite-baker` and `Tools/sprite-maker`
  both use it, so everything baked shares one look.
- `model_import`: any model in metres, Y up: FBX and OBJ through ufbx (vendor/ufbx), which converts
  units and axes in the geometry and the animation curves alike -- the centimetre armature of a
  Mixamo download included -- and glTF, IQM, M3D and VOX through raylib. Skeletons come out with
  parents first and unweighted end bones dropped to fit the bone budget; clips can be held in place
  and added from other files by bone name, their root motion carried relative to the source's rest.
- `file_dialog`: the system's Open, Save and Choose Folder dialogs for tools, through
  tinyfiledialogs (vendor/tinyfiledialogs): native on Windows and macOS, zenity or kdialog on Linux.
  Blocking, as those dialogs are; a queued answer lets scripted checks go through it with no window.
- `fps_camera`: movement smoothing, accepted-distance gait, footfall events, breath/bob and viewmodel sway. Input mappings and collision/terrain policy belong to callers.
- `viewmodel`: one scoped projection/depth function and camera-space transform helper.
- `transform`: local/world movement, rotation, conversion and bounded turning. Forward is +Z, up
  +Y, right -X, matching FpsCamera, ActorLookAt and glTF models. [Usage](TRANSFORMS.md).
- `collision3d`: a fixed-capacity set of Models with layers and a caller tag, and a closest-hit ray
  query over their meshes -- the 3D counterpart to `collision2d`'s shape queries. It stores each
  Model pointer, so a query always sees that model's current transform and meshes. Not a Scheme
  engine type yet: scripts cannot add models to it.
- `ui`: integer-pixel bevel primitives, immediate widgets, single-line text editing with caret,
  selection and system clipboard, nested clipped indent regions
  and an externally loaded bitmap font. Widgets only answer to the pointer where they are visible:
  a control clipped away by a panel takes no clicks. A button that stops accepting input mid-press
  reports nothing, and the keyboard is released as soon as a focused field stops being drawn.
  `UiNextId` gives a widget an identity of the caller's choosing, so a press survives relabelling. A normal button creates its compact indent automatically;
  `UiButtonBare` is the explicit escape hatch for flush button stacks already inside one indent.
- `ui_layout`: standard-gap row and column cursors, equal cells and conventional grouped indents.
- `ui_document`: a saved layout — a sized, styled surface plus its elements — with versioned
  plain-text load/save, drawn by editor and runtime through one call so it looks the same in both.
  Chrome and elements are separable for editors that draw between them. Every element names its
  container outright, so moving or resizing one never changes what holds it; contents are drawn over
  their container and clipped to it. Elements carry anchors, minimum and maximum sizes and an
  optional container flow; `UiDocumentResolve` places them for any other content size in one integer
  pass, leaving the authored rectangles alone. Siblings that stretch divide the change and stop at
  their own minimums without ever overlapping, and `UiDocumentMinimumSize` reports the size that
  actually fits. An element can carry a name, and `UiDocumentActivated` tells a game which one was
  used, so one layout can hold more than one button.
- `ui_editor`: topmost rectangle picking, eight-handle move/resize, selection handles, and snapping
  to a grid, to other rectangles' edges and centres, to the conventional gap and to whole rows,
  reporting the guide lines it matched. Snap distances, grid pitch and what counts as conventional
  are the caller's policy.
- `ui_menu`: right-click menus with screen-clamped cascading submenus and visible chain movement when
  space is made at a screen edge.
- `ui_browser`: a folder as a grid to pick from, images as thumbnails loaded a few a frame, with a
  name filter, Up, and Folder for the system dialog -- Hammer's texture browser, for any tool.
- `ui_containers`: retained draggable panels, fixed dock columns, and `UiDockBoard` floating apps.
  Body click activates on release; body drag detaches one app; grip drag moves its attached bar.
  Apps snap to neighbours and viewport edges, retain edge pins across resizes, and capture the
  entire pointer gesture including release outside. Rectangles/groups are caller-owned data;
  the engine supplies interaction, not application icons, game rules or a persistence format.
  Optional `groupHandle` makes an app a clickable root that drags the entire group. `edgeOnly`
  uses caller-owned `dragBackup` storage to restore all apps after an invalid drop; `verticalStack`
  keeps the root first and docks to left/right sides and corners. Removing a child preserves the
  root position. Existing unconstrained floating boards retain their behavior.
- `pixel_draw`: integer-world-pixel lines and paths. Pixels are 1×1 world quads, so a camera's
  zoom scales their stair steps and width exactly like point-filtered sprite texels. Closed paths
  blend each shared corner once. This is opt-in; ordinary smooth line rendering is unchanged.

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

Ownership: shader tables own loaded shaders; the uniform registry copies names but borrows shaders. MB transfers buffers to raylib when uploaded. Actor borrows its model, clips and aim-joint list, and owns its pose state. Call `ActorUploadPose` immediately before drawing each instance when models are shared. Models load through raylib's loaders; the engine's own formats are the sprite-sheet sidecar, UI documents, scenes, saves and input maps.

`make` always runs `tools/check_core_dependencies.py` on compiler-produced dependency lists for core sources and headers before compilation/linking. Relative, absolute and transitive includes escaping the allowed roots fail the build. The executable project includes only core headers and system C headers. This is build enforcement, not a sandbox against deliberately disabling the build rules.

Run instructions and verification live in `tests/integration/core/README.md`.

## Application authoring

`EngineConfigDefault()` supplies a 960×540 window, 60 FPS, a 1/60 fixed step and a 0.25 second catch-up
clamp. These are explicit defaults: overriding fixed_dt with zero still means variable-only, targetFps
zero means uncapped, and max_frame_dt zero means no clamp. No camera, UI or GPU feature is assumed.
All EngineProject callbacks are now optional; absent Update keeps running, absent Draw leaves the
application clear colour. Invalid configuration logs a reason before returning failure.

`idleFps` and `backgroundFps` optionally lower the frame limit (zero disables each policy). Background
means unfocused or minimized. Idle means no keyboard, mouse, text or resize activity for 250 ms and
an application `NeedsContinuousFrames` callback returning false. Input restores the normal limit;
fixed updates still consume elapsed time. Animation, exports and tests can keep continuous frames.
The lower limits never raise an already lower `targetFps`.
Tools can also opt into `waitWhenIdle`: EndDrawing waits for window/input events instead of polling
when the application reports no continuous work. Mouse/key/window events wake it immediately. The
callback must keep returning true for animation, text cursor blinking, pending work or deadlines;
external jobs must likewise keep frames running until completed. This option defaults to false.

`IsoMapQueryCache` caches collision grids and roof regions using keys from their source contents.
Zero-initialise it, call `IsoMapCachedBlocking`/`IsoMapCachedRoofRegion`, and release it with
`IsoMapQueryCacheFree`. Returned buffers are borrowed. Resizes, undo, direct map-array edits and
changed tileset blocking/footprints automatically invalidate the relevant result. Allocation failure
returns NULL. This avoids rebuilding collision or flood-filling a stationary actor's roof each frame.

`EngineApplicationDefault()` combines config, callbacks, context and clear colour. Run it with
`EngineRunApplication` from your own `main`, or define only `EngineApplicationMain(argc, argv)`: the
standard `main` in `core/entry.c` is part of libcore, and the linker takes it only when a program has
no `main` of its own. Returning a descriptor
does not extend the lifetime of its pointers: use static state or otherwise retain it for the run.
Games are built with `engine-build` from an `engine.project` manifest; see the root README.

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

`CoreMouseCaptureUpdate` owns relative pointer capture for first-person and free-look views. Call it
once per rendered frame with whether gameplay currently wants the pointer; use only its returned
delta. It releases capture while the window is unfocused and suppresses cursor rebasing after focus,
fullscreen, or size transitions. Call `CoreMouseCaptureRelease` before destroying the window.

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

## Model import texture overrides

`ModelImport` loads FBX/OBJ material images through ufbx and GLB/glTF images through raylib.
FBX texture nodes are resolved to their first usable file image (not a layered-shader evaluator);
embedded PNG/JPEG signatures and relocated Windows filenames are handled. Import reports include
the number of textured materials, including zero when the source has no images.

`ModelImportSetAlbedo(imported, path)` replaces all base-colour maps with an importer-owned
image, keeps UVs/geometry/animation intact and uses a white material tint. NULL/empty restores
the exact imported maps and tints. A failed load leaves the active appearance unchanged.
`ModelImportFree` also frees any active override; callers must not unload these textures directly.
Other material channels are not overridden. References: Godot `modules/fbx/fbx_document.cpp:126-136,
1053-1075`; raylib `src/rmodels.c:1193-1210` documents model/texture lifetime separation.

Sprite baking uses geometric normals, the imported base colour, the existing lighting bands and
Floyd–Steinberg dither. Imported normal/AO maps do not override that style, and baking does not
silently change a model's texture sampler. Texture cleanup and source-colour palette reduction
remain separate, explicit import operations.

An authored scene plate can set `backdrop 1` in its `.prop` sidecar. Collected map pieces carry
that role so frontends draw them after terrain and before standing objects. Collision remains
independent (`blocking`, footprint and map collision paths). Ordinary props default to standing
pieces; backdrops do not need a second copy of their texture for each placed object.
