/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#define _POSIX_C_SOURCE 200809L
#include "s7.h"
#include "game_s7.h"

#include "core/file.h"
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The store's Scheme frontend (docs/developer/store.md §5). A thing is an s7 c-object whose value
   word is its handle; fields are read and written through the store, which applies the rules; this
   file turns the store's refusals into the sentences of proposal A4. define-kind, its code walk and
   the catch around every handler are Scheme, in core/scheme/kinds.scm. */

typedef char GameS7HandleFitsAPointer[sizeof(void *) >= 8 ? 1 : -1];

/* Per kind, SLOTS Scheme values live in one protected vector. Staged slots hold what the file being
   loaded declares; they replace the live ones only when the whole file has loaded. */
enum
{
    SLOT_HANDLERS,        /* hash table: event symbol -> (procedure . "file:line") */
    SLOT_HELPERS,         /* hash table: helper symbol -> procedure */
    SLOT_HELPER_NAMES,    /* list of helper symbols */
    SLOT_STAGED_HANDLERS,
    SLOT_STAGED_HELPERS,
    SLOT_STAGED_HELPER_NAMES,
    SLOT_CHILDREN,        /* declared children: ((name kind ((key . value) ...) child ...) ...) */
    SLOT_STATES,          /* list of state names */
    SLOTS
};
#define STAGED 3 /* live slot + STAGED is its staged twin */
#define MAX_FIELDS 128
#define MAX_METHODS 32
#define TEXT 1024

typedef struct KindInfo
{
    bool scheme;         /* declared by define-kind */
    bool staged;         /* redeclared by the file loading now */
    int stateField;      /* `state`, when this kind declares states; -1 otherwise */
    StoreSymbol *events[2]; /* events handled: [0] live, [1] staged */
    int eventCount[2], eventCapacity[2];
} KindInfo;

typedef struct View
{
    StoreId id;
    int field;
} View;

typedef struct Method
{
    s7_pointer name;
    GameS7Function function;
} Method;

typedef struct Recent
{
    StoreKind kind;
    StoreSymbol event;
    double at;
} Recent;

static struct
{
    s7_scheme *sc;
    Store *store;
    s7_int thingTag, mapTag, gridTag, snapshotTag;
    s7_pointer env;               /* the game environment, or NULL before a game loads */
    s7_int envLoc;
    s7_pointer roots, things;     /* SLOTS per kind; thing objects by index */
    s7_int rootsLoc, thingsLoc;
    StoreKind *thingKinds;        /* the kind a thing index had, for #<removed soldier> */
    uint32_t *thingGenerations, thingCapacity;
    KindInfo *kinds;
    int kindCapacity;
    StoreSymbol *childSlots;      /* (%child self slot): the child name of each slot */
    int childSlotCount, childSlotCapacity;
    Method methods[MAX_METHODS];
    int methodCount;
    s7_pointer dispatch, loadFile, repl, freeze, guard;
    char gamePath[512], loading[512], prelude[512];
    bool loadingNow, reloadRequested;
    /* the handler running now */
    StoreSymbol event;
    StoreKind eventKind;
    StoreId eventThing;
    const char *location;
    bool goPending;
    StoreId goThing;
    StoreSymbol goState;
    const GameInput *input;
    int actionCount;
    char actionNames[GAME_S7_MAX_ACTIONS][32], actionKeys[GAME_S7_MAX_ACTIONS][32];
    void (*sink)(const char *message);
    Recent recent[64];
    int recentCount;
} game;

static bool Dispatch(Store *store, StoreId self, StoreSymbol event, const StoreValue *args,
                     int count, void *user);
static bool Throttled(StoreKind kind, StoreSymbol event);

// ---- small helpers ----------------------------------------------------------------------------
static bool IsNull(StoreId id) { return id.index == UINT32_MAX; }

static bool IsCollection(StoreType type)
{
    return type == STORE_LIST || type == STORE_SET || type == STORE_MAP || type == STORE_GRID;
}

static bool Prefixed(const char *error, const char *prefix)
{
    size_t length = strlen(prefix);
    return !strncmp(error, prefix, length) && error[length] == ':';
}

static const char *Rest(const char *error) // the sentence after a store error's "prefix: "
{
    const char *colon = strchr(error, ':');
    if (!colon)
        return error;
    colon++;
    while (*colon == ' ')
        colon++;
    return colon;
}

static const char *KindName(StoreKind kind)
{
    const char *name = kind >= 0 ? StoreKindName(game.store, kind) : NULL;
    return name ? name : "thing";
}

static const char *SymbolName(StoreSymbol symbol)
{
    const char *name = StoreSymbolName(game.store, symbol);
    return name ? name : "?";
}

static const char *EventName(void)
{
    return game.event >= 0 ? SymbolName(game.event) : "gameplay";
}

static StoreKind RemovedKind(StoreId id)
{
    return id.index < game.thingCapacity && game.thingGenerations[id.index] == id.generation
               ? game.thingKinds[id.index]
               : -1;
}

static const char *Describe(StoreId id, char *buf, size_t size) // "soldier #12"
{
    StoreKind kind = StoreKindOf(game.store, id);
    snprintf(buf, size, "%s #%u", KindName(kind >= 0 ? kind : RemovedKind(id)), id.index);
    return buf;
}

static const char *Player(int owner, char *buf, size_t size)
{
    if (owner == 0)
        return "the host";
    snprintf(buf, size, "player %d", owner);
    return buf;
}

static char *Copy(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void Report(const char *line)
{
    TraceLog(LOG_ERROR, "%s", line);
    if (game.sink)
        game.sink(line);
}

// ---- the handler time limit (proposal B2.6) ---------------------------------------------------
/* While a handler runs, the clock is read every 256th time s7 enters a body (the begin hook) or a
   script calls one of the engine's functions (GameS7LimitCheck): a loop whose body calls only
   engine functions never enters a body. Past the limit, or on an interrupt, the handler is
   stopped: the begin hook sets all_done, which makes s7 abandon the evaluation and return from the
   dispatcher's s7_call; GameS7LimitCheck raises handler-time-limit, which the dispatcher's catch
   takes quietly. Run then reports it once. Kept outside `game`, which GameS7Open clears. */
static double handlerLimit;               /* seconds; 0: no limit */
static volatile sig_atomic_t interrupted; /* GameS7SetInterrupted, from a signal handler */
static struct
{
    bool armed, stopped, timedOut;
    unsigned calls;
    double start;
} limit;

static bool OverLimit(void)
{
    if (!limit.armed)
        return false;
    if (limit.stopped || interrupted)
        return limit.stopped = true;
    if (handlerLimit <= 0 || ++limit.calls % 256)
        return false;
    if (Now() - limit.start > handlerLimit)
        return limit.stopped = limit.timedOut = true;
    return false;
}

static void BeginHook(s7_scheme *sc, bool *all_done)
{
    (void)sc;
    if (OverLimit() || interrupted) /* outside a handler too: the REPL, a load */
        *all_done = true;
}

void GameS7LimitCheck(s7_scheme *sc)
{
    if (OverLimit())
        s7_error(sc, s7_make_symbol(sc, "handler-time-limit"),
                 s7_list(sc, 1, s7_make_string(sc, "the handler was stopped")));
}

// "the tick handler of rusher #34 ran for over 50 ms and was stopped; the loop at x.scm:212 ..."
static void ReportStopped(void)
{
    const char *where = game.location, *slash = where ? strrchr(where, '/') : NULL;
    char line[TEXT];
    if (slash)
        where = slash + 1;
    snprintf(line, sizeof line,
             "the %s handler of %s #%u ran for over %.0f ms and was stopped; %s%s may never end",
             EventName(), KindName(game.eventKind), game.eventThing.index, handlerLimit * 1000.0,
             where ? "the loop at " : "a loop in it", where ? where : "");
    if (!Throttled(game.eventKind, game.event))
        Report(line);
}

void GameS7SetHandlerLimit(double seconds) { handlerLimit = seconds > 0 ? seconds : 0; }

void GameS7SetInterrupted(bool on) { interrupted = on; }

// ---- errors -----------------------------------------------------------------------------------
/* Every error a script sees is one symbol, game-error, with ("~A" text) as its data, so a script's
   own (catch #t ... (lambda (type info) (apply format #f info))) prints it whole. */
static s7_pointer Fail(s7_scheme *sc, const char *format, ...)
{
    char text[TEXT];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    return s7_error(sc, s7_make_symbol(sc, "game-error"),
                    s7_list(sc, 2, s7_make_string(sc, "~A"), s7_make_string(sc, text)));
}

s7_pointer GameS7Error(s7_scheme *sc, const char *message)
{
    return Fail(sc, "%s", message ? message : "error");
}

static s7_pointer Stale(s7_scheme *sc, StoreId id)
{
    char who[96];
    if (IsNull(id))
        return Fail(sc, "that thing is #f: it was removed, or never was");
    return Fail(sc, "%s has been removed", Describe(id, who, sizeof who));
}

/* The sentence for the store's last refusal (§5.5): rules 1 and 5 about a field get proposal A4's
   text; anything else is the store's own sentence. field is -1 when no field was involved. */
static void FailureText(StoreId target, int field, char *out, size_t size)
{
    Store *store = game.store;
    const char *error = StoreLastError(store);
    StoreKind kind = StoreKindOf(store, target);
    const StoreFieldDecl *d = field >= 0 && kind >= 0 ? StoreFieldAt(store, kind, field) : NULL;
    char a[32], b[32], who[96];
    StoreSymbol child = StoreChildName(store, target);
    const char *name = child != STORE_NO_SYMBOL ? SymbolName(child) : KindName(kind);
    if (d && Prefixed(error, "local-read") && StoreIsLocal(store, target) && !(d->flags & STORE_LOCAL))
        snprintf(out, size,
                 "%s on %s is local (this screen only). The %s handler of %s can't read it, "
                 "because other players and replays don't have it. If gameplay needs it, remove "
                 ":local from %s's declaration.",
                 d->name, name, EventName(), KindName(StoreKindOf(store, StoreCurrent(store))), name);
    else if (d && Prefixed(error, "local-read"))
        snprintf(out, size,
                 "%s is a local field (this screen only). The %s handler of %s can't read it, "
                 "because other players and replays don't have it. If gameplay needs it, remove "
                 ":local from its declaration.",
                 d->name, EventName(), KindName(StoreKindOf(store, StoreCurrent(store))));
    else if (d && Prefixed(error, "shared-write"))
        snprintf(out, size,
                 "%s on %s is shared state; a presentation handler can't write it. Declare the "
                 "field :local, or write it from a gameplay handler.",
                 d->name, name);
    else if (d && Prefixed(error, "not-owner"))
        snprintf(out, size,
                 "%s on %s #%u belongs to %s, and this handler runs for %s, so it can't write it. "
                 "Send a message instead: (send other 'collect 'medkit), with a matching (on "
                 "(collect what) ...) in %s.",
                 d->name, KindName(kind), target.index,
                 Player(StoreOwner(store, target), a, sizeof a),
                 Player(StoreCurrentOwner(store), b, sizeof b), KindName(kind));
    else if (Prefixed(error, "removed") && !IsNull(target))
        snprintf(out, size, "%s has been removed", Describe(target, who, sizeof who));
    else
        snprintf(out, size, "%s", Rest(error));
}

static s7_pointer StoreFailure(s7_scheme *sc, StoreId target, int field)
{
    char text[TEXT];
    FailureText(target, field, text, sizeof text);
    return Fail(sc, "%s", text);
}

static void Rule4Text(const char *what, const char *field, char *out, size_t size)
{
    snprintf(out, size,
             "can't store %s in %s: fields hold data so they can be saved and sent. Store a symbol "
             "and dispatch on it: (set! %s 'explode) ... (case %s ((explode) ...)).",
             what, field, field, field);
}

// ---- values -----------------------------------------------------------------------------------
enum
{
    CONVERTED,
    NOT_DATA,    /* rule 4: why names what it was */
    OUT_OF_RANGE /* why is the whole sentence */
};

static const char *What(s7_scheme *sc, s7_pointer p)
{
    if (s7_is_procedure(p))
        return "a procedure";
    if (s7_is_hash_table(p))
        return "a hash table";
    if (s7_is_input_port(sc, p) || s7_is_output_port(sc, p))
        return "a port";
    if (s7_is_pair(p))
        return "a list";
    if (s7_is_null(sc, p))
        return "an empty list";
    if (s7_is_let(p))
        return "an environment";
    if (s7_is_vector(p))
        return "a vector";
    if (s7_is_c_object(p))
        return s7_c_object_type(p) == game.mapTag    ? "a map view"
               : s7_c_object_type(p) == game.gridTag ? "a grid view"
                                                     : "a snapshot";
    return "that value";
}

bool GameS7ToVec3(s7_pointer p, Vector3 *out)
{
    if (s7_is_float_vector(p) && s7_vector_length(p) == 3)
    {
        s7_double *e = s7_float_vector_elements(p);
        *out = (Vector3){(float)e[0], (float)e[1], (float)e[2]};
        return true;
    }
    if (!game.sc || !s7_is_vector(p) || s7_vector_length(p) != 3)
        return false;
    float v[3];
    for (int i = 0; i < 3; i++)
    {
        s7_pointer e = s7_vector_ref(game.sc, p, i);
        if (!s7_is_real(e))
            return false;
        v[i] = (float)s7_number_to_real(game.sc, e);
    }
    *out = (Vector3){v[0], v[1], v[2]};
    return true;
}

s7_pointer GameS7Vec3(s7_scheme *sc, Vector3 v)
{
    s7_pointer vec = s7_make_float_vector(sc, 3, 1, NULL);
    s7_double *e = s7_float_vector_elements(vec);
    e[0] = v.x;
    e[1] = v.y;
    e[2] = v.z;
    return vec;
}

static s7_pointer Vec(s7_scheme *sc, double x, double y, double z)
{
    s7_pointer vec = s7_make_float_vector(sc, 3, 1, NULL);
    s7_double *e = s7_float_vector_elements(vec);
    e[0] = x;
    e[1] = y;
    e[2] = z;
    return vec;
}

bool GameS7ToId(s7_pointer p, StoreId *out)
{
    if (!game.sc || !s7_is_c_object(p) || s7_c_object_type(p) != game.thingTag)
        return false;
    uint64_t word = (uint64_t)(uintptr_t)s7_c_object_value(p);
    *out = (StoreId){(uint32_t)(word >> 32), (uint32_t)word};
    return true;
}

/* A Scheme value as a store value. want is the field's type when there is one: it turns #f into
   the empty reference and an integer into a float; any other mismatch is left for the store to
   refuse with its type sentence. */
static int Convert(s7_scheme *sc, s7_pointer p, StoreType want, StoreValue *out, const char **why)
{
    memset(out, 0, sizeof *out);
    *why = NULL;
    Vector3 v;
    StoreId id;
    if (s7_is_boolean(p))
    {
        if (p == s7_f(sc) && want == STORE_REF)
        {
            out->type = STORE_REF;
            out->as.ref = STORE_NULL;
        }
        else
        {
            out->type = STORE_BOOL;
            out->as.b = p != s7_f(sc);
        }
    }
    else if (s7_is_integer(p))
    {
        s7_int i = s7_integer(p);
        if (want == STORE_FLOAT)
        {
            out->type = STORE_FLOAT;
            out->as.f = (float)i;
        }
        else if (i < INT32_MIN || i > INT32_MAX)
            return (*why = "an integer field holds 32 bits: -2147483648 to 2147483647"), OUT_OF_RANGE;
        else
        {
            out->type = STORE_INT;
            out->as.i = (int32_t)i;
        }
    }
    else if (s7_is_real(p))
    {
        out->type = STORE_FLOAT;
        out->as.f = (float)s7_number_to_real(sc, p);
    }
    else if (s7_is_symbol(p))
    {
        out->type = STORE_SYMBOL;
        out->as.sym = StoreIntern(game.store, s7_symbol_name(p));
    }
    else if (s7_is_string(p))
    {
        if (s7_string_length(p) > STORE_STRING_MAX)
            return (*why = "a string in a field holds 63 bytes at most"), OUT_OF_RANGE;
        out->type = STORE_STRING;
        memcpy(out->as.str, s7_string(p), (size_t)s7_string_length(p));
    }
    else if (GameS7ToVec3(p, &v))
    {
        out->type = STORE_VEC3;
        out->as.v = v;
    }
    else if (GameS7ToId(p, &id))
    {
        out->type = STORE_REF;
        out->as.ref = id;
    }
    else
        return (*why = What(sc, p)), NOT_DATA;
    return CONVERTED;
}

bool GameS7ToValue(s7_scheme *sc, s7_pointer p, StoreValue *out, const char **why)
{
    const char *reason = NULL;
    bool ok = sc && out && Convert(sc, p, STORE_NONE, out, &reason) == CONVERTED;
    if (why)
        *why = reason;
    return ok;
}

static void ConvertOrFail(s7_scheme *sc, s7_pointer p, StoreType want, const char *field,
                          StoreValue *out)
{
    const char *why;
    int result = Convert(sc, p, want, out, &why);
    char text[TEXT];
    if (result == NOT_DATA)
    {
        Rule4Text(why, field, text, sizeof text);
        Fail(sc, "%s", text);
    }
    else if (result != CONVERTED)
        Fail(sc, "%s: %s", field, why);
}

// ---- things -----------------------------------------------------------------------------------
static bool EnsureThings(uint32_t count)
{
    if (count <= game.thingCapacity)
        return true;
    uint32_t capacity = game.thingCapacity ? game.thingCapacity : 256;
    while (capacity < count)
        capacity *= 2;
    StoreKind *kinds = realloc(game.thingKinds, capacity * sizeof *kinds);
    if (!kinds)
        return false;
    game.thingKinds = kinds;
    uint32_t *generations = realloc(game.thingGenerations, capacity * sizeof *generations);
    if (!generations)
        return false;
    game.thingGenerations = generations;
    s7_scheme *sc = game.sc;
    s7_pointer things = s7_make_vector(sc, capacity);
    for (uint32_t i = 0; i < capacity; i++)
    {
        s7_vector_set(sc, things, i, i < game.thingCapacity ? s7_vector_ref(sc, game.things, i) : s7_f(sc));
        if (i >= game.thingCapacity)
        {
            kinds[i] = -1;
            generations[i] = UINT32_MAX;
        }
    }
    s7_int loc = s7_gc_protect(sc, things);
    if (game.things)
        s7_gc_unprotect_at(sc, game.thingsLoc);
    game.things = things;
    game.thingsLoc = loc;
    game.thingCapacity = capacity;
    return true;
}

s7_pointer GameS7Thing(s7_scheme *sc, StoreId id)
{
    if (!sc || IsNull(id))
        return sc ? s7_f(sc) : NULL;
    uint64_t word = (uint64_t)id.index << 32 | id.generation;
    // One object per handle, so eq? and memq work on things.
    if (EnsureThings(id.index + 1))
    {
        s7_pointer cached = s7_vector_ref(sc, game.things, id.index);
        if (s7_is_c_object(cached) && s7_c_object_type(cached) == game.thingTag &&
            (uint64_t)(uintptr_t)s7_c_object_value(cached) == word)
            return cached;
    }
    s7_pointer thing = s7_make_c_object(sc, game.thingTag, (void *)(uintptr_t)word);
    if (id.index < game.thingCapacity)
    {
        s7_vector_set(sc, game.things, id.index, thing);
        StoreKind kind = StoreKindOf(game.store, id);
        if (kind >= 0)
        {
            game.thingKinds[id.index] = kind;
            game.thingGenerations[id.index] = id.generation;
        }
    }
    return thing;
}

s7_pointer GameS7FromValue(s7_scheme *sc, const StoreValue *value)
{
    switch (value->type)
    {
    case STORE_INT:
        return s7_make_integer(sc, value->as.i);
    case STORE_FLOAT:
        return s7_make_real(sc, value->as.f);
    case STORE_BOOL:
        return s7_make_boolean(sc, value->as.b);
    case STORE_SYMBOL:
    {
        const char *name = StoreSymbolName(game.store, value->as.sym);
        return name ? s7_make_symbol(sc, name) : s7_f(sc);
    }
    case STORE_STRING:
        return s7_make_string(sc, value->as.str);
    case STORE_VEC3:
        return GameS7Vec3(sc, value->as.v);
    case STORE_REF:
        return StoreAlive(game.store, value->as.ref) ? GameS7Thing(sc, value->as.ref) : s7_f(sc);
    case STORE_LIST:
    case STORE_SET:
    case STORE_MAP:
    case STORE_GRID:
        return s7_t(sc);
    default:
        return s7_f(sc);
    }
}

// A live thing argument, or the error that it is not one.
static StoreId ThingArg(s7_scheme *sc, s7_pointer p, const char *caller, int position)
{
    GameS7LimitCheck(sc);
    StoreId id;
    if (!GameS7ToId(p, &id))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a thing");
        return STORE_NULL;
    }
    if (!StoreAlive(game.store, id))
        Stale(sc, id);
    return id;
}

