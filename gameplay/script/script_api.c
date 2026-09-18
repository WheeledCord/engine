/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/iso_grid.h"
#include "script.h"

#include "core/camera2d.h"
#include "core/collision2d.h"
#include "core/audio.h"
#include "core/file.h"
#include "gameplay/iso_move.h"
#include "gameplay/scene.h"
#include "raymath.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The implementations behind the table. Each one is written once, against ScriptValue, and every
   language gets it. The signature comes from the same macro that declares the row, so a body cannot
   drift from what the table says it is. */

#define SCRIPT_BODY(id) static ScriptValue Script_##id(ScriptHost *host, const ScriptValue *a)
#define UNUSED_ARGUMENTS (void)a

// Prototypes, straight from the table.
#define SCRIPT_BINDING(id, name, result, help, types) SCRIPT_BODY(id);
#include "script_api.def"
#undef SCRIPT_BINDING

// ---- reaching the entity whose callback is running -----------------------------------------------
static ScriptEntity *Self(ScriptHost *host)
{
    return host && host->current ? (ScriptEntity *)host->current->data : NULL;
}

static ScriptClass *SelfClass(ScriptHost *host)
{
    if (!host || !host->current)
        return NULL;
    return ScriptHostClass(host, EntityClassname(host->current->world, host->current->entity));
}

static ScriptValue *Slot(ScriptHost *host, const char *field)
{
    ScriptClass *type = SelfClass(host);
    ScriptEntity *self = Self(host);
    if (!type || !self || !field)
        return NULL;
    for (size_t i = 0; i < type->slotCount; i++)
        if (!strcmp(type->fieldNames[i], field))
            return &self->slots[i];
    return NULL;
}

/* A handle may name a C entity too. Cross-entity script access is deliberately limited to a
   scripted class, whose field storage and transform are the public script contract. */
static ScriptEntity *EntityScript(ScriptHost *host, EntityHandle entity, ScriptClass **type)
{
    if (type)
        *type = NULL;
    if (!host || !EntityAlive(host->world, entity))
        return NULL;
    ScriptClass *found = ScriptHostClass(host, EntityClassname(host->world, entity));
    if (!found)
        return NULL;
    if (type)
        *type = found;
    return EntityData(host->world, entity);
}

static ScriptValue *EntitySlot(ScriptHost *host, EntityHandle entity, const char *field)
{
    ScriptClass *type = NULL;
    ScriptEntity *data = EntityScript(host, entity, &type);
    if (!data || !field)
        return NULL;
    for (size_t i = 0; i < type->slotCount; i++)
        if (!strcmp(type->fieldNames[i], field))
            return &data->slots[i];
    return NULL;
}

static Color Unpack(int rgba)
{
    return (Color){(unsigned char)((rgba >> 24) & 0xff), (unsigned char)((rgba >> 16) & 0xff),
                   (unsigned char)((rgba >> 8) & 0xff), (unsigned char)(rgba & 0xff)};
}

// ---- talking to the engine -----------------------------------------------------------------------
SCRIPT_BODY(log)
{
    (void)host;
    // A script printing is the script talking to whoever is running it, so it is not the engine's
    // log and does not disappear when the engine is asked to be quiet.
    printf("%s\n", a[0].as.string);
    fflush(stdout);
    return ScriptNone();
}

SCRIPT_BODY(rgba)
{
    (void)host;
    int r = a[0].as.integer & 0xff, g = a[1].as.integer & 0xff;
    int b = a[2].as.integer & 0xff, alpha = a[3].as.integer & 0xff;
    return ScriptInt((int)(((unsigned)r << 24) | ((unsigned)g << 16) | ((unsigned)b << 8) | (unsigned)alpha));
}

// ---- declaring classes ----------------------------------------------------------------------------
SCRIPT_BODY(class_new)
{
    if (!host || host->classCount == SCRIPT_CLASS_CAPACITY || ScriptHostClass(host, a[0].as.string))
        return ScriptBool(false);
    ScriptClass *type = &host->classes[host->classCount++];
    *type = (ScriptClass){0};
    snprintf(type->name, sizeof type->name, "%s", a[0].as.string);
    // Every scripted entity has these two, so a scene can place one without the script saying so.
    type->fields[0] = (EntityField){.name = "position",
                                    .type = ENTITY_VECTOR2,
                                    .offset = offsetof(ScriptEntity, transform.translation),
                                    .size = sizeof(Vector2)};
    type->fields[1] = (EntityField){.name = "rotation",
                                    .type = ENTITY_FLOAT,
                                    .offset = offsetof(ScriptEntity, transform.rotation),
                                    .size = sizeof(float)};
    type->fieldCount = 2;
    return ScriptBool(true);
}

SCRIPT_BODY(class_field)
{
    ScriptClass *type = ScriptHostClass(host, a[0].as.string);
    if (!type || type->registered || type->slotCount == SCRIPT_CLASS_FIELDS)
        return ScriptBool(false);
    size_t slot = type->slotCount;
    const char *kind = a[2].as.string;
    EntityFieldType fieldType;
    ScriptType slotType;
    size_t size;
    if (!strcmp(kind, "float"))
    {
        fieldType = ENTITY_FLOAT;
        slotType = SCRIPT_FLOAT;
        size = sizeof(float);
    }
    else if (!strcmp(kind, "vector2"))
    {
        fieldType = ENTITY_VECTOR2;
        slotType = SCRIPT_VECTOR2;
        size = sizeof(Vector2);
    }
    else if (!strcmp(kind, "int"))
    {
        fieldType = ENTITY_INT;
        slotType = SCRIPT_INT;
        size = sizeof(int);
    }
    else if (!strcmp(kind, "bool"))
    {
        fieldType = ENTITY_BOOL;
        slotType = SCRIPT_BOOL;
        size = sizeof(bool);
    }
    else
        return ScriptBool(false);
    snprintf(type->fieldNames[slot], sizeof type->fieldNames[slot], "%s", a[1].as.string);
    // A scene writes straight into the slot's value, so the tag is set in the defaults beforehand.
    type->defaults.slots[slot].type = slotType;
    type->fields[type->fieldCount++] =
        (EntityField){.name = type->fieldNames[slot],
                      .type = fieldType,
                      .offset = offsetof(ScriptEntity, slots) + slot * sizeof(ScriptValue) +
                                offsetof(ScriptValue, as),
                      .size = size};
    type->slotCount++;
    return ScriptBool(true);
}

