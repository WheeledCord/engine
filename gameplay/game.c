/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The runner of a Scheme-only project (docs/developer/store.md §6): a store with the built-in 3D
// kinds, the game file on top, one fixed tick per Update and, in a window, one frame per Draw
// through the draw path. Everything lives in one static Runner: there is one s7 per process.
#define _DEFAULT_SOURCE /* sigaction under -std=c99 */
#include "s7.h"

#include "game.h"

#include "core/audio.h"
#include "core/draw_path.h"
#include "core/engine.h"
#include "core/file.h"
#include "core/input_map.h"
#include "core/particles.h"
#include "core/replay.h"
#include "core/shader.h"
#include "core/store.h"
#include "core/store_net.h"
#include "core/store_net_enet.h"
#include "core/viewmodel.h"
#include "core/world3d.h"
#include "gameplay/script/game_s7.h"
#include "raymath.h"
#include "rlgl.h"
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define RUN_DT (1.0 / 60.0)
#define MAX_COMMANDS 64
#define MAX_CHUNKS 1024
#define MAX_MODELS 128
#define MAX_TEXTURES 128
#define MAX_SOUNDS 64
#define MAX_MATERIALS 512
#define MAX_HUD 4096
#define MAX_PROFILE 256
#define NAME 64
#define MAX_CLIP_WARNINGS 64
/* Skinned models (docs/developer/store.md §3 Animation, proposal B9.3): up to GPU_SKIN_BONES bones
   are skinned on the GPU by core/shaders/skinning.vs built with that many (with mvp, matModel and
   matNormal they fit GLSL 120's 512 vertex uniform components); more, on the CPU by raylib's
   UpdateModelAnimation. */
#define GPU_SKIN_BONES 24
/* raylib bakes a glTF clip into frames 17 ms apart (rmodels.c, GLTF_ANIMDELAY); a model thing's
   clock picks the frame. */
#define CLIP_FRAME_SECONDS 0.017
#define MAX_ANIMATION_STEP 0.1f /* a hitch longer than this advances a clock by only this much */

typedef struct ChunkEntry
{
    StoreId map;
    int chunkX, chunkZ;
    uint32_t mesh[3]; /* floor, wall, ceiling; 0 when empty */
} ChunkEntry;

typedef struct ModelEntry
{
    char name[NAME];
    Model model;
    uint32_t *meshes; /* draw path ids, per model mesh */
    bool placeholder; /* the file is missing: a magenta cube stands in */
    ModelAnimation *clips; /* the file's clips that fit its skeleton; NULL for none */
    int clipCount;
    bool cpu; /* skinned on the CPU: more than GPU_SKIN_BONES bones, or no skinning shader */
} ModelEntry;

/* A drawn model thing with clips, presentation only: its own clock and pose (§3 Animation). */
typedef struct Pose
{
    StoreId thing;
    const ModelEntry *model; /* what the pose was made for; another model starts afresh */
    StoreSymbol animation;   /* as last seen, to restart the clock when it changes */
    double clock;
    int clip, frame; /* clip -1: the rest pose */
    Matrix *bones;     /* GPU skinning: the thing's bone matrices, which its draw items point at */
    uint32_t *meshes;  /* CPU skinning: the thing's own draw path meshes, per model mesh */
    int meshCount;
} Pose;

/* Static batching (docs/developer/store.md §3.1, proposal B9.3): a model that is `:static`, or hangs
   under a thing that is, is merged with the others of its region into one world-space mesh per
   material at the first frame it is drawn, and the batch is drawn instead of it. */
#define STATIC_REGION_CELLS 8 /* a region is a tilemap chunk: 8x8 cells */
typedef enum StaticState
{
    STATIC_NONE,    /* not batched (yet): drawn as itself */
    STATIC_BATCHED, /* its items ride in its region's batch */
    STATIC_MOVED    /* it moved after it was batched (an error): drawn as itself from then on */
} StaticState;

typedef struct StaticModel
{
    StoreId thing;
    StaticState state;
    int region;
    uint64_t look;   /* what it was batched with (LookOf): another look unbatches it */
    DrawItem *items; /* its world-space items, as they went into the batch */
    int itemCount;
    /* Where it was batched: its gameplay world matrix (World3DWorldMatrix, so a moved ancestor
       counts too), its own position, rotation and scale (to say whether it or an ancestor moved)
       and its :static thing. */
    Matrix world;
    Vector3 position, rotation, scale;
    StoreId root;
    uint32_t movedFrame; /* the CheckStatics pass that took it out as moved: one error per root there */
} StaticModel;

/* A tilemap's 8x8-cell region (map null: things standing on no tilemap) and its batches. */
typedef struct StaticRegion
{
    StoreId map;
    int x, z;
    bool dirty;
    DrawItem *batches;
    int batchCount;
} StaticRegion;

/* A `sound` thing's voice (§3.1): a held core/audio.h voice looping its stream where it is. */
typedef struct Emitter
{
    StoreId thing;
    bool used;       /* the slot holds a thing's emitter */
    char stream[NAME];
    CoreAudioVoice voice;
    bool tried;      /* a voice was asked for this stream (once: no device or no file stays silent) */
    bool playing;
} Emitter;

/* The world pass's shader and its lighting uniforms: 0 is core/shaders/world.vs, 1 the skinning
   vertex shader for GPU_SKIN_BONES bones, both over world.fs. */
typedef struct WorldPass
{
    Shader shader;
    bool loaded;
    int normalLoc, lightDirLoc, lightColorLoc, ambientLoc, fogColorLoc, fogDensityLoc, viewPosLoc;
    int pointPositionLoc, pointColorLoc, pointRangeLoc; /* arrays of POINT_LIGHTS */
} WorldPass;

/* Point lights the world shader lights each pixel by (core/shaders/world.fs, §3.1). */
#define POINT_LIGHTS 4

typedef struct TextureEntry
{
    char name[NAME];
    Texture2D texture; /* id 0 when missing */
} TextureEntry;

typedef struct SoundEntry
{
    char name[NAME];
    char path[1024]; /* as CoreAudio caches it */
    bool loaded;
} SoundEntry;

typedef struct MaterialEntry
{
    unsigned int texture;
    Color tint;
    bool skinned;
    uint32_t id;
} MaterialEntry;

typedef enum HudType
{
    HUD_TEXT,
    HUD_RECT,
    HUD_RING,
    HUD_IMAGE
} HudType;

typedef struct HudCall
{
    HudType type;
    char text[128];
    float x, y, w, h;
    int size, align; /* align: 0 left, 1 centre, 2 right */
    Color color;
} HudCall;

typedef struct ProfileEntry
{
    char key[NAME];
    char value[192];
} ProfileEntry;

typedef struct Samples
{
    double *items;
    size_t count, capacity;
} Samples;

typedef struct Runner
{
    // Options.
    char dir[512], gameFile[1024], title[128], prelude[1024], worldVs[1024], worldFs[1024], fontPath[1024],
        skinVs[1024];
    const char *recordPath, *replayPath, *savePath, *loadPath, *shotDir;
    bool headless, bot, bench, present;
    uint64_t maxTicks, seed, hashEvery, shotEvery, lastShot;
    // The world.
    Store store;
    World3D world;
    bool storeOpen, worldOpen, scriptOpen, ready, ended;
    GameInput input;
    uint64_t botState;
    Replay record, replay;
    bool recording, replaying;
    int skipCommands; /* commands queued while loading, which the load queues again on replay */
    uint64_t lastPrinted;
    bool printedAny, warnedArgs;
    // Timing.
    Samples ticks, frames, hudCalls;
    uint64_t updates, frameCount;
    DrawStats lastStats;
    // Presentation (windowed only).
    bool gl, audioReady, replGreeted, fontLoaded;
    DrawPath path;
    WorldPass passes[2];
    Pose *poses; /* per thing index */
    uint32_t poseCapacity;
    char clipWarnings[MAX_CLIP_WARNINGS][NAME * 2]; /* "model clip" pairs already warned about */
    int clipWarningCount;
    bool skinCpuForced; /* --skin-on-cpu: every skinned model on the CPU path (a check's switch) */
    const char *printDrawKind; /* --print-draw-position KIND */
    bool noStaticBatch;        /* --no-static-batch: every static model drawn as itself (a check's switch) */
    StaticModel *statics;      /* per thing index */
    uint32_t staticCapacity;
    StaticRegion *regions;
    int regionCount, regionCapacity;
    DrawItem *scratch; /* one model's items while they are built */
    int scratchCapacity;
    DrawItem *viewItems; /* this frame's viewmodel items, for the viewmodel pass */
    int viewCount, viewCapacity;
    float viewmodelFov; /* the local camera's viewmodel-fov this frame */
    int pointLights;    /* point lights lit on the last frame */
    Emitter *emitters;  /* per thing index */
    uint32_t emitterCapacity;
    Texture2D white, grey;
    Font font;
    CoreAudio audio;
    CoreMouseCapture capture;
    bool captureWanted, cursorReported, overlay;
    Vector2 mouseDelta; /* captured since the last tick */
    InputMap keys;      /* one action per define-actions entry, in its order: the player's key bindings */
    CoreDebug debug;
    int missingAssets;
    char overlayText[3][128]; /* the overlay's lines on the last drawn frame, shown or not */
    struct
    {
        char text[256];
        uint64_t tick;
        double at;
    } errors[3];
    int errorCount;
    CoreParticles particles;
    uint64_t fxState;
    ChunkEntry chunks[MAX_CHUNKS];
    int chunkCount;
    ModelEntry models[MAX_MODELS];
    int modelCount;
    TextureEntry textures[MAX_TEXTURES];
    int textureCount;
    SoundEntry sounds[MAX_SOUNDS];
    int soundCount;
    MaterialEntry materials[MAX_MATERIALS];
    int materialCount;
    HudCall hud[MAX_HUD];
    int hudCount;
    ProfileEntry profile[MAX_PROFILE];
    int profileCount;
    bool profileRead;
    // Networking (docs/developer/store.md §9.6).
    int hostPort, joinPort;  /* --host PORT, --join ADDRESS:PORT; 0 when not given */
    char joinAddress[256];
    uint64_t botUntil;       /* --bot-until N: no bot input after tick N; 0 for none */
    const char *printKind, *printField, *countKind; /* --print-field KIND FIELD, --print-count KIND */
    bool selfStop;           /* the runner counts its ticks itself: in a window, or in a live session */
    bool lastTicked;         /* the last Update ran a tick (a client waiting for its welcome does not) */
    StoreNetLink link;       /* the session: link.net, over ENet (core/store_net_enet.h) when live */
    StoreNetConfig netConfig;
    bool netOpen;            /* run.link.net is in use */
    bool netLive;            /* ENet under it; false when replaying, when packets come from the file */
    bool arrived, failed;
    char netWhy[256];        /* why the session ended */
    double netStarted, paceStart, sleptMicros;
    uint64_t paceCount;
    ReplayPacket *pending;   /* arrived since the last recorded tick, for the recording */
    int pendingCount, pendingCapacity;
    ReplayCommand captured[MAX_COMMANDS]; /* commands queued for the next tick from outside it */
    int capturedCount;
    // The REPL in recordings (docs/developer/store.md §5.7).
    struct
    {
        char *text;        /* the line, malloced */
        int before, after; /* the pending player commands before and after it ran */
    } repl[MAX_COMMANDS]; /* REPL lines since the last recorded tick */
    int replCount;
    const char *replFilePath; /* --repl-file FILE */
    struct
    {
        uint64_t tick;
        char *text;
    } *replFile; /* its lines, in file order */
    int replFileCount, replFileNext;
} Runner;

static Runner run;
static uint64_t lastHash;

/* Ctrl+C: the first only sets flags. Update ends the run at its next call, and the handler time
   limit's checks stop a handler that is running, so a loop in one does not hold the exit up. A
   second Ctrl+C in the same run kills the process, for a loop those checks cannot see. */
static volatile sig_atomic_t interruptSeen;
static bool interruptSaid;

static void OnInterrupt(int number)
{
    if (interruptSeen)
    {
        signal(number, SIG_DFL);
        raise(number);
        return;
    }
    interruptSeen = 1;
    GameS7SetInterrupted(true);
}

static bool Interrupted(void)
{
    if (interruptSeen && !interruptSaid)
    {
        interruptSaid = true;
        printf("run: interrupted (press Ctrl+C again to kill)\n");
        fflush(stdout);
    }
    return interruptSeen != 0;
}

uint64_t GameLastHash(void) { return lastHash; }