static const char *NameArg(s7_scheme *sc, s7_pointer p, const char *caller, int position)
{
    if (!s7_is_symbol(p))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a symbol");
        return "";
    }
    return s7_symbol_name(p);
}

static double NumberArg(s7_scheme *sc, s7_pointer p, const char *caller, int position)
{
    GameS7LimitCheck(sc);
    if (!s7_is_real(p))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "a number");
        return 0.0;
    }
    return s7_number_to_real(sc, p);
}

static int IntArg(s7_scheme *sc, s7_pointer p, const char *caller, int position)
{
    if (!s7_is_integer(p))
    {
        s7_wrong_type_arg_error(sc, caller, position, p, "an integer");
        return 0;
    }
    return (int)s7_integer(p);
}

// Declared children, searched below each level before the next, so a nested child is in reach.
static StoreId FindChild(StoreId id, StoreSymbol name)
{
    Store *store = game.store;
    StoreId direct = StoreChildNamed(store, id, name);
    if (!IsNull(direct))
        return direct;
    for (StoreId c = StoreFirstChild(store, id); !IsNull(c); c = StoreNextSibling(store, c))
        if (!StoreIsGuest(store, c))
        {
            StoreId found = FindChild(c, name);
            if (!IsNull(found))
                return found;
        }
    return STORE_NULL;
}

// ---- fields -----------------------------------------------------------------------------------
static s7_pointer MakeView(s7_scheme *sc, StoreId id, int field, s7_int tag)
{
    View *view = malloc(sizeof *view);
    if (!view)
        return Fail(sc, "out of memory");
    view->id = id;
    view->field = field;
    return s7_make_c_object(sc, tag, view);
}

static s7_pointer ReadField(s7_scheme *sc, StoreId id, int field)
{
    GameS7LimitCheck(sc);
    Store *store = game.store;
    StoreKind kind = StoreKindOf(store, id);
    if (kind < 0)
        return Stale(sc, id);
    const StoreFieldDecl *d = StoreFieldAt(store, kind, field);
    if (!d)
        return Fail(sc, "%s has no field %d", KindName(kind), field);
    switch (d->type)
    {
    case STORE_LIST:
    case STORE_SET:
    {
        int count = StoreCountOf(store, id, field);
        if (count < 0)
            return StoreFailure(sc, id, field);
        s7_pointer list = s7_nil(sc);
        for (int i = count - 1; i >= 0; i--)
        {
            StoreValue v;
            if (!StoreGetAt(store, id, field, i, NULL, &v))
                return StoreFailure(sc, id, field);
            list = s7_cons(sc, GameS7FromValue(sc, &v), list);
        }
        return list;
    }
    case STORE_MAP:
    case STORE_GRID:
        if (StoreCountOf(store, id, field) < 0) // rule 1 applies to the view as to the field
            return StoreFailure(sc, id, field);
        return MakeView(sc, id, field, d->type == STORE_MAP ? game.mapTag : game.gridTag);
    default:
    {
        StoreValue v;
        if (!StoreGet(store, id, field, &v))
            return StoreFailure(sc, id, field);
        return GameS7FromValue(sc, &v);
    }
    }
}

// Converts every element first, so a bad one leaves the field as it was.
static bool ConvertList(s7_scheme *sc, s7_pointer list, const StoreFieldDecl *d, bool pairs,
                        StoreValue **keys, StoreValue **values, int *count, char *err, size_t size)
{
    *keys = *values = NULL;
    if (!s7_is_proper_list(sc, list))
    {
        const char *what = What(sc, list);
        if (s7_is_procedure(list) || s7_is_hash_table(list) || s7_is_let(list) ||
            s7_is_input_port(sc, list) || s7_is_output_port(sc, list))
            Rule4Text(what, d->name, err, size);
        else
            snprintf(err, size, "%s holds a %s; give it a list%s", d->name,
                     d->type == STORE_MAP ? "map" : d->type == STORE_SET ? "set" : "list",
                     pairs ? " of (key . value) pairs" : "");
        return false;
    }
    int n = (int)s7_list_length(sc, list);
    if (n > d->max)
    {
        snprintf(err, size, "%s holds %d %s at most", d->name, d->max, pairs ? "keys" : "items");
        return false;
    }
    *count = n;
    if (!n)
        return true;
    *values = calloc((size_t)n, sizeof **values);
    *keys = pairs ? calloc((size_t)n, sizeof **keys) : NULL;
    if (!*values || (pairs && !*keys))
    {
        snprintf(err, size, "out of memory");
        return false;
    }
    int i = 0;
    for (s7_pointer p = list; s7_is_pair(p); p = s7_cdr(p), i++)
    {
        s7_pointer item = s7_car(p), value = item;
        const char *why = NULL;
        int result = CONVERTED;
        if (pairs)
        {
            if (!s7_is_pair(item))
            {
                snprintf(err, size, "%s is a map; give it a list of (key . value) pairs", d->name);
                return false;
            }
            value = s7_cdr(item);
            result = Convert(sc, s7_car(item), d->key, &(*keys)[i], &why);
        }
        if (result == CONVERTED)
            result = Convert(sc, value, d->element, &(*values)[i], &why);
        if (result == NOT_DATA)
            Rule4Text(why, d->name, err, size);
        else if (result != CONVERTED)
            snprintf(err, size, "%s: %s", d->name, why);
        if (result != CONVERTED)
            return false;
    }
    return true;
}

/* Writes a field from Scheme: a scalar, a list for LIST and SET, an alist for MAP; a grid only
   through its view. engine writes a spawn's settings, which the spawner decides (rules 1 and 5 are
   not asked of scalars, as for a default). On failure err holds the sentence. */
static bool WriteField(s7_scheme *sc, StoreId id, int field, s7_pointer value, bool engine,
                       char *err, size_t size)
{
    Store *store = game.store;
    StoreKind kind = StoreKindOf(store, id);
    char who[96];
    if (kind < 0)
    {
        snprintf(err, size, "%s has been removed", Describe(id, who, sizeof who));
        return false;
    }
    const StoreFieldDecl *d = StoreFieldAt(store, kind, field);
    if (!d)
    {
        snprintf(err, size, "%s has no field %d", KindName(kind), field);
        return false;
    }
    if (d->type == STORE_GRID)
    {
        snprintf(err, size, "%s is a grid; change its cells with grid-set!, grid-fill! or grid-fill-rect!",
                 d->name);
        return false;
    }
    if (d->type == STORE_LIST || d->type == STORE_SET || d->type == STORE_MAP)
    {
        if (engine && (d->flags & STORE_ENGINE))
            return snprintf(err, size, "%s on %s is written by the engine", d->name, KindName(kind)), false;
        StoreValue *keys, *values;
        int count = 0;
        bool map = d->type == STORE_MAP;
        bool ok = ConvertList(sc, value, d, map, &keys, &values, &count, err, size);
        if (ok && !map)
            ok = (engine ? StoreSetListEngine(store, id, field, values, count)
                         : StoreSetList(store, id, field, values, count)) ||
                 (FailureText(id, field, err, size), false);
        if (ok && map)
        {
            // Replacing a map: empty it, then put each pair.
            int left;
            while (ok && (left = StoreCountOf(store, id, field)) > 0)
            {
                StoreValue key;
                ok = StoreGetAt(store, id, field, 0, &key, NULL) &&
                     StoreMapRemove(store, id, field, &key);
            }
            for (int i = 0; ok && i < count; i++)
                ok = StoreMapSet(store, id, field, &keys[i], &values[i]);
            if (!ok)
                FailureText(id, field, err, size);
        }
        free(keys);
        free(values);
        return ok;
    }
    StoreValue v;
    const char *why;
    int result = Convert(sc, value, d->type, &v, &why);
    if (result == NOT_DATA)
        return Rule4Text(why, d->name, err, size), false;
    if (result != CONVERTED)
        return snprintf(err, size, "%s: %s", d->name, why), false;
    if (engine && (d->flags & STORE_ENGINE))
        return snprintf(err, size, "%s on %s is written by the engine", d->name, KindName(kind)), false;
    if (!(engine ? StoreSetEngine(store, id, field, &v) : StoreSet(store, id, field, &v)))
        return FailureText(id, field, err, size), false;
    return true;
}

static s7_pointer SetField(s7_scheme *sc, StoreId id, int field, s7_pointer value)
{
    GameS7LimitCheck(sc);
    char err[TEXT];
    if (!WriteField(sc, id, field, value, false, err, sizeof err))
        return Fail(sc, "%s", err);
    return value;
}

// ---- the thing type ---------------------------------------------------------------------------
static bool RemoveThing(s7_scheme *sc, StoreId id)
{
    if (!StoreRemove(game.store, id))
        StoreFailure(sc, id, -1);
    return true;
}

// (thing 'name): a declared child, else a field; (thing 'method args...): remove or an added one.
static s7_pointer ThingRef(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId id;
    GameS7ToId(s7_car(args), &id);
    char who[96];
    if (!StoreAlive(store, id))
        return Stale(sc, id);
    if (!s7_is_pair(s7_cdr(args)))
        return Fail(sc, "(%s 'name) needs a field, child or method name", Describe(id, who, sizeof who));
    s7_pointer key = s7_cadr(args), rest = s7_cddr(args);
    const char *name = NameArg(sc, key, "thing", 1);
    StoreKind kind = StoreKindOf(store, id);
    if (s7_is_null(sc, rest))
    {
        StoreId child = FindChild(id, StoreIntern(store, name));
        if (!IsNull(child))
            return GameS7Thing(sc, child);
        int field = StoreFieldIndex(store, kind, name);
        if (field >= 0)
            return ReadField(sc, id, field);
    }
    if (!strcmp(name, "remove"))
        return s7_make_boolean(sc, RemoveThing(sc, id));
    for (int i = 0; i < game.methodCount; i++)
        if (game.methods[i].name == key)
            return game.methods[i].function(sc, s7_cons(sc, s7_car(args), rest));
    return Fail(sc, "%s has no field, child or method named %s", Describe(id, who, sizeof who), name);
}

static s7_pointer ThingSet(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId id;
    GameS7ToId(s7_car(args), &id);
    char who[96];
    if (!StoreAlive(store, id))
        return Stale(sc, id);
    if (s7_list_length(sc, args) != 3)
        return Fail(sc, "(set! (%s 'field) value) takes one field and one value",
                    Describe(id, who, sizeof who));
    const char *name = NameArg(sc, s7_cadr(args), "thing", 1);
    int field = StoreFieldIndex(store, StoreKindOf(store, id), name);
    if (field < 0)
        return Fail(sc, "%s has no field named %s%s", Describe(id, who, sizeof who), name,
                    IsNull(FindChild(id, StoreIntern(store, name))) ? "" : " (that is a child)");
    return SetField(sc, id, field, s7_caddr(args));
}

static s7_pointer ThingString(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    GameS7ToId(s7_car(args), &id);
    char text[128];
    StoreKind kind = StoreKindOf(game.store, id);
    if (kind >= 0)
        snprintf(text, sizeof text, "#<%s #%u>", KindName(kind), id.index);
    else
        snprintf(text, sizeof text, "#<removed %s>", KindName(RemovedKind(id)));
    return s7_make_string(sc, text);
}

static s7_pointer ThingEqual(s7_scheme *sc, s7_pointer args)
{
    StoreId a, b;
    return s7_make_boolean(sc, GameS7ToId(s7_car(args), &a) && GameS7ToId(s7_cadr(args), &b) &&
                                   a.index == b.index && a.generation == b.generation);
}

// ---- map and grid views -----------------------------------------------------------------------
static s7_pointer ViewFree(s7_scheme *sc, s7_pointer obj)
{
    (void)sc;
    free(s7_c_object_value(obj));
    return NULL;
}