SCRIPT_BODY(class_on)
{
    ScriptClass *type = ScriptHostClass(host, a[0].as.string);
    if (!type || type->registered)
        return ScriptBool(false);
    const char *event = a[1].as.string;
    char *slot = !strcmp(event, "spawn")     ? type->spawn
                 : !strcmp(event, "think")   ? type->think
                 : !strcmp(event, "draw")    ? type->draw
                 : !strcmp(event, "destroy") ? type->destroy
                                             : NULL;
    if (!slot)
        return ScriptBool(false);
    snprintf(slot, SCRIPT_NAME_CAPACITY, "%s", a[2].as.string);
    return ScriptBool(true);
}

SCRIPT_BODY(class_done)
{
    ScriptClass *type = ScriptHostClass(host, a[0].as.string);
    return ScriptBool(type && ScriptClassRegister(host, type));
}

// ---- entities ---------------------------------------------------------------------------------------
SCRIPT_BODY(self)
{
    UNUSED_ARGUMENTS;
    return ScriptHandle(host && host->current ? ScriptHostIdOf(host->current->entity) : 0);
}

SCRIPT_BODY(spawn)
{
    if (!host)
        return ScriptHandle(0);
    char position[64];
    snprintf(position, sizeof position, "%.9g %.9g", (double)a[1].as.vector2.x,
             (double)a[1].as.vector2.y);
    EntityProperty properties[] = {{"position", position}};
    EntityHandle spawned = EntitySpawnWith(host->world, a[0].as.string, properties, 1);
    return ScriptHandle(ScriptHostIdOf(spawned));
}

SCRIPT_BODY(destroy)
{
    return ScriptBool(host && EntityDestroy(host->world, ScriptHostHandleOf(host, a[0].as.entity)));
}

SCRIPT_BODY(alive)
{
    return ScriptBool(host && EntityAlive(host->world, ScriptHostHandleOf(host, a[0].as.entity)));
}

SCRIPT_BODY(classname)
{
    const char *name = host ? EntityClassname(host->world, ScriptHostHandleOf(host, a[0].as.entity)) : NULL;
    return ScriptString(name ? name : "");
}

SCRIPT_BODY(find_first)
{
    return ScriptHandle(host ? ScriptHostIdOf(EntityFirst(host->world, a[0].as.string)) : 0);
}

SCRIPT_BODY(find_next)
{
    if (!host)
        return ScriptHandle(0);
    EntityHandle after = ScriptHostHandleOf(host, a[0].as.entity);
    return ScriptHandle(ScriptHostIdOf(EntityNext(host->world, after, a[1].as.string)));
}

SCRIPT_BODY(entity_position)
{
    ScriptEntity *target = EntityScript(host, ScriptHostHandleOf(host, a[0].as.entity), NULL);
    return ScriptVector2(target ? target->transform.translation : (Vector2){0, 0});
}

SCRIPT_BODY(set_entity_position)
{
    ScriptEntity *target = EntityScript(host, ScriptHostHandleOf(host, a[0].as.entity), NULL);
    if (target)
        target->transform.translation = a[1].as.vector2;
    return ScriptNone();
}

SCRIPT_BODY(think_next)
{
    UNUSED_ARGUMENTS;
    if (host && host->current)
        EntityThinkNext(host->current);
    return ScriptNone();
}

SCRIPT_BODY(think_after)
{
    if (host && host->current)
        EntityThinkAfter(host->current, a[0].as.number);
    return ScriptNone();
}

// ---- fields ------------------------------------------------------------------------------------------
SCRIPT_BODY(get_number)
{
    ScriptValue *slot = Slot(host, a[0].as.string);
    if (!slot)
        return ScriptFloat(0);
    if (slot->type == SCRIPT_INT)
        return ScriptFloat((float)slot->as.integer);
    if (slot->type == SCRIPT_BOOL)
        return ScriptFloat(slot->as.boolean ? 1.0f : 0.0f);
    return ScriptFloat(slot->as.number);
}

SCRIPT_BODY(set_number)
{
    ScriptValue *slot = Slot(host, a[0].as.string);
    if (!slot)
        return ScriptNone();
    if (slot->type == SCRIPT_INT)
        slot->as.integer = (int)a[1].as.number;
    else if (slot->type == SCRIPT_BOOL)
        slot->as.boolean = a[1].as.number != 0;
    else
        slot->as.number = a[1].as.number;
    return ScriptNone();
}

SCRIPT_BODY(get_vector)
{
    ScriptValue *slot = Slot(host, a[0].as.string);
    return ScriptVector2(slot && slot->type == SCRIPT_VECTOR2 ? slot->as.vector2 : (Vector2){0, 0});
}

SCRIPT_BODY(set_vector)
{
    ScriptValue *slot = Slot(host, a[0].as.string);
    if (slot && slot->type == SCRIPT_VECTOR2)
        slot->as.vector2 = a[1].as.vector2;
    return ScriptNone();
}

SCRIPT_BODY(entity_get_number)
{
    ScriptValue *slot = EntitySlot(host, ScriptHostHandleOf(host, a[0].as.entity), a[1].as.string);
    if (!slot)
        return ScriptFloat(0);
    if (slot->type == SCRIPT_INT)
        return ScriptFloat((float)slot->as.integer);
    if (slot->type == SCRIPT_BOOL)
        return ScriptFloat(slot->as.boolean ? 1.0f : 0.0f);
    return ScriptFloat(slot->type == SCRIPT_FLOAT ? slot->as.number : 0);
}

SCRIPT_BODY(entity_set_number)
{
    ScriptValue *slot = EntitySlot(host, ScriptHostHandleOf(host, a[0].as.entity), a[1].as.string);
    if (slot && slot->type == SCRIPT_INT)
        slot->as.integer = (int)a[2].as.number;
    else if (slot && slot->type == SCRIPT_BOOL)
        slot->as.boolean = a[2].as.number != 0;
    else if (slot && slot->type == SCRIPT_FLOAT)
        slot->as.number = a[2].as.number;
    return ScriptNone();
}

SCRIPT_BODY(entity_get_vector)
{
    ScriptValue *slot = EntitySlot(host, ScriptHostHandleOf(host, a[0].as.entity), a[1].as.string);
    return ScriptVector2(slot && slot->type == SCRIPT_VECTOR2 ? slot->as.vector2 : (Vector2){0, 0});
}