static uint64_t SplitMix(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static bool Readable(const char *path)
{
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (file)
        fclose(file);
    return file != NULL;
}

// A game asset: beside the project first, then through the data root.
static const char *AssetPath(const char *name, char *buf, size_t size)
{
    if (!name || !name[0])
        return NULL;
    if (name[0] != '/' && (size_t)snprintf(buf, size, "%s/%s", run.dir, name) < size && Readable(buf))
        return buf;
    const char *resolved = CoreResolvePath(name, buf, size);
    return resolved && Readable(resolved) ? resolved : NULL;
}

static bool Field(StoreId id, const char *name, StoreValue *out)
{
    int f = StoreFieldIndex(&run.store, StoreKindOf(&run.store, id), name);
    return f >= 0 && StoreGet(&run.store, id, f, out);
}

static bool FieldTrue(StoreId id, const char *name)
{
    StoreValue v;
    return Field(id, name, &v) && v.type == STORE_BOOL && v.as.b;
}

static float FieldFloat(StoreId id, const char *name, float fallback)
{
    StoreValue v;
    if (!Field(id, name, &v))
        return fallback;
    return v.type == STORE_FLOAT ? v.as.f : v.type == STORE_INT ? (float)v.as.i : fallback;
}

static void Push(Samples *s, double value)
{
    if (s->count == s->capacity)
    {
        size_t capacity = s->capacity ? s->capacity * 2 : 1024;
        double *items = realloc(s->items, capacity * sizeof *items);
        if (!items)
            return;
        s->items = items;
        s->capacity = capacity;
    }
    s->items[s->count++] = value;
}

static int CompareDoubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void PrintSamples(const char *what, Samples *s)
{
    if (!s->count)
        return;
    qsort(s->items, s->count, sizeof *s->items, CompareDoubles);
    printf("bench %s count %zu p50 %.1f p99 %.1f max %.1f us\n", what, s->count,
           s->items[(s->count - 1) * 50 / 100], s->items[(s->count - 1) * 99 / 100],
           s->items[s->count - 1]);
}

static void PrintHash(void)
{
    uint64_t tick = StoreTickCount(&run.store);
    lastHash = StoreHash(&run.store);
    printf("tick %" PRIu64 " hash %016" PRIx64 "\n", tick, lastHash);
    fflush(stdout);
    run.lastPrinted = tick;
    run.printedAny = true;
}

/* ---- input ----------------------------------------------------------------------------------- */

// The key names define-actions and rebind! take, besides one letter or digit and Mouse1 to Mouse3.
static const struct
{
    const char *name;
    int code;
} keyNames[] = {{"Space", KEY_SPACE},   {"LeftShift", KEY_LEFT_SHIFT}, {"LeftControl", KEY_LEFT_CONTROL},
                {"Escape", KEY_ESCAPE}, {"Enter", KEY_ENTER},          {"Tab", KEY_TAB},
                {"Up", KEY_UP},         {"Down", KEY_DOWN},            {"Left", KEY_LEFT},
                {"Right", KEY_RIGHT}};
static const int mouseButtons[] = {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE};

// A key name such as "W", "Space" or "Mouse1" as the binding it means; false for a name not known.
static bool ParseKey(const char *name, InputBinding *out)
{
    if (!name)
        return false;
    if (name[0] && !name[1])
    {
        char c = name[0];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z')
            return *out = (InputBinding){INPUT_KEY, KEY_A + (c - 'A')}, true;
        if (c >= '0' && c <= '9')
            return *out = (InputBinding){INPUT_KEY, KEY_ZERO + (c - '0')}, true;
    }
    for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
        if (!strcmp(name, keyNames[i].name))
            return *out = (InputBinding){INPUT_KEY, keyNames[i].code}, true;
    if (!strncmp(name, "Mouse", 5) && name[5] >= '1' && name[5] <= '3' && !name[6])
        return *out = (InputBinding){INPUT_MOUSE_BUTTON, mouseButtons[name[5] - '1']}, true;
    return false;
}

// The name ParseKey takes for a binding: a table entry, or buf (2 characters) holding a letter or
// digit; NULL for a key the table does not name.
static const char *BindingName(InputBinding binding, char *buf)
{
    if (binding.type == INPUT_MOUSE_BUTTON)
    {
        static const char *const names[] = {"Mouse1", "Mouse2", "Mouse3"};
        for (size_t i = 0; i < sizeof mouseButtons / sizeof mouseButtons[0]; i++)
            if (binding.code == mouseButtons[i])
                return names[i];
        return NULL;
    }
    if (binding.type != INPUT_KEY)
        return NULL;
    if (binding.code >= KEY_A && binding.code <= KEY_Z)
        return buf[0] = (char)('A' + binding.code - KEY_A), buf[1] = 0, buf;
    if (binding.code >= KEY_ZERO && binding.code <= KEY_NINE)
        return buf[0] = (char)('0' + binding.code - KEY_ZERO), buf[1] = 0, buf;
    for (size_t i = 0; i < sizeof keyNames / sizeof keyNames[0]; i++)
        if (binding.code == keyNames[i].code)
            return keyNames[i].name;
    return NULL;
}

// The key bindings: one action per define-actions entry with its default key, then the player's
// changes from the project's input.map when there is one. An entry whose key name is not known has no
// binding. False when the actions cannot be set up (a name is repeated).
static bool OpenKeys(void)
{
    InputBinding defaults[GAME_S7_MAX_ACTIONS];
    InputMapDefinition definitions[GAME_S7_MAX_ACTIONS];
    int count = GameS7ActionCount();
    for (int i = 0; i < count; i++)
    {
        bool known = ParseKey(GameS7ActionKey(i), &defaults[i]);
        definitions[i] = (InputMapDefinition){GameS7ActionName(i), &defaults[i], known ? 1u : 0u};
    }
    if (!InputMapInit(&run.keys, definitions, (size_t)count))
        return false;
    char path[sizeof run.dir + 16];
    snprintf(path, sizeof path, "%s/input.map", run.dir);
    if (Readable(path) && !InputMapRead(&run.keys, path))
        fprintf(stderr, "trench: %s does not fit this game's actions, or gives two actions one key; "
                        "using the default keys\n",
                path);
    return true;
}

static GameInput FromEngine(const EngineInput *in)
{
    GameInput input = {0};
    for (int i = 0; in && i < GameS7ActionCount() && (size_t)i < run.keys.count && i < 32; i++)
    {
        // Held when any of the action's bindings is down, pressed when any was pressed: a binding is
        // read as InputActionRead reads it, except that Escape releases the mouse instead of acting.
        const InputMapAction *action = &run.keys.actions[i];
        for (size_t j = 0; j < action->count; j++)
        {
            InputBinding b = action->bindings[j];
            bool down = false, pressed = false;
            if (b.type == INPUT_KEY && b.code > KEY_NULL && b.code < CORE_KEY_COUNT && b.code != KEY_ESCAPE)
                down = in->down[b.code], pressed = in->pressed[b.code];
            else if (b.type == INPUT_MOUSE_BUTTON && b.code >= 0 && b.code < CORE_MOUSE_BUTTON_COUNT)
                down = in->mouseDown[b.code], pressed = in->mousePressed[b.code];
            input.held |= down ? 1u << i : 0u;
            input.pressed |= pressed ? 1u << i : 0u;
        }
    }
    // The captured pointer's motion since the last tick, which the first tick of a frame takes.
    input.mouseDx = run.mouseDelta.x;
    input.mouseDy = run.mouseDelta.y;
    run.mouseDelta = (Vector2){0, 0};
    return input;
}

// A random walk from the seed: each held bit flips with probability 1/60 per tick.
static GameInput FromBot(void)
{
    GameInput input = {0};
    uint32_t held = run.input.held;
    for (int i = 0; i < GameS7ActionCount() && i < 32; i++)
        if (SplitMix(&run.botState) % 60 == 0)
            held ^= 1u << i;
    input.held = held;
    input.pressed = held & ~run.input.held;
    input.mouseDx = (float)((int)(SplitMix(&run.botState) % 17) - 8);
    input.mouseDy = (float)((int)(SplitMix(&run.botState) % 17) - 8);
    return input;
}

/* ---- networking (docs/developer/store.md §9.6) ---------------------------------------------- */

#define NET_LINGER_SECONDS 5.0 /* a headless host that finished waits this long for its clients */

static bool PlaySoundHere(const char *name, bool placed, Vector3 at);
static void OnError(const char *message);
static int PresetNamed(const char *name);
static bool BurstHere(int which, Vector3 at);

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int LocalPlayer(void) { return run.netOpen && run.link.net.player ? run.link.net.player : 1; }

// Replaying, store_net's packets are dropped: the recording holds what came back. Live, the link sends.
static bool NetDrop(void *user, int peer, int channel, bool reliable, const void *data, size_t size)
{
    (void)user, (void)peer, (void)channel, (void)reliable, (void)data, (void)size;
    return true;
}

static void NetArrived(void *user, StoreId thing)
{
    (void)user;
    (void)thing;
    run.arrived = true; // their :local children are spawned after StoreNetBeforeTick
}

// Another machine's gameplay effect: shown here in a window; headless shows nothing.
static void NetEffect(void *user, const char *name, const char *what, Vector3 at)
{
    (void)user;
    if (!strcmp(name, "play-sound"))
        PlaySoundHere(what, !isnan(at.x), at);
    else if (!strcmp(name, "burst"))
        BurstHere(PresetNamed(what), at);
}

static void NetJoined(void *user, int player)
{
    (void)user;
    int owners[1] = {player};
    StoreSetLocalOwners(&run.store, owners, 1); /* store_net set these; the runner's view is the same */
    if (run.recording && run.record.role == REPLAY_ROLE_CLIENT)
        ReplaySetRole(&run.record, REPLAY_ROLE_CLIENT, player);
    printf("net: joined as player %d\n", player);
    fflush(stdout);
}

static void NetEnded(void *user, const char *why)
{
    (void)user;
    snprintf(run.netWhy, sizeof run.netWhy, "%s", why ? why : "the session ended");
}

// The runner registers node's position and rotation for interpolation (§9.2).
static void NetInterpolate(void)
{
    if (run.netLive)
    {
        StoreNetLinkInterpolate(&run.link, run.world.node, "position");
        StoreNetLinkInterpolate(&run.link, run.world.node, "rotation");
        return;
    }
    StoreNetInterpolate(&run.link.net, run.world.node, "position");
    StoreNetInterpolate(&run.link.net, run.world.node, "rotation");
}

static void Note(int peer, int channel, const void *data, size_t size);

// Every packet and transport event the link receives, kept for the recording.
static void NetTap(void *user, int peer, int channel, const void *data, size_t size)
{
    (void)user;
    Note(peer, channel, data, size);
    if (channel == REPLAY_PEER_LEFT)
    {
        printf("net: player %d left\n", peer);
        fflush(stdout); /* as the joined and hosting lines are: something may be watching the log */
    }
}

/* Starts a session: from --host/--join (atStart, before the first tick), from (host-game) or
   (join-game), or from a recording's header or event. Live, core/store_net_enet.h opens ENet under it;
   replaying opens no socket. A live client says hello once ENet has connected (the link does). */
static bool NetStart(bool host, const char *address, int port, bool atStart, char *why, size_t size)
{
    if (run.netOpen)
        return snprintf(why, size, "a session is open already"), false;
    bool live = !run.replaying;
    run.netConfig = (StoreNetConfig){NULL, NetDrop, NetArrived, NetEffect, NetJoined, NetEnded, run.title, false};
    bool ok;
    if (live)
    {
        ok = host ? StoreNetLinkHost(&run.link, &run.store, &run.netConfig, (uint16_t)port)
                  : StoreNetLinkJoin(&run.link, &run.store, &run.netConfig, address, (uint16_t)port);
        if (!ok)
        {
            snprintf(why, size, "%s", StoreNetLinkEnded(&run.link));
            StoreNetLinkClose(&run.link, 0);
            return false;
        }
        StoreNetLinkSetTap(&run.link, NetTap, NULL);
    }
    else
    {
        memset(&run.link, 0, sizeof run.link);
        ok = host ? StoreNetHost(&run.link.net, &run.store, &run.netConfig)
                  : StoreNetJoin(&run.link.net, &run.store, &run.netConfig);
        if (!ok)
        {
            StoreNetFree(&run.link.net);
            return snprintf(why, size, "the store could not start networking"), false;
        }
    }
    run.netOpen = true;
    run.netLive = live;
    NetInterpolate();
    run.netWhy[0] = 0;
    run.netStarted = Now();
    if (!atStart)
        Note(0, host ? REPLAY_NET_HOSTED : REPLAY_NET_JOINED, NULL, 0);
    if (live && host)
        printf("net: hosting on UDP port %d\n", port);
    else if (live)
        printf("net: joining %s:%d\n", address, port);
    fflush(stdout);
    return true;
}

// Keeps a packet or transport event for the recording, which writes it with the next tick.
static void Note(int peer, int channel, const void *data, size_t size)
{
    if (!run.recording)
        return;
    if (run.pendingCount == run.pendingCapacity)
    {
        int capacity = run.pendingCapacity ? run.pendingCapacity * 2 : 64;
        ReplayPacket *grown = realloc(run.pending, (size_t)capacity * sizeof *grown);
        if (!grown)
            return;
        run.pending = grown;
        run.pendingCapacity = capacity;
    }
    ReplayPacket *p = &run.pending[run.pendingCount];
    p->data = size ? malloc(size) : NULL;
    if (size && !p->data)
        return;
    if (size)
        memcpy(p->data, data, size);
    p->peer = peer;
    p->channel = channel;
    p->size = size;
    run.pendingCount++;
}

static void ClearPending(void)
{
    for (int i = 0; i < run.pendingCount; i++)
        free(run.pending[i].data);
    run.pendingCount = 0;
}

// Delivers one packet or transport event from a recording to store_net.
static void NetDeliver(int peer, int channel, const void *data, size_t size)
{
    char why[256];
    if (channel == REPLAY_NET_HOSTED || channel == REPLAY_NET_JOINED)
    {
        if (!NetStart(channel == REPLAY_NET_HOSTED, NULL, 0, false, why, sizeof why))
            TraceLog(LOG_WARNING, "RUN: the recording's session could not start: %s", why);
    }
    else if (channel == REPLAY_PEER_CONNECTED)
        StoreNetPeerConnected(&run.link.net, peer);
    else if (channel == REPLAY_PEER_LEFT)
        StoreNetPeerLeft(&run.link.net, peer);
    else
        StoreNetReceive(&run.link.net, peer, channel, data, size);
}

// A live session run headless keeps to the tick rate, so that machines meet: headless otherwise
// ticks as fast as it can. The sleep is left out of --bench's tick times.
static void Pace(void)
{
    double now = Now();
    if (!run.paceCount || now - (run.paceStart + (double)run.paceCount * RUN_DT) > 0.25)
        run.paceStart = now - (double)run.paceCount * RUN_DT; // started, or fell behind: no catching up
    double due = run.paceStart + (double)run.paceCount++ * RUN_DT;
    run.sleptMicros = 0;
    if (due > now)
    {
        struct timespec wait = {(time_t)(due - now), (long)((due - now - (double)(time_t)(due - now)) * 1e9)};
        nanosleep(&wait, NULL);
        run.sleptMicros = (Now() - now) * 1e6;
    }
}

// Ends the session: disconnects cleanly (a headless host at the end of its --ticks first waits up to
// 5 s for its clients to finish theirs) and puts the store's hooks back.
static void NetClose(void)
{
    if (run.netLive)
        StoreNetLinkClose(&run.link, run.headless && run.ended ? NET_LINGER_SECONDS : 0.0);
    else if (run.netOpen)
        StoreNetFree(&run.link.net);
    run.netOpen = run.netLive = false;
    ClearPending();
    free(run.pending);
    run.pending = NULL;
    run.pendingCapacity = 0;
}

// Why the session ended, once, and the process will exit nonzero.
static bool NetEndedRun(void)
{
    printf("run: the session ended: %s\n", run.netWhy[0] ? run.netWhy : "the host left the game");
    fflush(stdout);
    run.failed = true;
    return false;
}

// The calls game_s7.c answers local-player, players, host-game and join-game from.
static int NetPlayer(void *user)
{
    (void)user;
    return LocalPlayer();
}

static int NetPlayers(void *user, int *out, int max)
{
    (void)user;
    if (run.netOpen && run.link.net.joined)
        return StoreNetPlayers(&run.link.net, out, max);
    if (max > 0)
        out[0] = 1;
    return 1;
}

// A session is open: hosting, joined or joining, or replaying one (§5.7: no kind changes, no REPL
// writes to others' things).
static bool NetSession(void *user)
{
    (void)user;
    return run.netOpen;
}

static bool NetHostCall(void *user, int port, char *why, size_t size)
{
    (void)user;
    return NetStart(true, NULL, port, false, why, size);
}

static bool NetJoinCall(void *user, const char *address, int port, char *why, size_t size)
{
    (void)user;
    snprintf(run.joinAddress, sizeof run.joinAddress, "%s", address);
    run.joinPort = port;
    return NetStart(false, run.joinAddress, port, false, why, size);
}

/* ---- the tick -------------------------------------------------------------------------------- */

static void ClearRepl(void)
{
    for (int i = 0; i < run.replCount; i++)
        free(run.repl[i].text);
    run.replCount = 0;
}

/* A REPL line (typed, or from --repl-file): evaluated in the game, its answer printed with format,
   and, when recording, kept to be written with the next tick as a developer command (§5.7). */
static void EvalRepl(const char *line, const char *format)
{
    int before = StoreCommandsPending(&run.store, NULL, NULL, NULL, NULL, 0);
    char *answer = NULL;
    GameS7Eval(line, &answer);
    int after = StoreCommandsPending(&run.store, NULL, NULL, NULL, NULL, 0);
    printf(format, answer ? answer : "");
    fflush(stdout);
    free(answer);
    if (!run.recording)
        return;
    size_t length = strcspn(line, "\r\n");
    char *text = run.replCount < MAX_COMMANDS ? malloc(length + 1) : NULL;
    if (!text)
    {
        TraceLog(LOG_WARNING, "RUN: a REPL line could not be recorded; the replay will differ");
        return;
    }
    memcpy(text, line, length);
    text[length] = 0;
    run.repl[run.replCount].text = text;
    run.repl[run.replCount].before = before;
    run.repl[run.replCount++].after = after > before ? after : before;
}

// The commands queued for the next tick from outside it (the REPL, a key press, setup), taken
// before the network adds its own: those come back from the recorded packets on replay.
static void CaptureCommands(void)
{
    static StoreId targets[MAX_COMMANDS];
    static StoreSymbol events[MAX_COMMANDS];
    static StoreValue args[MAX_COMMANDS][STORE_MAX_ARGS];
    static int counts[MAX_COMMANDS];
    ReplayCommand *out = run.captured;
    int n = StoreCommandsPending(&run.store, targets, events, args, counts, MAX_COMMANDS);
    if (n > MAX_COMMANDS)
    {
        TraceLog(LOG_WARNING, "RUN: %d commands in one tick; recording the first %d", n, MAX_COMMANDS);
        n = MAX_COMMANDS;
    }
    // REPL lines go in where they ran; the commands a line queued are not recorded, since replaying
    // the line queues them again.
    int k = 0, r = 0;
    for (int i = run.skipCommands; i <= n && k < MAX_COMMANDS; i++)
    {
        while (r < run.replCount && run.repl[r].before <= i && k < MAX_COMMANDS)
        {
            memset(&out[k], 0, sizeof out[k]);
            out[k].target = STORE_NULL;
            out[k].kind = REPLAY_COMMAND_REPL;
            out[k++].text = run.repl[r++].text;
        }
        bool fromRepl = false;
        for (int j = 0; j < run.replCount; j++)
            fromRepl = fromRepl || (i >= run.repl[j].before && i < run.repl[j].after);
        if (i == n || fromRepl || k == MAX_COMMANDS)
            continue;
        memset(&out[k], 0, sizeof out[k]);
        out[k].target = targets[i];
        const char *name = StoreSymbolName(&run.store, events[i]);
        snprintf(out[k].event, sizeof out[k].event, "%s", name ? name : "");
        out[k].count = counts[i] < REPLAY_MAX_ARGS ? counts[i] : REPLAY_MAX_ARGS;
        if (counts[i] > REPLAY_MAX_ARGS && !run.warnedArgs)
        {
            run.warnedArgs = true;
            TraceLog(LOG_WARNING, "RUN: a recorded command keeps its first %d arguments", REPLAY_MAX_ARGS);
        }
        memcpy(out[k].args, args[i], (size_t)out[k].count * sizeof args[i][0]);
        k++;
    }
    run.capturedCount = k;
}

static void RecordTick(const GameInput *input)
{
    int packets = run.pendingCount < 65535 ? run.pendingCount : 65535;
    if (packets < run.pendingCount)
        TraceLog(LOG_WARNING, "RUN: %d packets in one tick; recording the first 65535", run.pendingCount);
    ReplayTick tick = {input->held, input->pressed, input->mouseDx, input->mouseDy, (uint16_t)run.capturedCount,
                       (uint16_t)packets};
    if (!ReplayWriteTick(&run.record, &tick, run.captured, run.pending))
        TraceLog(LOG_WARNING, "RUN: could not write tick %" PRIu64 " to %s", StoreTickCount(&run.store),
                 run.recordPath);
    run.skipCommands = 0;
    ClearPending();
    ClearRepl();
}

// The next recorded tick: its input, its commands queued again, its packets handed to store_net.
static bool ReplayInput(GameInput *input)
{
    static ReplayCommand commands[MAX_COMMANDS];
    ReplayTick tick;
    if (!ReplayReadTick(&run.replay, &tick, commands, MAX_COMMANDS))
        return false;
    *input = (GameInput){tick.actions, tick.pressed, tick.mouseDx, tick.mouseDy};
    int n = tick.commandCount < MAX_COMMANDS ? tick.commandCount : MAX_COMMANDS;
    for (int i = 0; i < n; i++)
    {
        ReplayCommand *c = &commands[i];
        if (c->kind == REPLAY_COMMAND_REPL)
        {
            // Evaluated again where it ran, its answer discarded (§5.7).
            char *answer = NULL;
            GameS7Eval(c->text ? c->text : "", &answer);
            free(answer);
            free(c->text);
            c->text = NULL;
            continue;
        }
        if (!StoreCommand(&run.store, c->target, StoreIntern(&run.store, c->event), c->args, c->count))
            TraceLog(LOG_WARNING, "RUN: replayed command %s refused: %s", c->event, StoreLastError(&run.store));
    }
    for (int i = 0; i < tick.packetCount; i++)
    {
        ReplayPacket p;
        if (!ReplayReadPacket(&run.replay, &p))
            return false;
        NetDeliver(p.peer, p.channel, p.data, p.size);
        free(p.data);
    }
    return true;
}

// A presentation frame without GL (--present): the frame and -changed handlers and draw-hud run and
// fill the HUD list, which is counted and cleared; nothing is drawn.
static void PresentHeadless(float dt)
{
    World3DUpdateTransforms(&run.world, 1.0f);
    StoreFrame(&run.store, dt);
    Push(&run.hudCalls, run.hudCount);
    run.hudCount = 0;
}

/* One tick (§6.2, §9.6): poll the network -> StoreNetBeforeTick -> input, World3DBeginTick,
   StoreTick -> StoreNetAfterTick -> flush. A client ticks nothing but the network until its welcome,
   and its tick count starts there. */
static bool Update(void *context, double dt, const EngineInput *in)
{
    (void)context;
    const CoreDiagnostics *d = CoreDiagnosticsCurrent();
    if (run.bench && run.lastTicked && d)
        Push(&run.ticks, d->tickMicrosLast - run.sleptMicros);
    run.lastTicked = false;
    run.sleptMicros = 0;
    if (Interrupted())
        return false;
    // Windowed, the runner stops the run itself, a call after the last tick, so that tick is drawn;
    // in a live session too, since a client's ticks start at its welcome.
    if (run.selfStop && run.maxTicks && run.updates >= run.maxTicks)
    {
        run.ended = true;
        return false;
    }
    if (run.netLive && run.headless)
        Pace();
    // --repl-file: its lines due before this tick, as if typed at the REPL since the last one.
    while (run.replFileNext < run.replFileCount && run.replFile[run.replFileNext].tick <= StoreTickCount(&run.store))
        EvalRepl(run.replFile[run.replFileNext++].text, "repl: %s\n");
    if (run.recording)
        CaptureCommands();
    GameInput input = {0};
    if (run.replaying)
    {
        if (!ReplayInput(&input))
        {
            run.ended = true;
            return false;
        }
    }
    else if (run.netLive)
    {
        StoreNetLinkPoll(&run.link, 0); // refused, no welcome in 5 s, or the host left: the link says why
        if (StoreNetLinkEnded(&run.link))
            snprintf(run.netWhy, sizeof run.netWhy, "%s", StoreNetLinkEnded(&run.link)), run.failed = true;
    }
    if (run.netOpen && (run.link.net.ended || run.failed))
        return NetEndedRun();
    if (run.netOpen && !run.link.net.joined)
        return !Interrupted();
    run.updates++;
    if (run.netOpen)
        StoreNetBeforeTick(&run.link.net);
    if (run.arrived)
    {
        run.arrived = false;
        GameS7RestoreLocalChildren();
    }
    if (run.replaying)
        ;
    else if (run.bot && run.botUntil && run.updates > run.botUntil)
        input = (GameInput){0};
    else if (run.bot)
        input = FromBot();
    else
        input = FromEngine(in);
    if (run.recording)
        RecordTick(&input);
    run.input = input;
    GameS7SetInput(&run.input);
    World3DBeginTick(&run.world);
    StoreTick(&run.store, (float)dt);
    if (run.netOpen)
        StoreNetAfterTick(&run.link.net);
    if (run.netLive)
        StoreNetLinkFlush(&run.link);
    run.lastTicked = true;
    if (run.present)
        PresentHeadless((float)dt);
    if (run.hashEvery && StoreTickCount(&run.store) % run.hashEvery == 0)
        PrintHash();
    return !Interrupted();
}

/* ---- presentation caches --------------------------------------------------------------------- */

static Texture2D TextureNamed(const char *name)
{
    if (!name || !name[0] || !run.gl)
        return (Texture2D){0};
    for (int i = 0; i < run.textureCount; i++)
        if (!strcmp(run.textures[i].name, name))
            return run.textures[i].texture;
    char buf[1024];
    const char *path = AssetPath(name, buf, sizeof buf);
    Texture2D texture = path ? LoadTexture(path) : (Texture2D){0};
    if (!texture.id)
    {
        TraceLog(LOG_WARNING, "RUN: no texture %s; drawing mid-grey", name);
        run.missingAssets++;
    }
    if (run.textureCount < MAX_TEXTURES)
    {
        TextureEntry *e = &run.textures[run.textureCount++];
        snprintf(e->name, sizeof e->name, "%s", name);
        e->texture = texture;
    }
    return texture;
}

static uint32_t MaterialFor(Texture2D texture, Color tint, bool skinned)
{
    if (!texture.id)
        texture = run.grey;
    for (int i = 0; i < run.materialCount; i++)
    {
        MaterialEntry *m = &run.materials[i];
        if (m->texture == texture.id && !memcmp(&m->tint, &tint, sizeof tint) && m->skinned == skinned)
            return m->id;
    }
    const WorldPass *pass = &run.passes[skinned ? 1 : 0];
    uint32_t id = DrawPathMaterial(&run.path, pass->shader, texture, tint, pass->normalLoc);
    if (id && run.materialCount < MAX_MATERIALS)
        run.materials[run.materialCount++] = (MaterialEntry){texture.id, tint, skinned, id};
    return id;
}

/* Whether every mesh of a model carries what raylib's skinning reads (UpdateModelAnimation reads
   every mesh's weights, and a mesh without them would crash it). */
static bool FullySkinned(const Model *model)
{
    for (int m = 0; m < model->meshCount; m++)
    {
        const Mesh *mesh = &model->meshes[m];
        if (!mesh->boneIds || !mesh->boneWeights || !mesh->boneMatrices || !mesh->animVertices ||
            mesh->boneCount != model->boneCount)
            return false;
    }
    return model->meshCount > 0;
}

/* A model file's clips that fit its skeleton; the model draws at rest with none. */
static void LoadClips(ModelEntry *e, const char *path)
{
    int count = 0;
    ModelAnimation *clips = LoadModelAnimations(path, &count);
    int kept = 0;
    for (int i = 0; clips && i < count; i++)
    {
        if (clips[i].frameCount > 0 && IsModelAnimationValid(e->model, clips[i]))
            clips[kept++] = clips[i];
        else
        {
            TraceLog(LOG_WARNING, "RUN: %s: clip %s does not fit the model's skeleton; it is not played", e->name,
                     clips[i].name);
            UnloadModelAnimation(clips[i]);
        }
    }
    if (kept && !FullySkinned(&e->model))
    {
        TraceLog(LOG_WARNING, "RUN: %s: a mesh has no bone weights; its clips are not played", e->name);
        UnloadModelAnimations(clips, kept);
        return;
    }
    if (!kept)
    {
        MemFree(clips);
        return;
    }
    e->clips = clips;
    e->clipCount = kept;
    e->cpu = run.skinCpuForced || e->model.boneCount > GPU_SKIN_BONES || !run.passes[1].loaded;
}

static ModelEntry *ModelNamed(const char *name)
{
    for (int i = 0; i < run.modelCount; i++)
        if (!strcmp(run.models[i].name, name))
            return &run.models[i];
    if (run.modelCount == MAX_MODELS)
        return NULL;
    ModelEntry *e = &run.models[run.modelCount++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    char buf[1024];
    const char *path = AssetPath(name, buf, sizeof buf);
    if (path)
        e->model = LoadModel(path);
    if (!e->model.meshCount)
    {
        TraceLog(LOG_WARNING, "RUN: no model %s; drawing a magenta 0.25 m cube (on a character, a 0.5 x 1.6 m box)",
                 name[0] ? name : "(no mesh named)");
        if (e->model.meshes || e->model.materials)
            UnloadModel(e->model);
        e->model = LoadModelFromMesh(GenMeshCube(1, 1, 1)); /* sized and stood up in AddModels */
        e->placeholder = true;
        run.missingAssets++;
    }
    e->meshes = calloc((size_t)(e->model.meshCount > 0 ? e->model.meshCount : 1), sizeof *e->meshes);
    for (int i = 0; e->meshes && i < e->model.meshCount; i++)
        e->meshes[i] = DrawPathMesh(&run.path, &e->model.meshes[i]);
    if (path && !e->placeholder && e->model.boneCount > 0)
        LoadClips(e, path);
    return e;
}

static float MaxScale(Matrix m)
{
    float a = m.m0 * m.m0 + m.m1 * m.m1 + m.m2 * m.m2, b = m.m4 * m.m4 + m.m5 * m.m5 + m.m6 * m.m6,
          c = m.m8 * m.m8 + m.m9 * m.m9 + m.m10 * m.m10;
    return sqrtf(fmaxf(a, fmaxf(b, c)));
}

static Color TintOf(StoreId id)
{
    StoreValue v;
    if (!Field(id, "tint", &v) || v.type != STORE_VEC3)
        return WHITE;
    return (Color){(unsigned char)(Clamp(v.as.v.x, 0, 1) * 255), (unsigned char)(Clamp(v.as.v.y, 0, 1) * 255),
                   (unsigned char)(Clamp(v.as.v.z, 0, 1) * 255), 255};
}

static StoreId *ThingsOf(StoreKind kind, int *count)
{
    int n = StoreThings(&run.store, kind, NULL, 0);
    StoreId *ids = malloc(sizeof *ids * (size_t)(n > 0 ? n : 1));
    *count = ids ? StoreThings(&run.store, kind, ids, n) : 0;
    return ids;
}

static ChunkEntry *ChunkFor(StoreId map, int chunkX, int chunkZ, bool *fresh)
{
    *fresh = false;
    for (int i = 0; i < run.chunkCount; i++)
    {
        ChunkEntry *c = &run.chunks[i];
        if (c->map.index == map.index && c->map.generation == map.generation && c->chunkX == chunkX &&
            c->chunkZ == chunkZ)
            return c;
    }
    if (run.chunkCount == MAX_CHUNKS)
        return NULL;
    *fresh = true;
    ChunkEntry *c = &run.chunks[run.chunkCount++];
    *c = (ChunkEntry){map, chunkX, chunkZ, {0, 0, 0}};
    return c;
}

// A chunk's arrays as a Mesh the draw path copies; world3d keeps owning them.
static void UploadPart(uint32_t *mesh, const MB *mb)
{
    if (mb->count < 3)
    {
        *mesh = 0;
        return;
    }
    Mesh view = {0};
    view.vertexCount = (int)mb->count;
    view.triangleCount = (int)mb->count / 3;
    view.vertices = mb->vertices;
    view.normals = mb->normals;
    view.texcoords = mb->uvs;
    uint32_t id = *mesh ? DrawPathMeshUpdate(&run.path, *mesh, &view) : DrawPathMesh(&run.path, &view);
    if (id)
        *mesh = id;
}

static void AddTilemaps(void)
{
    static const char *const textureFields[3] = {"floor-texture", "wall-texture", "ceiling-texture"};
    static const Color tints[3] = {{150, 150, 150, 255}, {210, 210, 210, 255}, {110, 110, 110, 255}};
    int count;
    StoreId *maps = ThingsOf(run.world.tilemap, &count);
    for (int m = 0; maps && m < count; m++)
    {
        Matrix world;
        if (!FieldTrue(maps[m], "visible") || !World3DWorldMatrix(&run.world, maps[m], &world))
            continue;
        int n = World3DTilemapChunks(&run.world, maps[m], NULL, 0);
        World3DChunk *chunks = n > 0 ? malloc(sizeof *chunks * (size_t)n) : NULL;
        n = chunks ? World3DTilemapChunks(&run.world, maps[m], chunks, n) : 0;
        uint32_t materials[3];
        for (int p = 0; p < 3; p++)
        {
            StoreValue v;
            materials[p] = MaterialFor(Field(maps[m], textureFields[p], &v) && v.type == STORE_STRING
                                           ? TextureNamed(v.as.str)
                                           : (Texture2D){0},
                                       tints[p], false);
        }
        for (int c = 0; c < n; c++)
        {
            bool fresh;
            ChunkEntry *entry = ChunkFor(maps[m], chunks[c].chunkX, chunks[c].chunkZ, &fresh);
            if (!entry)
                continue;
            const MB *parts[3] = {&chunks[c].floor, &chunks[c].wall, &chunks[c].ceiling};
            BoundingBox b = chunks[c].bounds;
            Vector3 center = Vector3Transform(Vector3Scale(Vector3Add(b.min, b.max), 0.5f), world);
            float radius = Vector3Distance(b.min, b.max) * 0.5f * MaxScale(world);
            for (int p = 0; p < 3; p++)
            {
                if (fresh || chunks[c].changed)
                    UploadPart(&entry->mesh[p], parts[p]);
                if (!entry->mesh[p] || !materials[p])
                    continue;
                DrawItem item = {entry->mesh[p], materials[p], world, center, radius, DRAW_LAYER_OPAQUE, NULL, 0};
                DrawPathAdd(&run.path, &item);
            }
        }
        free(chunks);
    }
    free(maps);
}

/* Whether a model thing is drawn on this machine: visible, and for-owner / hidden-for-owner agree. */
static bool DrawnHere(StoreId id)
{
    bool mine = StoreOwner(&run.store, id) == LocalPlayer();
    return FieldTrue(id, "visible") && !(FieldTrue(id, "for-owner") && !mine) &&
           !(FieldTrue(id, "hidden-for-owner") && mine);
}

static ModelEntry *ModelOf(StoreId id)
{
    StoreValue mesh;
    return ModelNamed(Field(id, "mesh", &mesh) && mesh.type == STORE_STRING ? mesh.as.str : "");
}

/* A CPU pose's own draw path meshes, one per model mesh, made (or rewritten in place) from the
   model's rest vertices without their bone arrays: these copies are drawn unskinned. */
static bool PoseMeshes(Pose *p, const ModelEntry *e)
{
    int need = e->model.meshCount;
    if (need > p->meshCount)
    {
        uint32_t *grown = realloc(p->meshes, sizeof *grown * (size_t)need);
        if (!grown)
            return false;
        memset(grown + p->meshCount, 0, sizeof *grown * (size_t)(need - p->meshCount));
        p->meshes = grown;
        p->meshCount = need;
    }
    for (int m = 0; m < need; m++)
    {
        Mesh view = e->model.meshes[m];
        view.boneIds = NULL;
        view.boneWeights = NULL;
        p->meshes[m] = p->meshes[m] ? DrawPathMeshUpdate(&run.path, p->meshes[m], &view) : DrawPathMesh(&run.path, &view);
    }
    return true;
}

/* The pose slot of a thing, fresh for a new thing in the slot or a thing whose model changed. */
static Pose *PoseSlot(StoreId id, const ModelEntry *e)
{
    if (id.index >= run.poseCapacity)
    {
        uint32_t capacity = run.poseCapacity ? run.poseCapacity : 64;
        while (capacity <= id.index)
            capacity *= 2;
        Pose *grown = realloc(run.poses, sizeof *grown * capacity);
        if (!grown)
            return NULL;
        memset(grown + run.poseCapacity, 0, sizeof *grown * (capacity - run.poseCapacity));
        run.poses = grown;
        run.poseCapacity = capacity;
    }
    Pose *p = &run.poses[id.index];
    if (p->thing.index == id.index && p->thing.generation == id.generation && p->model == e)
        return p;
    free(p->bones);
    p->bones = NULL;
    p->thing = id;
    p->model = e;
    p->animation = STORE_NO_SYMBOL - 1; /* never a symbol, so the first look restarts the clock */
    p->clock = 0;
    p->clip = -1;
    p->frame = 0;
    if (e->cpu)
        return PoseMeshes(p, e) ? p : NULL; /* the slot's old meshes are rewritten, not leaked */
    p->bones = malloc(sizeof *p->bones * (size_t)e->model.boneCount);
    return p->bones ? p : NULL;
}

/* Once per name and model: a clip the game asked for that the file does not have. */
static void WarnClip(const ModelEntry *e, const char *clip)
{
    char key[NAME * 2];
    snprintf(key, sizeof key, "%s %s", e->name, clip);
    for (int i = 0; i < run.clipWarningCount; i++)
        if (!strcmp(run.clipWarnings[i], key))
            return;
    if (run.clipWarningCount < MAX_CLIP_WARNINGS)
        snprintf(run.clipWarnings[run.clipWarningCount++], sizeof run.clipWarnings[0], "%s", key);
    TraceLog(LOG_WARNING, "RUN: %s has no clip %s; it holds the rest pose", e->name, clip);
}

/* Each drawn model thing with clips: its clock restarts when `animation` changes and otherwise runs
   at `animation-speed`; the clip's frame at that time is posed into its bone matrices (GPU) or its
   own meshes (CPU). A name the file lacks, or no animation, holds the rest pose. Runs before any
   item is built, so a socket's lookup finds every pose of this frame. */
static void UpdatePoses(float dt)
{
    if (!(dt > 0))
        dt = 0;
    if (dt > MAX_ANIMATION_STEP)
        dt = MAX_ANIMATION_STEP;
    int count;
    StoreId *ids = ThingsOf(run.world.model, &count);
    for (int i = 0; ids && i < count; i++)
    {
        if (!DrawnHere(ids[i]))
            continue;
        ModelEntry *e = ModelOf(ids[i]);
        Pose *p = e && e->clipCount ? PoseSlot(ids[i], e) : NULL;
        if (!p)
            continue;
        StoreValue v;
        StoreSymbol animation = Field(ids[i], "animation", &v) && v.type == STORE_SYMBOL ? v.as.sym : STORE_NO_SYMBOL;
        if (animation != p->animation)
        {
            p->animation = animation;
            p->clock = 0;
            p->clip = -1;
            const char *name = StoreSymbolName(&run.store, animation);
            for (int c = 0; name && c < e->clipCount && p->clip < 0; c++)
                if (!strcmp(e->clips[c].name, name))
                    p->clip = c;
            if (name && p->clip < 0)
                WarnClip(e, name);
        }
        else
            p->clock += (double)dt * FieldFloat(ids[i], "animation-speed", 1);
        Model model = e->model;
        if (p->clip >= 0)
        {
            const ModelAnimation *clip = &e->clips[p->clip];
            long frame = (long)floor(p->clock / CLIP_FRAME_SECONDS) % clip->frameCount;
            p->frame = (int)(frame < 0 ? frame + clip->frameCount : frame);
            if (e->cpu)
                UpdateModelAnimation(model, *clip, p->frame);
            else
            {
                UpdateModelAnimationBones(model, *clip, p->frame);
                memcpy(p->bones, model.meshes[0].boneMatrices, sizeof *p->bones * (size_t)model.boneCount);
            }
        }
        else if (!e->cpu)
            for (int b = 0; b < model.boneCount; b++)
                p->bones[b] = MatrixIdentity();
        for (int m = 0; e->cpu && m < model.meshCount; m++)
        {
            const Mesh *mesh = &model.meshes[m];
            DrawPathMeshPositions(&run.path, p->meshes[m], p->clip >= 0 ? mesh->animVertices : mesh->vertices,
                                  p->clip >= 0 ? mesh->animNormals : mesh->normals);
        }
    }
    free(ids);
}

/* A pose of this frame for a model thing drawn with e, or NULL. */
static const Pose *PoseOf(StoreId id, const ModelEntry *e)
{
    if (!e || !e->clipCount || id.index >= run.poseCapacity)
        return NULL;
    const Pose *p = &run.poses[id.index];
    return p->thing.index == id.index && p->thing.generation == id.generation && p->model == e ? p : NULL;
}

/* The runner's bone lookup (World3DSetBoneLookup), for drawing only: a posed model's bone as it is
   drawn this frame, in the model's own space (its file's root transform and its spin included). */
static World3DBone BoneOf(void *user, StoreId model, const char *bone, Matrix *out)
{
    (void)user;
    if (!DrawnHere(model))
        return WORLD3D_BONE_NOT_DRAWN;
    ModelEntry *e = ModelOf(model);
    const Pose *p = PoseOf(model, e);
    if (!p || p->clip < 0)
        return WORLD3D_BONE_NONE;
    for (int b = 0; b < e->model.boneCount; b++)
        if (!strcmp(e->model.bones[b].name, bone))
        {
            Transform t = e->clips[p->clip].framePoses[p->frame][b];
            Matrix pose = MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z),
                                                        QuaternionToMatrix(t.rotation)),
                                         MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
            *out = MatrixMultiply(MatrixMultiply(pose, e->model.transform),
                                  MatrixRotateY(FieldFloat(model, "spin", 0) * StoreTickTime(&run.store)));
            return WORLD3D_BONE_POSED;
        }
    return WORLD3D_BONE_NONE;
}

