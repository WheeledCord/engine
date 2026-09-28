/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/file.h"
#include "core/node.h"
#include "script.h"
#include "script_network.h"
#include "raymath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ScriptHost *active;

ScriptHost *ScriptHostActive(void) { return active; }

bool ScriptHostInit(ScriptHost *host, GameplayWorld *world)
{
    if (!host || !world)
        return false;
    *host = (ScriptHost){0};
    if (!EngineObjectsInit(&host->objects))
        return false;
    // The engine's own types, which a script makes by name.
    static const EngineType *const engineTypes[] = {
        &CoreCamera2DType, &CoreAudioType, &CoreAudioVoiceType, &Collision2DWorldType, &CoreNetClockType,
        &CoreNetInterpolatorType, &CoreNodeType, &CoreParticlesType, &CoreTextureType, &CoreTimerType,
        &CoreWaypointsType, &IsoRouterType, &IsoMoverType, &ScriptNetworkType,
    };
    for (size_t i = 0; i < sizeof engineTypes / sizeof engineTypes[0]; i++)
        if (!EngineObjectsRegisterType(&host->objects, engineTypes[i]))
        {
            EngineObjectsFree(&host->objects);
            return false;
        }
    host->world = world;
    active = host;
    return true;
}

bool ScriptHostRegisterType(ScriptHost *host, const EngineType *type)
{
    return host && EngineObjectsRegisterType(&host->objects, type);
}

void ScriptHostFree(ScriptHost *host)
{
    if (!host)
        return;
    for (size_t i = 0; i < host->sheetCount; i++)
        SpriteSheetUnload(&host->sheets[i]);
    for (size_t i = 0; i < host->soundCount; i++)
        UnloadSound(host->sounds[i]);
    if (host->audioReady)
        CloseAudioDevice();
    // Everything scripts made, and the handles of every scripted entity still standing.
    EngineObjectsFree(&host->objects);
    if (host->debugReady)
        CoreDebugFree(&host->debug);
    if (active == host)
        active = NULL;
    *host = (ScriptHost){0};
}

void ScriptHostUseLanguage(ScriptHost *host, const ScriptLanguage *language)
{
    if (host)
        host->language = language;
}

void ScriptHostSetAlpha(ScriptHost *host, float alpha)
{
    if (!host)
        return;
    host->alpha = alpha;
    host->objects.alpha = alpha;
}

void ScriptHostFlush(ScriptHost *host)
{
    if (!host || !host->pendingScene[0])
        return;
    char path[sizeof host->pendingScene];
    snprintf(path, sizeof path, "%s", host->pendingScene);
    host->pendingScene[0] = '\0';
    // Replacing the world destroys entities, so it waits until no callback of theirs is running.
    if (!GameplaySceneLoad(host->world, path, true))
        TraceLog(LOG_ERROR, "Script: could not load scene %s", path);
}

ScriptClass *ScriptHostClass(ScriptHost *host, const char *name)
{
    if (!host || !name)
        return NULL;
    for (size_t i = 0; i < host->classCount; i++)
        if (!strcmp(host->classes[i].name, name))
            return &host->classes[i];
    return NULL;
}

EngineObjectId ScriptHostObjectOf(ScriptHost *host, EntityHandle entity)
{
    if (!host || !EntityAlive(host->world, entity) ||
        !ScriptHostClass(host, EntityClassname(host->world, entity)))
        return ENGINE_OBJECT_NULL;
    const ScriptEntity *self = EntityData(host->world, entity);
    return self ? self->object : ENGINE_OBJECT_NULL;
}

// ---- the callbacks a scripted class gets -----------------------------------------------------
static bool Dispatch(EntityContext *entity, size_t nameOffset)
{
    ScriptHost *host = active;
    if (!host || !entity)
        return false;
    ScriptClass *type = ScriptHostClass(host, EntityClassname(entity->world, entity->entity));
    if (!type || !type->language || !type->language->Call)
        return false;
    const char *function = (const char *)type + nameOffset;
    if (!function[0])
        return true; // a class need not implement every event
    EntityContext *was = host->current;
    host->current = entity;
    bool ok = type->language->Call(type->language->user, function, ((ScriptEntity *)entity->data)->object);
    host->current = was;
    return ok;
}

static bool ScriptSpawn(EntityContext *entity)
{
    ScriptHost *host = active;
    ScriptEntity *self = entity->data;
    self->previous = self->transform;
    self->entity = entity->entity;
    // The payload does not move while the entity lives, so the object can simply point at it.
    ScriptClass *type = host ? ScriptHostClass(host, EntityClassname(entity->world, entity->entity)) : NULL;
    self->object = type ? EngineObjectAdopt(&host->objects, &type->type, self) : ENGINE_OBJECT_NULL;
    if (EngineObjectIdIsNull(self->object))
        return false;
    return Dispatch(entity, offsetof(ScriptClass, spawn));
}

static void ScriptThink(EntityContext *entity)
{
    ScriptEntity *self = entity->data;
    self->previous = self->transform;
    Dispatch(entity, offsetof(ScriptClass, think));
}

