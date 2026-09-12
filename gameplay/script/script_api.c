/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "script.h"

#include "core/file.h"
#include "gameplay/scene.h"
#include "raymath.h"
#include <stdio.h>
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

static Color Unpack(int rgba)
{
    return (Color){(unsigned char)((rgba >> 24) & 0xff), (unsigned char)((rgba >> 16) & 0xff),
                   (unsigned char)((rgba >> 8) & 0xff), (unsigned char)(rgba & 0xff)};
}

// ---- talking to the engine -----------------------------------------------------------------------
SCRIPT_BODY(log)
{
    (void)host;
    TraceLog(LOG_INFO, "SCRIPT %s", a[0].as.string);
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
                                  "vector2", "vector3", "string", "entity"};
    return type >= SCRIPT_NONE && type <= SCRIPT_ENTITY ? names[type] : "?";
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
