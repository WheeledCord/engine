/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gameplay/entity.h"
#include "core/transform.h"
#include "gameplay/scene.h"
#include "gameplay/systems.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Counter
{
    int thinks;
    float period;
    char name[64];
} Counter;

typedef struct Test
{
    int failures;
    int systemUpdates;
} Test;

static void Check(Test *test, bool condition, const char *message)
{
    printf("%s: %s\n", message, condition ? "PASS" : "FAIL");
    if (!condition)
        test->failures++;
}

static const Counter counterDefaults = {.period = 1.0f};
static const EntityField counterFields[] = {
    ENTITY_FIELD(Counter, name, ENTITY_STRING),
    {.name = "period", .type = ENTITY_FLOAT, .offset = offsetof(Counter, period),
     .size = sizeof(float), .ranged = true, .minimum = 0.01, .maximum = 60}
};

static bool Spawn(EntityContext *entity)
{
    Counter *counter = entity->data;
    EntityThinkAfter(entity, counter->period);
    return true;
}

static void Think(EntityContext *entity)
{
    Counter *counter = entity->data;
    counter->thinks++;
    EntityThinkAfter(entity, counter->period);
}

static void SystemUpdate(void *context, GameplayWorld *world, double dt)
{
    Test *test = context;
    (void)dt;
    test->systemUpdates++;
    GameplayWorldStep(world);
}

static EntityHandle FindCounter(const GameplayWorld *world)
{
    for (size_t i = 0; i < world->maxEntities; i++)
    {
        EntityHandle entity = {(uint32_t)i, world->entities[i].generation};
        if (EntityAlive(world, entity) && !strcmp(EntityClassname(world, entity), "demo_counter"))
            return entity;
    }
    return ENTITY_NULL;
}

