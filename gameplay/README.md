# Gameplay

Gameplay depends on core. Core never includes gameplay or a project. Entities remain fixed flat slots
with generational handles and ordinary C payloads. There is no mandatory transform or base class.

## Authoring

See `projects/authoring_demo/main.c` for a complete moving entity and application.

An EntityClass declares classname, payload size, optional copied defaults, optional fields, and only
the callbacks it implements. Use designated initializers. Registration copies the classname and class
record; defaults and field metadata are borrowed and must outlive the world. All registration happens
before the first spawn, so live entities cannot retain pointers invalidated by registry growth.

All callbacks receive EntityContext:

- `data`: the instance payload; `world` and `entity`: access to other entities.
- `app`: the project context set on the world, without an application global.
- `input`: the current simulation snapshot, or an empty snapshot outside stepping.
- `now`: gameplay time; `dt`: one fixed step; `elapsed`: time since spawn or the previous Think.
- `alpha` and `draw`: interpolation and the optional drawing context during Draw.

Callbacks and payload pointers are valid only during the call and until the entity is destroyed.
Destroying another entity is supported; a Destroy callback cannot recursively destroy itself twice.
Callbacks must not free/clear the world or recursively set properties; do that at a project boundary.
Think is still opt-in and scheduled. `EntityThinkNext(e)` requests the next simulation step;
`EntityThinkAfter(e, seconds)` requests a delay. Not rescheduling stops thinking. The absolute-time
`EntityScheduleThink` remains available, rounding to the nearest fixed tick as before.

## Properties and lifetime

Spawn order is now **allocate → copy defaults → apply properties → Spawn**. Spawn returns bool.
`EntitySpawnWith` accepts a list of string keyvalues; scene loading uses this same path. Spawn therefore
sees the final configured values. Invalid properties abort before Spawn; a failed Spawn calls Destroy
to clean any resources it partially created. Normal deletion, world clear and shutdown also call
Destroy exactly once. Defaults must be plain values or borrowed references, not independently owned
resource allocations. Allocate instance resources in Spawn.

Declare configurable members with `ENTITY_FIELD(Type, member, ENTITY_FLOAT)` or an explicit
EntityField initializer. Supported types: float, double, int, bool, fixed char array, Vector2, Vector3.
Explicit declarations can include numeric bounds. Vectors use whitespace-separated components;
bools accept true/false/1/0. Nonfinite numbers, out-of-range values, invalid syntax and strings that
would truncate are rejected before writing. Field metadata is public for future inspector use.
A custom KeyValue callback handles only keys absent from the field table; otherwise unknown keys fail.
Custom setters must validate before changing data and must not allocate entity-owned resources before
Spawn. There is no reflection code generator or new scene format.

Scene files still contain:

```text
entity "classname" {
  "key" "value"
}
```

Quoted values support escaped backslash, quote and newline. Comments begin with # or //.
The writer preserves explicitly supplied configuration, including subsequent EntityKeyValue edits;
it does not serialize arbitrary live simulation state or copy implicit defaults into the file.
Setting the same key replaces its source value. Failed loads can leave earlier complete entities in
the world, but never a partially configured instance. Replacing a scene destroys the old entities first.

## Optional application adapter

`GameplayProjectDefault()` and `GameplayApplication(&runtime, project)` connect a project to the core
application runner. Supply a class table, optional scene, context and whichever hooks are needed.
The adapter derives the aligned storage stride from the largest class. Capacity defaults to 1024 slots
and is configurable. It uses `config.fixed_dt` as the single source of gameplay timing; variable-only
applications continue to use core directly.

Order:

1. Allocate world, register classes, call project Init, then load the optional scene.
2. Each frame: FrameInput, optional UI build/capture, then zero or more updates.
3. Each update: project Update, registered systems, then entity Think dispatch exactly once.
4. Each draw: BeforeDraw, entity Draw, system Draw, AfterDraw, then the UI overlay.
5. Shutdown: destroy entities, reset systems, project Shutdown, free systems/world.

BeforeDraw/AfterDraw can scope a raylib camera. Project Init can load shared resources or register
systems before scene spawning. Shutdown releases those resources after entities are destroyed;
it is also called when project Init or scene loading fails. Adapter-managed systems must not step
the world themselves. Lower-level GameplayWorld and GameplaySystems APIs remain available when a
project needs to control sequencing directly.

## Migration from the earlier callbacks

Change callbacks to accept EntityContext; replace EntityData lookups with `e->data` and project globals
with `e->app`. Move default-value assignments out of Spawn into the class defaults value. Spawn now
returns true on success. Field declarations replace ordinary KeyValue parsing. Existing scene grammar,
handles and scheduling semantics remain the same. The updated gameplay_test exercises the migration.