/* ---- static batching (docs/developer/store.md §3.1) ------------------------------------------ */

static bool SameThing(StoreId a, StoreId b) { return a.index == b.index && a.generation == b.generation; }

/* Room for need items of size in a growing array. */
static bool Reserve(void **items, int *capacity, int need, size_t size)
{
    if (need <= *capacity)
        return true;
    int grown = *capacity ? *capacity : 16;
    while (grown < need)
        grown *= 2;
    void *more = realloc(*items, (size_t)grown * size);
    if (!more)
        return false;
    *items = more;
    *capacity = grown;
    return true;
}

/* The :static thing a model is batched under: the outermost of it and its ancestors with `static`
   set, or null for none. A model under a socket follows a bone, so it is never batched. */
static StoreId StaticRootOf(StoreId id)
{
    StoreId root = STORE_NULL;
    for (StoreId at = id; StoreAlive(&run.store, at); at = StoreParent(&run.store, at))
    {
        if (StoreKindIs(&run.store, StoreKindOf(&run.store, at), run.world.socket))
            return STORE_NULL;
        if (FieldTrue(at, "static"))
            root = at;
    }
    return root;
}

/* What a batched model was merged with: its :static thing and the fields that choose its items.
   Another look (a new mesh or tint, `static` turned off) takes it out of the batch. */
static uint64_t LookOf(StoreId id, StoreId root)
{
    static const char *const fields[] = {"mesh", "tint", "spin", "viewmodel", "cull-distance"};
    uint64_t h = 1469598103934665603ull;
    uint32_t words[2] = {root.index, root.generation};
    const unsigned char *bytes = (const unsigned char *)words;
    for (size_t k = 0; k < sizeof words; k++)
        h = (h ^ bytes[k]) * 1099511628211ull;
    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++)
    {
        StoreValue v;
        memset(&v, 0, sizeof v);
        Field(id, fields[f], &v);
        bytes = (const unsigned char *)&v;
        for (size_t k = 0; k < sizeof v; k++)
            h = (h ^ bytes[k]) * 1099511628211ull;
    }
    return h;
}

/* The static slot of a thing index, grown on demand; NULL when out of memory. */
static StaticModel *StaticSlot(StoreId id)
{
    if (id.index >= run.staticCapacity)
    {
        uint32_t capacity = run.staticCapacity ? run.staticCapacity : 64;
        while (capacity <= id.index)
            capacity *= 2;
        StaticModel *grown = realloc(run.statics, sizeof *grown * capacity);
        if (!grown)
            return NULL;
        memset(grown + run.staticCapacity, 0, sizeof *grown * (capacity - run.staticCapacity));
        run.statics = grown;
        run.staticCapacity = capacity;
    }
    StaticModel *s = &run.statics[id.index];
    if (!SameThing(s->thing, id)) /* a new thing in the slot starts unbatched */
    {
        free(s->items);
        memset(s, 0, sizeof *s);
        s->thing = id;
    }
    return s;
}

/* Takes a model out of its batch: its region is rebuilt at this frame's end without it. */
static void StaticRelease(StaticModel *s, StaticState state)
{
    if (s->state == STATIC_BATCHED && s->region >= 0 && s->region < run.regionCount)
        run.regions[s->region].dirty = true;
    free(s->items);
    s->items = NULL;
    s->itemCount = 0;
    s->state = state;
}