static View *ViewArg(s7_scheme *sc, s7_pointer p, s7_int tag, const char *caller, int position)
{
    if (!s7_is_c_object(p) || s7_c_object_type(p) != tag)
    {
        s7_wrong_type_arg_error(sc, caller, position, p, tag == game.mapTag ? "a map field" : "a grid field");
        return NULL;
    }
    return s7_c_object_value(p);
}

static const StoreFieldDecl *ViewDecl(s7_scheme *sc, const View *view)
{
    StoreKind kind = StoreKindOf(game.store, view->id);
    if (kind < 0)
        Stale(sc, view->id);
    return StoreFieldAt(game.store, kind, view->field);
}

// A map's keys (0), values (1) or (key . value) pairs (2); entries naming a removed thing are left
// out. raise false answers #f instead of raising (for printing).
static s7_pointer MapList(s7_scheme *sc, const View *view, int what, bool raise)
{
    Store *store = game.store;
    int count = StoreCountOf(store, view->id, view->field);
    if (count < 0)
        return raise ? StoreFailure(sc, view->id, view->field) : s7_f(sc);
    s7_pointer list = s7_nil(sc);
    for (int i = count - 1; i >= 0; i--)
    {
        StoreValue k, v;
        if (!StoreGetAt(store, view->id, view->field, i, &k, &v))
            return raise ? StoreFailure(sc, view->id, view->field) : s7_f(sc);
        if ((k.type == STORE_REF && !StoreAlive(store, k.as.ref)) ||
            (v.type == STORE_REF && !StoreAlive(store, v.as.ref)))
            continue;
        s7_pointer item = what == 0   ? GameS7FromValue(sc, &k)
                          : what == 1 ? GameS7FromValue(sc, &v)
                                      : s7_cons(sc, GameS7FromValue(sc, &k), GameS7FromValue(sc, &v));
        list = s7_cons(sc, item, list);
    }
    return list;
}

static s7_pointer MapRef(s7_scheme *sc, s7_pointer args)
{
    View *view = s7_c_object_value(s7_car(args));
    const StoreFieldDecl *d = ViewDecl(sc, view);
    if (s7_list_length(sc, args) != 2)
        return Fail(sc, "(%s key) takes one key", d->name);
    StoreValue key, out;
    ConvertOrFail(sc, s7_cadr(args), d->key, d->name, &key);
    if (!StoreMapGet(game.store, view->id, view->field, &key, &out))
        return Prefixed(StoreLastError(game.store), "missing") ? s7_f(sc)
                                                               : StoreFailure(sc, view->id, view->field);
    return GameS7FromValue(sc, &out);
}

static s7_pointer MapSet(s7_scheme *sc, s7_pointer args)
{
    View *view = s7_c_object_value(s7_car(args));
    const StoreFieldDecl *d = ViewDecl(sc, view);
    if (s7_list_length(sc, args) != 3)
        return Fail(sc, "(set! (%s key) value) takes one key and one value", d->name);
    StoreValue key, value;
    ConvertOrFail(sc, s7_cadr(args), d->key, d->name, &key);
    ConvertOrFail(sc, s7_caddr(args), d->element, d->name, &value);
    if (!StoreMapSet(game.store, view->id, view->field, &key, &value))
        return StoreFailure(sc, view->id, view->field);
    return s7_caddr(args);
}

static s7_pointer MapString(s7_scheme *sc, s7_pointer args)
{
    View *view = s7_c_object_value(s7_car(args));
    StoreKind kind = StoreKindOf(game.store, view->id);
    const StoreFieldDecl *d = kind >= 0 ? StoreFieldAt(game.store, kind, view->field) : NULL;
    if (!d)
        return s7_make_string(sc, "#<map of a removed thing>");
    s7_pointer alist = MapList(sc, view, 2, false);
    char *entries = s7_object_to_c_string(sc, alist);
    char text[TEXT];
    snprintf(text, sizeof text, "#<map %s %s>", d->name, entries ? entries : "");
    free(entries);
    return s7_make_string(sc, text);
}

static s7_pointer ViewEqual(s7_scheme *sc, s7_pointer args)
{
    s7_pointer a = s7_car(args), b = s7_cadr(args);
    if (!s7_is_c_object(b) || s7_c_object_type(a) != s7_c_object_type(b))
        return s7_f(sc);
    const View *x = s7_c_object_value(a), *y = s7_c_object_value(b);
    return s7_make_boolean(sc, x->id.index == y->id.index && x->id.generation == y->id.generation &&
                                   x->field == y->field);
}

static s7_pointer SchemeMapKeys(s7_scheme *sc, s7_pointer args)
{
    return MapList(sc, ViewArg(sc, s7_car(args), game.mapTag, "map-keys", 1), 0, true);
}

static s7_pointer SchemeMapValues(s7_scheme *sc, s7_pointer args)
{
    return MapList(sc, ViewArg(sc, s7_car(args), game.mapTag, "map-values", 1), 1, true);
}

static s7_pointer SchemeMapRemove(s7_scheme *sc, s7_pointer args)
{
    View *view = ViewArg(sc, s7_car(args), game.mapTag, "map-remove!", 1);
    const StoreFieldDecl *d = ViewDecl(sc, view);
    StoreValue key;
    ConvertOrFail(sc, s7_cadr(args), d->key, d->name, &key);
    if (StoreMapRemove(game.store, view->id, view->field, &key))
        return s7_t(sc);
    if (Prefixed(StoreLastError(game.store), "missing"))
        return s7_f(sc);
    return StoreFailure(sc, view->id, view->field);
}

static void GridCell(s7_scheme *sc, s7_pointer args, const char *caller, View **view, int *x, int *y)
{
    *view = ViewArg(sc, s7_car(args), game.gridTag, caller, 1);
    *x = IntArg(sc, s7_cadr(args), caller, 2);
    *y = IntArg(sc, s7_caddr(args), caller, 3);
}

static s7_pointer GridGet(s7_scheme *sc, s7_pointer args, const char *caller)
{
    View *view;
    int x, y;
    GridCell(sc, args, caller, &view, &x, &y);
    ViewDecl(sc, view);
    StoreValue out;
    if (!StoreGridGet(game.store, view->id, view->field, x, y, &out))
        return StoreFailure(sc, view->id, view->field);
    return GameS7FromValue(sc, &out);
}

static s7_pointer SchemeGridRef(s7_scheme *sc, s7_pointer args) { return GridGet(sc, args, "grid-ref"); }

static s7_pointer GridApply(s7_scheme *sc, s7_pointer args)
{
    if (s7_list_length(sc, args) != 3)
        return Fail(sc, "(grid x y) takes a column and a row");
    return GridGet(sc, args, "grid");
}

static s7_pointer GridFill(s7_scheme *sc, View *view, int x, int y, int w, int h, s7_pointer value)
{
    const StoreFieldDecl *d = ViewDecl(sc, view);
    StoreValue v;
    ConvertOrFail(sc, value, d->element, d->name, &v);
    if (!StoreGridFill(game.store, view->id, view->field, x, y, w, h, &v))
        return StoreFailure(sc, view->id, view->field);
    return value;
}

static s7_pointer SchemeGridSet(s7_scheme *sc, s7_pointer args)
{
    View *view;
    int x, y;
    GridCell(sc, args, "grid-set!", &view, &x, &y);
    return GridFill(sc, view, x, y, 1, 1, s7_cadddr(args));
}

static s7_pointer SchemeGridFill(s7_scheme *sc, s7_pointer args)
{
    View *view = ViewArg(sc, s7_car(args), game.gridTag, "grid-fill!", 1);
    const StoreFieldDecl *d = ViewDecl(sc, view);
    return GridFill(sc, view, 0, 0, d->max, d->height, s7_cadr(args));
}

static s7_pointer SchemeGridFillRect(s7_scheme *sc, s7_pointer args)
{
    View *view = ViewArg(sc, s7_car(args), game.gridTag, "grid-fill-rect!", 1);
    int n[4];
    s7_pointer p = s7_cdr(args);
    for (int i = 0; i < 4; i++, p = s7_cdr(p))
        n[i] = IntArg(sc, s7_car(p), "grid-fill-rect!", i + 2);
    return GridFill(sc, view, n[0], n[1], n[2], n[3], s7_car(p));
}

static s7_pointer SchemeGridWidth(s7_scheme *sc, s7_pointer args)
{
    return s7_make_integer(sc, ViewDecl(sc, ViewArg(sc, s7_car(args), game.gridTag, "grid-width", 1))->max);
}

static s7_pointer SchemeGridHeight(s7_scheme *sc, s7_pointer args)
{
    return s7_make_integer(sc, ViewDecl(sc, ViewArg(sc, s7_car(args), game.gridTag, "grid-height", 1))->height);
}

static s7_pointer GridString(s7_scheme *sc, s7_pointer args)
{
    View *view = s7_c_object_value(s7_car(args));
    StoreKind kind = StoreKindOf(game.store, view->id);
    const StoreFieldDecl *d = kind >= 0 ? StoreFieldAt(game.store, kind, view->field) : NULL;
    char text[128];
    if (d)
        snprintf(text, sizeof text, "#<grid %s %dx%d>", d->name, d->max, d->height);
    else
        snprintf(text, sizeof text, "#<grid of a removed thing>");
    return s7_make_string(sc, text);
}

// ---- kinds ------------------------------------------------------------------------------------
static bool EnsureKind(StoreKind kind)
{
    if (kind < 0)
        return false;
    if (kind < game.kindCapacity)
        return true;
    int capacity = game.kindCapacity ? game.kindCapacity : 16;
    while (capacity <= kind)
        capacity *= 2;
    KindInfo *kinds = realloc(game.kinds, (size_t)capacity * sizeof *kinds);
    if (!kinds)
        return false;
    game.kinds = kinds;
    memset(kinds + game.kindCapacity, 0, (size_t)(capacity - game.kindCapacity) * sizeof *kinds);
    for (int k = game.kindCapacity; k < capacity; k++)
        kinds[k].stateField = -1;
    s7_scheme *sc = game.sc;
    s7_pointer roots = s7_make_vector(sc, (s7_int)capacity * SLOTS);
    for (s7_int i = 0; i < (s7_int)capacity * SLOTS; i++)
        s7_vector_set(sc, roots, i, i < (s7_int)game.kindCapacity * SLOTS ? s7_vector_ref(sc, game.roots, i) : s7_f(sc));
    s7_int loc = s7_gc_protect(sc, roots);
    if (game.roots)
        s7_gc_unprotect_at(sc, game.rootsLoc);
    game.roots = roots;
    game.rootsLoc = loc;
    game.kindCapacity = capacity;
    return true;
}

static s7_pointer Root(StoreKind kind, int slot)
{
    if (kind < 0 || kind >= game.kindCapacity)
        return s7_f(game.sc);
    return s7_vector_ref(game.sc, game.roots, (s7_int)kind * SLOTS + slot);
}

static void SetRoot(StoreKind kind, int slot, s7_pointer value)
{
    if (EnsureKind(kind))
        s7_vector_set(game.sc, game.roots, (s7_int)kind * SLOTS + slot, value);
}

static bool IsScheme(StoreKind kind) { return kind >= 0 && kind < game.kindCapacity && game.kinds[kind].scheme; }

// The slot a declaration writes: the staged one while its file loads, the live one otherwise.
static int WriteSlot(StoreKind kind, int slot)
{
    return game.kinds[kind].staged ? slot + STAGED : slot;
}

static void AddEvent(KindInfo *info, int which, StoreSymbol event)
{
    for (int i = 0; i < info->eventCount[which]; i++)
        if (info->events[which][i] == event)
            return;
    if (info->eventCount[which] == info->eventCapacity[which])
    {
        int capacity = info->eventCapacity[which] ? info->eventCapacity[which] * 2 : 8;
        StoreSymbol *events = realloc(info->events[which], (size_t)capacity * sizeof *events);
        if (!events)
            return;
        info->events[which] = events;
        info->eventCapacity[which] = capacity;
    }
    info->events[which][info->eventCount[which]++] = event;
}

static bool HasEvent(const KindInfo *info, int which, StoreSymbol event)
{
    for (int i = 0; i < info->eventCount[which]; i++)
        if (info->events[which][i] == event)
            return true;
    return false;
}

static void FreshTables(StoreKind kind, int base)
{
    s7_scheme *sc = game.sc;
    SetRoot(kind, base + SLOT_HANDLERS, s7_make_hash_table(sc, 8));
    SetRoot(kind, base + SLOT_HELPERS, s7_make_hash_table(sc, 8));
    SetRoot(kind, base + SLOT_HELPER_NAMES, s7_nil(sc));
}

// End of a load: the staged declarations go live, or are dropped when the file failed.
static void FinishLoad(bool ok)
{
    s7_scheme *sc = game.sc;
    for (StoreKind k = 0; k < game.kindCapacity; k++)
    {
        KindInfo *info = &game.kinds[k];
        if (!info->staged)
            continue;
        info->staged = false;
        if (ok)
        {
            for (int i = 0; i < info->eventCount[0]; i++)
                if (!HasEvent(info, 1, info->events[0][i]))
                    StoreKindHandles(game.store, k, info->events[0][i], false);
            for (int i = 0; i < info->eventCount[1]; i++)
                StoreKindHandles(game.store, k, info->events[1][i], true);
            StoreSymbol *events = info->events[0];
            int capacity = info->eventCapacity[0];
            info->events[0] = info->events[1];
            info->eventCount[0] = info->eventCount[1];
            info->eventCapacity[0] = info->eventCapacity[1];
            info->events[1] = events;
            info->eventCapacity[1] = capacity;
            for (int slot = SLOT_HANDLERS; slot <= SLOT_HELPER_NAMES; slot++)
                SetRoot(k, slot, Root(k, slot + STAGED));
        }
        info->eventCount[1] = 0;
        for (int slot = SLOT_HANDLERS; slot <= SLOT_HELPER_NAMES; slot++)
            SetRoot(k, slot + STAGED, s7_f(sc));
    }
    game.loadingNow = false;
}

static bool TypeNamed(s7_pointer p, StoreType *out)
{
    if (s7_is_pair(p) && s7_is_symbol(s7_car(p)) && !strcmp(s7_symbol_name(s7_car(p)), "ref"))
        return (*out = STORE_REF), true;
    if (!s7_is_symbol(p))
        return false;
    static const struct
    {
        const char *name;
        StoreType type;
    } names[] = {{"int", STORE_INT},       {"integer", STORE_INT}, {"float", STORE_FLOAT},
                 {"real", STORE_FLOAT},    {"bool", STORE_BOOL},   {"boolean", STORE_BOOL},
                 {"symbol", STORE_SYMBOL}, {"vec3", STORE_VEC3},   {"ref", STORE_REF}};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(s7_symbol_name(p), names[i].name))
            return (*out = names[i].type), true;
    return false;
}

s7_pointer GameS7KeywordArg(s7_scheme *sc, s7_pointer args, const char *name)
{
    for (s7_pointer p = args; s7_is_pair(p); p = s7_cdr(p))
        if (s7_is_keyword(s7_car(p)) && s7_is_pair(s7_cdr(p)) &&
            !strcmp(s7_symbol_name(s7_keyword_to_symbol(sc, s7_car(p))), name))
            return s7_cadr(p);
    return NULL;
}