static void ScriptDraw(EntityContext *entity)
{
    Dispatch(entity, offsetof(ScriptClass, draw));
}

static void ScriptDestroy(EntityContext *entity)
{
    ScriptEntity *self = entity->data;
    if (!EngineObjectIdIsNull(self->object))
        Dispatch(entity, offsetof(ScriptClass, destroy));
    // After the script has said goodbye, so its destroy callback can still reach it.
    if (active)
        EngineObjectDestroy(&active->objects, self->object);
    self->object = ENGINE_OBJECT_NULL;
}

// Registering happens once the script has finished declaring the class, because the world seals its
// registry at the first spawn and the field table has to be complete by then.
const SpriteSheet *ScriptHostSheet(ScriptHost *host, const char *name)
{
    if (!host || !name || !*name)
        return NULL;
    for (size_t i = 0; i < host->sheetCount; i++)
        if (!strcmp(host->sheetNames[i], name))
            return &host->sheets[i];
    if (host->sheetCount == SCRIPT_SHEET_CAPACITY || strlen(name) >= sizeof host->sheetNames[0])
        return NULL;
    char atlasName[192], sheetName[192], atlas[512], meta[512];
    snprintf(atlasName, sizeof atlasName, "%s.png", name);
    snprintf(sheetName, sizeof sheetName, "%s.sheet", name);
    const char *atlasPath = CoreResolvePath(atlasName, atlas, sizeof atlas);
    const char *metaPath = CoreResolvePath(sheetName, meta, sizeof meta);
    SpriteSheet loaded;
    if (!atlasPath || !metaPath || !SpriteSheetLoad(&loaded, atlasPath, metaPath))
    {
        TraceLog(LOG_ERROR, "Script: no sprite sheet named %s", name);
        return NULL;
    }
    strcpy(host->sheetNames[host->sheetCount], name);
    host->sheets[host->sheetCount] = loaded;
    return &host->sheets[host->sheetCount++];
}

bool ScriptClassRegister(ScriptHost *host, ScriptClass *type)
{
    if (!host || !type || type->registered)
        return false;
    type->defaults.transform = Transform2DIdentity();
    type->defaults.previous = type->defaults.transform;
    type->defaults.entity = ENTITY_NULL;
    type->defaults.object = ENGINE_OBJECT_NULL;
    type->type = (EngineType){.name = type->name,
                              .parent = &ScriptEntityType,
                              .size = sizeof(ScriptEntity),
                              .properties = type->properties,
                              .propertyCount = (int)type->slotCount,
                              .help = "a scripted entity class"};
    EntityClass declaration = {.classname = type->name,
                               .size = sizeof(ScriptEntity),
                               // A scripted class carries its own shape like any other.
                               .alignment = ENTITY_ALIGNMENT_OF(ScriptEntity),
                               .defaults = &type->defaults,
                               .fields = type->fields,
                               .fieldCount = type->fieldCount,
                               .Spawn = ScriptSpawn,
                               .Think = ScriptThink,
                               .Draw = ScriptDraw,
                               .Destroy = ScriptDestroy};
    if (!EntityRegister(host->world, declaration))
    {
        TraceLog(LOG_ERROR, "Script: class %s was refused by the world", type->name);
        return false;
    }
    type->registered = true;
    type->language = host->language;
    return true;
}

// ---- host update ----------------------------------------------------------------------------

void ScriptHostStep(ScriptHost *host, float dt)
{
    if (!host)
        return;
    EngineObjectsStep(&host->objects, dt);
    ScriptHostFlush(host);
}

void ScriptHostUpdate(ScriptHost *host, double dt)
{
    if (!host || !host->debugReady)
        return;
    CoreDebugUpdate(&host->debug, dt);
    // Drawing must happen inside BeginDrawing/EndDrawing, so the project calls this from Draw.
    CoreDebugDraw(&host->debug);
}

