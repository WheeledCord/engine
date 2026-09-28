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
    if (!type || type->registered || type->slotCount == SCRIPT_CLASS_FIELDS ||
        EngineTypeProperty(&ScriptEntityType, a[1].as.string) || EngineTypeMethod(&ScriptEntityType, a[1].as.string))
        return ScriptBool(false);
    for (size_t i = 0; i < type->slotCount; i++)
        if (!strcmp(type->fieldNames[i], a[1].as.string))
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
    size_t offset = offsetof(ScriptEntity, slots) + slot * sizeof(ScriptValue) + offsetof(ScriptValue, as);
    type->fields[type->fieldCount++] = (EntityField){
        .name = type->fieldNames[slot], .type = fieldType, .offset = offset, .size = size};
    // The same field as a property of the class's engine type, which is how a script reads it.
    type->properties[slot] = (EngineProperty){type->fieldNames[slot], slotType, offset,
                                              ENGINE_PROPERTY_SCENE | ENGINE_PROPERTY_SAVE,
                                              NULL, NULL, "declared by the script"};
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
    return ScriptObject(host && host->current ? ScriptHostObjectOf(host, host->current->entity)
                                              : ENGINE_OBJECT_NULL);
}

SCRIPT_BODY(spawn)
{
    if (!host)
        return ScriptObject(ENGINE_OBJECT_NULL);
    char position[64];
    snprintf(position, sizeof position, "%.9g %.9g", (double)a[1].as.vector2.x,
             (double)a[1].as.vector2.y);
    EntityProperty properties[] = {{"position", position}};
    EntityHandle spawned = EntitySpawnWith(host->world, a[0].as.string, properties, 1);
    return ScriptObject(ScriptHostObjectOf(host, spawned));
}

/* The next living scripted entity of a class from a handle on: a class written in C has no object a
   script could hold, so the walk passes over it. */
static EngineObjectId NextScripted(ScriptHost *host, EntityHandle at, const char *classname)
{
    for (; EntityAlive(host->world, at); at = EntityNext(host->world, at, classname))
    {
        EngineObjectId object = ScriptHostObjectOf(host, at);
        if (!EngineObjectIdIsNull(object))
            return object;
    }
    return ENGINE_OBJECT_NULL;
}

SCRIPT_BODY(find_first)
{
    if (!host)
        return ScriptObject(ENGINE_OBJECT_NULL);
    return ScriptObject(NextScripted(host, EntityFirst(host->world, a[0].as.string), a[0].as.string));
}

SCRIPT_BODY(find_next)
{
    const ScriptEntity *after = host ? EngineObjectData(&host->objects, a[0].as.object, &ScriptEntityType) : NULL;
    if (!after)
        return ScriptObject(ENGINE_OBJECT_NULL);
    return ScriptObject(NextScripted(host, EntityNext(host->world, after->entity, a[1].as.string),
                                     a[1].as.string));
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

SCRIPT_BODY(elapsed)
{
    UNUSED_ARGUMENTS;
    return ScriptFloat(host && host->current ? (float)host->current->elapsed : 0.0f);
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

const char *ScriptTypeName(ScriptType type) { return EngineValueTypeName(type); }

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

SCRIPT_BODY(net_clock_now)
{
    (void)host; (void)a;
    return ScriptFloat((float)CoreNetClockNow());
}
