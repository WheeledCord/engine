/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "gameplay/runtime.h"
#include "core/ui.h"
#include "core/transform.h"
#include "core/ui_containers.h"
#include "core/ui_layout.h"

typedef struct Mover
{
    Transform2D transform, previous;
    float speed;
} Mover;
static const Mover defaults = {.transform = {.translation = {360, 260}, .scale = {1, 1}},
                               .speed = 180};
static const InputBinding left[] = {{INPUT_KEY, KEY_A}, {INPUT_KEY, KEY_LEFT}};
static const InputBinding right[] = {{INPUT_KEY, KEY_D}, {INPUT_KEY, KEY_RIGHT}};
static const InputBinding up[] = {{INPUT_KEY, KEY_W}, {INPUT_KEY, KEY_UP}};
static const InputBinding down[] = {{INPUT_KEY, KEY_S}, {INPUT_KEY, KEY_DOWN}};

static bool Spawn(EntityContext *entity)
{
    Mover *self = entity->data;
    self->previous = self->transform;
    EntityThinkNext(entity);
    return true;
}

static void Think(EntityContext *entity)
{
    Mover *self = entity->data;
    Vector2 direction = InputVector(entity->input, (InputAction){left, 2}, (InputAction){right, 2},
                                    (InputAction){up, 2}, (InputAction){down, 2});
    self->previous = self->transform;
    float dt = (float)entity->dt;
    Transform2DMoveWorld(&self->transform, Vector2Scale(direction, self->speed * dt));
    float turn = (float)(entity->input->down[KEY_E] - entity->input->down[KEY_Q]);
    Transform2DRotate(&self->transform, turn * 2.0f * dt);
    if (entity->input->down[KEY_SPACE])
        Transform2DMoveLocal(&self->transform, (Vector2){self->speed * dt, 0});
    EntityThinkNext(entity);
}

static void Draw(EntityContext *entity)
{
    Mover *self = entity->data;
    Transform2D render = self->transform;
    render.translation = Vector2Lerp(self->previous.translation, render.translation, entity->alpha);
    render.rotation = self->previous.rotation +
                      AngleDelta(self->previous.rotation, render.rotation) * entity->alpha;
    DrawRectanglePro((Rectangle){render.translation.x, render.translation.y, 32, 32},
                     (Vector2){16, 16}, render.rotation * RAD2DEG, SKYBLUE);
    // The arrow points along local +X; Space follows it regardless of world orientation.
    DrawTriangle(Transform2DPoint(render, (Vector2){12, 0}),
                 Transform2DPoint(render, (Vector2){-6, -8}),
                 Transform2DPoint(render, (Vector2){-6, 8}), DARKBLUE);
}

static const EntityField fields[] = {
    {.name = "position",
     .type = ENTITY_VECTOR2,
     .offset = offsetof(Mover, transform) + offsetof(Transform2D, translation),
     .size = sizeof(Vector2)},
    {.name = "speed",
     .type = ENTITY_FLOAT,
     .offset = offsetof(Mover, speed),
     .size = sizeof(float),
     .ranged = true,
     .minimum = 0,
     .maximum = 2000}};
static const EntityClass classes[] = {{.classname = "mover",
                                       .size = sizeof(Mover),
                                       .defaults = &defaults,
                                       .fields = fields,
                                       .fieldCount = 2,
                                       .Spawn = Spawn,
                                       .Think = Think,
                                       .Draw = Draw}};

static void Instruction(UiContext *ui, UiRect rect, void *text) { UiLabel(ui, rect, text); }

static void Controls(UiContext *ui, UiRect content, void *text)
{
    UiLayout column = UiColumn(ui, UiRectInset(content, ui->theme.containerGap));
    UiIndent(ui, UiLayoutNext(ui, &column, 24), Instruction,
             "WASD/arrows move | Q/E turn | Space follows the arrow.");
    UiLayout row = UiRow(ui, UiLayoutNext(ui, &column, 24));
    UiTextField(ui, UiLayoutNext(ui, &row, 280), text, 64);
    if (UiButton(ui, UiLayoutNext(ui, &row, 90), "Clear"))
        ((char *)text)[0] = 0;
    UiIndent(ui, UiLayoutNext(ui, &column, 24), Instruction,
             "Click outside the field to move again. Close the window to quit.");
}

static void BuildUi(GameplayRuntime *runtime, UiContext *ui)
{
    int height = ui->theme.frameWidth * 2 + ui->theme.titleHeight + ui->theme.containerGap * 4 + 72;
    UiDrawWindow(ui, (UiRect){12, 12, 670, height}, "Movement", Controls, runtime->project.context);
}

EngineApplication EngineApplicationMain(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static GameplayRuntime runtime;
    static UiContext ui;
    static char text[64];
    GameplayProject project = GameplayProjectDefault();
    project.config.title = "Authoring demo";
    project.classes = classes;
    project.classCount = 1;
    project.scene = "projects/authoring_demo/start.scene";
    project.context = text;
    project.ui = &ui;
    project.BuildUi = BuildUi;
    return GameplayApplication(&runtime, project);
}