// ---- what every scripted entity is ------------------------------------------------------------
static bool EntityClassnameGet(const void *object, EngineValue *out)
{
    const ScriptEntity *self = object;
    const char *name = active ? EntityClassname(active->world, self->entity) : NULL;
    *out = EngineString(name ? name : "");
    return true;
}
static bool EntityInterpolatedPosition(const void *object, EngineValue *out)
{
    const ScriptEntity *self = object;
    float alpha = active ? active->alpha : 1.0f;
    *out = EngineVector2(Vector2Lerp(self->previous.translation, self->transform.translation, alpha));
    return true;
}
static float InterpolatedRotation(const ScriptEntity *self)
{
    float alpha = active ? active->alpha : 1.0f;
    return self->previous.rotation + AngleDelta(self->previous.rotation, self->transform.rotation) * alpha;
}
static bool EntityInterpolatedRotation(const void *object, EngineValue *out)
{
    *out = EngineFloat(InterpolatedRotation(object));
    return true;
}
static bool EntityDestroyMethod(EngineCall *call)
{
    const ScriptEntity *self = call->data;
    call->result = EngineBool(active && EntityDestroy(active->world, self->entity));
    return true;
}
// Think again after a delay: through the callback in flight when it is this entity's, so the
// world's own rounding applies, and by absolute time otherwise.
static void ScheduleThink(const ScriptEntity *self, double delay, bool next)
{
    if (!active)
        return;
    EntityContext *current = active->current;
    if (current && current->entity.index == self->entity.index &&
        current->entity.generation == self->entity.generation)
    {
        if (next)
            EntityThinkNext(current);
        else
            EntityThinkAfter(current, delay);
        return;
    }
    double step = active->world->tickInterval;
    EntityScheduleThink(active->world, self->entity, GameplayTime(active->world) + (next ? step : delay));
}
static bool EntityThinkNextMethod(EngineCall *call)
{
    ScheduleThink(call->data, 0, true);
    return true;
}
static bool EntityThinkAfterMethod(EngineCall *call)
{
    ScheduleThink(call->data, call->arguments[0].as.number, false);
    return true;
}
static bool EntityRotate(EngineCall *call)
{
    Transform2DRotate(&((ScriptEntity *)call->data)->transform, call->arguments[0].as.number);
    return true;
}
static bool EntityMoveWorld(EngineCall *call)
{
    Transform2DMoveWorld(&((ScriptEntity *)call->data)->transform, call->arguments[0].as.vector2);
    return true;
}
static bool EntityMoveLocal(EngineCall *call)
{
    Transform2DMoveLocal(&((ScriptEntity *)call->data)->transform, call->arguments[0].as.vector2);
    return true;
}
static bool EntityLookAt(EngineCall *call)
{
    call->result = EngineBool(Transform2DLookAt(&((ScriptEntity *)call->data)->transform,
                                                call->arguments[0].as.vector2));
    return true;
}
static bool EntityToLocal(EngineCall *call)
{
    const ScriptEntity *self = call->data;
    // Drawn where the entity is this frame, so a shape follows the same interpolation its body does.
    Transform2D render = self->transform;
    render.translation = Vector2Lerp(self->previous.translation, render.translation,
                                     active ? active->alpha : 1.0f);
    render.rotation = InterpolatedRotation(self);
    call->result = EngineVector2(Transform2DPoint(render, call->arguments[0].as.vector2));
    return true;
}
static const EngineProperty entityProperties[] = {
    ENGINE_FIELD("position", ScriptEntity, transform.translation, ENGINE_VECTOR2,
                 ENGINE_PROPERTY_SCENE | ENGINE_PROPERTY_SAVE, "where it is"),
    ENGINE_FIELD("rotation", ScriptEntity, transform.rotation, ENGINE_FLOAT,
                 ENGINE_PROPERTY_SCENE | ENGINE_PROPERTY_SAVE, "which way it faces, in radians"),
    ENGINE_COMPUTED("classname", ENGINE_STRING, ENGINE_PROPERTY_READ_ONLY, EntityClassnameGet, NULL,
                    "the class it was spawned from"),
    ENGINE_COMPUTED("drawn-position", ENGINE_VECTOR2, ENGINE_PROPERTY_READ_ONLY,
                    EntityInterpolatedPosition, NULL, "where to draw it this frame, between steps"),
    ENGINE_COMPUTED("drawn-rotation", ENGINE_FLOAT, ENGINE_PROPERTY_READ_ONLY,
                    EntityInterpolatedRotation, NULL, "which way to draw it this frame, between steps"),
};
static const EngineMethod entityMethods[] = {
    {"destroy!", ENGINE_BOOL, {ENGINE_NONE}, 0, EntityDestroyMethod, "remove it from the world"},
    {"think-next!", ENGINE_NONE, {ENGINE_NONE}, 0, EntityThinkNextMethod, "think again next step"},
    {"think-after!", ENGINE_NONE, {ENGINE_FLOAT}, 1, EntityThinkAfterMethod,
     "think again after so many seconds"},
    {"rotate!", ENGINE_NONE, {ENGINE_FLOAT}, 1, EntityRotate, "turn by so many radians"},
    {"move-world!", ENGINE_NONE, {ENGINE_VECTOR2}, 1, EntityMoveWorld, "move along the world axes"},
    {"move-local!", ENGINE_NONE, {ENGINE_VECTOR2}, 1, EntityMoveLocal, "move along its own axes"},
    {"look-at!", ENGINE_BOOL, {ENGINE_VECTOR2}, 1, EntityLookAt, "face a point"},
    {"to-local", ENGINE_VECTOR2, {ENGINE_VECTOR2}, 1, EntityToLocal,
     "a point in its own space, placed where it is drawn this frame"},
};
const EngineType ScriptEntityType = {
    .name = "entity",
    .size = sizeof(ScriptEntity),
    .properties = entityProperties,
    .propertyCount = sizeof entityProperties / sizeof entityProperties[0],
    .methods = entityMethods,
    .methodCount = sizeof entityMethods / sizeof entityMethods[0],
    .help = "a scripted entity: a transform, the fields its class declared, and its callbacks",
};
