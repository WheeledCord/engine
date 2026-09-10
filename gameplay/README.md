# Gameplay

Gameplay depends on `core`; core never depends on gameplay or a project. This layer has no built-in
classnames, components, rendering policy or content.

`GameplayWorld` owns a fixed flat slot array and a fixed-stride payload buffer. An `EntityHandle` is an
index plus a generation: destroying a slot increments its generation, so stale handles fail every API
lookup. Entity classes are registered by classname at project startup. Each declaration supplies its
payload size plus optional `Spawn`, `KeyValue`, `Think` and `Draw` callbacks.

`EntitySpawn` zeroes the registered payload and calls Spawn. Scene keys are preserved by the world and
then delivered to KeyValue, which means the engine can write the same data without requiring a
project-specific serialization callback. Types should keep their key parsing deterministic: writing a
scene records source values, rather than reverse-engineering values from a type's private payload.

Think is opt-in and runs on an explicit fixed gameplay tick. `GameplayWorldConfig.tickInterval` must
match the fixed update interval chosen by the project, and the project calls `GameplayWorldStep` once per
fixed update. `EntityScheduleThink(world, entity, absoluteTime)` rounds the requested time to the nearest
tick. The world stores that integer tick and invokes Think when it becomes due. Before invocation it
clears the schedule, so a Think must schedule its next call explicitly.

The scene grammar is deliberately small and format-neutral:

```text
entity "classname" {
  "key" "value"
}
```

Quoted values accept `\\`, `\"` and `\n`; `#` and `//` begin comments outside quoted values. Loading
with `replaceWorld=true` clears live entities first. Failed loads report the path and line, and may leave
the replacement world partially loaded, so callers should load validated scene files at setup rather than
mid-session.

`GameplaySystems` is the reusable registry pattern from the old game: registered Update, Draw and Reset
callbacks are dispatched in insertion order. Projects choose whether a system advances the entity world;
the core loop does not know that gameplay exists.
