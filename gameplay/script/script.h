/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef GAMEPLAY_SCRIPT_H
#define GAMEPLAY_SCRIPT_H
#include "core/audio.h"
#include "core/camera2d.h"
#include "core/collision2d.h"
#include "core/diagnostics.h"
#include "core/sprite_sheet.h"
#include "core/transform.h"
#include "gameplay/iso_move.h"
#include "gameplay/runtime.h"
#include "core/net_clock.h"
#include "core/texture.h"
#include "core/timer.h"
#include "script_api.h"

/* What the frontends share: the world, the classes scripts declared, and which entity's callback is
   running. A scripted class is an ordinary EntityClass whose Spawn, Think, Draw and Destroy dispatch
   into functions the script named, so from the world's side there is nothing special about it. */

#define SCRIPT_NAME_CAPACITY 48
#define SCRIPT_CLASS_FIELDS 8
#define SCRIPT_CLASS_CAPACITY 32
#define SCRIPT_SOUND_CAPACITY 16
#define SCRIPT_SHEET_CAPACITY 32
#define SCRIPT_ADDED_CAPACITY 32 // calls a game may add on top of the engine's
#define SCRIPT_SAVE_CAPACITY 64
#define SCRIPT_SAVE_KEY 48
#define SCRIPT_SAVE_VALUE 128

// A saved key/value pair, held as text so a file is the only format there is.
typedef struct ScriptSaveEntry { char key[SCRIPT_SAVE_KEY]; char value[SCRIPT_SAVE_VALUE]; } ScriptSaveEntry;

/* The payload every scripted entity carries: a transform, where it was a step ago so drawing can
   interpolate, the fields the class declared, and its handles -- in the world, and as an engine
   object, which is how a script reaches it. */
typedef struct ScriptEntity
{
    Transform2D transform;
    Transform2D previous;
    ScriptValue slots[SCRIPT_CLASS_FIELDS];
    EntityHandle entity;
    EngineObjectId object;
} ScriptEntity;

/* One per language. Call runs a function the script named, handing it the entity the callback is
   for as an engine object. */
typedef struct ScriptLanguage
{
    const char *name;
    void *user;
    bool (*Call)(void *user, const char *function, EngineObjectId self);
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
    /* The class as an engine type: its declared fields are its properties, and everything an
       entity can do comes from ScriptEntityType, its parent. */
    EngineProperty properties[SCRIPT_CLASS_FIELDS];
    EngineType type;
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
    const ScriptBinding *added[SCRIPT_ADDED_CAPACITY]; // the game's own calls, borrowed
    int addedCount;
    char sheetNames[SCRIPT_SHEET_CAPACITY][128];
    SpriteSheet sheets[SCRIPT_SHEET_CAPACITY];
    size_t sheetCount;
    char soundPaths[SCRIPT_SOUND_CAPACITY][128];
    Sound sounds[SCRIPT_SOUND_CAPACITY];
    size_t soundCount;
    bool audioReady;
    /* Every engine object a script can reach: the engine's types (cameras, audio, collision worlds,
       pathfinders, movers, clocks, textures, timers), the scripted entities, and anything a game
       adopts. Stepped by ScriptHostStep. */
    EngineObjects objects;
    CoreDebug debug;                                 // one queue for all debug drawing
    bool debugReady;
    ScriptSaveEntry saves[SCRIPT_SAVE_CAPACITY];     // script key/value persistence
    size_t saveCount;
};

/* What every scripted entity is as an engine object: position and rotation, classname, where it is
   drawn between steps; destroy!, think-next!, think-after!, rotate!, move-world!, move-local!,
   look-at! and to-local. A class's own type inherits all of it. */
extern const EngineType ScriptEntityType;

/* One host per program: entity callbacks reach it through EntityClass, which carries no user data
   of its own. Init points that at this host and Free clears it. Init registers the engine's object
   types, so a script can make any of them by name. */
bool ScriptHostInit(ScriptHost *host, GameplayWorld *world);
void ScriptHostFree(ScriptHost *host);
ScriptHost *ScriptHostActive(void);

// Which frontend the classes declared from here on belong to.
void ScriptHostUseLanguage(ScriptHost *host, const ScriptLanguage *language);
// Applies anything a script asked for that had to wait for its callback to finish.
void ScriptHostFlush(ScriptHost *host);
/* One simulation step for the objects scripts own: timers count down, movers walk, audio streams
   are fed, and their signals fire. Then anything a callback asked to wait for is applied. Call it
   once per fixed update, after the world has stepped. */
void ScriptHostStep(ScriptHost *host, float dt);
// Drawing callbacks need to know how far between steps the frame is.
void ScriptHostSetAlpha(ScriptHost *host, float alpha);
/* Draws the debug queue and ages it. Call once per rendered frame from the project's Draw callback,
   after ScriptHostSetAlpha, because the debug overlay must draw inside BeginDrawing/EndDrawing. */
void ScriptHostUpdate(ScriptHost *host, double dt);

/* Lets a game's own type be made by name from scripts, and its own storage be reached as an object
   (EngineObjectAdopt on host->objects), on the same footing as the engine's. */
bool ScriptHostRegisterType(ScriptHost *host, const EngineType *type);

ScriptClass *ScriptHostClass(ScriptHost *host, const char *name);
// The engine object a scripted entity is reached through, or ENGINE_OBJECT_NULL for any other.
EngineObjectId ScriptHostObjectOf(ScriptHost *host, EntityHandle entity);
// Hands the finished class to the world. Nothing may be spawned from it before this.
bool ScriptClassRegister(ScriptHost *host, ScriptClass *type);
/* A sprite sheet by name, loaded once and kept: `name` is the prefix the two files share, so
   "assets/walk_2" is "assets/walk_2.png" described by "assets/walk_2.sheet". A script names a sheet
   the same way it names a sound, and a project sharing this cache draws from the same copy rather
   than loading its own. NULL when it cannot be found or there is no room left. */
const SpriteSheet *ScriptHostSheet(ScriptHost *host, const char *name);
#endif
