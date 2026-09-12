/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "script.h"

#include <stdio.h>
#include <string.h>

static ScriptHost *active;

ScriptHost *ScriptHostActive(void) { return active; }

bool ScriptHostInit(ScriptHost *host, GameplayWorld *world)
{
    if (!host || !world)
        return false;
    *host = (ScriptHost){0};
    host->world = world;
    active = host;
    return true;
}

void ScriptHostFree(ScriptHost *host)
{
    if (!host)
        return;
    for (size_t i = 0; i < host->soundCount; i++)
        UnloadSound(host->sounds[i]);
    if (host->audioReady)
        CloseAudioDevice();
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
    if (host)
        host->alpha = alpha;
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

// The script-side handle: the slot, plus as much of the generation as fits, so a handle kept past
// the entity's death is spotted rather than pointing at whatever took its place. Zero is nothing.
int ScriptHostIdOf(EntityHandle entity)
{
    if (entity.index == UINT32_MAX)
        return 0;
    return (int)(((entity.index + 1) << 8) | (entity.generation & 0xff));
}

EntityHandle ScriptHostHandleOf(ScriptHost *host, int id)
{
    if (!host || id <= 0)
        return ENTITY_NULL;
    uint32_t index = (uint32_t)(id >> 8) - 1;
    EntityHandle guess = {index, 0};
    // The world keeps the real generation; this only has to agree on its low bits.
    for (uint32_t generation = 0; generation < 256; generation++)
    {
        guess.generation = generation;
        if ((int)(generation & 0xff) == (id & 0xff) && EntityAlive(host->world, guess))
            return guess;
    }
    return ENTITY_NULL;
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
    bool ok = type->language->Call(type->language->user, function);
    host->current = was;
    return ok;
}

static bool ScriptSpawn(EntityContext *entity)
{
    ScriptEntity *self = entity->data;
    self->previous = self->transform;
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
    Dispatch(entity, offsetof(ScriptClass, destroy));
}

// Registering happens once the script has finished declaring the class, because the world seals its
// registry at the first spawn and the field table has to be complete by then.
bool ScriptClassRegister(ScriptHost *host, ScriptClass *type)
{
    if (!host || !type || type->registered)
        return false;
    type->defaults.transform = Transform2DIdentity();
    type->defaults.previous = type->defaults.transform;
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