SCRIPT_BODY(entity_set_vector)
{
    ScriptValue *slot = EntitySlot(host, ScriptHostHandleOf(host, a[0].as.entity), a[1].as.string);
    if (slot && slot->type == SCRIPT_VECTOR2)
        slot->as.vector2 = a[2].as.vector2;
    return ScriptNone();
}

// ---- the transform ------------------------------------------------------------------------------------
SCRIPT_BODY(position)
{
    UNUSED_ARGUMENTS;
    ScriptEntity *self = Self(host);
    return ScriptVector2(self ? self->transform.translation : (Vector2){0, 0});
}

SCRIPT_BODY(set_position)
{
    ScriptEntity *self = Self(host);
    if (self)
        self->transform.translation = a[0].as.vector2;
    return ScriptNone();
}

SCRIPT_BODY(rotation)
{
    UNUSED_ARGUMENTS;
    ScriptEntity *self = Self(host);
    return ScriptFloat(self ? self->transform.rotation : 0);
}

SCRIPT_BODY(rotate)
{
    ScriptEntity *self = Self(host);
    if (self)
        Transform2DRotate(&self->transform, a[0].as.number);
    return ScriptNone();
}

SCRIPT_BODY(move_world)
{
    ScriptEntity *self = Self(host);
    if (self)
        Transform2DMoveWorld(&self->transform, a[0].as.vector2);
    return ScriptNone();
}

SCRIPT_BODY(move_local)
{
    ScriptEntity *self = Self(host);
    if (self)
        Transform2DMoveLocal(&self->transform, a[0].as.vector2);
    return ScriptNone();
}

SCRIPT_BODY(look_at)
{
    ScriptEntity *self = Self(host);
    return ScriptBool(self && Transform2DLookAt(&self->transform, a[0].as.vector2));
}

SCRIPT_BODY(interpolated)
{
    UNUSED_ARGUMENTS;
    ScriptEntity *self = Self(host);
    if (!self)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(
        Vector2Lerp(self->previous.translation, self->transform.translation, host->alpha));
}

SCRIPT_BODY(interpolated_rotation)
{
    UNUSED_ARGUMENTS;
    ScriptEntity *self = Self(host);
    if (!self)
        return ScriptFloat(0);
    return ScriptFloat(self->previous.rotation +
                       AngleDelta(self->previous.rotation, self->transform.rotation) * host->alpha);
}

SCRIPT_BODY(to_local)
{
    ScriptEntity *self = Self(host);
    if (!self)
        return ScriptVector2(a[0].as.vector2);
    // Drawn where the entity is now, so a shape follows the same interpolation its body does.
    Transform2D render = self->transform;
    render.translation = Vector2Lerp(self->previous.translation, render.translation, host->alpha);
    render.rotation = self->previous.rotation +
                      AngleDelta(self->previous.rotation, render.rotation) * host->alpha;
    return ScriptVector2(Transform2DPoint(render, a[0].as.vector2));
}

// ---- input ---------------------------------------------------------------------------------------------
static const EngineInput *Input(ScriptHost *host)
{
    static const EngineInput empty;
    return host && host->current && host->current->input ? host->current->input : &empty;
}

SCRIPT_BODY(input_vector)
{
    UNUSED_ARGUMENTS;
    static const InputBinding left[] = {{INPUT_KEY, KEY_A}, {INPUT_KEY, KEY_LEFT}};
    static const InputBinding right[] = {{INPUT_KEY, KEY_D}, {INPUT_KEY, KEY_RIGHT}};
    static const InputBinding up[] = {{INPUT_KEY, KEY_W}, {INPUT_KEY, KEY_UP}};
    static const InputBinding down[] = {{INPUT_KEY, KEY_S}, {INPUT_KEY, KEY_DOWN}};
    return ScriptVector2(InputVector(Input(host), (InputAction){left, 2}, (InputAction){right, 2},
                                     (InputAction){up, 2}, (InputAction){down, 2}));
}

SCRIPT_BODY(key_down)
{
    int key = a[0].as.integer;
    const EngineInput *input = Input(host);
    return ScriptBool(key > 0 && key < CORE_KEY_COUNT && input->down[key]);
}

SCRIPT_BODY(key_pressed)
{
    int key = a[0].as.integer;
    const EngineInput *input = Input(host);
    return ScriptBool(key > 0 && key < CORE_KEY_COUNT && input->pressed[key]);
}

SCRIPT_BODY(mouse_position)
{
    UNUSED_ARGUMENTS;
    return ScriptVector2(Input(host)->mousePosition);
}

SCRIPT_BODY(mouse_delta)
{
    UNUSED_ARGUMENTS;
    return ScriptVector2(Input(host)->mouseDelta);
}

SCRIPT_BODY(mouse_wheel)
{
    UNUSED_ARGUMENTS;
    return ScriptFloat(Input(host)->wheel);
}

SCRIPT_BODY(mouse_down)
{
    int button = a[0].as.integer;
    const EngineInput *input = Input(host);
    return ScriptBool(button >= 0 && button < CORE_MOUSE_BUTTON_COUNT && input->mouseDown[button]);
}

SCRIPT_BODY(mouse_pressed)
{
    int button = a[0].as.integer;
    const EngineInput *input = Input(host);
    return ScriptBool(button >= 0 && button < CORE_MOUSE_BUTTON_COUNT && input->mousePressed[button]);
}

SCRIPT_BODY(key_code)
{
    (void)host;
    static const struct
    {
        const char *name;
        int key;
    } keys[] = {{"a", KEY_A},         {"b", KEY_B},        {"c", KEY_C},
                {"d", KEY_D},         {"e", KEY_E},        {"q", KEY_Q},
                {"r", KEY_R},         {"s", KEY_S},        {"w", KEY_W},
                {"x", KEY_X},         {"z", KEY_Z},        {"space", KEY_SPACE},
                {"enter", KEY_ENTER}, {"escape", KEY_ESCAPE}, {"left", KEY_LEFT},
                {"right", KEY_RIGHT}, {"up", KEY_UP},      {"down", KEY_DOWN},
                {"shift", KEY_LEFT_SHIFT}, {"control", KEY_LEFT_CONTROL}, {"tab", KEY_TAB}};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (!strcmp(keys[i].name, a[0].as.string))
            return ScriptInt(keys[i].key);
    return ScriptInt(0);
}

// ---- time -----------------------------------------------------------------------------------------------
SCRIPT_BODY(step)
{
    UNUSED_ARGUMENTS;
    return ScriptFloat(host && host->current ? (float)host->current->dt : 0);
}