// One (name spec default flags init) entry %kind-declare receives, as a declaration (§5.2).
static bool ParseField(s7_scheme *sc, const char *kind, s7_pointer entry, StoreFieldDecl *d,
                       s7_pointer *init, char *err, size_t size)
{
    memset(d, 0, sizeof *d);
    s7_pointer name = s7_car(entry), spec = s7_cadr(entry), value = s7_caddr(entry);
    s7_pointer flags = s7_cadddr(entry);
    *init = s7_car(s7_cddddr(entry));
    d->name = s7_symbol_name(name);
    for (s7_pointer p = flags; s7_is_pair(p); p = s7_cdr(p))
        d->flags |= !strcmp(s7_symbol_name(s7_car(p)), "local")    ? STORE_LOCAL
                    : !strcmp(s7_symbol_name(s7_car(p)), "hidden") ? STORE_HIDDEN
                                                                   : 0u;
    if (spec == s7_f(sc))
    {
        const char *why = NULL;
        if (Convert(sc, value, STORE_NONE, &d->init, &why) != CONVERTED || d->init.type == STORE_REF)
        {
            snprintf(err, size,
                     "define-kind %s: the default of %s is %s. A field holds a number, boolean, "
                     "symbol, string or vec3, or names its type: (ref kind), (list-of T :max n), "
                     "(set-of T :max n), (map-of K V :max n) or (grid-of T w h)",
                     kind, d->name, why ? why : "a thing");
            return false;
        }
        d->type = d->init.type;
        return true;
    }
    const char *form = s7_symbol_name(s7_car(spec));
    s7_pointer rest = s7_cdr(spec);
    bool ok = true;
    if (!strcmp(form, "ref"))
        d->type = STORE_REF;
    else if (!strcmp(form, "list-of") || !strcmp(form, "set-of"))
    {
        d->type = form[0] == 'l' ? STORE_LIST : STORE_SET;
        s7_pointer max = GameS7KeywordArg(sc, s7_cdr(rest), "max");
        ok = TypeNamed(s7_car(rest), &d->element) && max && s7_is_integer(max);
        d->max = ok ? (int)s7_integer(max) : 0;
    }
    else if (!strcmp(form, "map-of"))
    {
        d->type = STORE_MAP;
        s7_pointer max = GameS7KeywordArg(sc, s7_cddr(rest), "max");
        ok = TypeNamed(s7_car(rest), &d->key) && TypeNamed(s7_cadr(rest), &d->element) && max &&
             s7_is_integer(max);
        d->max = ok ? (int)s7_integer(max) : 0;
    }
    else if (!strcmp(form, "grid-of"))
    {
        d->type = STORE_GRID;
        ok = s7_list_length(sc, rest) == 3 && TypeNamed(s7_car(rest), &d->element) &&
             s7_is_integer(s7_cadr(rest)) && s7_is_integer(s7_caddr(rest));
        d->max = ok ? (int)s7_integer(s7_cadr(rest)) : 0;
        d->height = ok ? (int)s7_integer(s7_caddr(rest)) : 0;
    }
    if (!ok)
        snprintf(err, size,
                 "define-kind %s: field %s: write (list-of T :max n), (set-of T :max n), (map-of K "
                 "V :max n) or (grid-of T w h), with T one of int, float, bool, symbol, vec3 or "
                 "(ref kind)",
                 kind, d->name);
    return ok;
}

// A collection's :init as its default: a list, an alist, or a grid's rows (or cells row-major).
static bool ApplyInit(s7_scheme *sc, StoreKind kind, int field, s7_pointer init, char *err, size_t size)
{
    Store *store = game.store;
    const StoreFieldDecl *d = StoreFieldAt(store, kind, field);
    if (!s7_is_pair(init))
        return true;
    s7_pointer list = s7_car(init);
    if (!s7_is_list(sc, list))
        return snprintf(err, size, "define-kind %s: the :init of %s is not a list", KindName(kind), d->name), false;
    int index = 0, row = 0;
    for (s7_pointer p = list; s7_is_pair(p); p = s7_cdr(p), row++)
    {
        s7_pointer item = s7_car(p);
        bool rows = d->type == STORE_GRID && s7_is_pair(item); // a grid given as rows
        s7_pointer cells = rows ? item : s7_list(sc, 1, item);
        if (rows)
            index = row * d->max;
        for (s7_pointer c = cells; s7_is_pair(c); c = s7_cdr(c), index++)
        {
            StoreValue key, value;
            const char *why;
            s7_pointer v = s7_car(c);
            memset(&key, 0, sizeof key);
            bool converted = true;
            if (d->type == STORE_MAP)
            {
                converted = s7_is_pair(v) && Convert(sc, s7_car(v), d->key, &key, &why) == CONVERTED;
                v = s7_is_pair(v) ? s7_cdr(v) : v;
            }
            converted = converted && Convert(sc, v, d->element, &value, &why) == CONVERTED;
            if (!converted || !StoreKindSetDefaultAt(store, kind, field, index, &key, &value))
                return snprintf(err, size, "define-kind %s: the :init of %s: %s", KindName(kind), d->name,
                                converted ? Rest(StoreLastError(store)) : "it holds data of another type"),
                       false;
        }
    }
    return true;
}

// Children: the base's (replaced by one of the same name here), then this kind's new ones.
static s7_pointer FindDecl(s7_pointer decls, s7_pointer name)
{
    for (s7_pointer p = decls; s7_is_pair(p); p = s7_cdr(p))
        if (s7_car(s7_car(p)) == name)
            return s7_car(p);
    return NULL;
}

static s7_pointer MergeChildren(s7_scheme *sc, StoreKind base, s7_pointer own)
{
    s7_pointer inherited = Root(base, SLOT_CHILDREN);
    if (!s7_is_pair(inherited))
        return own;
    s7_pointer result = s7_nil(sc);
    for (s7_int i = s7_list_length(sc, own) - 1; i >= 0; i--)
    {
        s7_pointer decl = s7_list_ref(sc, own, i);
        if (!FindDecl(inherited, s7_car(decl)))
            result = s7_cons(sc, decl, result);
    }
    for (s7_int i = s7_list_length(sc, inherited) - 1; i >= 0; i--)
    {
        s7_pointer decl = s7_list_ref(sc, inherited, i), mine = FindDecl(own, s7_car(decl));
        result = s7_cons(sc, mine ? mine : decl, result);
    }
    return result;
}

// Whether a kind's own fields are what a new declaration says; the first that differs, if not.
static const char *ChangedField(StoreKind kind, StoreKind base, const StoreFieldDecl *own, int count)
{
    Store *store = game.store;
    int first = base >= 0 ? StoreFieldCount(store, base) : 0, existing = StoreFieldCount(store, kind) - first;
    for (int i = 0; i < count && i < existing; i++)
    {
        const StoreFieldDecl *a = StoreFieldAt(store, kind, first + i), *b = &own[i];
        if (strcmp(a->name, b->name) || a->type != b->type || a->element != b->element ||
            a->key != b->key || a->max != b->max || a->height != b->height || a->flags != b->flags)
            return b->name;
    }
    if (count > existing)
        return own[existing].name;
    if (existing > count)
        return StoreFieldAt(store, kind, first + count)->name;
    return NULL;
}

// (%kind-declare 'name 'base fields children settings)
static s7_pointer SchemeKindDeclare(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    s7_pointer nameP = s7_car(args), baseP = s7_cadr(args), fieldsP = s7_caddr(args);
    s7_pointer childrenP = s7_cadddr(args), settingsP = s7_car(s7_cddddr(args));
    const char *name = NameArg(sc, nameP, "define-kind", 1);
    StoreKind base = -1;
    if (baseP != s7_f(sc))
    {
        const char *baseName = s7_is_symbol(baseP) ? s7_symbol_name(baseP) : "?";
        base = StoreKindNamed(store, baseName);
        if (base < 0)
            return Fail(sc, "define-kind %s: it extends %s, but there is no kind named %s", name,
                        baseName, baseName);
    }
    int count = (int)s7_list_length(sc, fieldsP), ownCount = 0;
    if (count > MAX_FIELDS)
        return Fail(sc, "define-kind %s: %d fields at most", name, MAX_FIELDS);
    StoreFieldDecl decls[MAX_FIELDS], own[MAX_FIELDS];
    s7_pointer inits[MAX_FIELDS];
    char err[TEXT];
    int i = 0;
    for (s7_pointer p = fieldsP; s7_is_pair(p); p = s7_cdr(p), i++)
    {
        if (!ParseField(sc, name, s7_car(p), &decls[i], &inits[i], err, sizeof err))
            return Fail(sc, "%s", err);
        if (base < 0 || StoreFieldIndex(store, base, decls[i].name) < 0)
        {
            own[ownCount] = decls[i];
            if (IsCollection(own[ownCount].type))
                memset(&own[ownCount].init, 0, sizeof own[ownCount].init);
            ownCount++;
        }
    }
    StoreKind kind = StoreKindNamed(store, name);
    bool redeclared = kind >= 0;
    if (redeclared)
    {
        // Declared before: by a file loaded earlier (a reload), or by the REPL. Kinds keep their
        // ids, so only an identical layout is accepted (§5.7).
        if (!IsScheme(kind))
            return Fail(sc, "define-kind %s: the engine declares a kind named %s already", name, name);
        if (game.kinds[kind].staged)
            return Fail(sc, "define-kind %s: declared twice in %s", name, game.loading);
        if (StoreKindBase(store, kind) != base)
            return Fail(sc, "reload refused: %s now extends %s instead of %s; changing a kind's "
                            "fields waits for phase 2",
                        name, base >= 0 ? KindName(base) : "nothing",
                        StoreKindBase(store, kind) >= 0 ? KindName(StoreKindBase(store, kind)) : "nothing");
        const char *changed = ChangedField(kind, base, own, ownCount);
        if (changed)
            return Fail(sc, "reload refused: kind %s changed field %s; migrating fields waits for "
                            "phase 2",
                        name, changed);
    }
    else
    {
        const char *error = NULL;
        kind = StoreDeclareKind(store, name, base, own, ownCount, &error);
        if (kind < 0)
            return Fail(sc, "define-kind %s: %s", name, Rest(error ? error : StoreLastError(store)));
    }
    if (!EnsureKind(kind))
        return Fail(sc, "out of memory declaring %s", name);
    KindInfo *info = &game.kinds[kind];
    info->scheme = true;
    // Defaults: redeclared base fields, every scalar when declared again, and :init.
    for (i = 0; i < count; i++)
    {
        int field = StoreFieldIndex(store, kind, decls[i].name);
        bool inBase = base >= 0 && StoreFieldIndex(store, base, decls[i].name) >= 0;
        if (IsCollection(decls[i].type))
        {
            if (!ApplyInit(sc, kind, field, inits[i], err, sizeof err))
                return Fail(sc, "%s", err);
        }
        else if ((inBase || redeclared) && !StoreKindSetDefault(store, kind, field, &decls[i].init))
            return Fail(sc, "define-kind %s: %s", name, Rest(StoreLastError(store)));
    }
    // (is base :setting value ...): defaults of the base's fields.
    int baseCount = base >= 0 ? StoreFieldCount(store, base) : 0;
    for (s7_pointer p = settingsP; s7_is_pair(p); p = s7_cdr(p))
    {
        s7_pointer key = s7_car(s7_car(p)), value = s7_cdr(s7_car(p));
        if (!s7_is_symbol(key))
            return Fail(sc, "define-kind %s: (is %s ...) takes :setting value pairs", name, KindName(base));
        const char *setting = s7_symbol_name(key);
        const char *fieldName = !strcmp(setting, "at") ? "position" : setting;
        int field = StoreFieldIndex(store, kind, fieldName);
        if (field < 0 || field >= baseCount)
            return Fail(sc, "define-kind %s: %s has no field %s to set with :%s", name,
                        base >= 0 ? KindName(base) : "its base", fieldName, setting);
        const StoreFieldDecl *d = StoreFieldAt(store, kind, field);
        if (IsCollection(d->type))
            return Fail(sc, "define-kind %s: %s is a %s; give its default with (field %s ... :init ...)",
                        name, d->name, d->type == STORE_MAP ? "map" : "collection", d->name);
        StoreValue v;
        ConvertOrFail(sc, value, d->type, d->name, &v);
        if (!StoreKindSetDefault(store, kind, field, &v))
            return Fail(sc, "define-kind %s: %s", name, Rest(StoreLastError(store)));
    }
    if (game.loadingNow)
    {
        info->staged = true;
        info->eventCount[1] = 0;
        FreshTables(kind, STAGED);
    }
    else
    {
        // Declared again from the REPL: its old handlers go.
        for (int e = 0; e < info->eventCount[0]; e++)
            StoreKindHandles(store, kind, info->events[0][e], false);
        info->eventCount[0] = 0;
        FreshTables(kind, 0);
    }
    info->stateField = -1;
    SetRoot(kind, SLOT_STATES, s7_f(sc));
    SetRoot(kind, SLOT_CHILDREN, MergeChildren(sc, base, childrenP));
    StoreKindSetHandler(store, kind, Dispatch, NULL);
    return nameP;
}

static StoreKind SchemeKindArg(s7_scheme *sc, s7_pointer p, const char *caller)
{
    StoreKind kind = StoreKindNamed(game.store, NameArg(sc, p, caller, 1));
    if (!IsScheme(kind))
        Fail(sc, "%s: %s is not a kind declared with define-kind", caller, s7_symbol_name(p));
    return kind;
}

// (%kind-handler 'kind 'event procedure line ['state])
static s7_pointer SchemeKindHandler(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = SchemeKindArg(sc, s7_car(args), "%kind-handler");
    const char *event = NameArg(sc, s7_cadr(args), "%kind-handler", 2);
    s7_pointer procedure = s7_caddr(args), rest = s7_cdddr(args);
    s7_pointer line = s7_car(rest), state = s7_is_pair(s7_cdr(rest)) ? s7_cadr(rest) : NULL;
    char key[160], where[600];
    if (state)
        snprintf(key, sizeof key, "%s:%s", s7_symbol_name(state), event);
    else
        snprintf(key, sizeof key, "%s", event);
    snprintf(where, sizeof where, "%s:%lld", game.loadingNow ? game.loading : "repl",
             s7_is_integer(line) ? (long long)s7_integer(line) : 0LL);
    KindInfo *info = &game.kinds[kind];
    s7_hash_table_set(sc, Root(kind, WriteSlot(kind, SLOT_HANDLERS)), s7_make_symbol(sc, key),
                      s7_cons(sc, procedure, s7_make_string(sc, where)));
    StoreSymbol symbol = StoreIntern(game.store, event);
    AddEvent(info, info->staged ? 1 : 0, symbol);
    if (!info->staged)
        StoreKindHandles(game.store, kind, symbol, true);
    return s7_t(sc);
}

// (%kind-helper 'kind 'name procedure)
static s7_pointer SchemeKindHelper(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = SchemeKindArg(sc, s7_car(args), "%kind-helper");
    s7_pointer name = s7_cadr(args);
    s7_hash_table_set(sc, Root(kind, WriteSlot(kind, SLOT_HELPERS)), name, s7_caddr(args));
    int names = WriteSlot(kind, SLOT_HELPER_NAMES);
    SetRoot(kind, names, s7_cons(sc, name, Root(kind, names)));
    return s7_t(sc);
}

// (%kind-states 'kind 'initial '(state ...))
static s7_pointer SchemeKindStates(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = SchemeKindArg(sc, s7_car(args), "%kind-states");
    s7_pointer initial = s7_cadr(args), names = s7_caddr(args);
    bool known = false;
    for (s7_pointer p = names; s7_is_pair(p); p = s7_cdr(p))
        known = known || s7_car(p) == initial;
    if (!known)
        return Fail(sc, "define-kind %s: the first state, %s, is not one of its states",
                    KindName(kind), s7_is_symbol(initial) ? s7_symbol_name(initial) : "?");
    SetRoot(kind, SLOT_STATES, names);
    game.kinds[kind].stateField = StoreFieldIndex(game.store, kind, "state");
    return s7_t(sc);
}

static s7_pointer SchemeKindFieldNames(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = s7_is_symbol(s7_car(args)) ? StoreKindNamed(game.store, s7_symbol_name(s7_car(args))) : -1;
    if (kind < 0)
        return s7_f(sc);
    s7_pointer list = s7_nil(sc);
    for (int f = StoreFieldCount(game.store, kind) - 1; f >= 0; f--)
        list = s7_cons(sc, s7_make_symbol(sc, StoreFieldAt(game.store, kind, f)->name), list);
    return list;
}

static s7_pointer ChildNames(s7_scheme *sc, s7_pointer decls, s7_pointer names)
{
    for (s7_pointer p = decls; s7_is_pair(p); p = s7_cdr(p))
    {
        names = s7_cons(sc, s7_car(s7_car(p)), names);
        names = ChildNames(sc, s7_cdddr(s7_car(p)), names);
    }
    return names;
}

static s7_pointer SchemeKindChildNames(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = s7_is_symbol(s7_car(args)) ? StoreKindNamed(game.store, s7_symbol_name(s7_car(args))) : -1;
    return ChildNames(sc, Root(kind, SLOT_CHILDREN), s7_nil(sc));
}

