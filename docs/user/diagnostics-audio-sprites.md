# Diagnostics, audio, and sprite presentation

All three services are optional, caller-owned helpers. Include their headers directly; they do not
add a global game state or impose entity components.

`CoreDebug` queues screen-space lines, circles, rectangles and copied text from an update or draw
callback. Call `CoreDebugUpdate` to age persistent primitives, then `CoreDebugDraw` from `Draw`.
With `overlay` enabled it also shows the current FPS, fixed updates this frame, and remaining fixed
step backlog.

`CoreAudioInit` prepares an empty service with `master`, `sfx`, and `music` volume groups. The first
play opens raylib's device. Sounds and music resolve paths through the data root and are cached once
per path; `CoreAudioLoadSound` loads one ahead of its first play. A sound plays over itself on up to
`CORE_AUDIO_POLYPHONY` voices, so repeats overlap instead of cutting each other off.

For sound in a world, set the listener once a frame with `CoreAudioSetListener` (usually the camera)
and play with `CoreAudioPlaySoundAt`: the gain falls off linearly with distance to silence at the
range given — Godot's `max_distance` with attenuation disabled — and the sound pans toward the side
it is on. A sound out of range is not started. For a sound that has to be held — moved while it
plays, looped, stopped by itself — make a voice with `CoreAudioVoiceCreate` and use the
`CoreAudioVoice*` calls; `CoreAudioUpdate`, once per rendered frame, keeps held voices placed for
where the listener now is, restarts looping ones, and feeds music. A Scheme game plays sounds through
the runner (`play-sound`, and the `sound` kind; see [Running a Scheme game](kinds.md)).

`CoreAudioFree` unloads everything and closes only a device the service itself opened. Buses are
volume/mute groups, not effects sends or a mixer graph.

`SpritePresentationDefault` makes a white, unit-scale anchored presentation. Pass a modified value
to `SpriteSheetDraw` or `SpriteAnimDraw` for ground placement, scale, rotation, tint and flipping.
For a sorted batch, place items in an array and call `qsort(items, count, sizeof *items,
SpriteDrawItemCompare)`: lower layer/order paint first, and `sequence` makes ties deterministic.
Presentation intentionally does not batch or own textures; sheets remain responsible for load,
metadata, anchors and animation clocks.