SCRIPT_BODY(now)
{
    UNUSED_ARGUMENTS;
    return ScriptFloat(host ? (float)GameplayTime(host->world) : 0);
}

SCRIPT_BODY(alpha)
{
    UNUSED_ARGUMENTS;
    return ScriptFloat(host ? host->alpha : 1);
}

// ---- drawing ---------------------------------------------------------------------------------------------
SCRIPT_BODY(draw_rect)
{
    (void)host;
    Vector2 centre = a[0].as.vector2, size = a[1].as.vector2;
    DrawRectangleV((Vector2){centre.x - size.x / 2, centre.y - size.y / 2}, size,
                   Unpack(a[2].as.integer));
    return ScriptNone();
}

SCRIPT_BODY(draw_rect_rotated)
{
    (void)host;
    Vector2 centre = a[0].as.vector2, size = a[1].as.vector2;
    DrawRectanglePro((Rectangle){centre.x, centre.y, size.x, size.y},
                     (Vector2){size.x / 2, size.y / 2}, a[2].as.number * RAD2DEG,
                     Unpack(a[3].as.integer));
    return ScriptNone();
}

static IsoHex AsHex(Vector2 v)
{
    return (IsoHex){(int)v.x, (int)v.y};
}
static Vector2 FromHex(IsoHex hex)
{
    return (Vector2){(float)hex.x, (float)hex.y};
}
SCRIPT_BODY(hex_world)
{
    (void)host;
    return ScriptVector2(IsoHexToScreen(AsHex(a[0].as.vector2)));
}
SCRIPT_BODY(hex_at)
{
    (void)host;
    return ScriptVector2(FromHex(IsoScreenToHex(a[0].as.vector2)));
}
SCRIPT_BODY(hex_distance)
{
    (void)host;
    return ScriptInt(IsoHexDistance(AsHex(a[0].as.vector2), AsHex(a[1].as.vector2)));
}
SCRIPT_BODY(hex_neighbour)
{
    (void)host;
    return ScriptVector2(FromHex(IsoHexNeighbour(AsHex(a[0].as.vector2), (IsoDir)a[1].as.integer)));
}
SCRIPT_BODY(hex_facing)
{
    (void)host;
    return ScriptInt((int)IsoHexDirection(AsHex(a[0].as.vector2), AsHex(a[1].as.vector2)));
}

// ---- small math and queries -----------------------------------------------------------------------
SCRIPT_BODY(random_int)
{
    (void)host;
    // GetRandomValue is inclusive at both ends; a reversed range is folded, not an error, so a
    // script that forgot which end is which still gets a number in the range it named.
    int min = a[0].as.integer, max = a[1].as.integer;
    if (min > max)
    {
        int swap = min;
        min = max;
        max = swap;
    }
    return ScriptInt(GetRandomValue(min, max));
}

SCRIPT_BODY(vec_length)
{
    (void)host;
    return ScriptFloat(Vector2Length(a[0].as.vector2));
}

SCRIPT_BODY(vec_distance)
{
    (void)host;
    return ScriptFloat(Vector2Distance(a[0].as.vector2, a[1].as.vector2));
}

SCRIPT_BODY(vec_normalize)
{
    (void)host;
    return ScriptVector2(Vector2Normalize(a[0].as.vector2));
}

SCRIPT_BODY(sprite_size)
{
    const SpriteSheet *sheet = ScriptHostSheet(host, a[0].as.string);
    if (!sheet)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2((Vector2){(float)sheet->meta.cellWidth, (float)sheet->meta.cellHeight});
}

SCRIPT_BODY(text_width)
{
    (void)host;
    return ScriptInt(MeasureText(a[0].as.string, a[1].as.integer));
}

SCRIPT_BODY(draw_sprite)
{
    const SpriteSheet *sheet = ScriptHostSheet(host, a[0].as.string);
    if (!sheet)
        return ScriptBool(false);
    DrawTexturePro(sheet->atlas, SpriteSheetFrameRect(sheet, SpriteSheetFrameAt(sheet, a[2].as.number)),
                   SpriteSheetGroundRect(sheet, a[1].as.vector2), (Vector2){0, 0}, 0, WHITE);
    return ScriptBool(true);
}

SCRIPT_BODY(draw_circle)
{
    (void)host;
    DrawCircleV(a[0].as.vector2, a[1].as.number, Unpack(a[2].as.integer));
    return ScriptNone();
}

SCRIPT_BODY(draw_line)
{
    (void)host;
    DrawLineV(a[0].as.vector2, a[1].as.vector2, Unpack(a[2].as.integer));
    return ScriptNone();
}

SCRIPT_BODY(draw_triangle)
{
    (void)host;
    DrawTriangle(a[0].as.vector2, a[1].as.vector2, a[2].as.vector2, Unpack(a[3].as.integer));
    return ScriptNone();
}

SCRIPT_BODY(draw_text)
{
    (void)host;
    DrawText(a[0].as.string, (int)a[1].as.vector2.x, (int)a[1].as.vector2.y, a[2].as.integer,
             Unpack(a[3].as.integer));
    return ScriptNone();
}

// ---- sound and scenes ---------------------------------------------------------------------------------------
SCRIPT_BODY(play_sound)
{
    if (!host)
        return ScriptBool(false);
    if (!host->audioReady)
    {
        InitAudioDevice();
        host->audioReady = IsAudioDeviceReady();
        if (!host->audioReady)
            return ScriptBool(false);
    }
    for (size_t i = 0; i < host->soundCount; i++)
        if (!strcmp(host->soundPaths[i], a[0].as.string))
        {
            PlaySound(host->sounds[i]);
            return ScriptBool(true);
        }
    if (host->soundCount == SCRIPT_SOUND_CAPACITY)
        return ScriptBool(false);
    char resolved[512];
    const char *path = CoreResolvePath(a[0].as.string, resolved, sizeof resolved);
    Sound sound = path ? LoadSound(path) : (Sound){0};
    if (!IsSoundValid(sound))
        return ScriptBool(false);
    size_t slot = host->soundCount++;
    snprintf(host->soundPaths[slot], sizeof host->soundPaths[slot], "%s", a[0].as.string);
    host->sounds[slot] = sound;
    PlaySound(sound);
    return ScriptBool(true);
}

SCRIPT_BODY(load_scene)
{
    if (!host)
        return ScriptBool(false);
    // Loading destroys every entity, including the one asking, so it waits for the callback to end.
    snprintf(host->pendingScene, sizeof host->pendingScene, "%s", a[0].as.string);
    return ScriptBool(true);
}