typedef struct Probe
{
    int value;
    Vector2 position;
    void *resource;
    double step, elapsed;
    bool sawKey;
} Probe;
static int destroyed;
static bool ProbeSpawn(EntityContext *e)
{
    Probe *p = e->data;
    p->resource = malloc(1);
    EntityThinkAfter(e, 0.3);
    return p->resource && p->value != 9;
}
static void ProbeDestroy(EntityContext *e)
{
    Probe *p = e->data;
    free(p->resource);
    destroyed++;
    EntityDestroy(e->world, e->entity); // Recursive destruction must not invoke us twice.
}
static void ProbeThink(EntityContext *e)
{
    Probe *p = e->data;
    p->step = e->dt;
    p->elapsed = e->elapsed;
    p->sawKey = e->input->down[KEY_W] && e->app != NULL;
    EntityThinkNext(e);
}
static void AuthoringChecks(Test *test)
{
    const Probe defaults = {.value = 1};
    const EntityField fields[] = {
        {.name = "value", .type = ENTITY_INT, .offset = offsetof(Probe, value),
         .size = sizeof(int), .ranged = true, .minimum = 0, .maximum = 9},
        ENTITY_FIELD(Probe, position, ENTITY_VECTOR2)
    };
    EntityClass type = {.classname = "probe", .size = sizeof(Probe), .defaults = &defaults,
        .fields = fields, .fieldCount = 2, .Spawn = ProbeSpawn, .Destroy = ProbeDestroy, .Think = ProbeThink};
    GameplayWorld world = {0};
    bool ok = GameplayWorldInit(&world, (GameplayWorldConfig){8, 0.1}) &&
              EntityRegister(&world, type);
    Check(test, ok, "authoring world setup");
    if (!ok) { GameplayWorldFree(&world); return; }
    world.context = test;
    EntityProperty props[] = {{"value", "7"}, {"position", "12 24"}};
    EntityHandle handle = EntitySpawnWith(&world, "probe", props, 2);
    Probe *p = EntityData(&world, handle);
    Check(test, p && p->value == 7 && p->position.y == 24, "properties override defaults before Spawn");
    if (p)
    {
        Check(test, !EntityKeyValue(&world, handle, "value", "10") && p->value == 7 &&
                    !EntityKeyValue(&world, handle, "position", "1 nan") && p->position.x == 12 &&
                    !EntityKeyValue(&world, handle, "unknown", "1"), "invalid properties reject without partial writes");
        Check(test, EntityKeyValue(&world, handle, "value", "5") && EntityKeyValueCount(&world, handle) == 2 &&
                    !strcmp(EntityKeyValueAt(&world, handle, 0)->value, "5"), "property edits update saved source values");
        GameplayWorldStep(&world);
        GameplayWorldStep(&world);
        EngineInput input = {0}; input.down[KEY_W] = true;
        GameplayWorldStepInput(&world, &input);
        Check(test, fabs(p->step - 0.1) < 1e-9 && fabs(p->elapsed - 0.3) < 1e-9 && p->sawKey,
              "Think receives step time, elapsed time, input and project context");
        GameplayWorldStep(&world);
        Check(test, fabs(p->elapsed - 0.1) < 1e-9 && !p->sawKey, "next-step scheduling and scoped input");
    }
    int before = destroyed;
    EntityProperty fail = {"value", "9"};
    EntityHandle failed = EntitySpawnWith(&world, "probe", &fail, 1);
    Check(test, !EntityAlive(&world, failed) && destroyed == before + 1, "failed Spawn cleans partial resources exactly once");
    Check(test, !EntityRegister(&world, (EntityClass){.classname = "late", .size = 1}),
          "registration cannot invalidate live class pointers");
    GameplayWorldClear(&world);
    Check(test, destroyed == before + 2 && !EntityAlive(&world, handle), "world clear destroys remaining entities");
    GameplayWorldFree(&world);

    EngineInput frame = {0}, pending = {0};
    frame.down[KEY_W] = frame.pressed[KEY_W] = true;
    frame.mouseDelta = (Vector2){2, 3};
    EngineInputRoute(&pending, &frame, (EngineInputCapture){0});
    frame.pressed[KEY_W] = false;
    EngineInputRoute(&pending, &frame, (EngineInputCapture){0});
    Check(test, pending.pressed[KEY_W] && pending.mouseDelta.x == 4, "input survives frames with no update");
    EngineInputDrain(&pending);
    Check(test, pending.down[KEY_W] && !pending.pressed[KEY_W] && pending.mouseDelta.x == 0,
          "first update drains edges and deltas, keeps held input");
    frame.pressed[KEY_W] = true;
    EngineInputRoute(&pending, &frame, (EngineInputCapture){true, true});
    Check(test, !pending.down[KEY_W] && !pending.pressed[KEY_W] && pending.mouseDelta.x == 0,
          "UI capture filters current and accumulated gameplay input");
    const InputBinding bindings[] = {{INPUT_KEY, KEY_W}, {INPUT_KEY, KEY_UP}};
    InputAction action = {bindings, 2};
    frame = (EngineInput){0}; frame.down[KEY_W] = true;
    frame.down[KEY_UP] = frame.pressed[KEY_UP] = true;
    Check(test, InputActionRead(&frame, action).down && !InputActionRead(&frame, action).pressed,
          "alternate binding does not retrigger an already-held action");
    frame = (EngineInput){0}; frame.pressed[KEY_W] = frame.released[KEY_W] = true;
    InputActionState tap = InputActionRead(&frame, action);
    Check(test, tap.pressed && tap.released && !tap.down, "short action tap survives between updates");
}

