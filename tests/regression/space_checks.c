/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Input routing between frames and updates, 3D and 2D transforms, the 2D collision queries and the
// 2D camera: core contracts with no window of their own.
#include "checks.h"
#include "core/camera2d.h"
#include "core/collision2d.h"
#include "core/engine.h"
#include "core/transform.h"
#include "raymath.h"
#include <math.h>
#include <stdio.h>

static int failures;

static void Check(bool ok, const char *what)
{
    if (!ok)
        printf("FAIL: %s\n", what);
    failures += !ok;
}

static void InputRoutingChecks(void)
{
    EngineInput frame = {0}, pending = {0};
    frame.down[KEY_W] = frame.pressed[KEY_W] = true;
    frame.mouseDelta = (Vector2){2, 3};
    EngineInputRoute(&pending, &frame, (EngineInputCapture){0});
    frame.pressed[KEY_W] = false;
    EngineInputRoute(&pending, &frame, (EngineInputCapture){0});
    Check(pending.pressed[KEY_W] && pending.mouseDelta.x == 4, "input survives frames with no update");
    EngineInputDrain(&pending);
    Check(pending.down[KEY_W] && !pending.pressed[KEY_W] && pending.mouseDelta.x == 0,
          "first update drains edges and deltas, keeps held input");
    frame.pressed[KEY_W] = true;
    EngineInputRoute(&pending, &frame, (EngineInputCapture){true, true});
    Check(!pending.down[KEY_W] && !pending.pressed[KEY_W] && pending.mouseDelta.x == 0,
          "UI capture filters current and accumulated gameplay input");
    const InputBinding bindings[] = {{INPUT_KEY, KEY_W}, {INPUT_KEY, KEY_UP}};
    InputAction action = {bindings, 2};
    frame = (EngineInput){0};
    frame.down[KEY_W] = true;
    frame.down[KEY_UP] = frame.pressed[KEY_UP] = true;
    Check(InputActionRead(&frame, action).down && !InputActionRead(&frame, action).pressed,
          "alternate binding does not retrigger an already-held action");
    frame = (EngineInput){0};
    frame.pressed[KEY_W] = frame.released[KEY_W] = true;
    InputActionState tap = InputActionRead(&frame, action);
    Check(tap.pressed && tap.released && !tap.down, "short action tap survives between updates");
}

static void TransformChecks(void)
{
    Transform t = TransformIdentity();
    t.scale = (Vector3){2, 3, -4};
    TransformRotateWorld(&t, (Vector3){0, 1, 0}, PI / 2);
    TransformMoveLocal(&t, (Vector3){0, 0, 5}); // forward is +Z
    Check(Vector3Distance(t.translation, (Vector3){5, 0, 0}) < 1e-5f,
          "local forward follows rotation without scaling movement speed");
    TransformMoveWorld(&t, (Vector3){0, 0, 2});
    Vector3 point = {1, 2, 3}, world = TransformPoint(t, point), local = {0};
    Check(TransformInversePoint(t, world, &local) && Vector3Distance(local, point) < 1e-5f &&
              Vector3Distance(world, Vector3Transform(point, TransformMatrix(t))) < 1e-5f,
          "3D point conversion and draw matrix agree with nonuniform mirrored scale");
    t.scale.x = 0;
    local = point;
    Check(!TransformInversePoint(t, world, &local) && Vector3Equals(local, point),
          "singular transform rejects inverse without writing output");
    Check(TransformLookAt(&t, Vector3Add(t.translation, (Vector3){2, 1, -3}), (Vector3){0, 1, 0}) &&
              Vector3Distance(TransformForward(t), Vector3Normalize((Vector3){2, 1, -3})) < 1e-5f,
          "look-at points the forward axis at the target");
    Quaternion previous = t.rotation;
    Check(!TransformLookAt(&t, t.translation, (Vector3){0, 1, 0}) && QuaternionEquals(previous, t.rotation),
          "coincident look target preserves orientation");
    Transform a = TransformIdentity(), b = TransformIdentity();
    TransformRotateWorld(&a, (Vector3){0, 1, 0}, PI / 2);
    b = a;
    TransformRotateLocal(&a, (Vector3){1, 0, 0}, PI / 2);
    TransformRotateWorld(&b, (Vector3){1, 0, 0}, PI / 2);
    Check(Vector3Distance(TransformForward(a), (Vector3){0, -1, 0}) < 1e-5f &&
              Vector3Distance(TransformForward(b), (Vector3){1, 0, 0}) < 1e-5f,
          "local and world rotation use their declared axes");
    Transform2D t2 = Transform2DIdentity();
    t2.scale = (Vector2){-2, 3};
    Transform2DRotate(&t2, PI / 2);
    Transform2DMoveLocal(&t2, (Vector2){5, 0});
    Vector2 p2 = {2, 3}, w2 = Transform2DPoint(t2, p2), back = {0};
    Check(Vector2Distance(t2.translation, (Vector2){0, 5}) < 1e-5f &&
              Transform2DInversePoint(t2, w2, &back) && Vector2Distance(back, p2) < 1e-5f &&
              Vector2Distance(w2, Vector2Transform(p2, Transform2DMatrix(t2))) < 1e-5f,
          "2D clockwise local motion, inverse and matrix agree");
    Check(Transform2DLookAt(&t2, Vector2Add(t2.translation, (Vector2){0, -2})) &&
              Vector2Distance(Transform2DDirection(t2, (Vector2){1, 0}), (Vector2){0, -1}) < 1e-5f,
          "2D look-at turns local positive X toward target");
    float angle = AngleMoveTowards(179 * DEG2RAD, -179 * DEG2RAD, DEG2RAD);
    Quaternion turn = QuaternionRotateTowards(QuaternionIdentity(),
                                              QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2), PI / 4);
    Check(fabsf(AngleDelta(angle, PI)) < 1e-5f && MoveTowards(0, 1, 2) == 1 &&
              fabsf(Vector3Angle((Vector3){0, 0, -1}, Vector3RotateByQuaternion((Vector3){0, 0, -1}, turn)) -
                    PI / 4) < 1e-5f,
          "bounded movement and turning take shortest paths without overshoot");
}