// ---- camera resources ------------------------------------------------------------------------------------------
SCRIPT_BODY(camera_create)
{
    (void)a;
    int index = 0;
    int handle = ScriptResCreate(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, SCRIPT_RES_CAMERA2D, &index);
    if (!handle)
        return ScriptResource(0);
    host->cameras[index] = CoreCamera2DDefault();
    return ScriptResource(handle);
}

SCRIPT_BODY(camera_destroy)
{
    int index = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (index < 0)
        return ScriptBool(false);
    host->cameraSlots[index].live = false;
    host->cameraSlots[index].generation++;
    return ScriptBool(true);
}

SCRIPT_BODY(camera_position)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(host->cameras[i].position);
}

SCRIPT_BODY(camera_set_position)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptNone();
    host->cameras[i].position = a[1].as.vector2;
    return ScriptNone();
}

SCRIPT_BODY(camera_zoom)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    return ScriptFloat(i >= 0 ? host->cameras[i].zoom : 0);
}

SCRIPT_BODY(camera_set_zoom)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptNone();
    host->cameras[i].zoom = a[1].as.number;
    return ScriptNone();
}

SCRIPT_BODY(camera_follow)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptNone();
    CoreCamera2DFollow(&host->cameras[i], a[1].as.vector2, a[2].as.number);
    return ScriptNone();
}

SCRIPT_BODY(camera_world_to_screen)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(CoreCamera2DWorldToScreen(&host->cameras[i], host->alpha, a[1].as.vector2));
}

SCRIPT_BODY(camera_screen_to_world)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(CoreCamera2DScreenToWorld(&host->cameras[i], host->alpha, a[1].as.vector2));
}

SCRIPT_BODY(camera_interpolated)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(CoreCamera2DInterpolated(&host->cameras[i], host->alpha));
}

SCRIPT_BODY(camera_set_viewport)
{
    int i = ScriptResResolve(host->cameraSlots, SCRIPT_CAMERA_CAPACITY, a[0].as.integer, SCRIPT_RES_CAMERA2D);
    if (i < 0)
        return ScriptNone();
    CoreCamera2DSetViewport(&host->cameras[i], a[1].as.vector2);
    return ScriptNone();
}

// ---- audio resources -------------------------------------------------------------------------------------------
SCRIPT_BODY(audio_create)
{
    (void)a;
    int index = 0;
    int handle = ScriptResCreate(host->audioSlots, SCRIPT_AUDIO_CAPACITY, SCRIPT_RES_AUDIO, &index);
    if (!handle)
        return ScriptResource(0);
    if (!CoreAudioInit(&host->audios[index]))
    {
        host->audioSlots[index].live = false;
        return ScriptResource(0);
    }
    host->audioInitialized[index] = true;
    return ScriptResource(handle);
}

SCRIPT_BODY(audio_destroy)
{
    int index = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (index < 0)
        return ScriptBool(false);
    if (host->audioInitialized[index])
        CoreAudioFree(&host->audios[index]);
    host->audioInitialized[index] = false;
    host->audioSlots[index].live = false;
    host->audioSlots[index].generation++;
    return ScriptBool(true);
}

SCRIPT_BODY(audio_add_bus)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioAddBus(&host->audios[i], a[1].as.string, a[2].as.number));
}

SCRIPT_BODY(audio_bus_volume)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioSetBusVolume(&host->audios[i], a[1].as.string, a[2].as.number));
}

SCRIPT_BODY(audio_bus_muted)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioSetBusMuted(&host->audios[i], a[1].as.string, a[2].as.boolean));
}

SCRIPT_BODY(audio_play_sound)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioPlaySound(&host->audios[i], a[1].as.string, a[2].as.string));
}

SCRIPT_BODY(audio_play_music)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioPlayMusic(&host->audios[i], a[1].as.string, a[2].as.string));
}

SCRIPT_BODY(audio_stop_music)
{
    int i = ScriptResResolve(host->audioSlots, SCRIPT_AUDIO_CAPACITY, a[0].as.integer, SCRIPT_RES_AUDIO);
    if (i < 0)
        return ScriptBool(false);
    return ScriptBool(CoreAudioStopMusic(&host->audios[i], a[1].as.string));
}

// ---- collision resources ---------------------------------------------------------------------------------------
SCRIPT_BODY(collision_create)
{
    int index = 0;
    int handle = ScriptResCreate(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, SCRIPT_RES_COLLISION, &index);
    if (!handle)
        return ScriptResource(0);
    if (!Collision2DWorldInit(&host->collisions[index], a[0].as.integer, a[1].as.number))
    {
        host->collisionSlots[index].live = false;
        return ScriptResource(0);
    }
    host->collisionInitialized[index] = true;
    return ScriptResource(handle);
}

SCRIPT_BODY(collision_destroy)
{
    int index = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (index < 0)
        return ScriptBool(false);
    if (host->collisionInitialized[index])
        Collision2DWorldFree(&host->collisions[index]);
    host->collisionInitialized[index] = false;
    host->collisionSlots[index].live = false;
    host->collisionSlots[index].generation++;
    return ScriptBool(true);
}

SCRIPT_BODY(collision_add_circle)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptInt(0);
    Collision2DShape shape = {COLLISION2D_CIRCLE, a[1].as.vector2, {0, 0}, a[2].as.number};
    Collision2DFilter filter = {(uint32_t)a[3].as.integer, (uint32_t)a[4].as.integer};
    Collision2DHandle h = Collision2DWorldAdd(&host->collisions[i], shape, filter, NULL);
    if (h.index == UINT32_MAX)
        return ScriptInt(0);
    return ScriptInt((int)(((h.index + 1) << 8) | (h.generation & 0xff)));
}

SCRIPT_BODY(collision_remove)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptBool(false);
    int packed = a[1].as.integer;
    Collision2DHandle h = {(uint32_t)((packed >> 8) - 1), (uint32_t)(packed & 0xff)};
    return ScriptBool(Collision2DWorldRemove(&host->collisions[i], h));
}

SCRIPT_BODY(collision_move_circle)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptBool(false);
    int packed = a[1].as.integer;
    Collision2DHandle h = {(uint32_t)((packed >> 8) - 1), (uint32_t)(packed & 0xff)};
    Collision2DShape shape = {COLLISION2D_CIRCLE, a[2].as.vector2, {0, 0}, a[3].as.number};
    return ScriptBool(Collision2DWorldSetShape(&host->collisions[i], h, shape));
}