static void TransformChecks(Test *test)
{
    Transform t = TransformIdentity();
    t.scale = (Vector3){2, 3, -4};
    TransformRotateWorld(&t, (Vector3){0, 1, 0}, PI / 2);
    TransformMoveLocal(&t, (Vector3){0, 0, 5}); // forward is +Z
    Check(test, Vector3Distance(t.translation, (Vector3){5, 0, 0}) < 1e-5f,
          "local forward follows rotation without scaling movement speed");
    TransformMoveWorld(&t, (Vector3){0, 0, 2});
    Vector3 point = {1, 2, 3}, world = TransformPoint(t, point), local = {0};
    Check(test, TransformInversePoint(t, world, &local) && Vector3Distance(local, point) < 1e-5f &&
                Vector3Distance(world, Vector3Transform(point, TransformMatrix(t))) < 1e-5f,
          "3D point conversion and draw matrix agree with nonuniform mirrored scale");
    t.scale.x = 0; local = point;
    Check(test, !TransformInversePoint(t, world, &local) && Vector3Equals(local, point),
          "singular transform rejects inverse without writing output");
    Check(test, TransformLookAt(&t, Vector3Add(t.translation, (Vector3){2, 1, -3}), (Vector3){0, 1, 0}) &&
                Vector3Distance(TransformForward(t), Vector3Normalize((Vector3){2, 1, -3})) < 1e-5f,
          "look-at points the forward axis at the target");
    Quaternion previous = t.rotation;
    Check(test, !TransformLookAt(&t, t.translation, (Vector3){0, 1, 0}) && QuaternionEquals(previous, t.rotation),
          "coincident look target preserves orientation");
    Transform a = TransformIdentity(), b = TransformIdentity();
    TransformRotateWorld(&a, (Vector3){0, 1, 0}, PI / 2); b = a;
    TransformRotateLocal(&a, (Vector3){1, 0, 0}, PI / 2);
    TransformRotateWorld(&b, (Vector3){1, 0, 0}, PI / 2);
    Check(test, Vector3Distance(TransformForward(a), (Vector3){0, -1, 0}) < 1e-5f &&
                Vector3Distance(TransformForward(b), (Vector3){1, 0, 0}) < 1e-5f,
          "local and world rotation use their declared axes");
    Transform2D t2 = Transform2DIdentity();
    t2.scale = (Vector2){-2, 3};
    Transform2DRotate(&t2, PI / 2);
    Transform2DMoveLocal(&t2, (Vector2){5, 0});
    Vector2 p2 = {2, 3}, w2 = Transform2DPoint(t2, p2), back = {0};
    Check(test, Vector2Distance(t2.translation, (Vector2){0, 5}) < 1e-5f &&
                Transform2DInversePoint(t2, w2, &back) && Vector2Distance(back, p2) < 1e-5f &&
                Vector2Distance(w2, Vector2Transform(p2, Transform2DMatrix(t2))) < 1e-5f,
          "2D clockwise local motion, inverse and matrix agree");
    Check(test, Transform2DLookAt(&t2, Vector2Add(t2.translation, (Vector2){0, -2})) &&
                Vector2Distance(Transform2DDirection(t2, (Vector2){1, 0}), (Vector2){0, -1}) < 1e-5f,
          "2D look-at turns local positive X toward target");
    float angle = AngleMoveTowards(179 * DEG2RAD, -179 * DEG2RAD, DEG2RAD);
    Quaternion turn = QuaternionRotateTowards(QuaternionIdentity(), QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2), PI / 4);
    Check(test, fabsf(AngleDelta(angle, PI)) < 1e-5f && MoveTowards(0, 1, 2) == 1 &&
                fabsf(Vector3Angle((Vector3){0, 0, -1}, Vector3RotateByQuaternion((Vector3){0, 0, -1}, turn)) - PI / 4) < 1e-5f,
          "bounded movement and turning take shortest paths without overshoot");
}

int main(void)
{
    Test test = {0};
    GameplayWorld world = {0};
    GameplaySystems systems = {0};
    bool started = GameplayWorldInit(&world, (GameplayWorldConfig){8, 0.1}) &&
                   EntityRegister(&world, (EntityClass){.classname = "demo_counter", .size = sizeof(Counter),
                       .defaults = &counterDefaults, .fields = counterFields,
                       .fieldCount = sizeof counterFields / sizeof counterFields[0], .Spawn = Spawn, .Think = Think}) &&
                   GameplaySystemsAdd(&systems, (GameplaySystem){"world", &test, SystemUpdate, NULL, NULL}) &&
                   GameplaySceneLoad(&world, "projects/gameplay_test/assets/demo.scene", true);
    Check(&test, started, "gameplay setup and scene load");
    if (started)
    {
        EntityHandle first = FindCounter(&world);
        Counter *counter = EntityData(&world, first);
        Check(&test,
              counter && !strcmp(counter->name, "scene counter") && fabsf(counter->period - 0.2f) < 0.001f,
              "classname KeyValue configures flat entity payload");

        EntityHandle stale = EntitySpawn(&world, "demo_counter");
        Check(&test, EntityDestroy(&world, stale) && !EntityAlive(&world, stale),
              "destroy invalidates generational handle");
        EntityHandle reused = EntitySpawn(&world, "demo_counter");
        Check(&test, reused.index == stale.index && reused.generation != stale.generation,
              "reused slot receives a new generation");
        EntityDestroy(&world, reused);

        for (int i = 0; i < 10; i++)
            GameplaySystemsUpdate(&systems, &world, 0.1);
        counter = EntityData(&world, first);
        Check(&test, test.systemUpdates == 10 && counter && counter->thinks == 5,
              "system registry advances due Think only when scheduled");

        Check(&test, GameplaySceneWrite(&world, "build/core/gameplay-roundtrip.scene"),
              "scene writer preserves source keyvalues");
        Check(&test, GameplaySceneLoad(&world, "build/core/gameplay-roundtrip.scene", true),
              "written scene reloads through classname registry");
    }
    GameplaySystemsFree(&systems);
    GameplayWorldFree(&world);
    AuthoringChecks(&test);
    TransformChecks(&test);
    printf("GAMEPLAY TEST failures=%d\n", test.failures);
    return test.failures ? 1 : 0;
}
