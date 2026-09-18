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
#define SCRIPT_CAMERA_CAPACITY 4
#define SCRIPT_AUDIO_CAPACITY 4
#define SCRIPT_COLLISION_CAPACITY 4
#define SCRIPT_PATHFINDER_CAPACITY 4
#define SCRIPT_MOVER_CAPACITY 8
#define SCRIPT_PATH_STEP_CAP 4096 // longest route a solved path will hold
#define SCRIPT_SAVE_CAPACITY 64
#define SCRIPT_SAVE_KEY 48
#define SCRIPT_SAVE_VALUE 128

// One slot per resource in a pool: the generation that increments on free, and whether it is live.
typedef struct ScriptResSlot { uint8_t generation; bool live; } ScriptResSlot;

// What the pathfinder's blocked-hex callback needs: the array pathfinder-block! fills and the
// grid bounds it came from. Passed as IsoBlockedFn's user pointer.
typedef struct ScriptHexBlocked { const bool *blocked; int width, height; } ScriptHexBlocked;

// A saved key/value pair, held as text so a file is the only format there is.
typedef struct ScriptSaveEntry { char key[SCRIPT_SAVE_KEY]; char value[SCRIPT_SAVE_VALUE]; } ScriptSaveEntry;

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
    const ScriptBinding *added[SCRIPT_ADDED_CAPACITY]; // the game's own calls, borrowed
    int addedCount;
    char sheetNames[SCRIPT_SHEET_CAPACITY][128];
    SpriteSheet sheets[SCRIPT_SHEET_CAPACITY];
    size_t sheetCount;
    char soundPaths[SCRIPT_SOUND_CAPACITY][128];
    Sound sounds[SCRIPT_SOUND_CAPACITY];
    size_t soundCount;
    bool audioReady;
    // Resource pools: generational handles for caller-owned engine services.
    ScriptResSlot cameraSlots[SCRIPT_CAMERA_CAPACITY];
    CoreCamera2D cameras[SCRIPT_CAMERA_CAPACITY];
    ScriptResSlot audioSlots[SCRIPT_AUDIO_CAPACITY];
    CoreAudio audios[SCRIPT_AUDIO_CAPACITY];
    bool audioInitialized[SCRIPT_AUDIO_CAPACITY];
    ScriptResSlot collisionSlots[SCRIPT_COLLISION_CAPACITY];
    Collision2DWorld collisions[SCRIPT_COLLISION_CAPACITY];
    bool collisionInitialized[SCRIPT_COLLISION_CAPACITY];
    ScriptResSlot pathfinderSlots[SCRIPT_PATHFINDER_CAPACITY];
    IsoPathfinder pathfinders[SCRIPT_PATHFINDER_CAPACITY];
    bool *pathfinderBlocked[SCRIPT_PATHFINDER_CAPACITY]; // heap-allocated blocked arrays
    int pathfinderWidth[SCRIPT_PATHFINDER_CAPACITY];
    int pathfinderHeight[SCRIPT_PATHFINDER_CAPACITY];
    bool pathfinderInitialized[SCRIPT_PATHFINDER_CAPACITY];
    ScriptHexBlocked pathfinderCtx[SCRIPT_PATHFINDER_CAPACITY];
    IsoPath paths[SCRIPT_PATHFINDER_CAPACITY];       // the route the last solve filled
    bool pathInitialized[SCRIPT_PATHFINDER_CAPACITY];
    ScriptResSlot moverSlots[SCRIPT_MOVER_CAPACITY];
    IsoMover movers[SCRIPT_MOVER_CAPACITY];
    bool moverInitialized[SCRIPT_MOVER_CAPACITY];
    CoreDebug debug;                                 // one queue for all debug drawing
    bool debugReady;
    ScriptSaveEntry saves[SCRIPT_SAVE_CAPACITY];     // script key/value persistence
    size_t saveCount;
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
/* Advances per-frame resources owned by the host: audio streams, and the debug queue, which is
   also drawn here. Call once per rendered frame from the project's Draw callback, after
   ScriptHostSetAlpha, because the debug overlay must draw inside BeginDrawing/EndDrawing. */
void ScriptHostUpdate(ScriptHost *host, double dt);

ScriptClass *ScriptHostClass(ScriptHost *host, const char *name);
// The handle a script sees: the slot and enough of the generation to notice a stale one.
int ScriptHostIdOf(EntityHandle entity);
// Hands the finished class to the world. Nothing may be spawned from it before this.
bool ScriptClassRegister(ScriptHost *host, ScriptClass *type);
/* A sprite sheet by name, loaded once and kept: `name` is the prefix the two files share, so
   "assets/walk_2" is "assets/walk_2.png" described by "assets/walk_2.sheet". A script names a sheet
   the same way it names a sound, and a project sharing this cache draws from the same copy rather
   than loading its own. NULL when it cannot be found or there is no room left. */
const SpriteSheet *ScriptHostSheet(ScriptHost *host, const char *name);
EntityHandle ScriptHostHandleOf(ScriptHost *host, int id);

/* Generic resource pool operations. Resolve returns the pool index when the handle is valid for
   the given kind, or -1. Create finds a free slot, marks it live, and returns the new handle.
   Destroy marks a slot dead and increments its generation. */
int ScriptResResolve(const ScriptResSlot *slots, int capacity, int handle, ScriptResourceKind kind);
int ScriptResCreate(ScriptResSlot *slots, int capacity, ScriptResourceKind kind, int *outIndex);
bool ScriptResDestroy(ScriptResSlot *slots, int capacity, int handle, ScriptResourceKind kind);
#endif