SCRIPT_BODY(collision_query_circle)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptInt(0);
    return ScriptInt(Collision2DQueryCircle(&host->collisions[i], a[1].as.vector2, a[2].as.number,
                                            (uint32_t)a[3].as.integer, NULL, 0));
}

SCRIPT_BODY(collision_sweep_circle)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptFloat(1.0f);
    Collision2DSweep sweep;
    if (!Collision2DSweepCircle(&host->collisions[i], a[1].as.vector2, a[2].as.number,
                                a[3].as.vector2, (uint32_t)a[4].as.integer, &sweep))
        return ScriptFloat(1.0f);
    return ScriptFloat(sweep.fraction);
}

SCRIPT_BODY(collision_query_aabb)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptInt(0);
    Collision2DAabb aabb = Collision2DAabbMake(a[1].as.vector2, a[2].as.vector2);
    return ScriptInt(Collision2DQueryAabb(&host->collisions[i], aabb, (uint32_t)a[3].as.integer, NULL, 0));
}

SCRIPT_BODY(collision_add_aabb)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptInt(0);
    Collision2DShape shape = {COLLISION2D_AABB, a[1].as.vector2, a[2].as.vector2, 0.0f};
    Collision2DFilter filter = {(uint32_t)a[3].as.integer, (uint32_t)a[4].as.integer};
    Collision2DHandle h = Collision2DWorldAdd(&host->collisions[i], shape, filter, NULL);
    if (h.index == UINT32_MAX)
        return ScriptInt(0);
    return ScriptInt((int)(((h.index + 1) << 8) | (h.generation & 0xff)));
}

SCRIPT_BODY(collision_move_aabb)
{
    int i = ScriptResResolve(host->collisionSlots, SCRIPT_COLLISION_CAPACITY, a[0].as.integer, SCRIPT_RES_COLLISION);
    if (i < 0)
        return ScriptBool(false);
    int packed = a[1].as.integer;
    Collision2DHandle h = {(uint32_t)((packed >> 8) - 1), (uint32_t)(packed & 0xff)};
    Collision2DShape shape = {COLLISION2D_AABB, a[2].as.vector2, a[3].as.vector2, 0.0f};
    return ScriptBool(Collision2DWorldSetShape(&host->collisions[i], h, shape));
}

// ---- pathfinder resources --------------------------------------------------------------------------------------
static IsoHex HexOf(Vector2 v) { return (IsoHex){(int)v.x, (int)v.y}; }
static int HexIndex(int x, int y, int w) { return y * w + x; }

SCRIPT_BODY(pathfinder_create)
{
    int w = a[0].as.integer, h = a[1].as.integer;
    if (w <= 0 || h <= 0)
        return ScriptResource(0);
    int index = 0;
    int handle = ScriptResCreate(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, SCRIPT_RES_PATHFINDER, &index);
    if (!handle)
        return ScriptResource(0);
    if (!IsoPathfinderInit(&host->pathfinders[index], w, h))
    {
        host->pathfinderSlots[index].live = false;
        return ScriptResource(0);
    }
    host->pathfinderBlocked[index] = calloc((size_t)w * (size_t)h, sizeof(bool));
    if (!host->pathfinderBlocked[index])
    {
        IsoPathfinderFree(&host->pathfinders[index]);
        host->pathfinderSlots[index].live = false;
        return ScriptResource(0);
    }
    int steps = w * h < SCRIPT_PATH_STEP_CAP ? w * h : SCRIPT_PATH_STEP_CAP;
    if (!IsoPathInit(&host->paths[index], steps))
    {
        free(host->pathfinderBlocked[index]);
        host->pathfinderBlocked[index] = NULL;
        IsoPathfinderFree(&host->pathfinders[index]);
        host->pathfinderSlots[index].live = false;
        return ScriptResource(0);
    }
    host->pathfinderWidth[index] = w;
    host->pathfinderHeight[index] = h;
    host->pathfinderCtx[index] = (ScriptHexBlocked){host->pathfinderBlocked[index], w, h};
    host->pathfinderInitialized[index] = true;
    host->pathInitialized[index] = true;
    return ScriptResource(handle);
}

SCRIPT_BODY(pathfinder_destroy)
{
    int index = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (index < 0)
        return ScriptBool(false);
    if (host->pathfinderInitialized[index])
        IsoPathfinderFree(&host->pathfinders[index]);
    if (host->pathInitialized[index])
        IsoPathFree(&host->paths[index]);
    free(host->pathfinderBlocked[index]);
    host->pathfinderBlocked[index] = NULL;
    host->pathfinderInitialized[index] = false;
    host->pathInitialized[index] = false;
    host->pathfinderSlots[index].live = false;
    host->pathfinderSlots[index].generation++;
    return ScriptBool(true);
}

SCRIPT_BODY(pathfinder_block)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0)
        return ScriptBool(false);
    IsoHex hex = HexOf(a[1].as.vector2);
    if (hex.x < 0 || hex.x >= host->pathfinderWidth[i] || hex.y < 0 || hex.y >= host->pathfinderHeight[i])
        return ScriptBool(false);
    host->pathfinderBlocked[i][HexIndex(hex.x, hex.y, host->pathfinderWidth[i])] = true;
    return ScriptBool(true);
}

SCRIPT_BODY(pathfinder_unblock)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0)
        return ScriptBool(false);
    IsoHex hex = HexOf(a[1].as.vector2);
    if (hex.x < 0 || hex.x >= host->pathfinderWidth[i] || hex.y < 0 || hex.y >= host->pathfinderHeight[i])
        return ScriptBool(false);
    host->pathfinderBlocked[i][HexIndex(hex.x, hex.y, host->pathfinderWidth[i])] = false;
    return ScriptBool(true);
}

SCRIPT_BODY(pathfinder_blocked)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0)
        return ScriptBool(false);
    IsoHex hex = HexOf(a[1].as.vector2);
    if (hex.x < 0 || hex.x >= host->pathfinderWidth[i] || hex.y < 0 || hex.y >= host->pathfinderHeight[i])
        return ScriptBool(false);
    return ScriptBool(host->pathfinderBlocked[i][HexIndex(hex.x, hex.y, host->pathfinderWidth[i])]);
}