static s7_pointer SchemeKindHelperNames(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = s7_is_symbol(s7_car(args)) ? StoreKindNamed(game.store, s7_symbol_name(s7_car(args))) : -1;
    s7_pointer list = s7_nil(sc);
    for (StoreKind k = kind; k >= 0; k = StoreKindBase(game.store, k))
        if (IsScheme(k))
            for (s7_pointer p = Root(k, WriteSlot(k, SLOT_HELPER_NAMES)); s7_is_pair(p); p = s7_cdr(p))
                list = s7_cons(sc, s7_car(p), list);
    return list;
}

// (%child-slot 'name): a number for a child name, the same for every kind that declares one.
static s7_pointer SchemeChildSlot(s7_scheme *sc, s7_pointer args)
{
    StoreSymbol name = StoreIntern(game.store, NameArg(sc, s7_car(args), "%child-slot", 1));
    for (int i = 0; i < game.childSlotCount; i++)
        if (game.childSlots[i] == name)
            return s7_make_integer(sc, i);
    if (game.childSlotCount == game.childSlotCapacity)
    {
        int capacity = game.childSlotCapacity ? game.childSlotCapacity * 2 : 32;
        StoreSymbol *slots = realloc(game.childSlots, (size_t)capacity * sizeof *slots);
        if (!slots)
            return Fail(sc, "out of memory");
        game.childSlots = slots;
        game.childSlotCapacity = capacity;
    }
    game.childSlots[game.childSlotCount] = name;
    return s7_make_integer(sc, game.childSlotCount++);
}

// ---- field access from walked code ------------------------------------------------------------
static s7_pointer SchemeField(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    if (!GameS7ToId(s7_car(args), &id))
        return s7_wrong_type_arg_error(sc, "%field", 1, s7_car(args), "a thing");
    return ReadField(sc, id, (int)s7_integer(s7_cadr(args)));
}

static s7_pointer SchemeSetField(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    if (!GameS7ToId(s7_car(args), &id))
        return s7_wrong_type_arg_error(sc, "%set-field!", 1, s7_car(args), "a thing");
    return SetField(sc, id, (int)s7_integer(s7_cadr(args)), s7_caddr(args));
}

static s7_pointer SchemeChild(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "%child", 1);
    int slot = (int)s7_integer(s7_cadr(args));
    char who[96];
    if (slot < 0 || slot >= game.childSlotCount)
        return Fail(sc, "no child slot %d", slot);
    StoreId child = FindChild(id, game.childSlots[slot]);
    if (IsNull(child))
        return Fail(sc, "%s has no child named %s (it was removed, or never spawned)",
                    Describe(id, who, sizeof who), SymbolName(game.childSlots[slot]));
    return GameS7Thing(sc, child);
}

// (%helper-ref self 'name): the helper of self's kind, or of the nearest base declaring it.
static s7_pointer SchemeHelperRef(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "%helper-ref", 1);
    for (StoreKind k = StoreKindOf(game.store, id); k >= 0; k = StoreKindBase(game.store, k))
        if (IsScheme(k))
        {
            s7_pointer table = Root(k, WriteSlot(k, SLOT_HELPERS));
            s7_pointer helper = s7_is_hash_table(table) ? s7_hash_table_ref(sc, table, s7_cadr(args)) : s7_f(sc);
            if (helper != s7_f(sc))
                return helper;
        }
    char who[96];
    return Fail(sc, "%s has no helper named %s", Describe(id, who, sizeof who),
                s7_is_symbol(s7_cadr(args)) ? s7_symbol_name(s7_cadr(args)) : "?");
}

// ---- the dispatcher (§5.4) --------------------------------------------------------------------
static int StateField(StoreKind kind)
{
    for (StoreKind k = kind; k >= 0; k = StoreKindBase(game.store, k))
        if (IsScheme(k) && game.kinds[k].stateField >= 0)
            return game.kinds[k].stateField;
    return -1;
}

static s7_pointer Lookup(StoreKind kind, s7_pointer event)
{
    s7_scheme *sc = game.sc;
    for (StoreKind k = kind; k >= 0; k = StoreKindBase(game.store, k))
        if (IsScheme(k))
        {
            s7_pointer table = Root(k, SLOT_HANDLERS);
            s7_pointer entry = s7_is_hash_table(table) ? s7_hash_table_ref(sc, table, event) : s7_f(sc);
            if (entry != s7_f(sc))
                return entry;
        }
    return NULL;
}

// A state's handler replaces the kind's for the same event (B2.5); then base kinds.
static s7_pointer FindHandler(StoreId self, StoreKind kind, const char *event)
{
    s7_scheme *sc = game.sc;
    int field = StateField(kind);
    StoreValue state;
    if (field >= 0 && StoreGet(game.store, self, field, &state) && state.type == STORE_SYMBOL &&
        state.as.sym >= 0)
    {
        char key[160];
        snprintf(key, sizeof key, "%s:%s", SymbolName(state.as.sym), event);
        s7_pointer entry = Lookup(kind, s7_make_symbol(sc, key));
        if (entry)
            return entry;
    }
    return Lookup(kind, s7_make_symbol(sc, event));
}

static bool Run(StoreId self, StoreKind kind, StoreSymbol event, s7_pointer entry, s7_pointer args)
{
    s7_scheme *sc = game.sc;
    StoreSymbol oldEvent = game.event;
    StoreKind oldKind = game.eventKind;
    StoreId oldThing = game.eventThing;
    const char *oldLocation = game.location;
    game.event = event;
    game.eventKind = kind;
    game.eventThing = self;
    game.location = s7_is_string(s7_cdr(entry)) ? s7_string(s7_cdr(entry)) : NULL;
    bool outer = !limit.armed; // a nested handler counts against the one that called it
    if (outer)
    {
        memset(&limit, 0, sizeof limit);
        limit.armed = true;
        limit.start = Now();
    }
    s7_pointer result = s7_call(sc, game.dispatch, s7_list(sc, 2, s7_car(entry), args));
    if (outer)
    {
        if (limit.timedOut)
            ReportStopped();
        if (limit.stopped)
            result = s7_f(sc);
        memset(&limit, 0, sizeof limit);
    }
    game.event = oldEvent;
    game.eventKind = oldKind;
    game.eventThing = oldThing;
    game.location = oldLocation;
    return result != s7_f(sc);
}

static bool RunNamed(StoreId self, StoreKind kind, const char *event)
{
    s7_pointer entry = FindHandler(self, kind, event);
    if (!entry)
        return true;
    return Run(self, kind, StoreIntern(game.store, event), entry,
               s7_list(game.sc, 1, GameS7Thing(game.sc, self)));
}

// After a handler: a pending (go 'state) runs exit in the old state, then enter in the new one.
static void ApplyGo(StoreId self, StoreKind kind)
{
    for (int hops = 0; game.goPending && hops < 8; hops++)
    {
        game.goPending = false;
        if (game.goThing.index != self.index || game.goThing.generation != self.generation ||
            !StoreAlive(game.store, self))
            return;
        StoreValue next;
        memset(&next, 0, sizeof next);
        next.type = STORE_SYMBOL;
        next.as.sym = game.goState;
        if (!RunNamed(self, kind, "exit") || !StoreAlive(game.store, self))
            return;
        if (!StoreSet(game.store, self, StateField(kind), &next))
        {
            char text[TEXT], line[TEXT + 128];
            FailureText(self, StateField(kind), text, sizeof text);
            snprintf(line, sizeof line, "%s #%u go: %s", KindName(kind), self.index, text);
            Report(line);
            return;
        }
        if (!RunNamed(self, kind, "enter"))
            return;
    }
}

static bool Dispatch(Store *store, StoreId self, StoreSymbol event, const StoreValue *args,
                     int count, void *user)
{
    (void)user;
    s7_scheme *sc = game.sc;
    if (!sc || store != game.store)
        return false;
    StoreKind kind = StoreKindOf(store, self);
    const char *name = StoreSymbolName(store, event);
    if (kind < 0 || !name)
        return false;
    s7_pointer entry = FindHandler(self, kind, name);
    if (!entry)
        return false; // e.g. a tick only some states handle
    s7_pointer list = s7_nil(sc);
    for (int i = count - 1; i >= 0; i--)
        list = s7_cons(sc, GameS7FromValue(sc, &args[i]), list);
    list = s7_cons(sc, GameS7Thing(sc, self), list);
    game.goPending = false;
    bool ok = Run(self, kind, event, entry, list);
    if (ok)
        ApplyGo(self, kind);
    game.goPending = false;
    return ok;
}

// ---- error reports ----------------------------------------------------------------------------
static bool Throttled(StoreKind kind, StoreSymbol event)
{
    double now = Now();
    int oldest = 0;
    for (int i = 0; i < game.recentCount; i++)
    {
        Recent *r = &game.recent[i];
        if (r->kind == kind && r->event == event)
        {
            if (now - r->at < 1.0)
                return true;
            r->at = now;
            return false;
        }
        if (r->at < game.recent[oldest].at)
            oldest = i;
    }
    int slot = game.recentCount < (int)(sizeof game.recent / sizeof game.recent[0]) ? game.recentCount++ : oldest;
    game.recent[slot] = (Recent){kind, event, now};
    return false;
}

// Rule 2: s7's "can't set! score (it is immutable)" said as proposal A4 says it.
static bool Rule2(s7_scheme *sc, s7_pointer type, s7_pointer info, char *out, size_t size)
{
    if (!s7_is_symbol(type) || strcmp(s7_symbol_name(type), "immutable-error") ||
        !s7_is_pair(info) || s7_list_length(sc, info) < 3)
        return false;
    s7_pointer how = s7_list_ref(sc, info, 1), what = s7_list_ref(sc, info, 2);
    if (!s7_is_symbol(how) || strcmp(s7_symbol_name(how), "set!") || !s7_is_symbol(what))
        return false;
    const char *name = s7_symbol_name(what);
    char value[64] = "...";
    s7_pointer v = game.env ? s7_let_ref(sc, game.env, what) : s7_f(sc);
    if (s7_is_number(v) || s7_is_boolean(v) || s7_is_string(v))
    {
        char *text = s7_object_to_c_string(sc, v);
        snprintf(value, sizeof value, "%s", text ? text : "0");
        free(text);
    }
    else if (s7_is_symbol(v))
        snprintf(value, sizeof value, "'%s", s7_symbol_name(v));
    snprintf(out, size,
             "can't set! %s: top-level definitions are frozen once the game has loaded, because "
             "they aren't saved, sent to other players or replayed. Keep game-wide values on the "
             "game: add (field %s %s) to (define-kind game ...) and write (set! %s ...) in its "
             "handlers, or (set! ((game) '%s) ...) elsewhere.",
             name, name, value, name, name);
    return true;
}

// (%report context type info text file line), from the catch in kinds.scm.
static s7_pointer SchemeReport(s7_scheme *sc, s7_pointer args)
{
    int context = (int)s7_integer(s7_car(args));
    s7_pointer type = s7_cadr(args), info = s7_caddr(args), text = s7_cadddr(args);
    s7_pointer rest = s7_cddddr(args), file = s7_car(rest), line = s7_cadr(rest);
    char message[TEXT], where[600] = "", full[2 * TEXT];
    if (s7_is_symbol(type) && !strcmp(s7_symbol_name(type), "handler-time-limit"))
        return s7_make_string(sc, "the handler was stopped"); // Run reports it
    if (!Rule2(sc, type, info, message, sizeof message))
        snprintf(message, sizeof message, "%s", s7_is_string(text) ? s7_string(text) : "error");
    // Code the walk rewrote has no line of its own, and s7 then names the prelude's catch; the
    // handler's own (on ...) line is more use.
    if (s7_is_string(file) && s7_is_integer(line) && s7_integer(line) > 0 &&
        strcmp(s7_string(file), game.prelude))
        snprintf(where, sizeof where, "%s:%lld", s7_string(file), (long long)s7_integer(line));
    else if (context == 0 && game.location)
        snprintf(where, sizeof where, "%s", game.location);
    const char *open = where[0] ? " (" : "", *close = where[0] ? ")" : "";
    if (context == 0)
    {
        snprintf(full, sizeof full, "%s #%u %s: %s%s%s%s", KindName(game.eventKind),
                 game.eventThing.index, EventName(), message, open, where, close);
        if (!Throttled(game.eventKind, game.event))
            Report(full);
    }
    else if (context == 1)
    {
        snprintf(full, sizeof full, "%s%s%s%s", message, open, where, close);
        Report(full);
    }
    return s7_make_string(sc, message);
}

// ---- spawning and the tree (§5.3) -------------------------------------------------------------
/* The field a setting writes: a keyword's field (:at is position), or for the one value without a
   keyword, the first field the kind declares itself ((model "gun.glb") is the mesh). -2 for
   :local on a child, which is accepted and has no effect yet: the store has no local things. */
static int SettingField(StoreKind kind, s7_pointer key, bool child, char *err, size_t size)
{
    Store *store = game.store;
    if (key == s7_f(game.sc))
    {
        StoreKind base = StoreKindBase(store, kind);
        int field = base >= 0 ? StoreFieldCount(store, base) : 0;
        if (field >= StoreFieldCount(store, kind))
            return snprintf(err, size, "%s takes no value without a keyword", KindName(kind)), -1;
        return field;
    }
    const char *name = s7_symbol_name(key);
    if (child && !strcmp(name, "local"))
        return -2;
    int field = StoreFieldIndex(store, kind, !strcmp(name, "at") ? "position" : name);
    if (field < 0)
        snprintf(err, size, "spawn: %s has no field for :%s", KindName(kind), name);
    return field;
}

static bool ApplySettings(s7_scheme *sc, StoreId id, StoreKind kind, s7_pointer settings, bool child,
                          bool check, char *err, size_t size)
{
    for (s7_pointer p = settings; s7_is_pair(p); p = s7_cdr(p))
    {
        int field = SettingField(kind, s7_car(s7_car(p)), child, err, size);
        if (field == -1)
            return false;
        if (!check && field >= 0 && !WriteField(sc, id, field, s7_cdr(s7_car(p)), true, err, size))
            return false;
    }
    return true;
}

// Whether a child declaration says :local #t.
static bool DeclaredLocal(s7_pointer decl)
{
    for (s7_pointer p = s7_caddr(decl); s7_is_pair(p); p = s7_cdr(p))
    {
        s7_pointer key = s7_car(s7_car(p));
        if (s7_is_symbol(key) && !strcmp(s7_symbol_name(key), "local"))
            return s7_cdr(s7_car(p)) != s7_f(game.sc);
    }
    return false;
}

static bool SpawnDeclared(s7_scheme *sc, StoreId parent, s7_pointer decls, char *err, size_t size)
{
    Store *store = game.store;
    for (s7_pointer p = decls; s7_is_pair(p); p = s7_cdr(p))
    {
        s7_pointer decl = s7_car(p);
        const char *childName = s7_symbol_name(s7_car(decl)), *kindName = s7_symbol_name(s7_cadr(decl));
        s7_pointer settings = s7_caddr(decl), nested = s7_cdddr(decl);
        StoreKind kind = StoreKindNamed(store, kindName);
        if (kind < 0)
            return snprintf(err, size, "spawn: child %s is a %s, but there is no kind named %s",
                            childName, kindName, kindName),
                   false;
        if (!ApplySettings(sc, STORE_NULL, kind, settings, true, true, err, size))
            return false;
        StoreId id = StoreSpawn(store, kind, 0, parent, StoreIntern(store, childName));
        if (IsNull(id))
            return FailureText(parent, -1, err, size), false;
        // A :local child is marked before its own children spawn, so they are local too.
        if (DeclaredLocal(decl) && !StoreIsLocal(store, id) && !StoreMarkLocal(store, id))
            return snprintf(err, size, "%s", Rest(StoreLastError(store))), false;
        if (!ApplySettings(sc, id, kind, settings, true, false, err, size) ||
            !SpawnDeclared(sc, id, Root(kind, SLOT_CHILDREN), err, size) ||
            !SpawnDeclared(sc, id, nested, err, size))
            return false;
    }
    return true;
}

