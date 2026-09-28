# Scripting

Games are scripted in Scheme. Engine things a script holds — its own entities, cameras, timers,
audio, collision worlds, pathfinders, movers, network clocks, textures, and anything a game adds —
are engine objects: made by name with `(make 'type args...)`, read with `(obj 'property)`, written
with `(set! (obj 'property) value)`, called with `(obj 'method args...)`, listened to with
`(connect! obj 'signal procedure)`, and ended with `(free! obj)`. Each entity callback is handed its
entity as an argument. Everything else — drawing, input, time, small math, saving — is a
free-standing call.

Use the [scripting README](../../gameplay/script/README.md) for entity declaration, a game's own
objects and calls, and how each is declared once. `make script-api` writes the reference for every
object type and call into `build/core/`.