static void QueryAndCameraChecks(void)
{
    Collision2DWorld queries = {0};
    Collision2DShape circle = {COLLISION2D_CIRCLE, {4, 0}, {0, 0}, 1};
    Collision2DShape box = {COLLISION2D_AABB, {10, 0}, {2, 2}, 0};
    Collision2DFilter player = {1u, 2u};
    Collision2DFilter enemy = {2u, 1u};
    Collision2DHit hits[4] = {0};
    bool ready = Collision2DWorldInit(&queries, 4, 2) && Collision2DShouldCollide(player, enemy) &&
                 Collision2DOverlaps(circle, (Collision2DShape){COLLISION2D_AABB, {4, 0}, {1, 1}, 0});
    Collision2DHandle first = ready ? Collision2DWorldAdd(&queries, circle, enemy, (void *)"circle") : COLLISION2D_NULL;
    Collision2DHandle second = ready ? Collision2DWorldAdd(&queries, box, enemy, (void *)"box") : COLLISION2D_NULL;
    Collision2DHandle corner =
        ready ? Collision2DWorldAdd(&queries, (Collision2DShape){COLLISION2D_AABB, {10, 10}, {1, 1}, 0}, enemy,
                                    (void *)"corner")
              : COLLISION2D_NULL;
    Check(ready && first.index != UINT32_MAX && second.index != UINT32_MAX && corner.index != UINT32_MAX,
          "collision world accepts optional circle and AABB proxies");
    Check(Collision2DQueryCircle(&queries, (Vector2){4, 0}, 1, 2u, hits, 4) == 1 &&
              hits[0].handle.index == first.index && !Collision2DShouldCollide(player, (Collision2DFilter){2u, 0}),
          "collision queries use exact overlap and explicit layers and masks");
    Collision2DSweep sweep = {0};
    Check(Collision2DSweepCircle(&queries, (Vector2){0, 0}, .5f, (Vector2){12, 0}, 2u, &sweep) &&
              sweep.hit.handle.index == first.index && fabsf(sweep.fraction - .2083333f) < .001f &&
              sweep.normal.x < -.9f,
          "circle sweep returns the nearest standard query hit");
    Check(Collision2DSweepCircle(&queries, (Vector2){6, 6}, .5f, (Vector2){6, 6}, 2u, &sweep) &&
              sweep.hit.handle.index == corner.index && fabsf(sweep.fraction - .4410744f) < .001f,
          "circle sweep handles rounded AABB corners without an early box hit");
    Check(Collision2DWorldSetShape(&queries, first, (Collision2DShape){COLLISION2D_CIRCLE, {30, 0}, {0, 0}, 1}) &&
              Collision2DQueryCircle(&queries, (Vector2){4, 0}, 1, 2u, hits, 4) == 0,
          "moving a proxy updates the lazily rebuilt spatial hash");
    Check(Collision2DWorldRemove(&queries, first) &&
              Collision2DQueryAabb(&queries, Collision2DAabbMake((Vector2){4, 0}, (Vector2){2, 2}), 2u, hits, 4) == 0,
          "collision removal rebuilds the spatial hash and invalidates handles");
    Collision2DWorldFree(&queries);

    CoreCamera2D camera = CoreCamera2DDefault();
    camera.zoom = 2;
    camera.bounds = (Rectangle){0, 0, 100, 100};
    camera.clampBounds = true;
    CoreCamera2DSetViewport(&camera, (Vector2){40, 20});
    CoreCamera2DFollow(&camera, (Vector2){-10, 120}, 1);
    Vector2 screen = CoreCamera2DWorldToScreen(&camera, 1, camera.position);
    Check(Vector2Distance(camera.position, (Vector2){10, 95}) < 1e-5f &&
              Vector2Distance(screen, (Vector2){20, 10}) < 1e-5f &&
              Vector2Distance(CoreCamera2DScreenToWorld(&camera, 1, screen), camera.position) < 1e-5f,
          "camera convention clamps bounds and round-trips world and screen space");
    camera.followRate = 3;
    CoreCamera2DFollow(&camera, (Vector2){50, 50}, 1.0f / 3.0f);
    Check(camera.position.x > camera.previous.x && camera.position.x < 50 &&
              CoreCamera2DInterpolated(&camera, .5f).x > camera.previous.x,
          "camera follow smoothing keeps interpolation opt-in and deterministic");
}

int SpaceChecks(void)
{
    failures = 0;
    InputRoutingChecks();
    TransformChecks();
    QueryAndCameraChecks();
    return failures;
}