/* The region a point stands in: the 8x8-cell chunk of the first tilemap under it, or the region of
   things standing on no tilemap. -1 when out of memory. */
static int RegionAt(Vector3 at)
{
    StoreId map = STORE_NULL;
    int x = 0, z = 0, count;
    StoreId *maps = ThingsOf(run.world.tilemap, &count);
    for (int m = 0; maps && m < count; m++)
        if (World3DWorldToCell(&run.world, maps[m], at, &x, &z))
        {
            map = maps[m];
            x /= STATIC_REGION_CELLS;
            z /= STATIC_REGION_CELLS;
            break;
        }
    free(maps);
    if (map.index == STORE_NULL.index)
        x = z = 0;
    for (int r = 0; r < run.regionCount; r++)
        if (SameThing(run.regions[r].map, map) && run.regions[r].x == x && run.regions[r].z == z)
            return r;
    if (!Reserve((void **)&run.regions, &run.regionCapacity, run.regionCount + 1, sizeof *run.regions))
        return -1;
    run.regions[run.regionCount] = (StaticRegion){map, x, z, false, NULL, 0};
    return run.regionCount++;
}

static bool NodeFields(StoreId id, Vector3 *p, Vector3 *r, Vector3 *s)
{
    StoreValue a, b, c;
    if (!StoreGet(&run.store, id, run.world.position, &a) || !StoreGet(&run.store, id, run.world.rotation, &b) ||
        !StoreGet(&run.store, id, run.world.scale, &c))
        return false;
    *p = a.as.v, *r = b.as.v, *s = c.as.v;
    return true;
}

/* Puts a model's items in its region's batch, which is rebuilt at this frame's end. */
static bool StaticBatch(StaticModel *s, StoreId root, const DrawItem *items, int count, Vector3 at)
{
    Matrix world;
    Vector3 position, rotation, scale;
    if (!World3DWorldMatrix(&run.world, s->thing, &world) || !NodeFields(s->thing, &position, &rotation, &scale))
        return false;
    int region = RegionAt(at);
    DrawItem *copy = count > 0 ? malloc(sizeof *copy * (size_t)count) : NULL;
    if (region < 0 || !copy)
    {
        free(copy);
        return false;
    }
    memcpy(copy, items, sizeof *copy * (size_t)count);
    s->items = copy;
    s->itemCount = count;
    s->region = region;
    s->state = STATIC_BATCHED;
    s->look = LookOf(s->thing, root);
    s->world = world;
    s->position = position, s->rotation = rotation, s->scale = scale;
    s->root = root;
    run.regions[region].dirty = true;
    return true;
}

/* Before the models are gathered: a batched model whose gameplay world matrix changed since it was
   batched (its own position, rotation or scale, or any ancestor's) leaves its batch. On the machine
   that owns it the write was this machine's game code, so it is an error naming the flag, reported
   once per :static thing per frame, and the model is drawn as itself from then on. Elsewhere the
   move came from its owner and is no error: the model is batched again where it now stands. A
   removed model leaves its region's batch. Checked here, at the next frame, and not when the field
   is written: batching happens only where a window draws, so refusing the write would make gameplay
   differ between a headless replay and play (rule 1). */
static void CheckStatics(void)
{
    static const char *const names[3] = {"position", "rotation", "scale"};
    static uint32_t frame;
    frame++;
    for (uint32_t i = 0; i < run.staticCapacity; i++)
    {
        StaticModel *s = &run.statics[i];
        if (s->state != STATIC_BATCHED)
            continue;
        Matrix world;
        bool alive = StoreAlive(&run.store, s->thing);
        if (alive && World3DWorldMatrix(&run.world, s->thing, &world) && !memcmp(&world, &s->world, sizeof world))
            continue;
        if (!alive || !StoreOwnedHere(&run.store, s->thing) || !StoreAlive(&run.store, s->root) ||
            !FieldTrue(s->root, "static"))
        {
            StaticRelease(s, STATIC_NONE);
            continue;
        }
        bool reported = false;
        for (uint32_t j = 0; j < run.staticCapacity && !reported; j++)
            reported = run.statics[j].state == STATIC_MOVED && run.statics[j].movedFrame == frame &&
                       SameThing(run.statics[j].root, s->root);
        StaticRelease(s, STATIC_MOVED);
        s->movedFrame = frame;
        if (reported)
            continue;
        Vector3 now[3] = {{0, 0, 0}, {0, 0, 0}, {1, 1, 1}};
        NodeFields(s->thing, &now[0], &now[1], &now[2]);
        const Vector3 was[3] = {s->position, s->rotation, s->scale};
        int changed = -1;
        for (int f = 0; f < 3 && changed < 0; f++)
            if (memcmp(&was[f], &now[f], sizeof now[f]))
                changed = f;
        char what[128], text[256];
        const char *kind = StoreKindName(&run.store, StoreKindOf(&run.store, s->thing));
        const char *rootKind = StoreKindName(&run.store, StoreKindOf(&run.store, s->root));
        if (changed >= 0)
            snprintf(what, sizeof what, "%s on %s #%u changed", names[changed], kind, s->thing.index);
        else
            snprintf(what, sizeof what, "an ancestor of %s #%u moved it", kind, s->thing.index);
        if (SameThing(s->thing, s->root))
            snprintf(text, sizeof text, "%s, but %s is :static; remove :static if it moves", what, kind);
        else
            snprintf(text, sizeof text, "%s, but it hangs from %s #%u, which is :static; remove :static if it moves",
                     what, rootKind, s->root.index);
        TraceLog(LOG_ERROR, "RUN: %s", text);
        OnError(text);
    }
}

/* After the models are gathered: each region whose members changed is merged again, and every
   region's batches are drawn. */
static void AddStatics(void)
{
    for (int r = 0; r < run.regionCount; r++)
    {
        StaticRegion *region = &run.regions[r];
        if (region->dirty)
        {
            region->dirty = false;
            for (int b = 0; b < region->batchCount; b++)
                DrawPathMeshRelease(&run.path, region->batches[b].mesh);
            free(region->batches);
            region->batches = NULL;
            region->batchCount = 0;
            int count = 0, used = 0;
            for (uint32_t i = 0; i < run.staticCapacity; i++)
                if (run.statics[i].state == STATIC_BATCHED && run.statics[i].region == r)
                    count += run.statics[i].itemCount;
            DrawItem *items = count ? malloc(sizeof *items * (size_t)count) : NULL;
            for (uint32_t i = 0; items && i < run.staticCapacity; i++)
                if (run.statics[i].state == STATIC_BATCHED && run.statics[i].region == r)
                {
                    memcpy(items + used, run.statics[i].items, sizeof *items * (size_t)run.statics[i].itemCount);
                    used += run.statics[i].itemCount;
                }
            region->batches = items ? malloc(sizeof *region->batches * (size_t)count) : NULL;
            if (region->batches)
                region->batchCount = DrawPathStaticBatch(&run.path, items, used, region->batches, count);
            free(items);
        }
        for (int b = 0; b < region->batchCount; b++)
            DrawPathAdd(&run.path, &region->batches[b]);
    }
}

/* --bench: how many models ride in batches, in how many regions with members, as how many merged
   items of how many materials. */
static void PrintStatics(void)
{
    int models = 0, regions = 0, batches = 0, materials = 0;
    uint32_t seen[64];
    for (uint32_t i = 0; i < run.staticCapacity; i++)
        models += run.statics[i].state == STATIC_BATCHED;
    for (int r = 0; r < run.regionCount; r++)
    {
        regions += run.regions[r].batchCount > 0;
        batches += run.regions[r].batchCount;
        for (int b = 0; b < run.regions[r].batchCount; b++)
        {
            bool known = false;
            for (int k = 0; k < materials && !known; k++)
                known = seen[k] == run.regions[r].batches[b].material;
            if (!known && materials < 64)
                seen[materials++] = run.regions[r].batches[b].material;
        }
    }
    printf("bench static models %d regions %d batches %d materials %d\n", models, regions, batches, materials);
}

static void FreeStatics(void)
{
    for (uint32_t i = 0; i < run.staticCapacity; i++)
        free(run.statics[i].items);
    for (int r = 0; r < run.regionCount; r++)
        free(run.regions[r].batches);
    free(run.statics);
    free(run.regions);
    free(run.scratch);
    free(run.viewItems);
    run.statics = NULL;
    run.regions = NULL;
    run.scratch = run.viewItems = NULL;
    run.staticCapacity = 0;
    run.regionCount = run.regionCapacity = 0;
    run.scratchCapacity = run.viewCount = run.viewCapacity = 0;
}

/* A model thing's draw items at world, one per mesh, into run.scratch; their count. */
static int ModelItems(StoreId id, ModelEntry *e, const Pose *pose, Matrix world)
{
    if (!Reserve((void **)&run.scratch, &run.scratchCapacity, e->model.meshCount, sizeof *run.scratch))
        return 0;
    Color tint = e->placeholder ? (Color){255, 0, 255, 255} : TintOf(id);
    Matrix local = e->model.transform;
    if (e->placeholder) /* standing on the node's origin: a character's is its feet */
    {
        StoreId parent = StoreParent(&run.store, id);
        bool character = StoreAlive(&run.store, parent) &&
                         StoreKindIs(&run.store, StoreKindOf(&run.store, parent), run.world.character);
        Vector3 size = character ? (Vector3){0.5f, 1.6f, 0.5f} : (Vector3){0.25f, 0.25f, 0.25f};
        local = MatrixMultiply(MatrixScale(size.x, size.y, size.z), MatrixTranslate(0, size.y / 2, 0));
    }
    int n = 0;
    for (int m = 0; m < e->model.meshCount; m++)
    {
        bool cpu = pose && e->cpu && m < pose->meshCount && pose->meshes[m];
        bool gpu = pose && !e->cpu && e->model.meshes[m].boneIds;
        uint32_t mesh = cpu ? pose->meshes[m] : e->meshes[m];
        if (!mesh)
            continue;
        int which = e->model.meshMaterial ? e->model.meshMaterial[m] : 0;
        Texture2D texture = which >= 0 && which < e->model.materialCount && e->model.materials[which].maps
                                ? e->model.materials[which].maps[MATERIAL_MAP_DIFFUSE].texture
                                : (Texture2D){0};
        DrawItem item = {mesh, MaterialFor(texture, tint, gpu), MatrixMultiply(local, world), {0, 0, 0}, 0,
                         DRAW_LAYER_OPAQUE, gpu ? pose->bones : NULL, gpu ? e->model.boneCount : 0};
        const DrawPathMeshData *data = &run.path.meshes[mesh - 1];
        item.center = Vector3Transform(data->center, item.world);
        /* A posed mesh reaches past its rest bounds (a raised arm), so it is culled generously. */
        item.radius = data->radius * MaxScale(item.world) * (pose ? 2.0f : 1.0f);
        if (item.material)
            run.scratch[n++] = item;
    }
    return n;
}

static void AddModels(Camera3D camera)
{
    run.viewCount = 0;
    CheckStatics();
    int count;
    StoreId *ids = ThingsOf(run.world.model, &count);
    float now = StoreTickTime(&run.store);
    for (int i = 0; ids && i < count; i++)
    {
        StoreId id = ids[i];
        bool drawn = DrawnHere(id);
        StaticModel *s = run.noStaticBatch ? NULL : StaticSlot(id);
        StoreId root = s && s->state != STATIC_MOVED ? StaticRootOf(id) : STORE_NULL;
        if (s && s->state == STATIC_BATCHED)
        {
            if (drawn && root.index != STORE_NULL.index && LookOf(id, root) == s->look)
                continue; /* drawn in its region's batch */
            StaticRelease(s, STATIC_NONE);
        }
        if (!drawn)
            continue;
        Matrix world;
        if (!World3DDrawMatrix(&run.world, id, &world)) /* under a socket: on its bone, this frame */
            continue;
        float cull = FieldFloat(id, "cull-distance", 0);
        Vector3 at = {world.m12, world.m13, world.m14};
        if (cull > 0 && Vector3Distance(at, camera.position) > cull)
            continue;
        float spin = FieldFloat(id, "spin", 0);
        if (spin != 0)
            world = MatrixMultiply(MatrixRotateY(spin * now), world);
        ModelEntry *e = ModelOf(id);
        if (!e || !e->meshes)
            continue;
        const Pose *pose = PoseOf(id, e);
        int n = ModelItems(id, e, pose, world);
        bool viewmodel = FieldTrue(id, "viewmodel");
        /* Batched: static, not animated, spun, culled by distance or a viewmodel (§3.1). */
        if (s && root.index != STORE_NULL.index && !e->clipCount && spin == 0 && cull <= 0 && !viewmodel && n > 0 &&
            StaticBatch(s, root, run.scratch, n, at))
            continue;
        /* A viewmodel draws in the viewmodel pass for its owner, and like any model elsewhere. */
        if (viewmodel && StoreOwner(&run.store, id) == LocalPlayer())
        {
            if (Reserve((void **)&run.viewItems, &run.viewCapacity, run.viewCount + n, sizeof *run.viewItems))
            {
                memcpy(run.viewItems + run.viewCount, run.scratch, sizeof *run.scratch * (size_t)n);
                run.viewCount += n;
            }
            continue;
        }
        for (int k = 0; k < n; k++)
            DrawPathAdd(&run.path, &run.scratch[k]);
    }
    free(ids);
    AddStatics();
}

// The local player's for-owner camera, or one above the origin; run.viewmodelFov is its
// viewmodel-fov (60 without one).
static Camera3D ChooseCamera(void)
{
    Camera3D camera = {{0, 10, 10}, {0, 0, 0}, {0, 1, 0}, 60, CAMERA_PERSPECTIVE};
    run.viewmodelFov = 60;
    int count;
    StoreId *ids = ThingsOf(run.world.camera, &count);
    for (int i = 0; ids && i < count; i++)
    {
        Matrix m;
        if (StoreOwner(&run.store, ids[i]) != LocalPlayer() || !FieldTrue(ids[i], "for-owner") ||
            !World3DWorldMatrix(&run.world, ids[i], &m))
            continue;
        Vector3 at = {m.m12, m.m13, m.m14};
        Vector3 forward = Vector3Subtract(Vector3Transform((Vector3){0, 0, -1}, m), at);
        Vector3 up = Vector3Subtract(Vector3Transform((Vector3){0, 1, 0}, m), at);
        camera = (Camera3D){at, Vector3Add(at, forward), up, FieldFloat(ids[i], "fov", 75), CAMERA_PERSPECTIVE};
        run.viewmodelFov = FieldFloat(ids[i], "viewmodel-fov", 60);
        break;
    }
    free(ids);
    return camera;
}

/* The viewmodel pass (§3.1, proposal B9.2's layer 3): the owner's viewmodel models after the world,
   from the local camera's place with its viewmodel-fov and clip planes of 0.01 and 10 m, over a
   cleared depth buffer, so a gun never clips into a wall. Its draw counts join the frame's. */
#define VIEWMODEL_NEAR 0.01f
#define VIEWMODEL_FAR 10.0f
static Camera3D viewCamera;

static void DrawViewItems(void *context)
{
    (void)context;
    DrawPathBegin(&run.path, viewCamera, GetScreenWidth(), GetScreenHeight());
    for (int i = 0; i < run.viewCount; i++)
        DrawPathAdd(&run.path, &run.viewItems[i]);
    DrawStats s = DrawPathEnd(&run.path);
    run.lastStats.items += s.items;
    run.lastStats.visible += s.visible;
    run.lastStats.draws += s.draws;
    run.lastStats.shaderSwitches += s.shaderSwitches;
    run.lastStats.textureSwitches += s.textureSwitches;
    run.lastStats.boneUploads += s.boneUploads;
}

static void DrawViewmodels(Camera3D camera)
{
    if (!run.viewCount)
        return;
    viewCamera = camera;
    viewCamera.fovy = run.viewmodelFov;
    float aspect = GetScreenHeight() > 0 ? (float)GetScreenWidth() / (float)GetScreenHeight() : 1.0f;
    static float warnedFov = -1; /* one warning per bad value, not one per frame */
    if (!DrawViewmodelCleared((ViewmodelProjection){run.viewmodelFov, aspect, 1.0f}, VIEWMODEL_NEAR, VIEWMODEL_FAR,
                              DrawViewItems, NULL) &&
        warnedFov != run.viewmodelFov)
    {
        warnedFov = run.viewmodelFov;
        TraceLog(LOG_WARNING, "RUN: viewmodel-fov %.1f is not between 0 and 180; the viewmodels are not drawn",
                 (double)run.viewmodelFov);
    }
}

/* ---- sound emitters (§3.1) ------------------------------------------------------------------ */

void GameSoundHeard(Vector3 camera, Vector3 forward, Vector3 up, Vector3 at, float volume, float *gain, float *pan)
{
    CoreAudio listener;
    memset(&listener, 0, sizeof listener);
    CoreAudioSetListener(&listener, camera, forward, up);
    if (gain)
        *gain = CoreAudioGainAt(&listener, at, GAME_SOUND_RANGE, volume);
    if (pan)
        *pan = CoreAudioPanAt(&listener, at);
}

static void EmitterRelease(Emitter *e)
{
    if (CoreAudioVoiceValid(&run.audio, e->voice))
        CoreAudioVoiceFree(&run.audio, e->voice);
    memset(e, 0, sizeof *e);
    e->voice = (CoreAudioVoice){UINT32_MAX, 0};
}

static Emitter *EmitterSlot(StoreId id)
{
    if (id.index >= run.emitterCapacity)
    {
        uint32_t capacity = run.emitterCapacity ? run.emitterCapacity : 64;
        while (capacity <= id.index)
            capacity *= 2;
        Emitter *grown = realloc(run.emitters, sizeof *grown * capacity);
        if (!grown)
            return NULL;
        memset(grown + run.emitterCapacity, 0, sizeof *grown * (capacity - run.emitterCapacity));
        run.emitters = grown;
        run.emitterCapacity = capacity;
    }
    Emitter *e = &run.emitters[id.index];
    if (!e->used || !SameThing(e->thing, id))
    {
        if (e->used)
            EmitterRelease(e);
        memset(e, 0, sizeof *e);
        e->voice = (CoreAudioVoice){UINT32_MAX, 0};
        e->thing = id;
        e->used = true;
    }
    return e;
}

/* Each frame, after the listener moved to the camera: a `sound` thing with `playing` loops its
   `stream` on a held voice of its own at its drawn place, at `volume` falling off to silence at
   GAME_SOUND_RANGE; turning `playing` off stops it, and a removed thing's voice is freed. With no
   audio device or no such file it stays silent, with one warning. */