// The blocked-hex answer IsoPathfinderSolve and IsoMoverGoTo ask for, from the array the
// pathfinder-block! bindings fill. Off-grid is blocked: the search stops at the edge.
static bool ScriptBlockedHex(void *user, IsoHex hex)
{
    const ScriptHexBlocked *ctx = user;
    if (hex.x < 0 || hex.x >= ctx->width || hex.y < 0 || hex.y >= ctx->height)
        return true;
    return ctx->blocked[hex.y * ctx->width + hex.x];
}

SCRIPT_BODY(pathfinder_solve)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0)
        return ScriptInt(0);
    IsoPathClear(&host->paths[i]);
    if (!IsoPathfinderSolve(&host->pathfinders[i], &host->paths[i], HexOf(a[1].as.vector2),
                            HexOf(a[2].as.vector2), ScriptBlockedHex, &host->pathfinderCtx[i]))
        return ScriptInt(0);
    return ScriptInt(host->paths[i].count);
}

SCRIPT_BODY(pathfinder_path_length)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0)
        return ScriptInt(0);
    return ScriptInt(host->paths[i].count);
}

SCRIPT_BODY(pathfinder_path_get)
{
    int i = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[0].as.integer, SCRIPT_RES_PATHFINDER);
    int at = a[1].as.integer;
    if (i < 0 || at < 0 || at >= host->paths[i].count)
        return ScriptVector2((Vector2){0, 0});
    IsoHex hex = host->paths[i].hexes[at];
    return ScriptVector2((Vector2){(float)hex.x, (float)hex.y});
}

// ---- mover resources -------------------------------------------------------------------------------------------
SCRIPT_BODY(mover_create)
{
    int index = 0;
    int handle = ScriptResCreate(host->moverSlots, SCRIPT_MOVER_CAPACITY, SCRIPT_RES_MOVER, &index);
    if (!handle)
        return ScriptResource(0);
    int capacity = a[1].as.integer > 0 ? a[1].as.integer : SCRIPT_PATH_STEP_CAP;
    if (!IsoMoverInit(&host->movers[index], HexOf(a[0].as.vector2), capacity))
    {
        host->moverSlots[index].live = false;
        return ScriptResource(0);
    }
    host->moverInitialized[index] = true;
    return ScriptResource(handle);
}

SCRIPT_BODY(mover_destroy)
{
    int index = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (index < 0)
        return ScriptBool(false);
    if (host->moverInitialized[index])
        IsoMoverFree(&host->movers[index]);
    host->moverInitialized[index] = false;
    host->moverSlots[index].live = false;
    host->moverSlots[index].generation++;
    return ScriptBool(true);
}

SCRIPT_BODY(mover_goto)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    int f = ScriptResResolve(host->pathfinderSlots, SCRIPT_PATHFINDER_CAPACITY, a[1].as.integer, SCRIPT_RES_PATHFINDER);
    if (i < 0 || f < 0)
        return ScriptBool(false);
    return ScriptBool(IsoMoverGoTo(&host->movers[i], &host->pathfinders[f], HexOf(a[2].as.vector2),
                                   ScriptBlockedHex, &host->pathfinderCtx[f]));
}

SCRIPT_BODY(mover_truncate)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (i < 0)
        return ScriptNone();
    IsoMoverTruncate(&host->movers[i], a[1].as.integer);
    return ScriptNone();
}

SCRIPT_BODY(mover_remaining)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    return ScriptInt(i >= 0 ? IsoMoverRemainingSteps(&host->movers[i]) : 0);
}

SCRIPT_BODY(mover_stop)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (i < 0)
        return ScriptNone();
    IsoMoverStop(&host->movers[i]);
    return ScriptNone();
}

SCRIPT_BODY(mover_moving)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    return ScriptBool(i >= 0 && IsoMoverMoving(&host->movers[i]));
}

SCRIPT_BODY(mover_update)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (i < 0)
        return ScriptNone();
    IsoMoverUpdate(&host->movers[i], a[1].as.number);
    return ScriptNone();
}

SCRIPT_BODY(mover_screen)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    return ScriptVector2(IsoMoverScreen(&host->movers[i]));
}

SCRIPT_BODY(mover_hex)
{
    int i = ScriptResResolve(host->moverSlots, SCRIPT_MOVER_CAPACITY, a[0].as.integer, SCRIPT_RES_MOVER);
    if (i < 0)
        return ScriptVector2((Vector2){0, 0});
    IsoHex hex = host->movers[i].hex;
    return ScriptVector2((Vector2){(float)hex.x, (float)hex.y});
}

// ---- saved key/values ------------------------------------------------------------------------------------------
static ScriptSaveEntry *SaveFind(ScriptHost *host, const char *key)
{
    for (size_t i = 0; i < host->saveCount; i++)
        if (!strcmp(host->saves[i].key, key))
            return &host->saves[i];
    return NULL;
}

static ScriptSaveEntry *SaveSlot(ScriptHost *host, const char *key)
{
    ScriptSaveEntry *found = SaveFind(host, key);
    if (found)
        return found;
    if (host->saveCount == SCRIPT_SAVE_CAPACITY || strlen(key) >= SCRIPT_SAVE_KEY)
        return NULL;
    ScriptSaveEntry *entry = &host->saves[host->saveCount++];
    snprintf(entry->key, sizeof entry->key, "%s", key);
    return entry;
}

SCRIPT_BODY(save_set_number)
{
    ScriptSaveEntry *entry = SaveSlot(host, a[0].as.string);
    if (!entry)
        return ScriptBool(false);
    snprintf(entry->value, sizeof entry->value, "%g", a[1].as.number);
    return ScriptBool(true);
}

SCRIPT_BODY(save_set_string)
{
    if (strlen(a[1].as.string) >= SCRIPT_SAVE_VALUE)
        return ScriptBool(false);
    ScriptSaveEntry *entry = SaveSlot(host, a[0].as.string);
    if (!entry)
        return ScriptBool(false);
    snprintf(entry->value, sizeof entry->value, "%s", a[1].as.string);
    return ScriptBool(true);
}

SCRIPT_BODY(save_get_number)
{
    ScriptSaveEntry *entry = SaveFind(host, a[0].as.string);
    return ScriptFloat(entry ? (float)strtod(entry->value, NULL) : 0.0f);
}

SCRIPT_BODY(save_get_string)
{
    ScriptSaveEntry *entry = SaveFind(host, a[0].as.string);
    return ScriptString(entry ? entry->value : "");
}