// (spawn 'kind :at v :owner p :parent thing :field value ...)
static s7_pointer SchemeSpawn(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    const char *name = NameArg(sc, s7_car(args), "spawn", 1);
    StoreKind kind = StoreKindNamed(store, name);
    if (kind < 0)
        return Fail(sc, "spawn: there is no kind named %s", name);
    int owner = IsNull(StoreCurrent(store)) ? 0 : StoreCurrentOwner(store);
    StoreId parent = STORE_NULL;
    s7_pointer settings = s7_nil(sc);
    for (s7_pointer p = s7_cdr(args); s7_is_pair(p); p = s7_cddr(p))
    {
        s7_pointer key = s7_car(p);
        if (!s7_is_keyword(key))
            return Fail(sc, "spawn %s: expected a keyword such as :at, not %s", name,
                        s7_is_symbol(key) ? s7_symbol_name(key) : What(sc, key));
        if (!s7_is_pair(s7_cdr(p)))
            return Fail(sc, "spawn %s: %s needs a value", name, s7_symbol_name(key));
        s7_pointer symbol = s7_keyword_to_symbol(sc, key), value = s7_cadr(p);
        const char *setting = s7_symbol_name(symbol);
        if (!strcmp(setting, "owner"))
        {
            if (!s7_is_integer(value) || s7_integer(value) < 0 || s7_integer(value) > 63)
                return Fail(sc, "spawn %s: :owner is a player from 0 (the host) to 63", name);
            owner = (int)s7_integer(value);
        }
        else if (!strcmp(setting, "parent"))
            parent = value == s7_f(sc) ? STORE_NULL : ThingArg(sc, value, "spawn", 0);
        else
            settings = s7_cons(sc, s7_cons(sc, symbol, value), settings);
    }
    settings = s7_reverse(sc, settings);
    char err[TEXT];
    if (!ApplySettings(sc, STORE_NULL, kind, settings, false, true, err, sizeof err))
        return Fail(sc, "%s", err);
    StoreId id = StoreSpawn(store, kind, owner, parent, STORE_NO_SYMBOL);
    if (IsNull(id))
        return Fail(sc, "spawn %s: %s", name, Rest(StoreLastError(store)));
    s7_pointer thing = GameS7Thing(sc, id);
    if (!SpawnDeclared(sc, id, Root(kind, SLOT_CHILDREN), err, sizeof err) ||
        !ApplySettings(sc, id, kind, settings, false, false, err, sizeof err))
    {
        StoreRemove(store, id);
        return Fail(sc, "%s", err);
    }
    return thing;
}

// A save leaves local things out (§2.3); their declarations bring the missing ones back.
static bool RespawnLocal(s7_scheme *sc, StoreId thing, s7_pointer decls, char *err, size_t size)
{
    Store *store = game.store;
    for (s7_pointer p = decls; s7_is_pair(p); p = s7_cdr(p))
    {
        s7_pointer decl = s7_car(p);
        StoreId child = StoreChildNamed(store, thing, StoreIntern(store, s7_symbol_name(s7_car(decl))));
        if (IsNull(child) ? DeclaredLocal(decl) && !SpawnDeclared(sc, thing, s7_list(sc, 1, decl), err, size)
                          : !RespawnLocal(sc, child, s7_cdddr(decl), err, size))
            return false;
    }
    return true;
}

bool GameS7RestoreLocalChildren(void)
{
    s7_scheme *sc = game.sc;
    if (!sc)
        return false;
    int count = StoreThings(game.store, -1, NULL, 0);
    StoreId *ids = count ? malloc((size_t)count * sizeof *ids) : NULL;
    if (count && !ids)
        return false;
    count = StoreThings(game.store, -1, ids, count);
    char err[TEXT];
    bool ok = true;
    for (int i = 0; ok && i < count; i++)
        ok = RespawnLocal(sc, ids[i], Root(StoreKindOf(game.store, ids[i]), SLOT_CHILDREN), err, sizeof err);
    free(ids);
    if (!ok)
        Report(err);
    return ok;
}

static s7_pointer SchemeRemove(s7_scheme *sc, s7_pointer args)
{
    return s7_make_boolean(sc, RemoveThing(sc, ThingArg(sc, s7_car(args), "remove", 1)));
}

/* Writes a placement. engine: part of a tree change the store has already checked; otherwise
   through the rules (a detach! of a thing that is already a root only places it). */
static void Place(s7_scheme *sc, StoreId id, const char *field, Vector3 value, bool engine)
{
    int index = StoreFieldIndex(game.store, StoreKindOf(game.store, id), field);
    StoreValue v;
    memset(&v, 0, sizeof v);
    v.type = STORE_VEC3;
    v.as.v = value;
    if (index >= 0 && !(engine ? StoreSetEngine(game.store, id, index, &v) : StoreSet(game.store, id, index, &v)))
        StoreFailure(sc, id, index);
}

static Vector3 VecKeyword(s7_scheme *sc, s7_pointer args, const char *name, const char *caller, bool *given)
{
    s7_pointer p = GameS7KeywordArg(sc, args, name);
    Vector3 v = {0, 0, 0};
    *given = p != NULL;
    if (p && !GameS7ToVec3(p, &v))
        s7_wrong_type_arg_error(sc, caller, 0, p, "a vec3");
    return v;
}

/* (attach! thing parent :at v :rotation r). The placement is part of the tree change, which the
   store has already checked (rule 5 on the thing), so it is written through the engine's path. */
static s7_pointer SchemeAttach(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "attach!", 1);
    StoreId parent = ThingArg(sc, s7_cadr(args), "attach!", 2);
    bool at, rotated;
    Vector3 position = VecKeyword(sc, args, "at", "attach!", &at);
    Vector3 rotation = VecKeyword(sc, args, "rotation", "attach!", &rotated);
    if (!StoreAttach(game.store, id, parent))
        return StoreFailure(sc, id, -1);
    if (at)
        Place(sc, id, "position", position, true);
    if (rotated)
        Place(sc, id, "rotation", rotation, true);
    return s7_car(args);
}

/* (detach! thing :at world-position :up normal :yaw y) or (detach! thing :keep-world #t). The
   world transform is world3d's: :keep-world asks a world-position procedure when one is bound,
   and :up tilts the thing by the normal's slopes (exact for an upright normal). A thing that is
   already a root and is given a placement is only placed, under the rules: an orphaned handler
   seats the thing the engine has just detached (B3.7, C4). */
static s7_pointer SchemeDetach(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "detach!", 1);
    bool at, up;
    Vector3 position = VecKeyword(sc, args, "at", "detach!", &at);
    Vector3 normal = VecKeyword(sc, args, "up", "detach!", &up);
    s7_pointer yawP = GameS7KeywordArg(sc, args, "yaw"), keep = GameS7KeywordArg(sc, args, "keep-world");
    double yaw = yawP ? NumberArg(sc, yawP, "detach!", 0) : 0.0;
    if (keep && keep != s7_f(sc))
    {
        s7_pointer world = s7_name_to_value(sc, "world-position");
        if (!s7_is_procedure(world))
            return Fail(sc, "detach! :keep-world needs world-position, which the world module provides");
        if (!GameS7ToVec3(s7_call(sc, world, s7_list(sc, 1, s7_car(args))), &position))
            return Fail(sc, "detach! :keep-world: world-position gave no vec3");
        at = true;
    }
    bool root = IsNull(StoreParent(game.store, id)) && StoreAlive(game.store, id);
    if (!(root && (at || up || yawP)) && !StoreDetach(game.store, id))
        return StoreFailure(sc, id, -1);
    if (at)
        Place(sc, id, "position", position, !root);
    if (up || yawP)
    {
        float length = sqrtf(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        Vector3 n = up && length > 0 ? (Vector3){normal.x / length, normal.y / length, normal.z / length}
                                     : (Vector3){0, 1, 0};
        Place(sc, id, "rotation", (Vector3){atan2f(n.z, n.y), (float)yaw, -atan2f(n.x, n.y)}, !root);
    }
    return s7_car(args);
}

static s7_pointer SchemeParent(s7_scheme *sc, s7_pointer args)
{
    return GameS7Thing(sc, StoreParent(game.store, ThingArg(sc, s7_car(args), "parent", 1)));
}

static s7_pointer SchemeChildren(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "children", 1);
    s7_pointer list = s7_nil(sc);
    for (StoreId c = StoreFirstChild(game.store, id); !IsNull(c); c = StoreNextSibling(game.store, c))
        list = s7_cons(sc, GameS7Thing(sc, c), list);
    return s7_reverse(sc, list);
}

static s7_pointer SchemeFirstChild(s7_scheme *sc, s7_pointer args)
{
    return GameS7Thing(sc, StoreFirstChild(game.store, ThingArg(sc, s7_car(args), "first-child", 1)));
}

static s7_pointer SchemeChildNamed(s7_scheme *sc, s7_pointer args)
{
    StoreId id = ThingArg(sc, s7_car(args), "child", 1);
    const char *name = NameArg(sc, s7_cadr(args), "child", 2);
    return GameS7Thing(sc, FindChild(id, StoreIntern(game.store, name)));
}

static s7_pointer SchemeIs(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    const char *name = NameArg(sc, s7_cadr(args), "is?", 2);
    if (!GameS7ToId(s7_car(args), &id) || !StoreAlive(game.store, id))
        return s7_f(sc);
    StoreKind kind = StoreKindNamed(game.store, name);
    if (kind < 0)
        return Fail(sc, "is?: there is no kind named %s", name);
    return s7_make_boolean(sc, StoreKindIs(game.store, StoreKindOf(game.store, id), kind));
}

static s7_pointer SchemeKindOf(s7_scheme *sc, s7_pointer args)
{
    return s7_make_symbol(sc, KindName(StoreKindOf(game.store, ThingArg(sc, s7_car(args), "kind-of", 1))));
}

static s7_pointer SchemeIsThing(s7_scheme *sc, s7_pointer args)
{
    StoreId id;
    return s7_make_boolean(sc, GameS7ToId(s7_car(args), &id));
}

static s7_pointer ThingsOf(s7_scheme *sc, StoreKind kind, int limit)
{
    int count = StoreThings(game.store, kind, NULL, 0);
    if (limit && count > limit)
        count = limit;
    StoreId *ids = count ? malloc((size_t)count * sizeof *ids) : NULL;
    if (count && !ids)
        return Fail(sc, "out of memory");
    count = StoreThings(game.store, kind, ids, count);
    s7_pointer list = s7_nil(sc);
    for (int i = count - 1; i >= 0; i--)
        list = s7_cons(sc, GameS7Thing(sc, ids[i]), list);
    free(ids);
    return list;
}

static s7_pointer SchemeThings(s7_scheme *sc, s7_pointer args)
{
    StoreKind kind = -1;
    if (s7_is_pair(args))
    {
        const char *name = NameArg(sc, s7_car(args), "things", 1);
        kind = StoreKindNamed(game.store, name);
        if (kind < 0)
            return Fail(sc, "things: there is no kind named %s", name);
    }
    return ThingsOf(sc, kind, 0);
}

static s7_pointer SchemeGame(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    StoreKind kind = StoreKindNamed(game.store, "game");
    s7_pointer found = kind >= 0 ? ThingsOf(sc, kind, 1) : s7_nil(sc);
    if (!s7_is_pair(found))
        return Fail(sc, "(game) is the first thing of the kind named game, and there is none");
    return s7_car(found);
}

// The runner's networking (docs/developer/store.md §9.6); none: one machine, player 1.
static GameS7Network network;

void GameS7SetNetwork(const GameS7Network *hooks)
{
    memset(&network, 0, sizeof network);
    if (hooks)
        network = *hooks;
}

static s7_pointer SchemeLocalPlayer(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_integer(sc, network.player ? network.player(network.user) : 1);
}

static s7_pointer SchemePlayers(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    int ids[64], n = network.players ? network.players(network.user, ids, 64) : 0;
    if (!network.players)
        ids[0] = 1, n = 1;
    s7_pointer list = s7_nil(sc);
    for (int i = (n < 64 ? n : 64) - 1; i >= 0; i--)
        list = s7_cons(sc, s7_make_integer(sc, ids[i]), list);
    return list;
}

// host-game and join-game come from a key press: gameplay code runs again on replay and on other
// machines, so it can't open a session.
static s7_pointer Session(s7_scheme *sc, s7_pointer args, bool host)
{
    const char *name = host ? "host-game" : "join-game";
    s7_pointer address = host ? NULL : s7_car(args), port = host ? s7_car(args) : s7_cadr(args);
    if (StorePhaseNow(game.store) == STORE_PHASE_GAMEPLAY)
        return Fail(sc, "%s is for presentation: call it from a key press in a frame handler or from the "
                        "REPL. A gameplay handler runs again on replay and on other machines.",
                    name);
    if (address && !s7_is_string(address))
        return s7_wrong_type_arg_error(sc, name, 1, address, "an address string");
    if (!s7_is_integer(port) || s7_integer(port) < 1 || s7_integer(port) > 65535)
        return s7_wrong_type_arg_error(sc, name, host ? 1 : 2, port, "a port from 1 to 65535");
    char why[256] = "";
    if (host ? !network.host : !network.join)
        return Fail(sc, "%s needs the runner (trench run), which owns the sockets", name);
    bool ok = host ? network.host(network.user, (int)s7_integer(port), why, sizeof why)
                   : network.join(network.user, s7_string(address), (int)s7_integer(port), why, sizeof why);
    if (!ok)
        return Fail(sc, "%s: %s", name, why[0] ? why : "it could not start");
    return s7_t(sc);
}

static s7_pointer SchemeHostGame(s7_scheme *sc, s7_pointer args) { return Session(sc, args, true); }
static s7_pointer SchemeJoinGame(s7_scheme *sc, s7_pointer args) { return Session(sc, args, false); }

// ---- time, randomness, messages, states -------------------------------------------------------
// Gameplay draws from the running thing's stream, presentation from the local one, and code
// outside any handler (setup, the REPL) from the world's, so every draw is replayed.
static uint32_t Draw(uint32_t n)
{
    Store *store = game.store;
    switch (StorePhaseNow(store))
    {
    case STORE_PHASE_GAMEPLAY:
        return StoreRandom(store, StoreCurrent(store), n);
    case STORE_PHASE_PRESENTATION:
        return StoreRandomLocal(store, n);
    default:
        return StoreRandom(store, STORE_NULL, n);
    }
}

static s7_pointer SchemeRandom(s7_scheme *sc, s7_pointer args)
{
    GameS7LimitCheck(sc);
    s7_pointer n = s7_car(args);
    if (s7_is_integer(n))
    {
        s7_int limit = s7_integer(n);
        if (limit <= 0 || limit > (s7_int)UINT32_MAX)
            return Fail(sc, "random needs a whole number from 1 to 4294967295, not %lld", (long long)limit);
        return s7_make_integer(sc, Draw((uint32_t)limit));
    }
    double limit = NumberArg(sc, n, "random", 1);
    if (!(limit > 0.0))
        return Fail(sc, "random needs a number above 0");
    return s7_make_real(sc, limit * (double)Draw(1u << 24) / (double)(1u << 24));
}

static s7_pointer SchemeTickTime(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_real(sc, StoreTickTime(game.store));
}

static int MessageArgs(s7_scheme *sc, s7_pointer list, const char *event, StoreValue *out)
{
    int count = 0;
    for (s7_pointer p = list; s7_is_pair(p); p = s7_cdr(p), count++)
    {
        if (count == STORE_MAX_ARGS)
            Fail(sc, "%s: a message carries %d arguments at most", event, STORE_MAX_ARGS);
        const char *why;
        int result = Convert(sc, s7_car(p), STORE_NONE, &out[count], &why);
        if (result == NOT_DATA)
            Fail(sc, "can't send %s with %s: messages carry data so they can be saved and sent. Send "
                     "a symbol and dispatch on it.",
                 why, event);
        else if (result != CONVERTED)
            Fail(sc, "%s: %s", event, why);
    }
    return count;
}

// (send thing 'event args...): a message from gameplay code, a player command from presentation.
static s7_pointer SchemeSend(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId target = ThingArg(sc, s7_car(args), "send", 1);
    const char *event = NameArg(sc, s7_cadr(args), "send", 2);
    StoreValue values[STORE_MAX_ARGS];
    int count = MessageArgs(sc, s7_cddr(args), event, values);
    StoreSymbol symbol = StoreIntern(store, event);
    bool ok = StorePhaseNow(store) == STORE_PHASE_PRESENTATION
                  ? StoreCommand(store, target, symbol, values, count)
                  : StoreSend(store, target, symbol, values, count);
    if (!ok)
        return StoreFailure(sc, target, -1);
    return s7_t(sc);
}