static void UpdateEmitters(void)
{
    int count;
    StoreId *ids = ThingsOf(run.world.sound, &count);
    for (int i = 0; ids && i < count; i++)
    {
        Emitter *e = EmitterSlot(ids[i]);
        if (!e)
            continue;
        StoreValue v;
        const char *stream = Field(ids[i], "stream", &v) && v.type == STORE_STRING ? v.as.str : "";
        if (strcmp(e->stream, stream))
        {
            StoreId thing = e->thing;
            EmitterRelease(e);
            e->thing = thing;
            e->used = true;
            snprintf(e->stream, sizeof e->stream, "%s", stream);
        }
        bool playing = FieldTrue(ids[i], "playing") && stream[0];
        if (playing && !e->tried)
        {
            e->tried = true;
            char buf[1024];
            const char *path = AssetPath(stream, buf, sizeof buf);
            e->voice = path ? CoreAudioVoiceCreate(&run.audio, path, "game") : (CoreAudioVoice){UINT32_MAX, 0};
            if (!path)
            {
                TraceLog(LOG_WARNING, "RUN: no sound %s", stream);
                run.missingAssets++;
            }
            else if (!CoreAudioVoiceValid(&run.audio, e->voice))
                TraceLog(LOG_WARNING, "RUN: sound %s could not be loaded or no audio device; it is silent", stream);
        }
        if (!CoreAudioVoiceValid(&run.audio, e->voice))
            continue;
        Matrix m;
        Vector3 at = World3DDrawMatrix(&run.world, ids[i], &m) ? (Vector3){m.m12, m.m13, m.m14} : (Vector3){0, 0, 0};
        float volume = FieldFloat(ids[i], "volume", 1);
        if (playing && !e->playing)
        {
            CoreAudioVoiceSetLoop(&run.audio, e->voice, true);
            CoreAudioVoicePlayAt(&run.audio, e->voice, at, GAME_SOUND_RANGE, volume);
        }
        else if (playing)
        {
            CoreAudioVoiceMove(&run.audio, e->voice, at);
            CoreAudioVoiceSetGain(&run.audio, e->voice, volume);
        }
        else if (e->playing)
            CoreAudioVoiceStop(&run.audio, e->voice);
        e->playing = playing;
    }
    free(ids);
    for (uint32_t i = 0; i < run.emitterCapacity; i++)
        if (run.emitters[i].used && !StoreAlive(&run.store, run.emitters[i].thing))
            EmitterRelease(&run.emitters[i]);
}

static void FreeEmitters(void)
{
    for (uint32_t i = 0; i < run.emitterCapacity; i++)
        if (run.emitters[i].used)
            EmitterRelease(&run.emitters[i]);
    free(run.emitters);
    run.emitters = NULL;
    run.emitterCapacity = 0;
}

static void DrawHud(void)
{
    for (int i = 0; i < run.hudCount; i++)
    {
        const HudCall *h = &run.hud[i];
        switch (h->type)
        {
        case HUD_TEXT:
        {
            // The engine's 16 px bitmap font, scaled by size/16.
            float width = MeasureTextEx(run.font, h->text, (float)h->size, 1).x;
            float x = h->x - (h->align == 1 ? width / 2 : h->align == 2 ? width : 0);
            DrawTextEx(run.font, h->text, (Vector2){floorf(x), floorf(h->y)}, (float)h->size, 1, h->color);
            break;
        }
        case HUD_RECT: DrawRectangle((int)h->x, (int)h->y, (int)h->w, (int)h->h, h->color); break;
        case HUD_RING:
            DrawRing((Vector2){h->x, h->y}, fmaxf(h->w - 1.5f, 0), h->w + 1.5f, -90, -90 + 360 * h->h, 48, h->color);
            break;
        case HUD_IMAGE:
        {
            Texture2D t = TextureNamed(h->text);
            if (t.id)
                DrawTexture(t, (int)h->x, (int)h->y, WHITE);
            break;
        }
        }
    }
    Push(&run.hudCalls, run.hudCount);
    run.hudCount = 0;
}

// The stdin REPL, between frames, as script_s7.c's.
static void PollRepl(void)
{
    if (!run.replGreeted)
    {
        run.replGreeted = true;
        printf("\ns7 REPL on the running game. Try (things 'node)\n> ");
        fflush(stdout);
    }
    for (;;)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        struct timeval nothing = {0, 0};
        if (select(STDIN_FILENO + 1, &readable, NULL, NULL, &nothing) <= 0)
            return;
        char line[512];
        if (!fgets(line, sizeof line, stdin))
            return;
        if (strspn(line, " \t\r\n") == strlen(line))
            continue;
        EvalRepl(line, "%s\n> ");
    }
}

static double Seconds(void) { return run.gl ? GetTime() : 0; }

/* Whether a sphere reaches into the frustum DrawPathBegin took for this frame. */
static bool InView(Vector3 at, float radius)
{
    for (int p = 0; p < 6; p++)
    {
        const float *plane = run.path.planes[p];
        if (plane[0] * at.x + plane[1] * at.y + plane[2] * at.z + plane[3] < -radius)
            return false;
    }
    return true;
}

typedef struct PointLight
{
    Vector3 position, color; /* color times energy */
    float range, distance;   /* distance: to the camera, to choose the nearest */
} PointLight;

// The world shader's lights for this frame (§3.1), after DrawPathBegin: the first directional
// light (its rotation turning (0,-1,0)) or a default sun, over the first ambient light's energy or
// 0.3; the four point lights nearest the camera among the visible ones whose range reaches into
// the view (unused slots have zero colour); fog to the clear colour.
static void SetLights(Camera3D camera)
{
    Vector3 dir = Vector3Normalize((Vector3){0.5f, -1.0f, 0.3f}), color = {1, 1, 1}, ambient = {0.3f, 0.3f, 0.3f};
    bool haveSun = false, haveAmbient = false;
    PointLight near[POINT_LIGHTS];
    int nearCount = 0;
    int count;
    StoreId *ids = ThingsOf(run.world.light, &count);
    for (int i = 0; ids && i < count; i++)
    {
        StoreValue type;
        const char *name = Field(ids[i], "type", &type) && type.type == STORE_SYMBOL
                               ? StoreSymbolName(&run.store, type.as.sym)
                               : NULL;
        float energy = FieldFloat(ids[i], "energy", 1);
        StoreValue c;
        Vector3 tint = Field(ids[i], "color", &c) && c.type == STORE_VEC3 ? c.as.v : (Vector3){1, 1, 1};
        Matrix m;
        if (name && !strcmp(name, "directional") && !haveSun && World3DWorldMatrix(&run.world, ids[i], &m))
        {
            haveSun = true;
            Vector3 at = {m.m12, m.m13, m.m14};
            Vector3 turned = Vector3Subtract(Vector3Transform((Vector3){0, -1, 0}, m), at);
            if (Vector3Length(turned) > 0)
                dir = Vector3Normalize(turned);
            color = Vector3Scale(tint, energy);
        }
        else if (name && !strcmp(name, "ambient") && !haveAmbient)
        {
            haveAmbient = true;
            ambient = (Vector3){energy, energy, energy};
        }
        else if (name && !strcmp(name, "point") && FieldTrue(ids[i], "visible") &&
                 World3DDrawMatrix(&run.world, ids[i], &m))
        {
            PointLight light = {{m.m12, m.m13, m.m14}, Vector3Scale(tint, energy), FieldFloat(ids[i], "range", 10), 0};
            light.distance = Vector3Distance(light.position, camera.position);
            if (!(light.range > 0) || !InView(light.position, light.range))
                continue;
            /* Kept sorted nearest first; a light farther than the fourth is dropped. */
            int at = nearCount < POINT_LIGHTS ? nearCount++ : POINT_LIGHTS;
            while (at > 0 && near[at - 1].distance > light.distance)
            {
                if (at < POINT_LIGHTS)
                    near[at] = near[at - 1];
                at--;
            }
            if (at < POINT_LIGHTS)
                near[at] = light;
        }
    }
    free(ids);
    Vector3 positions[POINT_LIGHTS], colors[POINT_LIGHTS];
    float ranges[POINT_LIGHTS];
    for (int i = 0; i < POINT_LIGHTS; i++)
    {
        bool used = i < nearCount;
        positions[i] = used ? near[i].position : (Vector3){0, 0, 0};
        colors[i] = used ? near[i].color : (Vector3){0, 0, 0};
        ranges[i] = used ? near[i].range : 1.0f;
    }
    Color clear = {30, 32, 36, 255};
    Vector3 fog = {clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f};
    float density = 0.02f;
    for (int i = 0; i < 2; i++) /* static and skinned models are lit and fogged alike */
    {
        const WorldPass *pass = &run.passes[i];
        if (!pass->loaded)
            continue;
        SetShaderValue(pass->shader, pass->lightDirLoc, &dir, SHADER_UNIFORM_VEC3);
        SetShaderValue(pass->shader, pass->lightColorLoc, &color, SHADER_UNIFORM_VEC3);
        SetShaderValue(pass->shader, pass->ambientLoc, &ambient, SHADER_UNIFORM_VEC3);
        SetShaderValueV(pass->shader, pass->pointPositionLoc, positions, SHADER_UNIFORM_VEC3, POINT_LIGHTS);
        SetShaderValueV(pass->shader, pass->pointColorLoc, colors, SHADER_UNIFORM_VEC3, POINT_LIGHTS);
        SetShaderValueV(pass->shader, pass->pointRangeLoc, ranges, SHADER_UNIFORM_FLOAT, POINT_LIGHTS);
        SetShaderValue(pass->shader, pass->fogColorLoc, &fog, SHADER_UNIFORM_VEC3);
        SetShaderValue(pass->shader, pass->fogDensityLoc, &density, SHADER_UNIFORM_FLOAT);
        SetShaderValue(pass->shader, pass->viewPosLoc, &camera.position, SHADER_UNIFORM_VEC3);
    }
    run.pointLights = nearCount;
}

// Handler errors, kept by the sink with their tick, shown for five seconds along the bottom.
static void OnError(const char *message)
{
    if (run.errorCount == 3)
    {
        memmove(&run.errors[0], &run.errors[1], 2 * sizeof run.errors[0]);
        run.errorCount = 2;
    }
    // The error layer does not say whether the engine or game code raised it; both read `game:`.
    snprintf(run.errors[run.errorCount].text, sizeof run.errors[0].text, "game: %.240s", message);
    run.errors[run.errorCount].tick = StoreTickCount(&run.store);
    run.errors[run.errorCount].at = Seconds();
    run.errorCount++;
}

static void DrawErrors(void)
{
    double now = Seconds();
    int line = 0;
    for (int i = run.errorCount - 1; i >= 0; i--)
    {
        if (now - run.errors[i].at > 5.0)
            continue;
        char text[300];
        snprintf(text, sizeof text, "tick %" PRIu64 " %s", run.errors[i].tick, run.errors[i].text);
        Vector2 at = {12, (float)GetScreenHeight() - 24.0f * (float)(++line) - 8};
        DrawTextEx(run.font, text, at, 16, 1, (Color){255, 70, 70, 255});
    }
}

// The diagnostics overlay's own lines, beside CoreDebug's timing readout; --bench prints the same.
static int OverlayLines(char lines[3][128])
{
    const CoreDiagnostics *d = CoreDiagnosticsCurrent();
    snprintf(lines[0], 128, "tick us last %.0f max %.0f | frame us last %.0f | fps %d", d ? d->tickMicrosLast : 0,
             d ? d->tickMicrosMax : 0, d ? d->frameMicrosLast : 0, GetFPS());
    snprintf(lines[1], 128, "draw items %d visible %d draws %d shader switches %d", run.lastStats.items,
             run.lastStats.visible, run.lastStats.draws, run.lastStats.shaderSwitches);
    snprintf(lines[2], 128, "things %u | missing assets: %d", StoreCount(&run.store), run.missingAssets);
    return 3;
}

static void DrawOverlay(void)
{
    char (*lines)[128] = run.overlayText;
    int n = OverlayLines(lines);
    if (!run.overlay)
        return;
    DrawRectangle(0, 0, 520, 20 + 18 * n + 8, (Color){0, 0, 0, 160});
    for (int i = 0; i < n; i++)
        CoreDebugText(&run.debug, (Vector2){(float)run.debug.x, (float)run.debug.y + 18.0f * (float)(i + 1)}, lines[i],
                      (Color){140, 255, 140, 255}, 0);
    CoreDebugDraw(&run.debug);
}

static void TakeShot(void)
{
    /* A frame can run several ticks, so a shot is due on the first frame at or after each multiple
       of --shot-every, and is named by that multiple: waiting for the exact tick misses it whenever
       the frame that would have shown it ran two ticks. */
    uint64_t now = StoreTickCount(&run.store);
    if (!run.shotEvery || !now)
        return;
    uint64_t tick = now - now % run.shotEvery;
    if (!tick || tick <= run.lastShot)
        return;
    run.lastShot = tick;
    char path[1024];
    snprintf(path, sizeof path, "%s/shot_%" PRIu64 ".png", run.shotDir, tick);
    MakeDirectory(run.shotDir);
    Image image = LoadImageFromScreen();
    bool ok = image.data && ExportImage(image, path);
    UnloadImage(image);
    if (ok)
        printf("shot %" PRIu64 " %s\n", tick, path);
    else
        TraceLog(LOG_WARNING, "RUN: could not write %s", path);
    // --print-draw-position KIND: where the first thing of KIND was drawn in the frame just shot.
    StoreKind kind = run.printDrawKind ? StoreKindNamed(&run.store, run.printDrawKind) : -1;
    StoreId first[1];
    Matrix drawn;
    if (kind >= 0 && StoreThings(&run.store, kind, first, 1) == 1 && World3DDrawMatrix(&run.world, first[0], &drawn))
        printf("draw-position %s %" PRIu64 " %.4f %.4f %.4f\n", run.printDrawKind, tick, (double)drawn.m12,
               (double)drawn.m13, (double)drawn.m14);
    else if (run.printDrawKind)
        printf("draw-position %s %" PRIu64 " none\n", run.printDrawKind, tick);
    fflush(stdout);
}

// Once per rendered frame, before the ticks: the pointer is captured while wanted and focused,
// Escape toggles that, F3 the overlay; neither key reaches the game.
static void FrameInput(void *context, const EngineInput *frame)
{
    (void)context;
    if (frame->pressed[KEY_ESCAPE])
        run.captureWanted = !run.captureWanted;
    if (frame->pressed[KEY_F3])
    {
        run.overlay = !run.overlay;
        printf("overlay %s\n", run.overlay ? "on" : "off");
        fflush(stdout);
    }
    Vector2 delta = CoreMouseCaptureUpdate(&run.capture, run.captureWanted && IsWindowFocused());
    run.mouseDelta = Vector2Add(run.mouseDelta, delta);
    if (!run.cursorReported)
    {
        run.cursorReported = true;
        printf("run: cursor captured %s\n", IsCursorHidden() ? "yes" : "no");
        fflush(stdout);
    }
}

static void Draw(void *context, float alpha)
{
    (void)context;
    const CoreDiagnostics *d = CoreDiagnosticsCurrent();
    if (run.bench && run.frameCount > 0 && d)
        Push(&run.frames, d->frameMicrosLast);
    run.frameCount++;
    float dt = GetFrameTime();
    World3DUpdateTransforms(&run.world, alpha);
    StoreFrame(&run.store, dt);
    CoreParticlesUpdate(&run.particles, dt, NULL, NULL);
    CoreDebugUpdate(&run.debug, dt);
    Camera3D camera = ChooseCamera();
    if (run.audioReady)
    {
        CoreAudioSetListener(&run.audio, camera.position, Vector3Subtract(camera.target, camera.position), camera.up);
        UpdateEmitters();
        CoreAudioUpdate(&run.audio);
    }
    DrawPathBegin(&run.path, camera, GetScreenWidth(), GetScreenHeight());
    SetLights(camera); /* after DrawPathBegin: point lights are chosen against its frustum */
    BeginMode3D(camera);
    AddTilemaps();
    UpdatePoses(dt);
    AddModels(camera);
    run.lastStats = DrawPathEnd(&run.path);
    CoreParticlesDraw(&run.particles, camera, NULL, NULL);
    DrawViewmodels(camera);
    EndMode3D();
    DrawHud();
    DrawErrors();
    DrawOverlay();
    rlDrawRenderBatchActive();
    TakeShot();
    PollRepl();
}

/* ---- Scheme calls ---------------------------------------------------------------------------- */

static bool Presentation(void) { return StorePhaseNow(&run.store) != STORE_PHASE_GAMEPLAY; }

static s7_pointer Refuse(s7_scheme *sc, const char *format, const char *name)
{
    char text[256];
    snprintf(text, sizeof text, format, name);
    return GameS7Error(sc, text);
}

// The argument helpers also check the handler time limit (GameS7LimitCheck).
static bool NumberArg(s7_scheme *sc, s7_pointer p, const char *caller, int position, float *out)
{
    GameS7LimitCheck(sc);
    if (!s7_is_real(p))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a number");
        return false;
    }
    *out = (float)s7_number_to_real(sc, p);
    return true;
}

static bool VecArg(s7_scheme *sc, s7_pointer p, const char *caller, int position, Vector3 *out)
{
    GameS7LimitCheck(sc);
    if (!GameS7ToVec3(p, out))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a vec3");
        return false;
    }
    return true;
}

static bool ThingArg(s7_scheme *sc, s7_pointer p, const char *caller, int position, StoreId *out)
{
    GameS7LimitCheck(sc);
    if (!GameS7ToId(p, out))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a thing");
        return false;
    }
    if (!StoreAlive(&run.store, *out))
    {
        Refuse(sc, "%s: that thing has been removed", caller);
        return false;
    }
    return true;
}

static bool KindArg(s7_scheme *sc, s7_pointer p, const char *caller, StoreKind *out)
{
    const char *name = s7_is_symbol(p) ? s7_symbol_name(p) : s7_is_string(p) ? s7_string(p) : NULL;
    *out = name ? StoreKindNamed(&run.store, name) : -1;
    if (*out < 0)
    {
        char text[160];
        snprintf(text, sizeof text, "%s: no kind named %s", caller, name ? name : "(not a name)");
        GameS7Error(sc, text);
        return false;
    }
    return true;
}

static Color ColorValue(s7_scheme *sc, s7_pointer p, Color fallback)
{
    static const struct
    {
        const char *name;
        Color color;
    } names[] = {{"white", {255, 255, 255, 255}}, {"black", {0, 0, 0, 255}},     {"red", {230, 41, 55, 255}},
                 {"green", {0, 228, 48, 255}},    {"blue", {0, 121, 241, 255}}, {"yellow", {253, 249, 0, 255}},
                 {"gray", {130, 130, 130, 255}},  {"grey", {130, 130, 130, 255}}};
    if (!p)
        return fallback;
    if (s7_is_integer(p))
    {
        uint32_t u = (uint32_t)s7_integer(p);
        return (Color){(unsigned char)(u >> 24), (unsigned char)(u >> 16), (unsigned char)(u >> 8),
                       (unsigned char)u};
    }
    for (size_t i = 0; s7_is_symbol(p) && i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(s7_symbol_name(p), names[i].name))
            return names[i].color;
    s7_wrong_type_arg_error(sc, "color", 0, p, "an integer from rgba or white black red green blue yellow gray");
    return fallback;
}

static s7_pointer Hit(s7_scheme *sc, World3DHit hit)
{
    if (!hit.hit)
        return s7_f(sc);
    return s7_list(sc, 4, GameS7Thing(sc, hit.thing), GameS7Vec3(sc, hit.point), GameS7Vec3(sc, hit.normal),
                   s7_make_real(sc, hit.distance));
}

static s7_pointer SchemeWorldPosition(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    Vector3 at;
    if (!ThingArg(sc, s7_car(args), "world-position", 1, &id))
        return s7_f(sc);
    if (!World3DWorldPosition(&run.world, id, &at))
        return Refuse(sc, "%s: world-position needs a node", StoreKindName(&run.store, StoreKindOf(&run.store, id)));
    return GameS7Vec3(sc, at);
}

