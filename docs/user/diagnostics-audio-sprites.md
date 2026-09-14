# Diagnostics, audio, and sprite presentation

All three services are optional, caller-owned helpers. Include their headers directly; they do not
add a global game state or impose entity components.

`CoreDebug` queues screen-space lines, circles, rectangles and copied text from an update or draw
callback. Call `CoreDebugUpdate` to age persistent primitives, then `CoreDebugDraw` from `Draw`.
With `overlay` enabled it also shows the current FPS, fixed updates this frame, and remaining fixed
step backlog. `GameplayDebugEntityCounts` adds one-frame entity count lines to that queue; it is a
bridge, not a world debugger or query visualizer.

`CoreAudioInit` prepares an empty service with `master`, `sfx`, and `music` volume groups. The first
play opens raylib's device. `CoreAudioPlaySound` and `CoreAudioPlayMusic` resolve paths through the
data root and cache each path once. Call `CoreAudioUpdate` once per rendered frame for music.
`CoreAudioFree` unloads everything and closes only a device the service itself opened. Buses are
volume/mute groups, not effects sends or a mixer graph.

`SpritePresentationDefault` makes a white, unit-scale anchored presentation. Pass a modified value
to `SpriteSheetDraw` or `SpriteAnimDraw` for ground placement, scale, rotation, tint and flipping.
For a sorted batch, place items in an array and call `qsort(items, count, sizeof *items,
SpriteDrawItemCompare)`: lower layer/order paint first, and `sequence` makes ties deterministic.
Presentation intentionally does not batch or own textures; sheets remain responsible for load,
metadata, anchors and animation clocks.