// (after seconds 'event args...): a timer on the thing whose handler is running.
static s7_pointer SchemeAfter(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId self = StoreCurrent(store);
    if (IsNull(self))
        return Fail(sc, "after sets a timer on the thing whose handler is running; call it from a handler");
    double seconds = NumberArg(sc, s7_car(args), "after", 1);
    const char *event = NameArg(sc, s7_cadr(args), "after", 2);
    StoreValue values[STORE_MAX_ARGS];
    int count = MessageArgs(sc, s7_cddr(args), event, values);
    if (!StoreAfter(store, self, (float)seconds, StoreIntern(store, event), values, count))
        return StoreFailure(sc, self, -1);
    return s7_t(sc);
}

// (go 'state): takes effect when the running handler returns (B2.5).
static s7_pointer SchemeGo(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId self = StoreCurrent(store);
    if (IsNull(self))
        return Fail(sc, "go changes the state of the thing whose handler is running; call it from a handler");
    StoreKind kind = StoreKindOf(store, self);
    const char *state = NameArg(sc, s7_car(args), "go", 1);
    if (StateField(kind) < 0)
        return Fail(sc, "%s has no states: add (states initial (name (on ...) ...) ...) to its define-kind",
                    KindName(kind));
    bool known = false;
    for (StoreKind k = kind; k >= 0 && !known; k = StoreKindBase(store, k))
        for (s7_pointer p = Root(k, SLOT_STATES); s7_is_pair(p); p = s7_cdr(p))
            known = known || s7_car(p) == s7_car(args);
    if (!known)
        return Fail(sc, "%s has no state named %s", KindName(kind), state);
    game.goPending = true;
    game.goThing = self;
    game.goState = StoreIntern(store, state);
    return s7_car(args);
}

// ---- input (§5.6) -----------------------------------------------------------------------------
static s7_pointer SchemeDefineActions(s7_scheme *sc, s7_pointer args)
{
    s7_pointer list = s7_car(args);
    int count = 0;
    char names[GAME_S7_MAX_ACTIONS][32], keys[GAME_S7_MAX_ACTIONS][32];
    for (s7_pointer p = list; s7_is_pair(p); p = s7_cdr(p), count++)
    {
        s7_pointer action = s7_car(p);
        if (!s7_is_pair(action) || !s7_is_symbol(s7_car(action)) || !s7_is_pair(s7_cdr(action)) ||
            !s7_is_string(s7_cadr(action)))
            return Fail(sc, "define-actions: each action is (name \"Key\")");
        if (count == GAME_S7_MAX_ACTIONS)
            return Fail(sc, "define-actions: %d actions at most", GAME_S7_MAX_ACTIONS);
        if (strlen(s7_symbol_name(s7_car(action))) >= sizeof names[0] ||
            strlen(s7_string(s7_cadr(action))) >= sizeof keys[0])
            return Fail(sc, "define-actions: names and keys are 31 characters at most");
        snprintf(names[count], sizeof names[count], "%s", s7_symbol_name(s7_car(action)));
        snprintf(keys[count], sizeof keys[count], "%s", s7_string(s7_cadr(action)));
    }
    memcpy(game.actionNames, names, sizeof names);
    memcpy(game.actionKeys, keys, sizeof keys);
    game.actionCount = count;
    return s7_make_integer(sc, count);
}

static bool Held(s7_scheme *sc, s7_pointer p, const char *caller, bool pressed)
{
    const char *name = NameArg(sc, p, caller, 1);
    for (int i = 0; i < game.actionCount; i++)
        if (!strcmp(game.actionNames[i], name))
            return game.input && ((pressed ? game.input->pressed : game.input->held) >> i & 1u);
    Fail(sc, "%s: no action named %s; declare it with (define-actions (%s \"Key\") ...)", caller, name, name);
    return false;
}

static s7_pointer SchemeHeld(s7_scheme *sc, s7_pointer args)
{
    return s7_make_boolean(sc, Held(sc, s7_car(args), "held?", false));
}

static s7_pointer SchemePressed(s7_scheme *sc, s7_pointer args)
{
    return s7_make_boolean(sc, Held(sc, s7_car(args), "pressed?", true));
}

// (input-vector 'left 'right 'forward 'back): x right, z back, normalised; forward is -z.
static s7_pointer SchemeInputVector(s7_scheme *sc, s7_pointer args)
{
    bool b[4];
    s7_pointer p = args;
    for (int i = 0; i < 4; i++, p = s7_cdr(p))
        b[i] = Held(sc, s7_car(p), "input-vector", false);
    double x = (double)b[1] - (double)b[0], z = (double)b[3] - (double)b[2];
    double length = sqrt(x * x + z * z);
    return length > 0.0 ? Vec(sc, x / length, 0.0, z / length) : Vec(sc, 0.0, 0.0, 0.0);
}

static s7_pointer SchemeMouseMotion(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return game.input ? Vec(sc, game.input->mouseDx, game.input->mouseDy, 0.0) : Vec(sc, 0.0, 0.0, 0.0);
}

// ---- vectors ----------------------------------------------------------------------------------
typedef struct D3
{
    double x, y, z;
} D3;

static D3 VecArg(s7_scheme *sc, s7_pointer p, const char *caller, int position)
{
    GameS7LimitCheck(sc);
    if (s7_is_float_vector(p) && s7_vector_length(p) == 3)
    {
        s7_double *e = s7_float_vector_elements(p);
        return (D3){e[0], e[1], e[2]};
    }
    Vector3 v;
    if (!GameS7ToVec3(p, &v))
        s7_wrong_type_arg_error(sc, caller, position, p, "a vec3");
    return (D3){v.x, v.y, v.z};
}