// One line per pair, "key value", with the value last so a value may contain spaces. Numbers are
// saved as they were spelled; reading one back through save-get-number parses it again.
SCRIPT_BODY(save_write)
{
    CoreAtomicFile atomic;
    FILE *file = CoreAtomicBegin(&atomic, a[0].as.string);
    if (!file)
        return ScriptBool(false);
    for (size_t i = 0; i < host->saveCount; i++)
        fprintf(file, "%s %s\n", host->saves[i].key, host->saves[i].value);
    return ScriptBool(CoreAtomicCommit(&atomic, true));
}

SCRIPT_BODY(save_read)
{
    char path[512];
    const char *resolved = CoreResolvePath(a[0].as.string, path, sizeof path);
    char *text = CoreReadFile(resolved ? resolved : a[0].as.string);
    if (!text)
        return ScriptBool(false);
    host->saveCount = 0;
    char *line = strtok(text, "\n");
    while (line)
    {
        char *space = strchr(line, ' ');
        if (space && host->saveCount < SCRIPT_SAVE_CAPACITY)
        {
            *space = '\0';
            ScriptSaveEntry *entry = &host->saves[host->saveCount++];
            snprintf(entry->key, sizeof entry->key, "%s", line);
            snprintf(entry->value, sizeof entry->value, "%s", space + 1);
        }
        line = strtok(NULL, "\n");
    }
    CoreFreeFile(text);
    return ScriptBool(true);
}

// ---- debug drawing ---------------------------------------------------------------------------------------------
// One queue for the whole host, made on the first thing a script asks to see. It ages and draws in
// ScriptHostUpdate, which the project calls from its Draw callback.
static CoreDebug *DebugQueue(ScriptHost *host)
{
    if (!host->debugReady && CoreDebugInit(&host->debug, false))
        host->debugReady = true;
    return host->debugReady ? &host->debug : NULL;
}

SCRIPT_BODY(debug_line)
{
    CoreDebug *debug = DebugQueue(host);
    return ScriptBool(debug && CoreDebugLine(debug, a[0].as.vector2, a[1].as.vector2,
                                              Unpack(a[2].as.integer), a[3].as.number));
}

SCRIPT_BODY(debug_circle)
{
    CoreDebug *debug = DebugQueue(host);
    return ScriptBool(debug && CoreDebugCircle(debug, a[0].as.vector2, a[1].as.number,
                                               Unpack(a[2].as.integer), a[3].as.number));
}

SCRIPT_BODY(debug_rect)
{
    CoreDebug *debug = DebugQueue(host);
    Rectangle rect = {a[0].as.vector2.x - a[1].as.vector2.x * 0.5f,
                      a[0].as.vector2.y - a[1].as.vector2.y * 0.5f,
                      a[1].as.vector2.x, a[1].as.vector2.y};
    return ScriptBool(debug && CoreDebugRect(debug, rect, Unpack(a[2].as.integer), a[3].as.number));
}

SCRIPT_BODY(debug_text)
{
    CoreDebug *debug = DebugQueue(host);
    return ScriptBool(debug && CoreDebugText(debug, a[1].as.vector2, a[0].as.string,
                                              Unpack(a[2].as.integer), a[3].as.number));
}

// ---- the table -----------------------------------------------------------------------------------------------
#define UNWRAP(...) {__VA_ARGS__}
#define SCRIPT_BINDING(id, name, result, help, types) static const ScriptType id##_types[] = UNWRAP types;
#include "script_api.def"
#undef SCRIPT_BINDING

#define SCRIPT_BINDING(id, name, result, help, types)                                              \
    {name, result, id##_types + 1, (int)(sizeof id##_types / sizeof(ScriptType)) - 1, Script_##id,  \
     help},
static const ScriptBinding table[] = {
#include "script_api.def"
};
#undef SCRIPT_BINDING

const ScriptBinding *ScriptBindings(int *count)
{
    if (count)
        *count = (int)(sizeof table / sizeof table[0]);
    return table;
}

bool ScriptAddBinding(ScriptHost *host, const ScriptBinding *binding)
{
    if (!host || !binding || !binding->name || !binding->call ||
        host->addedCount == SCRIPT_ADDED_CAPACITY)
        return false;
    if (ScriptBindingNamed(binding->name))
    {
        TraceLog(LOG_ERROR, "Script: %s is already a call of the engine's", binding->name);
        return false;
    }
    for (int i = 0; i < host->addedCount; i++)
        if (!strcmp(host->added[i]->name, binding->name))
            return false;
    host->added[host->addedCount++] = binding;
    return true;
}

int ScriptBindingCount(const ScriptHost *host)
{
    int count = 0;
    ScriptBindings(&count);
    return count + (host ? host->addedCount : 0);
}

const ScriptBinding *ScriptBindingAt(const ScriptHost *host, int index)
{
    int count = 0;
    const ScriptBinding *engine = ScriptBindings(&count);
    if (index < 0)
        return NULL;
    if (index < count)
        return &engine[index];
    index -= count;
    return host && index < host->addedCount ? host->added[index] : NULL;
}

const ScriptBinding *ScriptBindingNamed(const char *name)
{
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name))
            return &table[i];
    return NULL;
}

const char *ScriptTypeName(ScriptType type)
{
    static const char *names[] = {"none",    "bool",    "int",    "float",
                                  "vector2", "vector3", "string", "entity", "resource"};
    return type >= SCRIPT_NONE && type <= SCRIPT_RESOURCE ? names[type] : "?";
}

bool ScriptInvoke(ScriptHost *host, const ScriptBinding *binding, const ScriptValue *arguments,
                  int count, ScriptValue *result, const char **message)
{
    static char complaint[192];
    if (!binding)
        return false;
    if (count != binding->argumentCount)
    {
        snprintf(complaint, sizeof complaint, "%s takes %d argument%s, not %d", binding->name,
                 binding->argumentCount, binding->argumentCount == 1 ? "" : "s", count);
        if (message)
            *message = complaint;
        return false;
    }
    for (int i = 0; i < count; i++)
        if (arguments[i].type != binding->arguments[i])
        {
            snprintf(complaint, sizeof complaint, "%s wants %s for argument %d, was given %s",
                     binding->name, ScriptTypeName(binding->arguments[i]), i + 1,
                     ScriptTypeName(arguments[i].type));
            if (message)
                *message = complaint;
            return false;
        }
    ScriptValue value = binding->call(host, arguments);
    if (result)
        *result = value;
    return true;
}
