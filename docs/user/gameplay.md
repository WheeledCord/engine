# Gameplay, entities, and scenes

The optional gameplay layer gives a project a flat generational entity world, registered entity classes, typed fields, scheduled Think callbacks, systems, and scene load/save. It does not impose a transform, physics model, spatial index, or game policy.

Define an `EntityClass` with a payload type and callbacks, register it before spawning, and give it field declarations when scene data should configure it. Spawn receives fully applied defaults and properties. Use a handle, not a retained payload pointer, outside an entity callback.

See [GameplayWorld XML reference](../api/gameplay/GameplayWorld.xml) for the lifetime contract and `gameplay/README.md` for the full authoring example.