static s7_pointer MakeD3(s7_scheme *sc, D3 v) { return Vec(sc, v.x, v.y, v.z); }
static double Length(D3 v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

static s7_pointer SchemeVec3(s7_scheme *sc, s7_pointer args)
{
    return Vec(sc, NumberArg(sc, s7_car(args), "vec3", 1), NumberArg(sc, s7_cadr(args), "vec3", 2),
               NumberArg(sc, s7_caddr(args), "vec3", 3));
}

static s7_pointer SchemeVx(s7_scheme *sc, s7_pointer args) { return s7_make_real(sc, VecArg(sc, s7_car(args), "vx", 1).x); }
static s7_pointer SchemeVy(s7_scheme *sc, s7_pointer args) { return s7_make_real(sc, VecArg(sc, s7_car(args), "vy", 1).y); }
static s7_pointer SchemeVz(s7_scheme *sc, s7_pointer args) { return s7_make_real(sc, VecArg(sc, s7_car(args), "vz", 1).z); }

static s7_pointer Pairwise(s7_scheme *sc, s7_pointer args, const char *caller, int op)
{
    D3 a = VecArg(sc, s7_car(args), caller, 1), b = VecArg(sc, s7_cadr(args), caller, 2);
    return op == 0   ? MakeD3(sc, (D3){a.x + b.x, a.y + b.y, a.z + b.z})
           : op == 1 ? MakeD3(sc, (D3){a.x - b.x, a.y - b.y, a.z - b.z})
                     : MakeD3(sc, (D3){a.x * b.x, a.y * b.y, a.z * b.z});
}

static s7_pointer SchemeVAdd(s7_scheme *sc, s7_pointer args) { return Pairwise(sc, args, "v+", 0); }
static s7_pointer SchemeVSub(s7_scheme *sc, s7_pointer args) { return Pairwise(sc, args, "v-", 1); }
static s7_pointer SchemeVMul(s7_scheme *sc, s7_pointer args) { return Pairwise(sc, args, "v*", 2); }

static s7_pointer SchemeVScale(s7_scheme *sc, s7_pointer args)
{
    D3 v = VecArg(sc, s7_car(args), "vscale", 1);
    double k = NumberArg(sc, s7_cadr(args), "vscale", 2);
    return MakeD3(sc, (D3){v.x * k, v.y * k, v.z * k});
}

static s7_pointer SchemeVLength(s7_scheme *sc, s7_pointer args)
{
    return s7_make_real(sc, Length(VecArg(sc, s7_car(args), "vlength", 1)));
}

static s7_pointer SchemeVDistance(s7_scheme *sc, s7_pointer args)
{
    D3 a = VecArg(sc, s7_car(args), "vdistance", 1), b = VecArg(sc, s7_cadr(args), "vdistance", 2);
    return s7_make_real(sc, Length((D3){a.x - b.x, a.y - b.y, a.z - b.z}));
}

static s7_pointer SchemeVNormalize(s7_scheme *sc, s7_pointer args)
{
    D3 v = VecArg(sc, s7_car(args), "vnormalize", 1);
    double length = Length(v);
    return length > 0.0 ? MakeD3(sc, (D3){v.x / length, v.y / length, v.z / length}) : MakeD3(sc, v);
}

static s7_pointer SchemeVDot(s7_scheme *sc, s7_pointer args)
{
    D3 a = VecArg(sc, s7_car(args), "vdot", 1), b = VecArg(sc, s7_cadr(args), "vdot", 2);
    return s7_make_real(sc, a.x * b.x + a.y * b.y + a.z * b.z);
}

static D3 Cross(D3 a, D3 b)
{
    return (D3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static s7_pointer SchemeVCross(s7_scheme *sc, s7_pointer args)
{
    return MakeD3(sc, Cross(VecArg(sc, s7_car(args), "vcross", 1), VecArg(sc, s7_cadr(args), "vcross", 2)));
}

// About +Y by an angle in radians: (0 0 -1) turned by yaw is (aim yaw 0).
static s7_pointer SchemeRotateY(s7_scheme *sc, s7_pointer args)
{
    D3 v = VecArg(sc, s7_car(args), "rotate-y", 1);
    double a = NumberArg(sc, s7_cadr(args), "rotate-y", 2), c = cos(a), s = sin(a);
    return MakeD3(sc, (D3){v.x * c + v.z * s, v.y, -v.x * s + v.z * c});
}

// The yaw that turns +Z toward a vector: a model facing +Z faces it with rotation (0 yaw 0).
static s7_pointer SchemeHeading(s7_scheme *sc, s7_pointer args)
{
    D3 v = VecArg(sc, s7_car(args), "heading", 1);
    return s7_make_real(sc, atan2(v.x, v.z));
}

// Where a camera with rotation (pitch yaw 0) looks: -Z turned by pitch, then yaw.
static s7_pointer SchemeAim(s7_scheme *sc, s7_pointer args)
{
    double yaw = NumberArg(sc, s7_car(args), "aim", 1), pitch = NumberArg(sc, s7_cadr(args), "aim", 2);
    return MakeD3(sc, (D3){-sin(yaw) * cos(pitch), sin(pitch), -cos(yaw) * cos(pitch)});
}

// A direction turned by up to `amount` radians, uniformly over the cone, from `random`'s stream.
static s7_pointer SchemeSpread(s7_scheme *sc, s7_pointer args)
{
    D3 v = VecArg(sc, s7_car(args), "spread", 1);
    double amount = NumberArg(sc, s7_cadr(args), "spread", 2), length = Length(v);
    if (length <= 0.0)
        return MakeD3(sc, v);
    D3 n = {v.x / length, v.y / length, v.z / length};
    D3 a = fabs(n.y) < 0.99 ? (D3){0, 1, 0} : (D3){1, 0, 0};
    D3 u = Cross(a, n);
    double ul = Length(u);
    u = (D3){u.x / ul, u.y / ul, u.z / ul};
    D3 w = Cross(n, u);
    double angle = amount * sqrt((double)Draw(65536) / 65536.0);
    double turn = 2.0 * 3.14159265358979323846 * (double)Draw(65536) / 65536.0;
    double c = cos(angle), s = sin(angle), ct = cos(turn), st = sin(turn);
    D3 d = {n.x * c + (u.x * ct + w.x * st) * s, n.y * c + (u.y * ct + w.y * st) * s,
            n.z * c + (u.z * ct + w.z * st) * s};
    return MakeD3(sc, (D3){d.x * length, d.y * length, d.z * length});
}

// ---- the REPL's calls, the clock, rule 3 ------------------------------------------------------
static s7_pointer SchemeInspect(s7_scheme *sc, s7_pointer args)
{
    Store *store = game.store;
    StoreId id = ThingArg(sc, s7_car(args), "inspect", 1);
    StoreKind kind = StoreKindOf(store, id);
    s7_pointer list = s7_nil(sc);
    s7_int loc = s7_gc_protect(sc, list);
    for (int f = StoreFieldCount(store, kind) - 1; f >= 0; f--)
    {
        const StoreFieldDecl *d = StoreFieldAt(store, kind, f);
        if (d->flags & STORE_HIDDEN)
            continue;
        View view = {id, f};
        s7_pointer value = d->type == STORE_MAP ? MapList(sc, &view, 2, true) : ReadField(sc, id, f);
        list = s7_cons(sc, s7_cons(sc, s7_make_symbol(sc, d->name), value), list);
        s7_gc_unprotect_at(sc, loc);
        loc = s7_gc_protect(sc, list);
    }
    s7_gc_unprotect_at(sc, loc);
    return list;
}

static s7_pointer SchemeReload(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    game.reloadRequested = true; // done by GameS7Eval once the line has been evaluated
    return s7_t(sc);
}

static s7_pointer SchemeSaveGame(s7_scheme *sc, s7_pointer args)
{
    if (!s7_is_string(s7_car(args)))
        return s7_wrong_type_arg_error(sc, "save-game", 1, s7_car(args), "a file name");
    if (!StoreSave(game.store, s7_string(s7_car(args))))
        return Fail(sc, "save-game: %s", Rest(StoreLastError(game.store)));
    return s7_t(sc);
}

static s7_pointer SchemeLoadGame(s7_scheme *sc, s7_pointer args)
{
    if (!s7_is_string(s7_car(args)))
        return s7_wrong_type_arg_error(sc, "load-game", 1, s7_car(args), "a file name");
    if (!StoreLoad(game.store, s7_string(s7_car(args))))
        return Fail(sc, "load-game: %s", Rest(StoreLastError(game.store)));
    return s7_make_boolean(sc, GameS7RestoreLocalChildren());
}

static s7_pointer SnapshotFree(s7_scheme *sc, s7_pointer obj)
{
    (void)sc;
    StoreSnapshotFree(s7_c_object_value(obj));
    return NULL;
}

static s7_pointer SchemeSnapshot(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    StoreSnapshot *snapshot = StoreSnapshotTake(game.store);
    if (!snapshot)
        return Fail(sc, "snapshot: only between ticks");
    return s7_make_c_object(sc, game.snapshotTag, snapshot);
}

static s7_pointer SchemeRestore(s7_scheme *sc, s7_pointer args)
{
    s7_pointer p = s7_car(args);
    if (!s7_is_c_object(p) || s7_c_object_type(p) != game.snapshotTag)
        return s7_wrong_type_arg_error(sc, "restore", 1, p, "a snapshot");
    if (!StoreSnapshotRestore(game.store, s7_c_object_value(p)))
        return Fail(sc, "restore: %s", Rest(StoreLastError(game.store)));
    return s7_t(sc);
}

static s7_pointer SchemeRealTime(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_real(sc, Now());
}

static s7_pointer SchemeCurrentTime(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_integer(sc, (s7_int)time(NULL));
}

static s7_pointer SchemeGameplay(s7_scheme *sc, s7_pointer args)
{
    (void)args;
    return s7_make_boolean(sc, StorePhaseNow(game.store) == STORE_PHASE_GAMEPLAY);
}

static s7_pointer SchemeRule3(s7_scheme *sc, s7_pointer args)
{
    const char *name = NameArg(sc, s7_car(args), "%rule-3", 1);
    if (!strcmp(name, "real-time") || !strcmp(name, "current-time"))
        return Fail(sc, "%s is for presentation. Gameplay code runs again on replay and on other "
                        "machines, where the clock differs. Use (tick-time), seconds since the world "
                        "started.",
                    name);
    return Fail(sc, "%s is for presentation. Gameplay code runs again on replay and on other "
                    "machines, where the files differ. Read what the game needs when it loads, into "
                    "top-level definitions.",
                name);
}

// ---- registration -----------------------------------------------------------------------------
typedef struct Call
{
    const char *name;
    s7_function function;
    int required, optional;
    bool rest, safe;
    const char *help;
} Call;

static const Call calls[] = {
    {"%kind-declare", SchemeKindDeclare, 5, 0, false, false, "(%kind-declare 'name 'base fields children settings)"},
    {"%kind-handler", SchemeKindHandler, 4, 1, false, false, "(%kind-handler 'kind 'event procedure line ['state])"},
    {"%kind-helper", SchemeKindHelper, 3, 0, false, false, "(%kind-helper 'kind 'name procedure)"},
    {"%kind-states", SchemeKindStates, 3, 0, false, false, "(%kind-states 'kind 'initial '(state ...))"},
    {"%kind-field-names", SchemeKindFieldNames, 1, 0, false, true, "field names of a kind, or #f"},
    {"%kind-child-names", SchemeKindChildNames, 1, 0, false, true, "declared child names of a kind"},
    {"%kind-helper-names", SchemeKindHelperNames, 1, 0, false, true, "helper names of a kind and its bases"},
    {"%child-slot", SchemeChildSlot, 1, 0, false, true, "the slot of a child name"},
    {"%field", SchemeField, 2, 0, false, true, "(%field self i)"},
    {"%set-field!", SchemeSetField, 3, 0, false, true, "(%set-field! self i value)"},
    {"%child", SchemeChild, 2, 0, false, true, "(%child self slot)"},
    {"%helper-ref", SchemeHelperRef, 2, 0, false, true, "(%helper-ref self 'name)"},
    {"%report", SchemeReport, 6, 0, false, true, "reports a script error"},
    {"%define-actions", SchemeDefineActions, 1, 0, false, true, "(%define-actions '((name \"Key\") ...))"},
    {"%gameplay?", SchemeGameplay, 0, 0, false, true, "whether a gameplay handler is running"},
    {"%rule-3", SchemeRule3, 1, 0, false, true, "raises the rule 3 error for a name"},
    {"thing?", SchemeIsThing, 1, 0, false, true, "(thing? x) whether x is a thing, live or removed"},
    {"spawn", SchemeSpawn, 1, 0, true, false, "(spawn 'kind :at v :owner p :parent thing :field value ...)"},
    {"remove", SchemeRemove, 1, 0, false, false, "(remove thing) removes it and the children it declared"},
    {"attach!", SchemeAttach, 2, 0, true, false, "(attach! thing parent :at v :rotation r)"},
    {"detach!", SchemeDetach, 1, 0, true, false, "(detach! thing :at v :up normal :yaw y) or (detach! thing :keep-world #t)"},
    {"parent", SchemeParent, 1, 0, false, true, "(parent thing) or #f"},
    {"children", SchemeChildren, 1, 0, false, true, "(children thing): declared children, then guests"},
    {"first-child", SchemeFirstChild, 1, 0, false, true, "(first-child thing) or #f"},
    {"child", SchemeChildNamed, 2, 0, false, true, "(child thing 'name): a declared child or #f"},
    {"is?", SchemeIs, 2, 0, false, true, "(is? thing 'kind): the kind or one derived from it"},
    {"kind-of", SchemeKindOf, 1, 0, false, true, "(kind-of thing) its kind's name"},
    {"things", SchemeThings, 0, 1, false, true, "(things 'kind): things of a kind and derived kinds, in id order"},
    {"game", SchemeGame, 0, 0, false, true, "(game): the first thing of the kind named game"},
    {"local-player", SchemeLocalPlayer, 0, 0, false, true, "(local-player): this machine's player, 1 on the host"},
    {"players", SchemePlayers, 0, 0, false, true, "(players): the players in the session, ascending"},
    {"host-game", SchemeHostGame, 1, 0, false, true, "(host-game port): host a session (presentation only)"},
    {"join-game", SchemeJoinGame, 2, 0, false, true, "(join-game address port): join one (presentation only)"},
    {"random", SchemeRandom, 1, 0, false, true, "(random n): [0, n) from the running thing's stream"},
    {"tick-time", SchemeTickTime, 0, 0, false, true, "(tick-time): seconds since the world started"},
    {"after", SchemeAfter, 2, 0, true, true, "(after seconds 'event args...) a timer on self"},
    {"send", SchemeSend, 2, 0, true, true, "(send thing 'event args...)"},
    {"go", SchemeGo, 1, 0, false, true, "(go 'state) when this handler returns"},
    {"held?", SchemeHeld, 1, 0, false, true, "(held? 'action)"},
    {"pressed?", SchemePressed, 1, 0, false, true, "(pressed? 'action): went down this tick"},
    {"input-vector", SchemeInputVector, 4, 0, false, true, "(input-vector 'left 'right 'forward 'back)"},
    {"mouse-motion", SchemeMouseMotion, 0, 0, false, true, "(mouse-motion): vec3 dx dy 0"},
    {"vec3", SchemeVec3, 3, 0, false, true, "(vec3 x y z)"},
    {"vx", SchemeVx, 1, 0, false, true, "(vx v)"},
    {"vy", SchemeVy, 1, 0, false, true, "(vy v)"},
    {"vz", SchemeVz, 1, 0, false, true, "(vz v)"},
    {"v+", SchemeVAdd, 2, 0, false, true, "(v+ a b)"},
    {"v-", SchemeVSub, 2, 0, false, true, "(v- a b)"},
    {"v*", SchemeVMul, 2, 0, false, true, "(v* a b) componentwise"},
    {"vscale", SchemeVScale, 2, 0, false, true, "(vscale v k)"},
    {"vlength", SchemeVLength, 1, 0, false, true, "(vlength v)"},
    {"vdistance", SchemeVDistance, 2, 0, false, true, "(vdistance a b)"},
    {"vnormalize", SchemeVNormalize, 1, 0, false, true, "(vnormalize v); zero stays zero"},
    {"vdot", SchemeVDot, 2, 0, false, true, "(vdot a b)"},
    {"vcross", SchemeVCross, 2, 0, false, true, "(vcross a b)"},
    {"rotate-y", SchemeRotateY, 2, 0, false, true, "(rotate-y v radians)"},
    {"heading", SchemeHeading, 1, 0, false, true, "(heading v): the yaw that turns +Z toward v"},
    {"aim", SchemeAim, 2, 0, false, true, "(aim yaw pitch): where a camera looks"},
    {"spread", SchemeSpread, 2, 0, false, true, "(spread v radians): v turned randomly, replayably"},
    {"map-keys", SchemeMapKeys, 1, 0, false, true, "(map-keys m) sorted"},
    {"map-values", SchemeMapValues, 1, 0, false, true, "(map-values m) in key order"},
    {"map-remove!", SchemeMapRemove, 2, 0, false, true, "(map-remove! m key)"},
    {"grid-ref", SchemeGridRef, 3, 0, false, true, "(grid-ref g x y)"},
    {"grid-set!", SchemeGridSet, 4, 0, false, true, "(grid-set! g x y value)"},
    {"grid-fill!", SchemeGridFill, 2, 0, false, true, "(grid-fill! g value)"},
    {"grid-fill-rect!", SchemeGridFillRect, 6, 0, false, true, "(grid-fill-rect! g x y w h value)"},
    {"grid-width", SchemeGridWidth, 1, 0, false, true, "(grid-width g)"},
    {"grid-height", SchemeGridHeight, 1, 0, false, true, "(grid-height g)"},
    {"inspect", SchemeInspect, 1, 0, false, true, "(inspect thing): every field as an alist"},
    {"reload", SchemeReload, 0, 0, false, true, "(reload) the game files, at the REPL"},
    {"save-game", SchemeSaveGame, 1, 0, false, false, "(save-game path)"},
    {"load-game", SchemeLoadGame, 1, 0, false, false, "(load-game path)"},
    {"snapshot", SchemeSnapshot, 0, 0, false, true, "(snapshot) the world, between ticks"},
    {"restore", SchemeRestore, 1, 0, false, false, "(restore snapshot)"},
    {"real-time", SchemeRealTime, 0, 0, false, true, "(real-time) seconds on a monotonic clock"},
    {"current-time", SchemeCurrentTime, 0, 0, false, true, "(current-time) seconds since 1970"},
};

bool GameS7Define(const char *name, GameS7Function function, int required, int optional, bool rest,
                  const char *help)
{
    if (!game.sc || !name || !function)
        return false;
    s7_define_function(game.sc, name, function, required, optional, rest, help ? help : "");
    return true;
}

bool GameS7DefineTyped(const char *name, GameS7Function function, int required, int optional,
                       bool rest, const char *help, s7_pointer signature)
{
    if (!game.sc || !name || !function)
        return false;
    s7_define_typed_function(game.sc, name, function, required, optional, rest, help ? help : "",
                             signature);
    return true;
}

bool GameS7DefineMethod(const char *name, GameS7Function function)
{
    if (!game.sc || !name || !function || game.methodCount == MAX_METHODS)
        return false;
    game.methods[game.methodCount++] = (Method){s7_make_symbol(game.sc, name), function};
    return true;
}

static s7_pointer Protected(s7_scheme *sc, const char *name)
{
    s7_pointer value = s7_name_to_value(sc, name);
    if (!s7_is_procedure(value))
        return NULL;
    s7_gc_protect(sc, value);
    return value;
}

static bool Readable(const char *path)
{
    FILE *file = path ? fopen(path, "r") : NULL;
    if (file)
        fclose(file);
    return file != NULL;
}

bool GameS7Open(Store *store, const char *preludePath)
{
    if (game.sc || !store || !preludePath)
        return false;
    char resolved[512];
    const char *actual = CoreResolvePath(preludePath, resolved, sizeof resolved);
    if (!Readable(actual))
    {
        TraceLog(LOG_ERROR, "GAME: no Scheme prelude at %s", preludePath);
        return false;
    }
    void (*sink)(const char *) = game.sink;
    const GameInput *input = game.input;
    memset(&game, 0, sizeof game);
    game.sink = sink;
    game.input = input;
    game.event = STORE_NO_SYMBOL;
    game.eventKind = -1;
    game.eventThing = STORE_NULL;
    s7_scheme *sc = s7_init();
    if (!sc)
        return false;
    game.sc = sc;
    game.store = store;
    snprintf(game.prelude, sizeof game.prelude, "%s", actual);

    game.thingTag = s7_make_c_type(sc, "thing");
    s7_c_type_set_ref(sc, game.thingTag, ThingRef);
    s7_c_type_set_set(sc, game.thingTag, ThingSet);
    s7_c_type_set_to_string(sc, game.thingTag, ThingString);
    s7_c_type_set_is_equal(sc, game.thingTag, ThingEqual);
    s7_c_type_set_is_equivalent(sc, game.thingTag, ThingEqual);
    game.mapTag = s7_make_c_type(sc, "map-view");
    s7_c_type_set_gc_free(sc, game.mapTag, ViewFree);
    s7_c_type_set_ref(sc, game.mapTag, MapRef);
    s7_c_type_set_set(sc, game.mapTag, MapSet);
    s7_c_type_set_to_string(sc, game.mapTag, MapString);
    s7_c_type_set_is_equal(sc, game.mapTag, ViewEqual);
    game.gridTag = s7_make_c_type(sc, "grid-view");
    s7_c_type_set_gc_free(sc, game.gridTag, ViewFree);
    s7_c_type_set_ref(sc, game.gridTag, GridApply);
    s7_c_type_set_to_string(sc, game.gridTag, GridString);
    s7_c_type_set_is_equal(sc, game.gridTag, ViewEqual);
    game.snapshotTag = s7_make_c_type(sc, "snapshot");
    s7_c_type_set_gc_free(sc, game.snapshotTag, SnapshotFree);

    for (size_t i = 0; i < sizeof calls / sizeof calls[0]; i++)
    {
        const Call *c = &calls[i];
        if (c->safe)
            s7_define_typed_function(sc, c->name, c->function, c->required, c->optional, c->rest,
                                     c->help, NULL);
        else
            s7_define_function(sc, c->name, c->function, c->required, c->optional, c->rest, c->help);
    }
    if (!EnsureKind(0) || !EnsureThings(1) || !s7_load(sc, actual))
    {
        TraceLog(LOG_ERROR, "GAME: could not load the Scheme prelude %s", actual);
        GameS7Close();
        return false;
    }
    memset(&limit, 0, sizeof limit);
    s7_set_begin_hook(sc, BeginHook);
    game.dispatch = Protected(sc, "%dispatch");
    game.loadFile = Protected(sc, "%load-file");
    game.repl = Protected(sc, "%repl");
    game.freeze = Protected(sc, "%freeze!");
    game.guard = Protected(sc, "%guard");
    if (!game.dispatch || !game.loadFile || !game.repl || !game.freeze || !game.guard)
    {
        TraceLog(LOG_ERROR, "GAME: the prelude %s is incomplete", actual);
        GameS7Close();
        return false;
    }
    return true;
}

// Rule 3's names, shadowed in each game environment (§5.7).
static void Guard(s7_scheme *sc, s7_pointer env)
{
    static const char *const names[] = {"real-time",        "current-time", "open-input-file",
                                        "open-output-file", "load",         "system"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    {
        s7_pointer symbol = s7_make_symbol(sc, names[i]);
        s7_pointer original = s7_let_ref(sc, s7_rootlet(sc), symbol);
        if (!s7_is_procedure(original))
            original = s7_f(sc);
        s7_define(sc, env, symbol, s7_call(sc, game.guard, s7_list(sc, 3, symbol, original, env)));
    }
}

static bool LoadInto(const char *path)
{
    s7_scheme *sc = game.sc;
    char resolved[512], line[700];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    if (!Readable(actual))
    {
        snprintf(line, sizeof line, "no game file at %s", path);
        Report(line);
        return false;
    }
    s7_pointer env = s7_sublet(sc, s7_rootlet(sc), s7_nil(sc));
    s7_int loc = s7_gc_protect(sc, env);
    Guard(sc, env);
    snprintf(game.loading, sizeof game.loading, "%s", actual);
    game.loadingNow = true;
    bool loaded = s7_call(sc, game.loadFile, s7_list(sc, 2, s7_make_string(sc, actual), env)) != s7_f(sc);
    FinishLoad(loaded);
    if (!loaded)
    {
        s7_gc_unprotect_at(sc, loc);
        return false;
    }
    s7_call(sc, game.freeze, s7_list(sc, 1, env));
    if (game.env)
        s7_gc_unprotect_at(sc, game.envLoc);
    game.env = env;
    game.envLoc = loc;
    return true;
}

bool GameS7LoadGame(const char *path)
{
    if (!game.sc || !path)
        return false;
    snprintf(game.gamePath, sizeof game.gamePath, "%s", path);
    return LoadInto(path);
}

bool GameS7Reload(void)
{
    if (!game.sc || !game.gamePath[0])
        return false;
    return LoadInto(game.gamePath);
}

bool GameS7Eval(const char *text, char **answer)
{
    if (answer)
        *answer = NULL;
    if (!game.sc || !text)
        return false;
    s7_scheme *sc = game.sc;
    game.reloadRequested = false;
    s7_pointer env = game.env ? game.env : s7_rootlet(sc);
    s7_pointer result = s7_call(sc, game.repl, s7_list(sc, 2, s7_make_string(sc, text), env));
    bool ok = s7_is_pair(result) && s7_car(result) == s7_t(sc);
    s7_pointer shown = s7_is_pair(result) ? s7_cdr(result) : result;
    if (answer)
        *answer = ok ? s7_object_to_c_string(sc, shown) : Copy(s7_is_string(shown) ? s7_string(shown) : "error");
    if (ok && game.reloadRequested)
    {
        game.reloadRequested = false;
        ok = GameS7Reload();
        if (answer)
        {
            free(*answer);
            *answer = Copy(ok ? "reloaded" : "reload refused; the reason is above");
        }
    }
    return ok;
}

void GameS7Close(void)
{
    if (game.sc)
        s7_free(game.sc);
    for (int k = 0; k < game.kindCapacity; k++)
    {
        free(game.kinds[k].events[0]);
        free(game.kinds[k].events[1]);
    }
    free(game.kinds);
    free(game.thingKinds);
    free(game.thingGenerations);
    free(game.childSlots);
    void (*sink)(const char *) = game.sink;
    memset(&game, 0, sizeof game);
    game.sink = sink;
}

s7_scheme *GameS7Scheme(void) { return game.sc; }

void GameS7SetInput(const GameInput *input) { game.input = input; }

int GameS7ActionCount(void) { return game.actionCount; }

const char *GameS7ActionName(int action)
{
    return action >= 0 && action < game.actionCount ? game.actionNames[action] : NULL;
}

const char *GameS7ActionKey(int action)
{
    return action >= 0 && action < game.actionCount ? game.actionKeys[action] : NULL;
}

void GameS7SetErrorSink(void (*sink)(const char *message)) { game.sink = sink; }