static s7_pointer SchemeRaycast(s7_scheme *sc, s7_pointer args)
{
    Vector3 from, dir;
    float max;
    StoreId ignore = STORE_NULL;
    if (!VecArg(sc, s7_car(args), "raycast", 1, &from) || !VecArg(sc, s7_cadr(args), "raycast", 2, &dir) ||
        !NumberArg(sc, s7_caddr(args), "raycast", 3, &max))
        return s7_f(sc);
    // :ignore is a thing, #f, or a list of things (a soldier and the item in its hand, C4).
    s7_pointer p = GameS7KeywordArg(sc, args, "ignore");
    if (p && s7_is_pair(p))
    {
        StoreId ignored[16];
        int count = 0;
        for (; s7_is_pair(p); p = s7_cdr(p))
        {
            if (count == 16)
                return GameS7Error(sc, "raycast: :ignore takes 16 things at most");
            if (s7_car(p) != s7_f(sc) && !ThingArg(sc, s7_car(p), "raycast", 4, &ignored[count++]))
                return s7_f(sc);
        }
        return Hit(sc, World3DRaycastIgnoring(&run.world, from, dir, max, ignored, count, false));
    }
    if (p && p != s7_f(sc) && !s7_is_null(sc, p) && !ThingArg(sc, p, "raycast", 4, &ignore))
        return s7_f(sc);
    return Hit(sc, World3DRaycast(&run.world, from, dir, max, ignore));
}

static s7_pointer SchemeLineOfSight(s7_scheme *sc, s7_pointer args)
{
    Vector3 from, to;
    if (!VecArg(sc, s7_car(args), "line-of-sight?", 1, &from) || !VecArg(sc, s7_cadr(args), "line-of-sight?", 2, &to))
        return s7_f(sc);
    return s7_make_boolean(sc, World3DLineOfSight(&run.world, from, to));
}

typedef struct Where
{
    s7_scheme *sc;
    s7_pointer predicate;
} Where;

static bool AcceptWhere(StoreId id, void *user)
{
    Where *w = user;
    return s7_call(w->sc, w->predicate, s7_list(w->sc, 1, GameS7Thing(w->sc, id))) != s7_f(w->sc);
}

static s7_pointer SchemeNearest(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind;
    Vector3 point;
    float max = -1;
    if (!KindArg(sc, s7_car(args), "nearest", &kind) || !VecArg(sc, s7_cadr(args), "nearest", 2, &point))
        return s7_f(sc);
    s7_pointer p = GameS7KeywordArg(sc, args, "max");
    if (p && !NumberArg(sc, p, "nearest", 3, &max))
        return s7_f(sc);
    Where where = {sc, GameS7KeywordArg(sc, args, "where")};
    if (where.predicate && !s7_is_procedure(where.predicate))
        return s7_wrong_type_arg_error(sc, "nearest", 4, where.predicate, "a procedure");
    StoreId id = World3DNearest(&run.world, kind, point, max, where.predicate ? AcceptWhere : NULL, &where);
    return GameS7Thing(sc, id);
}

static s7_pointer SchemeOverlapping(s7_scheme *sc, s7_pointer args)
{
    StoreId area;
    StoreKind kind;
    if (!ThingArg(sc, s7_car(args), "overlapping", 1, &area) || !KindArg(sc, s7_cadr(args), "overlapping", &kind))
        return s7_nil(sc);
    int n = World3DOverlapping(&run.world, area, kind, NULL, 0);
    if (n < 0)
        return GameS7Error(sc, "overlapping: the first argument must be an area");
    StoreId *ids = malloc(sizeof *ids * (size_t)(n > 0 ? n : 1));
    n = ids ? World3DOverlapping(&run.world, area, kind, ids, n) : 0;
    s7_pointer list = s7_nil(sc);
    for (int i = n - 1; i >= 0; i--)
        list = s7_cons(sc, GameS7Thing(sc, ids[i]), list);
    free(ids);
    return list;
}

// The first camera under a thing, depth first in child order, or STORE_NULL.
static StoreId CameraUnder(StoreId thing)
{
    for (StoreId c = StoreFirstChild(&run.store, thing); StoreAlive(&run.store, c); c = StoreNextSibling(&run.store, c))
    {
        if (StoreKindIs(&run.store, StoreKindOf(&run.store, c), run.world.camera))
            return c;
        StoreId below = CameraUnder(c);
        if (StoreAlive(&run.store, below))
            return below;
    }
    return STORE_NULL;
}

/* (aimed-at camera 'kind distance), or (aimed-at 'kind distance) from a handler: the camera is then
   the first one under the handler's thing (a soldier's eye, C4). Areas count as targets by their
   shape (sphere or box), since a carried item is an area that rays otherwise walk through. */
static s7_pointer SchemeAimedAt(s7_scheme *sc, s7_pointer args)
{
    StoreId camera;
    StoreKind kind;
    float distance;
    Vector3 from;
    if (s7_is_symbol(s7_car(args)))
    {
        if (!s7_is_null(sc, s7_cddr(args)))
            return GameS7Error(sc, "aimed-at: (aimed-at 'kind distance) or (aimed-at camera 'kind distance)");
        StoreId self = StoreCurrent(&run.store);
        camera = StoreAlive(&run.store, self) ? CameraUnder(self) : STORE_NULL;
        if (!StoreAlive(&run.store, camera))
            return Refuse(sc, "aimed-at 'kind distance looks through a camera under the handler's thing, and %s",
                          StoreAlive(&run.store, self) ? "it has none; pass one: (aimed-at eye 'kind distance)"
                                                       : "no handler is running; pass a camera");
        if (!KindArg(sc, s7_car(args), "aimed-at", &kind) || !NumberArg(sc, s7_cadr(args), "aimed-at", 2, &distance))
            return s7_f(sc);
    }
    else if (s7_is_null(sc, s7_cddr(args)))
        return GameS7Error(sc, "aimed-at: (aimed-at 'kind distance) or (aimed-at camera 'kind distance)");
    else if (!ThingArg(sc, s7_car(args), "aimed-at", 1, &camera) || !KindArg(sc, s7_cadr(args), "aimed-at", &kind) ||
             !NumberArg(sc, s7_caddr(args), "aimed-at", 3, &distance))
        return s7_f(sc);
    if (!World3DWorldPosition(&run.world, camera, &from))
        return GameS7Error(sc, "aimed-at: the first argument must be a node");
    // Forward is -Z turned by the camera's rotation, then each ancestor's, from the tick's fields.
    Vector3 dir = {0, 0, -1};
    for (StoreId t = camera; StoreAlive(&run.store, t) && StoreKindIs(&run.store, StoreKindOf(&run.store, t), run.world.node);
         t = StoreParent(&run.store, t))
    {
        StoreValue r;
        if (StoreGet(&run.store, t, run.world.rotation, &r) && r.type == STORE_VEC3)
            dir = Vector3RotateByQuaternion(dir, QuaternionFromEuler(r.as.v.x, r.as.v.y, r.as.v.z));
    }
    StoreId parent = StoreParent(&run.store, camera);
    StoreId ignore = StoreAlive(&run.store, parent) ? parent : camera;
    World3DHit hit = World3DRaycastIgnoring(&run.world, from, dir, distance, &ignore, 1, true);
    for (StoreId t = hit.hit ? hit.thing : STORE_NULL; StoreAlive(&run.store, t); t = StoreParent(&run.store, t))
        if (StoreKindIs(&run.store, StoreKindOf(&run.store, t), kind))
            return GameS7Thing(sc, t);
    return s7_f(sc);
}

static s7_pointer SchemePathNext(s7_scheme *sc, s7_pointer args)
{
    StoreId map;
    Vector3 from, to, next;
    if (!ThingArg(sc, s7_car(args), "path-next", 1, &map) || !VecArg(sc, s7_cadr(args), "path-next", 2, &from) ||
        !VecArg(sc, s7_caddr(args), "path-next", 3, &to))
        return s7_f(sc);
    return World3DPathNext(&run.world, map, from, to, &next) ? GameS7Vec3(sc, next) : s7_f(sc);
}

static s7_pointer SchemeCellToWorld(s7_scheme *sc, s7_pointer args)
{
    StoreId map;
    Vector3 at;
    if (!ThingArg(sc, s7_car(args), "cell->world", 1, &map))
        return s7_f(sc);
    if (!s7_is_integer(s7_cadr(args)) || !s7_is_integer(s7_caddr(args)))
        return s7_wrong_type_arg_error(sc, "cell->world", 2, s7_cdr(args), "two integers");
    return World3DCellToWorld(&run.world, map, (int)s7_integer(s7_cadr(args)), (int)s7_integer(s7_caddr(args)), &at)
               ? GameS7Vec3(sc, at)
               : s7_f(sc);
}

static s7_pointer SchemeWorldToCell(s7_scheme *sc, s7_pointer args)
{
    StoreId map;
    Vector3 at;
    int x, z;
    if (!ThingArg(sc, s7_car(args), "world->cell", 1, &map) || !VecArg(sc, s7_cadr(args), "world->cell", 2, &at))
        return s7_f(sc);
    if (!World3DWorldToCell(&run.world, map, at, &x, &z))
        return s7_f(sc);
    return s7_list(sc, 2, s7_make_integer(sc, x), s7_make_integer(sc, z));
}

// (move-and-slide!) on the running thing, (move-and-slide! thing), or (thing 'move-and-slide!).
static s7_pointer SchemeMoveAndSlide(s7_scheme *sc, s7_pointer args)
{
    StoreId id = StoreCurrent(&run.store);
    if (s7_is_pair(args) && !ThingArg(sc, s7_car(args), "move-and-slide!", 1, &id))
        return s7_f(sc);
    if (StorePhaseNow(&run.store) == STORE_PHASE_PRESENTATION)
        return GameS7Error(sc, "move-and-slide! changes shared state; a presentation handler can't call it");
    if (!World3DMoveAndSlide(&run.world, id, (float)RUN_DT))
        return GameS7Error(sc, "move-and-slide! moves a character; call it from a character's handler or pass one");
    return s7_t(sc);
}

static s7_pointer SchemeTeleport(s7_scheme *sc, s7_pointer args)
{
    // (teleport! where) moves the running thing, as move-and-slide! does; (teleport! thing where) another.
    StoreId id = StoreCurrent(&run.store);
    Vector3 at;
    bool one = !s7_is_pair(s7_cdr(args));
    if ((!one && !ThingArg(sc, s7_car(args), "teleport!", 1, &id)) ||
        !VecArg(sc, one ? s7_car(args) : s7_cadr(args), "teleport!", one ? 1 : 2, &at))
        return s7_f(sc);
    if (StorePhaseNow(&run.store) == STORE_PHASE_PRESENTATION)
        return GameS7Error(sc, "teleport! changes shared state; a presentation handler can't call it");
    if (!World3DTeleport(&run.world, id, at))
        return GameS7Error(sc, "teleport! needs a node");
    return s7_t(sc);
}

static HudCall *HudAdd(s7_scheme *sc, HudType type, const char *caller)
{
    if (!Presentation())
    {
        Refuse(sc, "%s is for presentation: call it from a draw-hud handler", caller);
        return NULL;
    }
    if (run.hudCount == MAX_HUD)
        return NULL;
    HudCall *h = &run.hud[run.hudCount++];
    memset(h, 0, sizeof *h);
    h->type = type;
    h->color = WHITE;
    return h;
}

static s7_pointer SchemeDrawText(s7_scheme *sc, s7_pointer args)
{
    float x, y, size = 20;
    if (!NumberArg(sc, s7_cadr(args), "draw-text", 2, &x) || !NumberArg(sc, s7_caddr(args), "draw-text", 3, &y))
        return s7_f(sc);
    s7_pointer p = GameS7KeywordArg(sc, args, "size");
    if (p && !NumberArg(sc, p, "draw-text", 4, &size))
        return s7_f(sc);
    Color color = ColorValue(sc, GameS7KeywordArg(sc, args, "color"), WHITE);
    s7_pointer align = GameS7KeywordArg(sc, args, "align");
    HudCall *h = HudAdd(sc, HUD_TEXT, "draw-text");
    if (!h)
        return s7_f(sc);
    s7_pointer text = s7_car(args);
    if (s7_is_string(text))
        snprintf(h->text, sizeof h->text, "%s", s7_string(text));
    else
    {
        char *shown = s7_object_to_c_string(sc, text);
        snprintf(h->text, sizeof h->text, "%s", shown ? shown : "");
        free(shown);
    }
    h->x = x;
    h->y = y;
    h->size = (int)size;
    h->color = color;
    if (align && s7_is_symbol(align))
        h->align = !strcmp(s7_symbol_name(align), "center") || !strcmp(s7_symbol_name(align), "centre") ? 1
                   : !strcmp(s7_symbol_name(align), "right")                                             ? 2
                                                                                                         : 0;
    return s7_t(sc);
}

static s7_pointer SchemeDrawRect(s7_scheme *sc, s7_pointer args)
{
    float v[4];
    s7_pointer p = args;
    for (int i = 0; i < 4; i++, p = s7_cdr(p))
        if (!NumberArg(sc, s7_car(p), "draw-rect", i + 1, &v[i]))
            return s7_f(sc);
    Color color = ColorValue(sc, GameS7KeywordArg(sc, args, "color"), WHITE);
    HudCall *h = HudAdd(sc, HUD_RECT, "draw-rect");
    if (h)
        h->x = v[0], h->y = v[1], h->w = v[2], h->h = v[3], h->color = color;
    return s7_t(sc);
}

// (draw-ring x y r [fill] :color c): fill, 0 to 1, draws that much of the ring clockwise from the
// top (a hold ring, C4); 1 when left out.
static s7_pointer SchemeDrawRing(s7_scheme *sc, s7_pointer args)
{
    float v[4] = {0, 0, 0, 1};
    s7_pointer p = args;
    for (int i = 0; i < 3; i++, p = s7_cdr(p))
        if (!NumberArg(sc, s7_car(p), "draw-ring", i + 1, &v[i]))
            return s7_f(sc);
    if (s7_is_pair(p) && !s7_is_keyword(s7_car(p)) && !NumberArg(sc, s7_car(p), "draw-ring", 4, &v[3]))
        return s7_f(sc);
    Color color = ColorValue(sc, GameS7KeywordArg(sc, args, "color"), WHITE);
    HudCall *h = HudAdd(sc, HUD_RING, "draw-ring");
    if (h)
        h->x = v[0], h->y = v[1], h->w = v[2], h->h = v[3] != v[3] ? 1 : fminf(fmaxf(v[3], 0), 1), h->color = color;
    return s7_t(sc);
}

static s7_pointer SchemeDrawImage(s7_scheme *sc, s7_pointer args)
{
    float x, y;
    if (!s7_is_string(s7_car(args)))
        return s7_wrong_type_arg_error(sc, "draw-image", 1, s7_car(args), "a file name");
    if (!NumberArg(sc, s7_cadr(args), "draw-image", 2, &x) || !NumberArg(sc, s7_caddr(args), "draw-image", 3, &y))
        return s7_f(sc);
    HudCall *h = HudAdd(sc, HUD_IMAGE, "draw-image");
    if (h)
    {
        snprintf(h->text, sizeof h->text, "%s", s7_string(s7_car(args)));
        h->x = x;
        h->y = y;
    }
    return s7_t(sc);
}

static s7_pointer SchemeScreenWidth(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_integer(sc, run.gl ? GetScreenWidth() : 1280);
}

static s7_pointer SchemeScreenHeight(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_integer(sc, run.gl ? GetScreenHeight() : 720);
}

