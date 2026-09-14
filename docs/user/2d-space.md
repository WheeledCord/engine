# 2D space, camera, and collision queries

`core/transform.h`, `core/camera2d.h`, and `core/collision2d.h` are independent, opt-in tools. They do not add a mandatory entity component or physics simulation.

## Space convention

Use `Transform2D` for an object's local-to-world work. Its world convention is screen-like: positive X is right and positive Y is down. Store the transform in the project payload that needs it; do not duplicate it in an engine-owned entity type.

For a camera, keep one `CoreCamera2D` in project state. Set its viewport whenever the drawable size changes, update it from the fixed simulation, and use `alpha` passed to `Draw` for display conversion:

```c
CoreCamera2D camera = CoreCamera2DDefault();
camera.viewport = (Vector2){960, 540};
camera.zoom = 2.0f;
camera.followRate = 8.0f;              // zero means snap to the target
camera.bounds = (Rectangle){0, 0, 1600, 900};
camera.clampBounds = true;

/* Update: camera.previous is preserved for Draw interpolation. */
CoreCamera2DFollow(&state->camera, player->transform.translation, (float)dt);

/* Draw: draw sprites at CoreCamera2DWorldToScreen(&state->camera, alpha, world). */
Vector2 mouseWorld = CoreCamera2DScreenToWorld(&state->camera, alpha, GetMousePosition());
```

The camera position is its world-space centre. Bounds are clamped against the visible area at the chosen zoom; when bounds are smaller than the view, the camera centres them. `CoreCamera2DWorldToScreen` and `CoreCamera2DScreenToWorld` round-trip under the same camera and `alpha`.

## Collision queries, not physics

Make one `Collision2DWorld` for a gameplay domain that needs nearby-object queries. Register plain circles or AABBs with a project-owned pointer. Update a proxy whenever its owner moves, query it, then decide movement response in game code.

```c
enum { LAYER_PLAYER = 1u << 0, LAYER_ENEMY = 1u << 1, LAYER_WALL = 1u << 2 };

Collision2DWorldInit(&state->queries, 256, 64.0f);
Collision2DHandle player = Collision2DWorldAdd(&state->queries,
    (Collision2DShape){COLLISION2D_CIRCLE, spawn, {0, 0}, 12},
    (Collision2DFilter){LAYER_PLAYER, LAYER_ENEMY | LAYER_WALL}, playerData);

Collision2DWorldSetShape(&state->queries, player, movedShape);
Collision2DHit hits[16];
int touching = Collision2DQueryCircle(&state->queries, movedShape.center, movedShape.radius,
                                      LAYER_ENEMY, hits, 16);

Collision2DSweep hit;
if (Collision2DSweepCircle(&state->queries, movedShape.center, movedShape.radius, desiredMove,
                           LAYER_WALL, &hit))
    desiredMove = Vector2Scale(desiredMove, hit.fraction);
```

Layer/mask filtering follows the familiar symmetric rule for two objects: both `a.layer & b.mask` and `b.layer & a.mask` must be nonzero. A query has only an accepted-layer `mask`, so it is appropriate for questions such as “which enemies overlap this attack?” The broad phase rebuilds lazily after add, remove, or shape changes; query results are exact for circle/AABB overlap. Circle sweeps select the nearest candidate and are intentionally only a movement query, never a movement response.

Call `Collision2DWorldRemove` when an owner dies and `Collision2DWorldFree` when the project state ends. Do not retain a collision handle after removal; its generation deliberately makes stale handles invalid.

See [CoreCamera2D](../api/core/CoreCamera2D.xml) and [Collision2DWorld](../api/core/Collision2DWorld.xml) for exact API contracts.
