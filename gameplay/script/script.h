/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_H
#define GAMEPLAY_SCRIPT_H
#include "core/transform.h"
#include "gameplay/runtime.h"
#include "script_api.h"

/* What the frontends share: the world, the classes scripts declared, and which entity's callback is
   running. A scripted class is an ordinary EntityClass whose Spawn, Think, Draw and Destroy dispatch
   into functions the script named, so from the world's side there is nothing special about it. */

#define SCRIPT_NAME_CAPACITY 48
#define SCRIPT_CLASS_FIELDS 8
#define SCRIPT_CLASS_CAPACITY 32
#define SCRIPT_SOUND_CAPACITY 16

// The payload every scripted entity carries: a transform, where it was a step ago so drawing can
// interpolate, and the fields the class declared.
typedef struct ScriptEntity
{
    Transform2D transform;
    Transform2D previous;
    ScriptValue slots[SCRIPT_CLASS_FIELDS];
} ScriptEntity;

// One per language. Call runs a function the script named; arguments are not passed, because a
// callback asks for what it needs through the bindings (self, dt, alpha).
typedef struct ScriptLanguage
{
    const char *name;
    void *user;
    bool (*Call)(void *user, const char *function);
} ScriptLanguage;

typedef struct ScriptClass
{
    char name[SCRIPT_NAME_CAPACITY];
    char spawn[SCRIPT_NAME_CAPACITY];
    char think[SCRIPT_NAME_CAPACITY];
    char draw[SCRIPT_NAME_CAPACITY];
    char destroy[SCRIPT_NAME_CAPACITY];
    char fieldNames[SCRIPT_CLASS_FIELDS][SCRIPT_NAME_CAPACITY];
    EntityField fields[SCRIPT_CLASS_FIELDS + 2]; // the declared ones, plus position and rotation
    size_t fieldCount;
    size_t slotCount;
    ScriptEntity defaults;
    const ScriptLanguage *language;
    bool registered;
} ScriptClass;

struct ScriptHost
{
    GameplayWorld *world;
    const ScriptLanguage *language; // whichever frontend is loading scripts at the moment
    EntityContext *current;         // the callback in flight, or NULL outside one
    float alpha;
    char pendingScene[256]; // a scene a script asked for, loaded once callbacks are finished
    ScriptClass classes[SCRIPT_CLASS_CAPACITY];
    size_t classCount;
    char soundPaths[SCRIPT_SOUND_CAPACITY][128];
    Sound sounds[SCRIPT_SOUND_CAPACITY];
    size_t soundCount;
    bool audioReady;
};

/* One host per program: entity callbacks reach it through EntityClass, which carries no user data
   of its own. Init points that at this host and Free clears it. */
bool ScriptHostInit(ScriptHost *host, GameplayWorld *world);
void ScriptHostFree(ScriptHost *host);
ScriptHost *ScriptHostActive(void);

// Which frontend the classes declared from here on belong to.
void ScriptHostUseLanguage(ScriptHost *host, const ScriptLanguage *language);
// Applies anything a script asked for that had to wait for its callback to finish.
void ScriptHostFlush(ScriptHost *host);
// Drawing callbacks need to know how far between steps the frame is.
void ScriptHostSetAlpha(ScriptHost *host, float alpha);

ScriptClass *ScriptHostClass(ScriptHost *host, const char *name);
// The handle a script sees: the slot and enough of the generation to notice a stale one.
int ScriptHostIdOf(EntityHandle entity);
// Hands the finished class to the world. Nothing may be spawned from it before this.
bool ScriptClassRegister(ScriptHost *host, ScriptClass *type);
EntityHandle ScriptHostHandleOf(ScriptHost *host, int id);
#endif