// Plays a sound here, at a place or not; false when there is no audio or no such sound.
static bool PlaySoundHere(const char *name, bool placed, Vector3 at)
{
    if (!run.gl || !run.audioReady)
        return false;
    SoundEntry *e = NULL;
    for (int i = 0; i < run.soundCount && !e; i++)
        if (!strcmp(run.sounds[i].name, name))
            e = &run.sounds[i];
    if (!e && run.soundCount < MAX_SOUNDS)
    {
        // Loaded once through the engine's audio service; a missing file is one warning.
        e = &run.sounds[run.soundCount++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", name);
        const char *path = AssetPath(name, e->path, sizeof e->path);
        if (!path)
        {
            TraceLog(LOG_WARNING, "RUN: no sound %s", name);
            run.missingAssets++;
        }
        else
        {
            if (path != e->path)
                snprintf(e->path, sizeof e->path, "%s", path);
            e->loaded = CoreAudioLoadSound(&run.audio, e->path, "game");
            if (!e->loaded)
                TraceLog(LOG_WARNING, "RUN: sound %s could not be loaded or no audio device; it is silent", name);
        }
    }
    if (!e || !e->loaded)
        return false;
    return placed ? CoreAudioPlaySoundAt(&run.audio, e->path, "game", at, 40.0f, 1.0f)
                  : CoreAudioPlaySound(&run.audio, e->path, "game");
}

// Gameplay code's effects are shown on every machine (§9.3): a sound without :at travels with a NaN x.
static void ShareEffect(const char *name, const char *what, Vector3 at)
{
    if (run.netOpen && StorePhaseNow(&run.store) == STORE_PHASE_GAMEPLAY)
        StoreNetEffect(&run.link.net, name, what, at);
}

static s7_pointer SchemePlaySound(s7_scheme *sc, s7_pointer args)
{
    if (!s7_is_string(s7_car(args)))
        return s7_wrong_type_arg_error(sc, "play-sound", 1, s7_car(args), "a file name");
    Vector3 at = {0, 0, 0};
    s7_pointer where = GameS7KeywordArg(sc, args, "at");
    if (where && !VecArg(sc, where, "play-sound", 2, &at))
        return s7_f(sc);
    const char *name = s7_string(s7_car(args));
    ShareEffect("play-sound", name, where ? at : (Vector3){NAN, 0, 0});
    return s7_make_boolean(sc, PlaySoundHere(name, where != NULL, at));
}

static float FxUnit(void) { return (float)(SplitMix(&run.fxState) >> 40) / (float)(1u << 24); }

static const struct
{
    const char *name;
    int count;
    float life, speed, size0, size1, gravity, drag;
    Color color0, color1;
    CoreParticleBlend blend;
} presets[] = {
        {"muzzle-flash", 12, 0.1f, 2.0f, 0.25f, 0.05f, 0.0f, 4.0f, {255, 220, 120, 255}, {255, 120, 0, 0}, CORE_PARTICLE_ADDITIVE},
        {"blood", 24, 0.5f, 3.0f, 0.08f, 0.04f, 9.8f, 1.0f, {160, 0, 0, 255}, {90, 0, 0, 0}, CORE_PARTICLE_ALPHA},
        {"dust", 16, 1.0f, 0.8f, 0.2f, 0.6f, -0.3f, 1.5f, {160, 150, 130, 160}, {160, 150, 130, 0}, CORE_PARTICLE_ALPHA},
        {"sparks", 20, 0.4f, 6.0f, 0.05f, 0.02f, 9.8f, 0.5f, {255, 230, 150, 255}, {255, 120, 0, 0}, CORE_PARTICLE_ADDITIVE},
};

static int PresetNamed(const char *name)
{
    for (int i = 0; name && i < (int)(sizeof presets / sizeof presets[0]); i++)
        if (!strcmp(name, presets[i].name))
            return i;
    return -1;
}

// Emits a preset's particles here; false without a window.
static bool BurstHere(int which, Vector3 at)
{
    if (!run.gl || which < 0)
        return false;
    for (int i = 0; i < presets[which].count; i++)
    {
        Vector3 dir = Vector3Normalize((Vector3){FxUnit() * 2 - 1, FxUnit() * 2 - 1, FxUnit() * 2 - 1});
        CoreParticle particle = {0};
        particle.position = at;
        particle.velocity = Vector3Scale(dir, presets[which].speed * (0.5f + FxUnit()));
        particle.life = presets[which].life * (0.75f + 0.5f * FxUnit());
        particle.size0 = presets[which].size0;
        particle.size1 = presets[which].size1;
        particle.color0 = presets[which].color0;
        particle.color1 = presets[which].color1;
        particle.gravity = presets[which].gravity;
        particle.drag = presets[which].drag;
        particle.texture = run.white;
        particle.blend = presets[which].blend;
        CoreParticleEmit(&run.particles, &particle);
    }
    return true;
}

static s7_pointer SchemeBurst(s7_scheme *sc, s7_pointer args)
{
    s7_pointer name = s7_car(args);
    int which = s7_is_symbol(name) ? PresetNamed(s7_symbol_name(name)) : -1;
    if (which < 0)
        return s7_wrong_type_arg_error(sc, "burst", 1, name, "one of muzzle-flash blood dust sparks");
    Vector3 at = {0, 0, 0};
    s7_pointer p = GameS7KeywordArg(sc, args, "at");
    if (p && !VecArg(sc, p, "burst", 2, &at))
        return s7_f(sc);
    ShareEffect("burst", presets[which].name, at);
    return BurstHere(which, at) ? s7_t(sc) : s7_f(sc);
}

static void ProfilePath(char *buf, size_t size) { snprintf(buf, size, "%s/profile.txt", run.dir); }

static void ProfileRead(void)
{
    if (run.profileRead)
        return;
    run.profileRead = true;
    char path[1024], line[320];
    ProfilePath(path, sizeof path);
    FILE *file = fopen(path, "r");
    while (file && fgets(line, sizeof line, file) && run.profileCount < MAX_PROFILE)
    {
        line[strcspn(line, "\r\n")] = 0;
        char *space = strchr(line, ' ');
        if (!space || space == line)
            continue;
        *space = 0;
        ProfileEntry *e = &run.profile[run.profileCount++];
        snprintf(e->key, sizeof e->key, "%.63s", line);
        snprintf(e->value, sizeof e->value, "%.191s", space + 1);
    }
    if (file)
        fclose(file);
}

static const char *KeyName(s7_pointer key)
{
    return s7_is_symbol(key) ? s7_symbol_name(key) : s7_is_string(key) ? s7_string(key) : NULL;
}

static s7_pointer SchemeProfileRef(s7_scheme *sc, s7_pointer args)
{
    const char *key = KeyName(s7_car(args));
    s7_pointer fallback = s7_is_pair(s7_cdr(args)) ? s7_cadr(args) : s7_f(sc);
    if (!key)
        return s7_wrong_type_arg_error(sc, "profile-ref", 1, s7_car(args), "a symbol or string");
    if (!Presentation())
        return Refuse(sc, "%s is for presentation: gameplay can't read this player's files", "profile-ref");
    ProfileRead();
    for (int i = 0; i < run.profileCount; i++)
        if (!strcmp(run.profile[i].key, key))
        {
            s7_pointer port = s7_open_input_string(sc, run.profile[i].value);
            s7_pointer value = s7_read(sc, port);
            s7_close_input_port(sc, port);
            return value == s7_eof_object(sc) ? fallback : value;
        }
    return fallback;
}

static s7_pointer SchemeProfileSet(s7_scheme *sc, s7_pointer args)
{
    const char *key = KeyName(s7_car(args));
    if (!key || strchr(key, ' '))
        return s7_wrong_type_arg_error(sc, "profile-set!", 1, s7_car(args), "a symbol or string without spaces");
    if (!Presentation())
        return Refuse(sc, "%s is for presentation: gameplay can't write this player's files", "profile-set!");
    ProfileRead();
    char *text = s7_object_to_c_string(sc, s7_cadr(args));
    if (!text || strchr(text, '\n') || strlen(text) >= sizeof run.profile[0].value)
    {
        free(text);
        return GameS7Error(sc, "profile-set!: the value must print on one line of under 192 characters");
    }
    ProfileEntry *e = NULL;
    for (int i = 0; i < run.profileCount && !e; i++)
        if (!strcmp(run.profile[i].key, key))
            e = &run.profile[i];
    if (!e && run.profileCount < MAX_PROFILE)
    {
        e = &run.profile[run.profileCount++];
        snprintf(e->key, sizeof e->key, "%s", key);
    }
    if (e)
        snprintf(e->value, sizeof e->value, "%s", text);
    free(text);
    char path[1024];
    ProfilePath(path, sizeof path);
    CoreAtomicFile atomic;
    FILE *file = CoreAtomicBegin(&atomic, path);
    bool ok = file != NULL;
    for (int i = 0; file && i < run.profileCount; i++)
        ok = fprintf(file, "%s %s\n", run.profile[i].key, run.profile[i].value) > 0 && ok;
    if (!file || !CoreAtomicCommit(&atomic, ok))
        return GameS7Error(sc, "profile-set!: could not write profile.txt");
    return s7_cadr(args);
}

// An action named by a symbol or string, with its current bindings.
static bool ActionArg(s7_scheme *sc, s7_pointer p, const char *caller, const char **name, InputAction *out)
{
    *name = KeyName(p);
    if (!*name)
    {
        s7_wrong_type_arg_error(sc, caller, 1, p, "an action name");
        return false;
    }
    *out = InputMapActionGet(&run.keys, *name);
    if (!out->bindings)
    {
        char text[192];
        snprintf(text, sizeof text, "%s: no action named %.40s; declare it with (define-actions (%.40s \"Key\") ...)",
                 caller, *name, *name);
        GameS7Error(sc, text);
        return false;
    }
    return true;
}

// (rebind! 'action "Key") gives the action that key for this player and saves the keys in the project's
// input.map. Answers #t, or the name of the action that already has the key (nothing changes then).
static s7_pointer SchemeRebind(s7_scheme *sc, s7_pointer args)
{
    const char *name;
    InputAction action;
    if (!ActionArg(sc, s7_car(args), "rebind!", &name, &action))
        return s7_f(sc);
    if (!s7_is_string(s7_cadr(args)))
        return s7_wrong_type_arg_error(sc, "rebind!", 2, s7_cadr(args), "a key name");
    if (!Presentation())
        return Refuse(sc, "%s is for presentation: gameplay can't change this player's keys", "rebind!");
    InputBinding binding;
    if (!ParseKey(s7_string(s7_cadr(args)), &binding))
    {
        char text[160];
        snprintf(text, sizeof text, "rebind!: no key named \"%.40s\"", s7_string(s7_cadr(args)));
        return GameS7Error(sc, text);
    }
    if (action.count == 0)
        return Refuse(sc, "rebind!: %s has no key to change; its define-actions key is not one the engine knows", name);
    const char *conflict;
    if (!InputMapSet(&run.keys, name, 0, binding, &conflict))
        return conflict ? s7_make_symbol(sc, conflict) : Refuse(sc, "rebind!: could not change %s", name);
    char path[sizeof run.dir + 16];
    snprintf(path, sizeof path, "%s/input.map", run.dir);
    if (!InputMapWrite(&run.keys, path))
        return GameS7Error(sc, "rebind!: could not write input.map");
    return s7_t(sc);
}

// (binding 'action) is the name of the action's key ("W", "Space", "Mouse1"), or #f when it has none.
static s7_pointer SchemeBinding(s7_scheme *sc, s7_pointer args)
{
    const char *name;
    InputAction action;
    char letter[2];
    if (!ActionArg(sc, s7_car(args), "binding", &name, &action))
        return s7_f(sc);
    const char *key = action.count ? BindingName(action.bindings[0], letter) : NULL;
    return key ? s7_make_string(sc, key) : s7_f(sc);
}

static s7_pointer SchemeRgba(s7_scheme *sc, s7_pointer args)
{
    // Integers or reals, each rounded and clamped to 0..255: (* 300 hurt) is a fine alpha.
    s7_int c[4] = {0, 0, 0, 255};
    s7_pointer p = args;
    for (int i = 0; i < 4 && s7_is_pair(p); i++, p = s7_cdr(p))
    {
        if (!s7_is_real(s7_car(p)))
            return s7_wrong_type_arg_error(sc, "rgba", i + 1, s7_car(p), "a number 0 to 255");
        double v = s7_is_integer(s7_car(p)) ? (double)s7_integer(s7_car(p)) : s7_real(s7_car(p));
        c[i] = v != v ? 0 : v <= 0 ? 0 : v >= 255 ? 255 : (s7_int)floor(v + 0.5);
    }
    return s7_make_integer(sc, c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3]);
}

static bool RegisterCalls(void)
{
    static const struct
    {
        const char *name;
        GameS7Function function;
        int required, optional;
        bool rest;
        const char *help;
    } calls[] = {
        {"world-position", SchemeWorldPosition, 1, 0, false, "(world-position thing) -> vec3"},
        {"raycast", SchemeRaycast, 3, 0, true, "(raycast from dir max :ignore thing) -> (thing point normal distance) or #f"},
        {"line-of-sight?", SchemeLineOfSight, 2, 0, false, "(line-of-sight? from to): nothing static between"},
        {"nearest", SchemeNearest, 2, 0, true, "(nearest 'kind point :max m :where predicate) -> thing or #f"},
        {"overlapping", SchemeOverlapping, 2, 0, false, "(overlapping area 'kind) -> list"},
        {"aimed-at", SchemeAimedAt, 2, 1, false, "(aimed-at [camera] 'kind distance) -> thing or #f"},
        {"path-next", SchemePathNext, 3, 0, false, "(path-next tilemap from to) -> vec3"},
        {"cell->world", SchemeCellToWorld, 3, 0, false, "(cell->world tilemap x z) -> vec3"},
        {"world->cell", SchemeWorldToCell, 2, 0, false, "(world->cell tilemap point) -> (x z) or #f"},
        {"move-and-slide!", SchemeMoveAndSlide, 0, 1, false, "(move-and-slide! [character]) by velocity * dt"},
        {"teleport!", SchemeTeleport, 1, 1, false, "(teleport! [node] world-position)"},
        {"draw-text", SchemeDrawText, 3, 0, true, "(draw-text text x y :size s :color c :align 'left|'center|'right)"},
        {"draw-rect", SchemeDrawRect, 4, 0, true, "(draw-rect x y w h :color c)"},
        {"draw-ring", SchemeDrawRing, 3, 0, true, "(draw-ring x y r [fill] :color c)"},
        {"draw-image", SchemeDrawImage, 3, 0, false, "(draw-image \"file\" x y)"},
        {"screen-width", SchemeScreenWidth, 0, 0, false, "(screen-width)"},
        {"screen-height", SchemeScreenHeight, 0, 0, false, "(screen-height)"},
        {"play-sound", SchemePlaySound, 1, 0, true, "(play-sound \"file\" :at v)"},
        {"burst", SchemeBurst, 1, 0, true, "(burst 'muzzle-flash|'blood|'dust|'sparks :at v)"},
        {"profile-ref", SchemeProfileRef, 1, 1, false, "(profile-ref 'key default)"},
        {"profile-set!", SchemeProfileSet, 2, 0, false, "(profile-set! 'key value)"},
        {"rebind!", SchemeRebind, 2, 0, false, "(rebind! 'action \"Key\") -> #t, or the action that has the key"},
        {"binding", SchemeBinding, 1, 0, false, "(binding 'action) -> \"Key\" or #f"},
        {"rgba", SchemeRgba, 3, 1, false, "(rgba r g b [a]) -> #xRRGGBBAA"},
    };
    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++)
        if (!GameS7Define(calls[i].name, calls[i].function, calls[i].required, calls[i].optional, calls[i].rest,
                          calls[i].help))
            return false;
    char *answer = NULL;
    bool ok = GameS7DefineMethod("move-and-slide!", SchemeMoveAndSlide) &&
              GameS7DefineMethod("teleport!", SchemeTeleport) &&
              GameS7Eval("(begin (define (hit-thing h) (car h)) (define (hit-point h) (cadr h)) "
                         "(define (hit-normal h) (caddr h)) (define (hit-distance h) (cadddr h)))",
                         &answer);
    if (!ok)
        fprintf(stderr, "trench: %s\n", answer ? answer : "a method could not be added");
    free(answer);
    return ok;
}

/* ---- lifecycle ------------------------------------------------------------------------------- */

static bool Fail(const char *format, const char *detail)
{
    fprintf(stderr, "trench: ");
    fprintf(stderr, format, detail);
    fprintf(stderr, "\n");
    return false;
}

// The engine's bitmap font at 16 px, as core/ui.c loads it: fixed cells positioned by their top-left.
static bool LoadHudFont(void)
{
    run.font = LoadFontEx(run.fontPath, 16, NULL, 0);
    if (!IsFontValid(run.font) || run.font.texture.id == GetFontDefault().texture.id)
        return false;
    for (int i = 0; i < run.font.glyphCount; i++)
        run.font.glyphs[i].offsetX = 0, run.font.glyphs[i].offsetY = 0;
    SetTextureFilter(run.font.texture, TEXTURE_FILTER_POINT);
    run.fontLoaded = true;
    return true;
}

// Fits the window to 80% of its monitor at 16:9, centred.
static void FitWindow(void)
{
    int monitor = GetCurrentMonitor();
    int mw = GetMonitorWidth(monitor), mh = GetMonitorHeight(monitor);
    if (mw <= 0 || mh <= 0)
        return;
    int w = mw * 8 / 10, h = mh * 8 / 10;
    if (w * 9 > h * 16)
        w = h * 16 / 9;
    else
        h = w * 9 / 16;
    Vector2 origin = GetMonitorPosition(monitor);
    SetWindowSize(w, h);
    SetWindowPosition((int)origin.x + (mw - w) / 2, (int)origin.y + (mh - h) / 2);
    printf("run: window %dx%d on monitor %dx%d\n", w, h, mw, mh);
}

static bool InitPresentation(void)
{
    run.gl = true;
    FitWindow();
    DrawPathInit(&run.path);
    ShaderFile files = {run.worldVs, run.worldFs}, skinned = {run.skinVs, run.worldFs};
    char bones[64];
    snprintf(bones, sizeof bones, "#define SKINNING_BONES %d", GPU_SKIN_BONES);
    if (!CoreLoadShaders(&files, 1, &run.passes[0].shader))
        return Fail("could not build the world shader %s", run.worldFs);
    run.passes[0].loaded = true;
    run.passes[1].loaded = CoreLoadShaderDefined(&skinned, bones, &run.passes[1].shader);
    if (!run.passes[1].loaded)
        TraceLog(LOG_WARNING, "RUN: could not build the skinning shader %s; skinned models are posed on the CPU",
                 run.skinVs);
    for (int i = 0; i < 2; i++)
    {
        WorldPass *pass = &run.passes[i];
        pass->normalLoc = GetShaderLocation(pass->shader, "matNormal");
        pass->lightDirLoc = GetShaderLocation(pass->shader, "lightDir");
        pass->lightColorLoc = GetShaderLocation(pass->shader, "lightColor");
        pass->ambientLoc = GetShaderLocation(pass->shader, "ambient");
        pass->fogColorLoc = GetShaderLocation(pass->shader, "fogColor");
        pass->fogDensityLoc = GetShaderLocation(pass->shader, "fogDensity");
        pass->viewPosLoc = GetShaderLocation(pass->shader, "viewPos");
        pass->pointPositionLoc = GetShaderLocation(pass->shader, "pointPosition");
        pass->pointColorLoc = GetShaderLocation(pass->shader, "pointColor");
        pass->pointRangeLoc = GetShaderLocation(pass->shader, "pointRange");
    }
    World3DSetBoneLookup(&run.world, BoneOf, NULL);
    if (!LoadHudFont())
        return Fail("could not load the HUD font %s", run.fontPath);
    run.white = (Texture2D){rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Image grey = GenImageColor(1, 1, (Color){160, 160, 160, 255});
    run.grey = LoadTextureFromImage(grey);
    UnloadImage(grey);
    run.fxState = run.seed ^ 0x5eedull;
    run.audioReady = CoreAudioInit(&run.audio) && CoreAudioAddBus(&run.audio, "game", 1.0f);
    CoreDebugInit(&run.debug, true);
    run.debug.font = &run.font;
    return CoreParticlesInit(&run.particles, 2048) || Fail("out of memory for particles%s", "");
}

static void FreePresentation(void)
{
    World3DSetBoneLookup(&run.world, NULL, NULL);
    FreeStatics();
    for (uint32_t i = 0; i < run.poseCapacity; i++)
    {
        free(run.poses[i].bones);
        free(run.poses[i].meshes);
    }
    free(run.poses);
    run.poses = NULL;
    run.poseCapacity = 0;
    for (int i = 0; i < run.modelCount; i++)
    {
        UnloadModelAnimations(run.models[i].clips, run.models[i].clipCount);
        UnloadModel(run.models[i].model);
        free(run.models[i].meshes);
    }
    for (int i = 0; i < run.textureCount; i++)
        if (run.textures[i].texture.id)
            UnloadTexture(run.textures[i].texture);
    if (run.grey.id)
        UnloadTexture(run.grey);
    if (run.fontLoaded)
        UnloadFont(run.font);
    for (int i = 0; i < 2; i++)
        if (run.passes[i].loaded)
            CoreUnloadShaders(&run.passes[i].shader, 1);
    memset(run.passes, 0, sizeof run.passes);
    FreeEmitters(); /* before the audio service its voices belong to */
    CoreAudioFree(&run.audio);
    CoreDebugFree(&run.debug);
    CoreMouseCaptureRelease(&run.capture);
    DrawPathFree(&run.path);
    CoreParticlesFree(&run.particles);
    run.gl = false;
}

static bool Init(void *context)
{
    (void)context;
    // The prelude is the engine's, found through the root EngineRunApplication just set; the
    // project's own directory becomes the root after that.
    char buf[1024];
    const char *prelude = CoreResolvePath("core/scheme/kinds.scm", buf, sizeof buf);
    if (!prelude || !Readable(prelude))
        return Fail("no Scheme prelude core/scheme/kinds.scm beside the engine%s", "");
    snprintf(run.prelude, sizeof run.prelude, "%s", prelude);
    // The world shader and the HUD font are the engine's too, found the same way.
    static const char *const engineFiles[4] = {"core/shaders/world.vs", "core/shaders/world.fs",
                                               "core/fonts/unifont-17.0.04.bdf", "core/shaders/skinning.vs"};
    char *targets[4] = {run.worldVs, run.worldFs, run.fontPath, run.skinVs};
    for (int i = 0; i < 4; i++)
    {
        const char *path = CoreResolvePath(engineFiles[i], buf, sizeof buf);
        if (!run.headless && (!path || !Readable(path)))
            return Fail("no %s beside the engine", engineFiles[i]);
        snprintf(targets[i], sizeof run.worldVs, "%s", path ? path : "");
    }
    CoreSetDataRoot(run.dir);
    GameS7SetErrorSink(OnError);
    uint64_t recordedKinds = 0;
    if (run.replayPath)
    {
        if (!ReplayOpenRead(&run.replay, run.replayPath, &run.seed, &recordedKinds))
            return Fail("%s is not a recording", run.replayPath);
        run.replaying = true;
    }
    run.botState = run.seed;
    if (!StoreInit(&run.store, run.seed))
        return Fail("out of memory%s", "");
    run.storeOpen = true;
    if (!World3DInit(&run.world, &run.store))
        return Fail("could not declare the built-in 3D kinds%s", "");
    run.worldOpen = true;
    StoreHooks hooks = {0};
    World3DHooks(&run.world, &hooks);
    StoreSetHooks(&run.store, &hooks);
    if (!GameS7Open(&run.store, run.prelude))
        return Fail("could not start Scheme with %s", run.prelude);
    run.scriptOpen = true;
    GameS7Network network = {NULL, NetPlayer, NetPlayers, NetHostCall, NetJoinCall, NetSession};
    GameS7SetNetwork(&network);
    if (!RegisterCalls())
        return Fail("could not register the runner's calls%s", "");
    if (!GameS7LoadGame(run.gameFile))
        return Fail("the game file %s did not load", run.gameFile);
    if (!OpenKeys())
        return Fail("could not set up the key bindings of %s (does define-actions repeat a name?)", run.gameFile);
    if (run.loadPath)
    {
        if (!StoreLoad(&run.store, run.loadPath) || !GameS7RestoreLocalChildren())
            return Fail("could not load %s", run.loadPath);
    }
    StoreKind game = StoreKindNamed(&run.store, "game");
    if (game < 0)
        return Fail("%s declares no kind named game", run.gameFile);
    // The session: from the flags, or replaying, from the recording's header.
    if (run.replaying && (run.hostPort || run.joinPort))
        printf("run: --host and --join are ignored while replaying; the recording says %s\n",
               run.replay.role == REPLAY_ROLE_HOST     ? "it hosted"
               : run.replay.role == REPLAY_ROLE_CLIENT ? "it joined"
                                                       : "it was not networked");
    ReplayRole role = run.replaying ? run.replay.role
                      : run.hostPort ? REPLAY_ROLE_HOST
                      : run.joinPort ? REPLAY_ROLE_CLIENT
                                     : REPLAY_ROLE_NONE;
    if (run.replaying && role == REPLAY_ROLE_CLIENT)
        printf("run: replaying player %d's side of a session\n", run.replay.player);
    // A joining runner spawns no game of its own: the world is the host's (§9.4).
    if (role != REPLAY_ROLE_CLIENT && StoreThings(&run.store, game, NULL, 0) == 0)
    {
        char *answer = NULL;
        bool ok = GameS7Eval("(spawn 'game)", &answer);
        if (!ok)
            fprintf(stderr, "trench: (spawn 'game): %s\n", answer ? answer : "");
        free(answer);
        if (!ok)
            return false;
        /* The engine, not the game, announces players (proposal B3.7): in phase 1 the local player
           joins a fresh world at once. A loaded world already holds their things. Outside a tick
           this queues as a command, delivered in the first tick after the game's start. */
        StoreId root[1];
        if (StoreThings(&run.store, game, root, 1) == 1)
        {
            StoreValue player = {.type = STORE_INT};
            player.as.i = 1;
            StoreSend(&run.store, root[0], StoreIntern(&run.store, "player-joined"), &player, 1);
        }
    }
    static const int owners[] = {0, 1};
    StoreSetLocalOwners(&run.store, owners, 2);
    uint64_t kinds = StoreKindsHash(&run.store);
    if (run.replaying && kinds != recordedKinds)
    {
        fprintf(stderr,
                "trench: %s was recorded against other kinds (kinds hash %016" PRIx64 ", this game's is %016" PRIx64
                "); refusing to replay it\n",
                run.replayPath, recordedKinds, kinds);
        return false;
    }
    if (run.recordPath)
    {
        if (!ReplayOpenWrite(&run.record, run.recordPath, run.seed, "trench phase 2", kinds) ||
            (role != REPLAY_ROLE_NONE && !ReplaySetRole(&run.record, role, role == REPLAY_ROLE_HOST ? 1 : 0)))
            return Fail("could not write %s", run.recordPath);
        run.recording = true;
    }
    run.skipCommands = StoreCommandsPending(&run.store, NULL, NULL, NULL, NULL, 0);
    // Last, so that store_net's hooks go in front of world3d's (StoreNetHost, StoreNetJoin).
    char why[256];
    if (role != REPLAY_ROLE_NONE &&
        !NetStart(role == REPLAY_ROLE_HOST, run.joinAddress,
                  role == REPLAY_ROLE_HOST ? run.hostPort : run.joinPort, true, why, sizeof why))
        return Fail("%s", why);
    if (!run.headless && !InitPresentation())
        return false;
    run.ready = true;
    return true;
}

// --print-field KIND FIELD and --print-count KIND, at the end: `field KIND FIELD VALUE` for the first
// thing of that kind (or `field KIND FIELD none`), and `count KIND N`, derived kinds included.
static void PrintAsked(void)
{
    if (run.printKind)
    {
        StoreKind kind = StoreKindNamed(&run.store, run.printKind);
        StoreId first[1];
        StoreValue v;
        char text[160] = "none";
        if (kind >= 0 && StoreThings(&run.store, kind, first, 1) >= 1 && Field(first[0], run.printField, &v))
            switch (v.type)
            {
            case STORE_INT: snprintf(text, sizeof text, "%d", (int)v.as.i); break;
            case STORE_FLOAT: snprintf(text, sizeof text, "%g", (double)v.as.f); break;
            case STORE_BOOL: snprintf(text, sizeof text, "%s", v.as.b ? "#t" : "#f"); break;
            case STORE_SYMBOL: snprintf(text, sizeof text, "%s", StoreSymbolName(&run.store, v.as.sym)); break;
            case STORE_STRING: snprintf(text, sizeof text, "\"%s\"", v.as.str); break;
            case STORE_VEC3:
                snprintf(text, sizeof text, "%g %g %g", (double)v.as.v.x, (double)v.as.v.y, (double)v.as.v.z);
                break;
            default: snprintf(text, sizeof text, "(a %d)", (int)v.type); break;
            }
        printf("field %s %s %s\n", run.printKind, run.printField, text);
    }
    if (run.countKind)
    {
        StoreKind kind = StoreKindNamed(&run.store, run.countKind);
        printf("count %s %d\n", run.countKind, kind >= 0 ? StoreThings(&run.store, kind, NULL, 0) : 0);
    }
    fflush(stdout);
}

static void Shutdown(void *context)
{
    (void)context;
    const CoreDiagnostics *d = CoreDiagnosticsCurrent();
    if (run.ready)
    {
        if (run.bench && run.lastTicked && d)
            Push(&run.ticks, d->tickMicrosLast - run.sleptMicros);
        if (run.savePath && !StoreSave(&run.store, run.savePath))
            fprintf(stderr, "trench: could not save %s: %s\n", run.savePath, StoreLastError(&run.store));
        if (!run.printedAny || run.lastPrinted != StoreTickCount(&run.store))
            PrintHash();
        if (run.netOpen)
            printf("net state hash %016" PRIx64 "\n", StoreNetStateHash(&run.link.net));
        PrintAsked();
        if (run.bench)
        {
            PrintSamples("tick", &run.ticks);
            if (run.gl)
            {
                PrintSamples("frame", &run.frames);
                printf("bench draw items %d visible %d draws %d shader-switches %d texture-switches %d\n",
                       run.lastStats.items, run.lastStats.visible, run.lastStats.draws,
                       run.lastStats.shaderSwitches, run.lastStats.textureSwitches);
                PrintStatics();
                for (int i = 0; i < 3; i++) /* as the overlay read on the last drawn frame */
                    printf("overlay: %s\n", run.overlayText[i]);
            }
            double seconds = Now() - run.netStarted;
            if (run.netLive && seconds > 0)
                printf("net sent %.0f B/s received %.0f B/s over %.1f s\n", (double)run.link.wireSent / seconds,
                       (double)run.link.wireReceived / seconds, seconds); /* ENet's wire bytes */
            if (run.hudCalls.count)
            {
                qsort(run.hudCalls.items, run.hudCalls.count, sizeof *run.hudCalls.items, CompareDoubles);
                printf("bench hud calls per frame p50 %.0f\n", run.hudCalls.items[(run.hudCalls.count - 1) / 2]);
            }
            fflush(stdout);
        }
    }
    NetClose(); /* before the store and world3d go: it puts their hooks back */
    GameS7SetNetwork(NULL);
    if (run.gl)
        FreePresentation();
    GameS7SetErrorSink(NULL);
    GameS7SetInput(NULL);
    if (run.scriptOpen)
        GameS7Close();
    InputMapFree(&run.keys);
    if (run.worldOpen)
        World3DFree(&run.world);
    if (run.storeOpen)
        StoreFree(&run.store);
    ReplayClose(&run.record);
    ReplayClose(&run.replay);
    free(run.ticks.items);
    free(run.frames.items);
    free(run.hudCalls.items);
    run.ticks = run.frames = run.hudCalls = (Samples){0};
    run.scriptOpen = run.worldOpen = run.storeOpen = run.ready = false;
    CoreSetDataRoot(GetApplicationDirectory());
}

/* ---- arguments ------------------------------------------------------------------------------- */

static int Usage(const char *problem)
{
    if (problem)
        fprintf(stderr, "trench: %s\n", problem);
    fprintf(stderr, "usage: trench run <dir> [--headless] [--ticks N] [--seed S] [--record FILE] "
                    "[--replay FILE] [--hash-every N] [--bot] [--bench] [--save FILE] [--load FILE] "
                    "[--present] [--shot-every N] [--shot-dir DIR] [--no-time-limit] [--host PORT] "
                    "[--join ADDRESS:PORT] [--bot-until N] [--print-field KIND FIELD] [--print-count KIND] "
                    "[--print-draw-position KIND] [--skin-on-cpu] [--no-static-batch] [--repl-file FILE]\n");
    return 2;
}

// engine.project: `game <file.scm>` names the game file; `name <title>` titles the window.
static bool ReadProject(void)
{
    char path[1024], line[1024];
    snprintf(path, sizeof path, "%s/engine.project", run.dir);
    FILE *file = fopen(path, "r");
    if (!file)
        return Fail("no engine.project in %s", run.dir);
    const char *base = strrchr(run.dir, '/');
    snprintf(run.title, sizeof run.title, "%.127s", base ? base + 1 : run.dir);
    while (fgets(line, sizeof line, file))
    {
        line[strcspn(line, "\r\n")] = 0;
        char *key = line + strspn(line, " \t");
        size_t keyLength = strcspn(key, " \t");
        char *value = key + keyLength;
        value += strspn(value, " \t");
        size_t end = strlen(value);
        while (end > 0 && (value[end - 1] == ' ' || value[end - 1] == '\t'))
            value[--end] = 0;
        if (keyLength == 4 && !strncmp(key, "game", 4) && *value)
        {
            if (value[0] == '/')
                snprintf(run.gameFile, sizeof run.gameFile, "%s", value);
            else
                snprintf(run.gameFile, sizeof run.gameFile, "%s/%s", run.dir, value);
        }
        else if (keyLength == 4 && !strncmp(key, "name", 4) && *value)
            snprintf(run.title, sizeof run.title, "%.127s", value);
    }
    fclose(file);
    if (!run.gameFile[0])
        return Fail("%s/engine.project has no `game <file.scm>` line", run.dir);
    return true;
}

static bool Count(const char *text, uint64_t *out)
{
    char *end;
    unsigned long long value = strtoull(text, &end, 0);
    if (!*text || *end)
        return false;
    *out = (uint64_t)value;
    return true;
}

static void FreeReplFile(void)
{
    for (int i = 0; i < run.replFileCount; i++)
        free(run.replFile[i].text);
    free(run.replFile);
    run.replFile = NULL;
    run.replFileCount = run.replFileNext = 0;
}

/* --repl-file FILE: lines `TICK TEXT`, each evaluated at the REPL just before tick TICK (0 is the
   first), in file order; blank lines and lines starting with ; are skipped. A debugging aid, and a
   deterministic REPL for checks. */
static bool ReadReplFile(const char *path)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return fprintf(stderr, "trench: can't read --repl-file %s\n", path), false;
    char line[4096];
    int number = 0;
    bool ok = true;
    while (ok && fgets(line, sizeof line, file))
    {
        number++;
        const char *p = line + strspn(line, " \t");
        if (!*p || *p == '\n' || *p == '\r' || *p == ';')
            continue;
        char *end;
        unsigned long long tick = strtoull(p, &end, 10);
        if (end == p || (*end != ' ' && *end != '\t'))
        {
            fprintf(stderr, "trench: %s:%d: a line is TICK TEXT, such as 30 (inspect (game))\n", path, number);
            ok = false;
            break;
        }
        end += strspn(end, " \t");
        size_t length = strcspn(end, "\r\n");
        void *grown = realloc(run.replFile, (size_t)(run.replFileCount + 1) * sizeof *run.replFile);
        char *text = malloc(length + 1);
        if (grown)
            run.replFile = grown;
        if (!grown || !text)
        {
            free(text);
            ok = false;
            break;
        }
        memcpy(text, end, length);
        text[length] = 0;
        run.replFile[run.replFileCount].tick = tick;
        run.replFile[run.replFileCount++].text = text;
    }
    fclose(file);
    return ok;
}

int GameRun(int argc, char **argv)
{
    memset(&run, 0, sizeof run);
    lastHash = 0;
    SetTraceLogLevel(LOG_WARNING); /* raylib's INFO lines would bury the warnings */
    int i = 1;
    if (i < argc && !strcmp(argv[i], "run"))
        i++;
    if (i >= argc || argv[i][0] == '-')
        return Usage("which project directory?");
    snprintf(run.dir, sizeof run.dir, "%s", argv[i++]);
    for (size_t n = strlen(run.dir); n > 1 && run.dir[n - 1] == '/'; n--)
        run.dir[n - 1] = 0;
    run.seed = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 32);
    double handlerLimit = 0.05; /* proposal B2.6: a handler that runs longer is stopped */
    for (; i < argc; i++)
    {
        const char *flag = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;
        bool takes = true;
        if (!strcmp(flag, "--headless"))
            run.headless = true, takes = false;
        else if (!strcmp(flag, "--bot"))
            run.bot = true, takes = false;
        else if (!strcmp(flag, "--bench"))
            run.bench = true, takes = false;
        else if (!strcmp(flag, "--present"))
            run.present = true, takes = false;
        else if (!strcmp(flag, "--no-time-limit"))
            handlerLimit = 0, takes = false;
        else if (!strcmp(flag, "--skin-on-cpu"))
            run.skinCpuForced = true, takes = false;
        else if (!strcmp(flag, "--no-static-batch"))
            run.noStaticBatch = true, takes = false;
        else if (strcmp(flag, "--ticks") && strcmp(flag, "--seed") && strcmp(flag, "--hash-every") &&
                 strcmp(flag, "--record") && strcmp(flag, "--replay") && strcmp(flag, "--save") &&
                 strcmp(flag, "--load") && strcmp(flag, "--shot-every") && strcmp(flag, "--shot-dir") &&
                 strcmp(flag, "--host") && strcmp(flag, "--join") && strcmp(flag, "--bot-until") &&
                 strcmp(flag, "--print-field") && strcmp(flag, "--print-count") &&
                 strcmp(flag, "--print-draw-position") && strcmp(flag, "--repl-file"))
            return Usage("unknown flag");
        else if (!value)
            return Usage("a flag is missing its value");
        else if (!strcmp(flag, "--print-draw-position"))
            run.printDrawKind = value;
        else if (!strcmp(flag, "--repl-file"))
            run.replFilePath = value;
        else if (!strcmp(flag, "--host"))
        {
            uint64_t port;
            if (!Count(value, &port) || port < 1 || port > 65535)
                return Usage("--host takes a UDP port from 1 to 65535");
            run.hostPort = (int)port;
        }
        else if (!strcmp(flag, "--join"))
        {
            const char *colon = strrchr(value, ':');
            uint64_t port = 0;
            if (!colon || colon == value || (size_t)(colon - value) >= sizeof run.joinAddress ||
                !Count(colon + 1, &port) || port < 1 || port > 65535)
                return Usage("--join takes ADDRESS:PORT, such as 127.0.0.1:7777");
            snprintf(run.joinAddress, sizeof run.joinAddress, "%.*s", (int)(colon - value), value);
            run.joinPort = (int)port;
        }
        else if (!strcmp(flag, "--bot-until"))
        {
            if (!Count(value, &run.botUntil))
                return Usage("--bot-until takes a number");
        }
        else if (!strcmp(flag, "--print-field"))
        {
            if (i + 2 >= argc)
                return Usage("--print-field takes a kind and a field");
            run.printKind = value;
            run.printField = argv[i + 2];
            i++;
        }
        else if (!strcmp(flag, "--print-count"))
            run.countKind = value;
        else if (!strcmp(flag, "--ticks"))
        {
            if (!Count(value, &run.maxTicks))
                return Usage("--ticks takes a number");
        }
        else if (!strcmp(flag, "--seed"))
        {
            if (!Count(value, &run.seed))
                return Usage("--seed takes a number");
        }
        else if (!strcmp(flag, "--hash-every"))
        {
            if (!Count(value, &run.hashEvery))
                return Usage("--hash-every takes a number");
        }
        else if (!strcmp(flag, "--shot-every"))
        {
            if (!Count(value, &run.shotEvery) || !run.shotEvery)
                return Usage("--shot-every takes a number above 0");
        }
        else if (!strcmp(flag, "--shot-dir"))
            run.shotDir = value;
        else if (!strcmp(flag, "--record"))
            run.recordPath = value;
        else if (!strcmp(flag, "--replay"))
            run.replayPath = value;
        else if (!strcmp(flag, "--save"))
            run.savePath = value;
        else
            run.loadPath = value;
        if (takes)
            i++;
    }
    if (run.present && !run.headless)
        return Usage("--present is for --headless runs; a window presents anyway");
    if ((run.shotEvery || run.shotDir) && run.headless)
        return Usage("--shot-every and --shot-dir need a window");
    if ((run.printDrawKind || run.skinCpuForced || run.noStaticBatch) && run.headless)
        return Usage("--print-draw-position, --skin-on-cpu and --no-static-batch need a window (headless draws "
                     "nothing)");
    if (run.printDrawKind && !run.shotEvery)
        return Usage("--print-draw-position prints with each screenshot: give --shot-every too");
    if (run.shotEvery && !run.shotDir)
        run.shotDir = ".";
    if (run.hostPort && run.joinPort)
        return Usage("--host and --join: a machine hosts or joins, not both");
    if (run.replFilePath && run.replayPath)
        printf("run: --repl-file is ignored while replaying; the recording holds the REPL lines it ran\n");
    else if (run.replFilePath && !ReadReplFile(run.replFilePath))
    {
        FreeReplFile();
        return 1;
    }
    // A live session stops itself: a client's ticks start at its welcome, which the engine's own
    // count would not know.
    run.selfStop = !run.headless || ((run.hostPort || run.joinPort) && !run.replayPath);
    if (!ReadProject())
        return FreeReplFile(), 1;
    EngineApplication app = EngineApplicationDefault();
    app.config.title = run.title;
    // Opened at a size every display holds, then fitted to the monitor in Init.
    app.config.width = 1024;
    app.config.height = 576;
    app.config.windowFlags = FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT;
    app.config.fixed_dt = RUN_DT;
    app.config.headless = run.headless;
    app.config.maxTicks = run.selfStop ? 0 : run.maxTicks; /* windowed, Update stops after the last tick is drawn */
    run.captureWanted = true;
    app.callbacks = (EngineProject){Init, FrameInput, Update, Draw, Shutdown};
    app.clearColor = (Color){30, 32, 36, 255};
    GameS7SetHandlerLimit(handlerLimit);
    interruptSeen = 0;
    interruptSaid = false;
    GameS7SetInterrupted(false);
    struct sigaction interrupt, previous;
    memset(&interrupt, 0, sizeof interrupt);
    interrupt.sa_handler = OnInterrupt; /* no SA_RESTART: a blocking read returns at once */
    sigemptyset(&interrupt.sa_mask);
    sigaction(SIGINT, &interrupt, &previous);
    int result = EngineRunApplication(&app);
    sigaction(SIGINT, &previous, NULL);
    FreeReplFile();
    ClearRepl();
    GameS7SetHandlerLimit(0);
    return result || run.failed ? 1 : 0;
}
