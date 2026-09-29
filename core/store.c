/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "store_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_MAX 256
#define DELIVERY_CAP 10000
#define SYMBOL_START 0
#define SYMBOL_TICK 1
#define SYMBOL_FRAME 2
#define SYMBOL_DRAW_HUD 3
#define LOCAL_STREAM_SALT 0x6C6F63616C2D7270ull /* "local-rp": the presentation stream's offset */

struct StoreMessage
{
    StoreId target;
    StoreSymbol event;
    int count;
    StoreValue args[STORE_MAX_ARGS];
};

struct StoreSystem
{
    StoreSystemFn run;
    void *user;
};

typedef struct SnapshotPool
{
    unsigned char *shared;
    uint32_t rows, freeCount;
    int sharedSize;
    uint32_t *freeRows;
} SnapshotPool;

struct StoreSnapshot
{
    int kindCount;
    int32_t symbolCount;
    StoreThing *things;
    uint32_t thingCount, firstFree;
    SnapshotPool *pools;
    StoreTimer *timers;
    int timerCount;
    uint64_t timerSequence;
    StoreId *starts;
    int startCount;
    uint64_t tickCount, worldRandom;
    float dt;
};

static bool MakeMessage(const Store *store, StoreMessage *m, StoreId target, StoreSymbol event,
                        const StoreValue *args, int count);
static bool Push(StoreMessage **items, int *count, int *capacity, const StoreMessage *m);

// ---- small helpers ----------------------------------------------------------------------------
static void *Grow(void *items, int *capacity, int need, size_t size)
{
    if (need <= *capacity)
        return items;
    int cap = *capacity ? *capacity : 8;
    while (cap < need)
        cap *= 2;
    void *grown = realloc(items, (size_t)cap * size);
    if (grown)
        *capacity = cap;
    return grown;
}

static char *CopyString(const char *text)
{
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

static void *Duplicate(const void *data, size_t size)
{
    void *copy = malloc(size ? size : 1);
    if (copy && size)
        memcpy(copy, data, size);
    return copy;
}

static uint64_t SplitMix(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Lemire's multiply-and-reject: uniform without the bias of a plain modulo.
static uint32_t Uniform(uint64_t *state, uint32_t n)
{
    if (!n)
        return 0;
    uint64_t m = (uint64_t)(uint32_t)(SplitMix(state) >> 32) * n;
    uint32_t low = (uint32_t)m;
    if (low < n)
    {
        uint32_t threshold = (0u - n) % n;
        while (low < threshold)
        {
            m = (uint64_t)(uint32_t)(SplitMix(state) >> 32) * n;
            low = (uint32_t)m;
        }
    }
    return (uint32_t)(m >> 32);
}

bool StoreFail(const Store *store, bool rule, const char *format, ...)
{
    if (!store->error)
        return false;
    va_list args;
    va_start(args, format);
    vsnprintf(store->error, ERROR_MAX, format, args);
    va_end(args);
    if (rule && store->hooks.error)
        store->hooks.error(store->hooks.user, store->error);
    return false;
}

const char *StoreTypeName(StoreType type)
{
    static const char *const names[] = {"nothing", "int",  "float", "bool", "symbol", "string",
                                        "vec3",    "ref",  "list",  "set",  "map",    "grid"};
    return (unsigned)type < sizeof names / sizeof names[0] ? names[type] : "unknown";
}

static bool IsScalar(StoreType type) { return type >= STORE_INT && type <= STORE_REF; }
static bool IsCollection(StoreType type) { return type >= STORE_LIST && type <= STORE_GRID; }
static bool IsElement(StoreType type) { return IsScalar(type) && type != STORE_STRING; }

static int ElementSize(StoreType type)
{
    switch (type)
    {
    case STORE_INT:
    case STORE_FLOAT:
    case STORE_BOOL:
    case STORE_SYMBOL:
        return 4;
    case STORE_VEC3:
        return 12;
    case STORE_REF:
        return 8;
    case STORE_STRING:
        return STORE_STRING_MAX + 1;
    default:
        return 0;
    }
}

static int FieldSize(const StoreFieldDecl *decl)
{
    switch (decl->type)
    {
    case STORE_LIST:
    case STORE_SET:
        return 4 + decl->max * ElementSize(decl->element);
    case STORE_MAP:
        return 4 + decl->max * (ElementSize(decl->key) + ElementSize(decl->element));
    case STORE_GRID:
        return decl->max * decl->height * ElementSize(decl->element);
    default:
        return ElementSize(decl->type);
    }
}

static int32_t ReadCount(const unsigned char *p)
{
    int32_t count;
    memcpy(&count, p, 4);
    return count;
}

static void WriteCount(unsigned char *p, int32_t count) { memcpy(p, &count, 4); }

/* Writes value into a scalar slot of the given type, converting INT<->FLOAT; false on a mismatch.
   Strings are zero-padded, so equal values are equal bytes for compare, snapshot and hash. */
static bool WriteScalar(unsigned char *p, StoreType type, const StoreValue *value)
{
    if (!value)
        return false;
    switch (type)
    {
    case STORE_INT:
    {
        int32_t i;
        if (value->type == STORE_INT)
            i = value->as.i;
        else if (value->type == STORE_FLOAT && isfinite(value->as.f) &&
                 value->as.f == floorf(value->as.f) && value->as.f >= -2147483648.0f &&
                 value->as.f < 2147483648.0f)
            i = (int32_t)value->as.f;
        else
            return false;
        memcpy(p, &i, 4);
        return true;
    }
    case STORE_FLOAT:
    {
        float f;
        if (value->type == STORE_FLOAT)
            f = value->as.f;
        else if (value->type == STORE_INT)
            f = (float)value->as.i;
        else
            return false;
        memcpy(p, &f, 4);
        return true;
    }
    case STORE_BOOL:
    {
        if (value->type != STORE_BOOL)
            return false;
        int32_t b = value->as.b ? 1 : 0;
        memcpy(p, &b, 4);
        return true;
    }
    case STORE_SYMBOL:
        if (value->type != STORE_SYMBOL)
            return false;
        memcpy(p, &value->as.sym, 4);
        return true;
    case STORE_STRING:
    {
        if (value->type != STORE_STRING)
            return false;
        const char *end = memchr(value->as.str, 0, STORE_STRING_MAX + 1);
        if (!end)
            return false;
        size_t length = (size_t)(end - value->as.str);
        memcpy(p, value->as.str, length);
        memset(p + length, 0, STORE_STRING_MAX + 1 - length);
        return true;
    }
    case STORE_VEC3:
    {
        if (value->type != STORE_VEC3)
            return false;
        float v[3] = {value->as.v.x, value->as.v.y, value->as.v.z};
        memcpy(p, v, 12);
        return true;
    }
    case STORE_REF:
    {
        if (value->type != STORE_REF)
            return false;
        uint32_t r[2] = {value->as.ref.index, value->as.ref.generation};
        memcpy(p, r, 8);
        return true;
    }
    default:
        return false;
    }
}

static void ReadScalar(const unsigned char *p, StoreType type, StoreValue *out)
{
    memset(out, 0, sizeof *out);
    out->type = type;
    switch (type)
    {
    case STORE_INT:
        memcpy(&out->as.i, p, 4);
        break;
    case STORE_FLOAT:
        memcpy(&out->as.f, p, 4);
        break;
    case STORE_BOOL:
    {
        int32_t b;
        memcpy(&b, p, 4);
        out->as.b = b != 0;
        break;
    }
    case STORE_SYMBOL:
        memcpy(&out->as.sym, p, 4);
        break;
    case STORE_STRING:
        memcpy(out->as.str, p, STORE_STRING_MAX + 1);
        out->as.str[STORE_STRING_MAX] = 0;
        break;
    case STORE_VEC3:
    {
        float v[3];
        memcpy(v, p, 12);
        out->as.v = (Vector3){v[0], v[1], v[2]};
        break;
    }
    case STORE_REF:
    {
        uint32_t r[2];
        memcpy(r, p, 8);
        out->as.ref = (StoreId){r[0], r[1]};
        break;
    }
    default:
        break;
    }
}

// The zero of a type: STORE_NO_SYMBOL for a symbol and STORE_NULL for a ref, 0 otherwise.
static void WriteZero(unsigned char *p, StoreType type)
{
    int size = ElementSize(type);
    memset(p, 0, (size_t)size);
    if (type == STORE_SYMBOL)
    {
        StoreSymbol none = STORE_NO_SYMBOL;
        memcpy(p, &none, 4);
    }
    else if (type == STORE_REF)
    {
        uint32_t none[2] = {UINT32_MAX, 0};
        memcpy(p, none, 8);
    }
}

// ---- symbols ----------------------------------------------------------------------------------
static uint32_t HashName(const char *name)
{
    uint32_t h = 2166136261u;
    for (; *name; name++)
        h = (h ^ (unsigned char)*name) * 16777619u;
    return h;
}

static StoreSymbol Lookup(const Store *store, const char *name)
{
    if (!store->symbolTableSize || !name)
        return STORE_NO_SYMBOL;
    uint32_t mask = store->symbolTableSize - 1;
    for (uint32_t i = HashName(name) & mask; store->symbolTable[i]; i = (i + 1) & mask)
        if (!strcmp(store->symbols[store->symbolTable[i] - 1], name))
            return store->symbolTable[i] - 1;
    return STORE_NO_SYMBOL;
}

static bool Rehash(Store *store, uint32_t size)
{
    int32_t *table = calloc(size, sizeof *table);
    if (!table)
        return false;
    for (int32_t s = 0; s < store->symbolCount; s++)
    {
        uint32_t i = HashName(store->symbols[s]) & (size - 1);
        while (table[i])
            i = (i + 1) & (size - 1);
        table[i] = s + 1;
    }
    free(store->symbolTable);
    store->symbolTable = table;
    store->symbolTableSize = size;
    return true;
}

StoreSymbol StoreIntern(Store *store, const char *name)
{
    if (!store || !name || !*name)
        return STORE_NO_SYMBOL;
    StoreSymbol found = Lookup(store, name);
    if (found != STORE_NO_SYMBOL)
        return found;
    if ((uint32_t)(store->symbolCount + 1) * 2 > store->symbolTableSize &&
        !Rehash(store, store->symbolTableSize ? store->symbolTableSize * 2 : 64))
        return STORE_NO_SYMBOL;
    char **symbols = Grow(store->symbols, &store->symbolCapacity, store->symbolCount + 1,
                          sizeof *symbols);
    if (!symbols)
        return STORE_NO_SYMBOL;
    store->symbols = symbols;
    char *copy = CopyString(name);
    if (!copy)
        return STORE_NO_SYMBOL;
    StoreSymbol symbol = store->symbolCount++;
    symbols[symbol] = copy;
    uint32_t mask = store->symbolTableSize - 1, i = HashName(name) & mask;
    while (store->symbolTable[i])
        i = (i + 1) & mask;
    store->symbolTable[i] = symbol + 1;
    return symbol;
}

const char *StoreSymbolName(const Store *store, StoreSymbol symbol)
{
    if (!store || symbol < 0 || symbol >= store->symbolCount)
        return NULL;
    return store->symbols[symbol];
}

static const char *SymbolText(const Store *store, StoreSymbol symbol)
{
    const char *name = StoreSymbolName(store, symbol);
    return name ? name : "#f";
}

// ---- kinds ------------------------------------------------------------------------------------
static StoreKindData *Kind(const Store *store, StoreKind kind)
{
    return store && kind >= 0 && kind < store->kindCount ? store->kinds[kind] : NULL;
}

static void FreeKind(StoreKindData *k)
{
    if (!k)
        return;
    for (int f = 0; f < k->fieldCount; f++)
        free((char *)k->fields[f].name);
    free(k->name);
    free(k->fields);
    free(k->offsets);
    free(k->sizes);
    free(k->sharedTemplate);
    free(k->localTemplate);
    free(k->shared);
    free(k->local);
    free(k->shadow);
    free(k->shadowThing);
    free(k->freeRows);
    free(k->events);
    free(k->warned);
    free(k->changed);
    free(k);
}

static bool OwnHandles(const StoreKindData *k, StoreSymbol event)
{
    for (int i = 0; i < k->eventCount; i++)
        if (k->events[i] == event)
            return true;
    return false;
}

bool StoreKindHandlesEvent(const Store *store, StoreKind kind, StoreSymbol event)
{
    for (const StoreKindData *k = Kind(store, kind); k; k = Kind(store, k->base))
        if (OwnHandles(k, event))
            return true;
    return false;
}

// Works out from the base chain what each kind runs and which of its fields a frame compares.
static void RecomputeKinds(Store *store)
{
    char name[256];
    for (StoreKind kind = 0; kind < store->kindCount; kind++)
    {
        StoreKindData *k = store->kinds[kind];
        k->callHandler = NULL;
        k->callUser = NULL;
        for (const StoreKindData *b = k; b; b = Kind(store, b->base))
            if (b->handler)
            {
                k->callHandler = b->handler;
                k->callUser = b->user;
                break;
            }
        k->handlesTick = StoreKindHandlesEvent(store, kind, SYMBOL_TICK);
        k->handlesFrame = StoreKindHandlesEvent(store, kind, SYMBOL_FRAME);
        k->handlesDrawHud = StoreKindHandlesEvent(store, kind, SYMBOL_DRAW_HUD);
        k->watches = false;
        for (int f = 0; f < k->fieldCount; f++)
        {
            k->changed[f] = STORE_NO_SYMBOL;
            if (k->fields[f].flags & STORE_LOCAL)
                continue;
            if (snprintf(name, sizeof name, "%s-changed", k->fields[f].name) >= (int)sizeof name)
                continue;
            StoreSymbol event = Lookup(store, name);
            if (event != STORE_NO_SYMBOL && StoreKindHandlesEvent(store, kind, event))
            {
                k->changed[f] = event;
                k->watches = true;
            }
        }
    }
}

static bool ValidField(const StoreFieldDecl *d, const char **why)
{
    if (!d->name || !*d->name)
        return (*why = "has no name"), false;
    if (IsScalar(d->type))
        return true;
    if (!IsCollection(d->type))
        return (*why = "has no type"), false;
    if (!IsElement(d->element))
        return (*why = "has an element type a collection cannot hold"), false;
    if (d->type == STORE_MAP && d->key != STORE_INT && d->key != STORE_SYMBOL && d->key != STORE_REF)
        return (*why = "needs a map key of int, symbol or ref"), false;
    if (d->max < 1 || d->max > (1 << 20))
        return (*why = "needs a capacity from 1 to 1048576"), false;
    if (d->type == STORE_GRID && (d->height < 1 || d->height > (1 << 20) ||
                                  (int64_t)d->max * d->height > (1 << 22)))
        return (*why = "needs a height from 1, and 4194304 cells at most"), false;
    if (d->init.type != STORE_NONE)
        return (*why = "is a collection: give its defaults with StoreKindSetDefaultAt"), false;
    return true;
}

static bool SameShape(const StoreFieldDecl *a, const StoreFieldDecl *b)
{
    if (a->type != b->type || (a->flags & (STORE_LOCAL | STORE_ENGINE)) !=
                                  (b->flags & (STORE_LOCAL | STORE_ENGINE)))
        return false;
    if (!IsCollection(a->type))
        return true;
    return a->element == b->element && a->max == b->max &&
           (a->type != STORE_MAP || a->key == b->key) &&
           (a->type != STORE_GRID || a->height == b->height);
}

static unsigned char *TemplateOf(StoreKindData *k, int field)
{
    return ((k->fields[field].flags & STORE_LOCAL) ? k->localTemplate : k->sharedTemplate) +
           k->offsets[field];
}

// A field's default at declaration: the type's zero, or init for a scalar.
static bool WriteDefault(StoreKindData *k, int field)
{
    StoreFieldDecl *d = &k->fields[field];
    unsigned char *p = TemplateOf(k, field);
    memset(p, 0, (size_t)k->sizes[field]);
    if (d->type == STORE_GRID)
    {
        int size = ElementSize(d->element);
        for (int c = 0; c < d->max * d->height; c++)
            WriteZero(p + c * size, d->element);
        return true;
    }
    if (IsCollection(d->type))
        return true;
    if (d->init.type == STORE_NONE)
    {
        WriteZero(p, d->type);
        ReadScalar(p, d->type, &d->init);
        return true;
    }
    if (!WriteScalar(p, d->type, &d->init))
        return false;
    ReadScalar(p, d->type, &d->init);
    return true;
}

StoreKind StoreDeclareKind(Store *store, const char *name, StoreKind base,
                           const StoreFieldDecl *fields, int count, const char **error)
{
    const char *why = NULL;
    StoreKindData *k = NULL;
    if (error)
        *error = NULL;
    if (!store || !store->error)
        return -1;
    StoreKindData *b = Kind(store, base);
    if (!name || !*name)
        why = "a kind needs a name";
    else if (StoreKindNamed(store, name) >= 0)
        why = "exists";
    else if (base != -1 && !b)
        why = "names a base kind that does not exist";
    else if (count < 0 || (count && !fields))
        why = "has no field list";
    if (why)
    {
        StoreFail(store, false, "kind: %s %s", name ? name : "", why);
        goto fail;
    }
    int baseCount = b ? b->fieldCount : 0;
    k = calloc(1, sizeof *k);
    if (!k)
        goto oom;
    k->base = base;
    k->name = CopyString(name);
    size_t most = (size_t)baseCount + (size_t)count + 1;
    k->fields = calloc(most, sizeof *k->fields);
    k->offsets = calloc(most, sizeof *k->offsets);
    k->sizes = calloc(most, sizeof *k->sizes);
    k->changed = calloc(most, sizeof *k->changed);
    if (!k->name || !k->fields || !k->offsets || !k->sizes || !k->changed)
        goto oom;
    for (int f = 0; f < baseCount; f++)
    {
        k->fields[f] = b->fields[f];
        k->fields[f].name = CopyString(b->fields[f].name);
        if (!k->fields[f].name)
            goto oom;
        k->fieldCount++;
        k->offsets[f] = b->offsets[f];
        k->sizes[f] = b->sizes[f];
    }
    k->sharedSize = b ? b->sharedSize : 0;
    k->localSize = b ? b->localSize : 0;
    int firstOwn = k->fieldCount;
    // Place the kind's own fields, checking each; a base field of the same name is redeclared.
    for (int i = 0; i < count; i++)
    {
        const StoreFieldDecl *d = &fields[i];
        if (!ValidField(d, &why))
        {
            StoreFail(store, false, "kind: field %s of %s %s", d->name ? d->name : "", name, why);
            goto fail;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(fields[j].name, d->name))
            {
                StoreFail(store, false, "kind: %s declares %s twice", name, d->name);
                goto fail;
            }
        int existing = -1;
        for (int f = 0; f < baseCount; f++)
            if (!strcmp(k->fields[f].name, d->name))
                existing = f;
        if (existing >= 0)
        {
            if (!SameShape(&k->fields[existing], d))
            {
                StoreFail(store, false,
                          "kind: %s redeclares %s from its base with a different type; a "
                          "derived kind may only change a field's default",
                          name, d->name);
                goto fail;
            }
            continue;
        }
        int f = k->fieldCount;
        k->fields[f] = *d;
        k->fields[f].name = CopyString(d->name);
        if (!k->fields[f].name)
            goto oom;
        k->fieldCount++;
        k->sizes[f] = FieldSize(d);
        int *size = (d->flags & STORE_LOCAL) ? &k->localSize : &k->sharedSize;
        k->offsets[f] = *size;
        *size += k->sizes[f];
    }
    k->sharedTemplate = calloc((size_t)k->sharedSize + 1, 1);
    k->localTemplate = calloc((size_t)k->localSize + 1, 1);
    if (!k->sharedTemplate || !k->localTemplate)
        goto oom;
    if (b)
    {
        memcpy(k->sharedTemplate, b->sharedTemplate, (size_t)b->sharedSize);
        memcpy(k->localTemplate, b->localTemplate, (size_t)b->localSize);
    }
    for (int f = firstOwn; f < k->fieldCount; f++)
        if (!WriteDefault(k, f))
        {
            StoreFail(store, false, "kind: the default of %s in %s is a %s, not a %s",
                      k->fields[f].name, name, StoreTypeName(k->fields[f].init.type),
                      StoreTypeName(k->fields[f].type));
            goto fail;
        }
    // Redeclared base fields: only the default changes.
    for (int i = 0; i < count; i++)
        for (int f = 0; f < baseCount; f++)
            if (!strcmp(k->fields[f].name, fields[i].name) && fields[i].init.type != STORE_NONE)
            {
                if (IsCollection(k->fields[f].type) ||
                    !WriteScalar(TemplateOf(k, f), k->fields[f].type, &fields[i].init))
                {
                    StoreFail(store, false, "kind: the default of %s in %s is a %s, not a %s",
                              fields[i].name, name, StoreTypeName(fields[i].init.type),
                              StoreTypeName(k->fields[f].type));
                    goto fail;
                }
                ReadScalar(TemplateOf(k, f), k->fields[f].type, &k->fields[f].init);
            }
    StoreKindData **kinds = Grow(store->kinds, &store->kindCapacity, store->kindCount + 1,
                                 sizeof *kinds);
    if (!kinds)
        goto oom;
    store->kinds = kinds;
    kinds[store->kindCount++] = k;
    RecomputeKinds(store); // the new kind inherits its base's handler and handled events
    return store->kindCount - 1;
oom:
    StoreFail(store, false, "kind: out of memory declaring %s", name);
fail:
    FreeKind(k);
    if (error)
        *error = store->error;
    return -1;
}

StoreKind StoreKindNamed(const Store *store, const char *name)
{
    for (StoreKind kind = 0; store && name && kind < store->kindCount; kind++)
        if (!strcmp(store->kinds[kind]->name, name))
            return kind;
    return -1;
}

const char *StoreKindName(const Store *store, StoreKind kind)
{
    const StoreKindData *k = Kind(store, kind);
    return k ? k->name : NULL;
}

StoreKind StoreKindBase(const Store *store, StoreKind kind)
{
    const StoreKindData *k = Kind(store, kind);
    return k ? k->base : -1;
}

bool StoreKindIs(const Store *store, StoreKind kind, StoreKind base)
{
    if (!Kind(store, base))
        return false;
    for (; Kind(store, kind); kind = store->kinds[kind]->base)
        if (kind == base)
            return true;
    return false;
}

int StoreFieldIndex(const Store *store, StoreKind kind, const char *name)
{
    const StoreKindData *k = Kind(store, kind);
    for (int f = 0; k && name && f < k->fieldCount; f++)
        if (!strcmp(k->fields[f].name, name))
            return f;
    return -1;
}

int StoreFieldCount(const Store *store, StoreKind kind)
{
    const StoreKindData *k = Kind(store, kind);
    return k ? k->fieldCount : 0;
}

const StoreFieldDecl *StoreFieldAt(const Store *store, StoreKind kind, int field)
{
    const StoreKindData *k = Kind(store, kind);
    return k && field >= 0 && field < k->fieldCount ? &k->fields[field] : NULL;
}

// ---- collections, on raw field bytes ----------------------------------------------------------
static int CompareElement(const Store *store, StoreType type, const unsigned char *a,
                          const unsigned char *b)
{
    StoreValue x, y;
    ReadScalar(a, type, &x);
    ReadScalar(b, type, &y);
    switch (type)
    {
    case STORE_INT:
        return (x.as.i > y.as.i) - (x.as.i < y.as.i);
    case STORE_BOOL:
        return (int)x.as.b - (int)y.as.b;
    case STORE_FLOAT:
        return (x.as.f > y.as.f) - (x.as.f < y.as.f);
    case STORE_SYMBOL:
    {
        if (x.as.sym == y.as.sym)
            return 0;
        const char *p = StoreSymbolName(store, x.as.sym), *q = StoreSymbolName(store, y.as.sym);
        if (!p || !q)
            return p ? 1 : -1;
        return strcmp(p, q);
    }
    case STORE_REF:
        if (x.as.ref.index != y.as.ref.index)
            return x.as.ref.index < y.as.ref.index ? -1 : 1;
        return (x.as.ref.generation > y.as.ref.generation) -
               (x.as.ref.generation < y.as.ref.generation);
    case STORE_VEC3:
    {
        float u[3] = {x.as.v.x, x.as.v.y, x.as.v.z}, v[3] = {y.as.v.x, y.as.v.y, y.as.v.z};
        for (int i = 0; i < 3; i++)
            if (u[i] != v[i])
                return u[i] < v[i] ? -1 : 1;
        return 0;
    }
    default:
        return memcmp(a, b, (size_t)ElementSize(type));
    }
}

// Finds key in a sorted map; answers its position, or where it would go with found false.
static int MapFind(const Store *store, const StoreFieldDecl *d, const unsigned char *p,
                   const unsigned char *key, bool *found)
{
    int ks = ElementSize(d->key), pair = ks + ElementSize(d->element), count = ReadCount(p);
    int low = 0, high = count;
    while (low < high)
    {
        int mid = (low + high) / 2;
        int c = CompareElement(store, d->key, p + 4 + mid * pair, key);
        if (!c)
            return (*found = true), mid;
        if (c < 0)
            low = mid + 1;
        else
            high = mid;
    }
    *found = false;
    return low;
}

static bool MapPut(const Store *store, const StoreFieldDecl *d, unsigned char *p,
                   const StoreValue *key, const StoreValue *value, const char *what)
{
    int ks = ElementSize(d->key), vs = ElementSize(d->element), pair = ks + vs;
    unsigned char k[12], v[12];
    memset(k, 0, sizeof k);
    memset(v, 0, sizeof v);
    if (!WriteScalar(k, d->key, key))
        return StoreFail(store, true, "type: the keys of %s are %s, not %s", what,
                         StoreTypeName(d->key), StoreTypeName(key ? key->type : STORE_NONE));
    if (!WriteScalar(v, d->element, value))
        return StoreFail(store, true, "type: the values of %s are %s, not %s", what,
                         StoreTypeName(d->element), StoreTypeName(value ? value->type : STORE_NONE));
    bool found;
    int at = MapFind(store, d, p, k, &found), count = ReadCount(p);
    unsigned char *slot = p + 4 + at * pair;
    if (!found)
    {
        if (count >= d->max)
            return StoreFail(store, true, "capacity: %s holds %d keys at most", what, d->max);
        memmove(slot + pair, slot, (size_t)(count - at) * (size_t)pair);
        memcpy(slot, k, (size_t)ks);
        WriteCount(p, count + 1);
    }
    memcpy(slot + ks, v, (size_t)vs);
    return true;
}

// Fills a LIST or SET from values; a SET is sorted and loses duplicates. Unchanged on failure.
static bool ListPut(const Store *store, const StoreFieldDecl *d, unsigned char *p,
                    const StoreValue *items, int count, const char *what)
{
    int es = ElementSize(d->element);
    if (count < 0 || (count && !items))
        return StoreFail(store, false, "range: %s was given no items", what);
    if (d->type == STORE_LIST && count > d->max)
        return StoreFail(store, true, "capacity: %s holds %d items at most", what, d->max);
    size_t bytes = (size_t)(count ? count : 1) * (size_t)es;
    unsigned char *buffer = malloc(bytes);
    if (!buffer)
        return StoreFail(store, false, "memory: out of memory writing %s", what);
    int kept = 0;
    for (int i = 0; i < count; i++)
    {
        unsigned char *e = buffer + kept * es;
        memset(e, 0, (size_t)es);
        if (!WriteScalar(e, d->element, &items[i]))
        {
            free(buffer);
            return StoreFail(store, true, "type: %s holds %s items, and item %d is a %s", what,
                             StoreTypeName(d->element), i, StoreTypeName(items[i].type));
        }
        if (d->type == STORE_SET)
        {
            int at = kept;
            while (at > 0 && CompareElement(store, d->element, buffer + (at - 1) * es, e) > 0)
                at--;
            if (at > 0 && !CompareElement(store, d->element, buffer + (at - 1) * es, e))
                continue; // a duplicate
            unsigned char held[12];
            memcpy(held, e, (size_t)es);
            memmove(buffer + (at + 1) * es, buffer + at * es, (size_t)(kept - at) * (size_t)es);
            memcpy(buffer + at * es, held, (size_t)es);
        }
        kept++;
    }
    if (kept > d->max)
    {
        free(buffer);
        return StoreFail(store, true, "capacity: %s holds %d items at most", what, d->max);
    }
    WriteCount(p, kept);
    memcpy(p + 4, buffer, (size_t)kept * (size_t)es);
    memset(p + 4 + kept * es, 0, (size_t)(d->max - kept) * (size_t)es);
    free(buffer);
    return true;
}

bool StoreKindSetDefault(Store *store, StoreKind kind, int field, const StoreValue *value)
{
    StoreKindData *k = Kind(store, kind);
    if (!k || field < 0 || field >= k->fieldCount)
        return store ? StoreFail(store, false, "field: no field %d in that kind", field) : false;
    StoreFieldDecl *d = &k->fields[field];
    if (IsCollection(d->type))
        return StoreFail(store, false, "type: %s of %s is a %s; use StoreKindSetDefaultAt",
                         d->name, k->name, StoreTypeName(d->type));
    unsigned char *p = TemplateOf(k, field);
    unsigned char held[STORE_STRING_MAX + 1];
    memcpy(held, p, (size_t)k->sizes[field]);
    if (!WriteScalar(p, d->type, value))
    {
        memcpy(p, held, (size_t)k->sizes[field]);
        return StoreFail(store, true, "type: %s of %s holds %s, not %s", d->name, k->name,
                         StoreTypeName(d->type), StoreTypeName(value ? value->type : STORE_NONE));
    }
    ReadScalar(p, d->type, &d->init);
    return true;
}

bool StoreKindSetDefaultAt(Store *store, StoreKind kind, int field, int index,
                           const StoreValue *key, const StoreValue *value)
{
    StoreKindData *k = Kind(store, kind);
    if (!k || field < 0 || field >= k->fieldCount)
        return store ? StoreFail(store, false, "field: no field %d in that kind", field) : false;
    StoreFieldDecl *d = &k->fields[field];
    unsigned char *p = TemplateOf(k, field);
    int es = ElementSize(d->element);
    switch (d->type)
    {
    case STORE_LIST:
    {
        int count = ReadCount(p);
        if (index < 0 || index > count || index >= d->max)
            return StoreFail(store, true, "capacity: %s holds %d items at most", d->name, d->max);
        unsigned char e[12];
        memset(e, 0, sizeof e);
        if (!WriteScalar(e, d->element, value))
            return StoreFail(store, true, "type: %s holds %s items", d->name,
                             StoreTypeName(d->element));
        memcpy(p + 4 + index * es, e, (size_t)es);
        if (index == count)
            WriteCount(p, count + 1);
        return true;
    }
    case STORE_SET:
    {
        int count = ReadCount(p);
        StoreValue *items = calloc((size_t)count + 1, sizeof *items);
        if (!items)
            return StoreFail(store, false, "memory: out of memory");
        for (int i = 0; i < count; i++)
            ReadScalar(p + 4 + i * es, d->element, &items[i]);
        if (value)
            items[count] = *value;
        bool ok = ListPut(store, d, p, items, count + 1, d->name);
        free(items);
        return ok;
    }
    case STORE_MAP:
        return MapPut(store, d, p, key, value, d->name);
    case STORE_GRID:
    {
        if (index < 0 || index >= d->max * d->height)
            return StoreFail(store, false, "range: %s has %d cells", d->name, d->max * d->height);
        unsigned char e[12];
        if (!WriteScalar(e, d->element, value))
            return StoreFail(store, true, "type: %s holds %s cells", d->name,
                             StoreTypeName(d->element));
        memcpy(p + index * es, e, (size_t)es);
        return true;
    }
    default:
        return StoreFail(store, false, "type: %s of %s is not a collection", d->name, k->name);
    }
}

// ---- things -----------------------------------------------------------------------------------
static StoreThing *ThingAny(const Store *store, StoreId id)
{
    if (!store || id.index >= store->thingCount)
        return NULL;
    StoreThing *t = &store->things[id.index];
    return (t->flags & STORE_THING_LIVE) && t->generation == id.generation ? t : NULL;
}

static StoreThing *Thing(const Store *store, StoreId id)
{
    StoreThing *t = ThingAny(store, id);
    return t && !(t->flags & STORE_THING_REMOVED) ? t : NULL;
}

static StoreId IdOf(const Store *store, uint32_t index)
{
    if (index == STORE_NO_INDEX)
        return STORE_NULL;
    return (StoreId){index, store->things[index].generation};
}

static bool Visible(const StoreThing *t)
{
    return (t->flags & (STORE_THING_LIVE | STORE_THING_REMOVED)) == STORE_THING_LIVE;
}

static bool IsLocalOwner(const Store *store, int owner)
{
    return owner >= 0 && owner < 64 && (store->localOwners >> owner & 1u);
}

static const char *KindNameOf(const Store *store, const StoreThing *t)
{
    return store->kinds[t->kind]->name;
}

static bool StaleFail(const Store *store, StoreId id)
{
    if (id.index == UINT32_MAX)
        return StoreFail(store, false, "removed: no thing (#f)");
    return StoreFail(store, false, "removed: thing #%u (generation %u) has been removed", id.index,
                     id.generation);
}

// Rows are shared by a kind's shared and local pools; freeRows can always hold every row.
static bool AllocRow(StoreKindData *k, uint32_t *row)
{
    if (k->freeCount)
    {
        *row = k->freeRows[--k->freeCount];
        return true;
    }
    if (k->rows == k->rowCapacity)
    {
        uint32_t cap = k->rowCapacity ? k->rowCapacity * 2 : 16;
        if (k->sharedSize)
        {
            unsigned char *shared = realloc(k->shared, (size_t)cap * (size_t)k->sharedSize);
            if (!shared)
                return false;
            k->shared = shared;
        }
        if (k->localSize)
        {
            unsigned char *local = realloc(k->local, (size_t)cap * (size_t)k->localSize);
            if (!local)
                return false;
            k->local = local;
        }
        uint32_t *freeRows = realloc(k->freeRows, (size_t)cap * sizeof *freeRows);
        if (!freeRows)
            return false;
        k->freeRows = freeRows;
        k->rowCapacity = cap;
    }
    *row = k->rows++;
    return true;
}

static void ResetBlocks(StoreKindData *k, uint32_t row, bool shared, bool local)
{
    if (shared && k->sharedSize)
        memcpy(k->shared + (size_t)row * (size_t)k->sharedSize, k->sharedTemplate,
               (size_t)k->sharedSize);
    if (local && k->localSize)
        memcpy(k->local + (size_t)row * (size_t)k->localSize, k->localTemplate,
               (size_t)k->localSize);
}

bool StoreReserveSlots(Store *store, uint32_t count, uint32_t generation)
{
    if (count > store->thingCapacity)
    {
        uint32_t cap = store->thingCapacity ? store->thingCapacity : 64;
        while (cap < count)
            cap *= 2;
        StoreThing *things = realloc(store->things, (size_t)cap * sizeof *things);
        if (!things)
            return false;
        store->things = things;
        store->thingCapacity = cap;
    }
    for (uint32_t i = store->thingCount; i < count; i++)
    {
        StoreThing *t = &store->things[i];
        memset(t, 0, sizeof *t);
        t->generation = generation ? generation : 1;
        t->kind = -1;
        t->row = 0;
        t->parent = t->firstChild = t->nextSibling = STORE_NO_INDEX;
        t->childName = STORE_NO_SYMBOL;
    }
    if (count > store->thingCount)
        store->thingCount = count;
    return true;
}

// The lowest free slot, so which slot a spawn takes depends only on which are free.
static bool AllocSlot(Store *store, uint32_t *index)
{
    uint32_t i = store->firstFree;
    while (i < store->thingCount && (store->things[i].flags & STORE_THING_LIVE))
        i++;
    if (i == store->thingCount && !StoreReserveSlots(store, i + 1, 1))
        return false;
    *index = i;
    store->firstFree = i + 1;
    return true;
}

static void Unlink(Store *store, uint32_t child)
{
    StoreThing *c = &store->things[child];
    uint32_t parent = c->parent;
    c->parent = STORE_NO_INDEX;
    if (parent == STORE_NO_INDEX || parent >= store->thingCount)
        return;
    uint32_t *link = &store->things[parent].firstChild;
    while (*link != STORE_NO_INDEX && *link != child)
        link = &store->things[*link].nextSibling;
    if (*link == child)
        *link = c->nextSibling;
    c->nextSibling = STORE_NO_INDEX;
}

void StoreLinkChild(Store *store, uint32_t child, uint32_t parent, StoreSymbol name, bool guest)
{
    StoreThing *c = &store->things[child];
    uint32_t *link = &store->things[parent].firstChild;
    // Declared children go before the first guest; guests go last.
    while (*link != STORE_NO_INDEX && (guest || !(store->things[*link].flags & STORE_THING_GUEST)))
        link = &store->things[*link].nextSibling;
    c->nextSibling = *link;
    *link = child;
    c->parent = parent;
    c->childName = guest ? STORE_NO_SYMBOL : name;
    c->flags = guest ? (c->flags | STORE_THING_GUEST) : (c->flags & ~STORE_THING_GUEST);
}

static void SetOwnerTree(Store *store, uint32_t index, int owner)
{
    store->things[index].owner = owner;
    for (uint32_t c = store->things[index].firstChild; c != STORE_NO_INDEX;
         c = store->things[c].nextSibling)
        SetOwnerTree(store, c, owner);
}

static bool PushStart(Store *store, StoreId id)
{
    StoreId *starts = Grow(store->starts, &store->startCapacity, store->startCount + 1,
                           sizeof *starts);
    if (!starts)
        return false;
    store->starts = starts;
    starts[store->startCount++] = id;
    return true;
}

// Fills a free slot with a thing of a kind at its defaults, in a row already taken.
static void Place(Store *store, StoreId id, StoreKind kind, uint32_t row, int owner, int spawner,
                  uint64_t random)
{
    StoreThing *t = &store->things[id.index];
    memset(t, 0, sizeof *t);
    t->generation = id.generation;
    t->kind = kind;
    t->row = row;
    t->parent = t->firstChild = t->nextSibling = STORE_NO_INDEX;
    t->childName = STORE_NO_SYMBOL;
    t->owner = owner;
    t->spawner = spawner;
    t->flags = STORE_THING_LIVE;
    t->random = random;
    ResetBlocks(store->kinds[kind], row, true, true);
}

bool StorePlaceThing(Store *store, StoreId id, StoreKind kind, int owner, int spawner,
                     uint64_t random)
{
    StoreKindData *k = Kind(store, kind);
    uint32_t row;
    if (!k || id.index >= store->thingCount || (store->things[id.index].flags & STORE_THING_LIVE) ||
        !AllocRow(k, &row))
        return false;
    Place(store, id, kind, row, owner, spawner, random);
    if (id.index == store->firstFree)
        store->firstFree++;
    return true;
}

StoreId StoreSpawn(Store *store, StoreKind kind, int owner, StoreId parent, StoreSymbol childName)
{
    if (!store || !store->error)
        return STORE_NULL;
    StoreKindData *k = Kind(store, kind);
    if (!k)
        return StoreFail(store, false, "kind: no kind %d to spawn", kind), STORE_NULL;
    bool root = parent.index == UINT32_MAX;
    if (!root && !Thing(store, parent))
        return StaleFail(store, parent), STORE_NULL;
    if (root && (owner < 0 || owner > 63))
        return StoreFail(store, false, "owner: %d is not an owner from 0 to 63", owner), STORE_NULL;
    StoreMessage m;
    uint32_t row, index;
    if (!AllocRow(k, &row))
        return StoreFail(store, false, "memory: out of memory spawning %s", k->name), STORE_NULL;
    StoreId *starts = Grow(store->starts, &store->startCapacity, store->startCount + 1,
                           sizeof *starts);
    if (starts)
        store->starts = starts;
    if (!starts || !AllocSlot(store, &index))
    {
        k->freeRows[k->freeCount++] = row;
        return StoreFail(store, false, "memory: out of memory spawning %s", k->name), STORE_NULL;
    }
    // The new thing's stream comes from whoever is spawning it, so nobody else's draws shift.
    uint64_t *stream = &store->worldRandom;
    StoreThing *spawner = store->phase == STORE_PHASE_GAMEPLAY ? Thing(store, store->current) : NULL;
    if (spawner)
        stream = &spawner->random;
    else if (store->phase == STORE_PHASE_PRESENTATION)
        stream = &store->localRandom;
    uint64_t random = SplitMix(stream);
    StoreId id = {index, store->things[index].generation};
    Place(store, id, kind, row, owner, owner, random);
    if (!root)
    {
        StoreThing *p = &store->things[parent.index];
        store->things[index].owner = p->owner;
        store->things[index].spawner = p->spawner;
        StoreLinkChild(store, index, parent.index, childName, false);
    }
    // Spawned while the tick's queue is open: start later this tick, before its first tick.
    // Otherwise (outside a tick, in a frame, in step 7) it waits for the next StoreTick.
    if (!store->queueOpen || !MakeMessage(store, &m, id, SYMBOL_START, NULL, 0) ||
        !Push(&store->queue, &store->queueCount, &store->queueCapacity, &m))
        PushStart(store, id);
    if (store->hooks.spawned)
        store->hooks.spawned(store->hooks.user, id);
    return id;
}

static void MarkRemoved(Store *store, uint32_t index)
{
    StoreThing *t = &store->things[index];
    t->flags |= STORE_THING_REMOVED;
    store->pendingRemovals++;
    for (uint32_t c = t->firstChild; c != STORE_NO_INDEX; c = store->things[c].nextSibling)
        if (!(store->things[c].flags & (STORE_THING_GUEST | STORE_THING_REMOVED)))
            MarkRemoved(store, c);
}

static void DetachIndex(Store *store, uint32_t index)
{
    Unlink(store, index);
    StoreThing *t = &store->things[index];
    t->flags &= ~STORE_THING_GUEST;
    t->childName = STORE_NO_SYMBOL;
    SetOwnerTree(store, index, 0);
}

static void FreeThing(Store *store, uint32_t index)
{
    StoreThing *t = &store->things[index];
    StoreKindData *k = store->kinds[t->kind];
    StoreId id = {index, t->generation};
    int kept = 0;
    for (int i = 0; i < store->timerCount; i++)
        if (store->timers[i].target.index != id.index ||
            store->timers[i].target.generation != id.generation)
            store->timers[kept++] = store->timers[i];
    store->timerCount = kept;
    k->freeRows[k->freeCount++] = t->row;
    if (t->parent != STORE_NO_INDEX && Visible(&store->things[t->parent]))
        Unlink(store, index);
    t->flags = 0;
    t->kind = -1;
    t->parent = t->firstChild = t->nextSibling = STORE_NO_INDEX;
    t->childName = STORE_NO_SYMBOL;
    if (++t->generation == 0)
        t->generation = 1;
    if (index < store->firstFree)
        store->firstFree = index;
}

// Step 7: guests of removed things are detached (orphan hook first), then storage is freed.
static void ApplyRemovals(Store *store)
{
    if (!store->pendingRemovals)
        return;
    StorePhase phase = store->phase;
    StoreId current = store->current;
    int currentOwner = store->currentOwner;
    store->phase = STORE_PHASE_NONE;
    store->current = STORE_NULL;
    store->currentOwner = -1;
    for (uint32_t i = 0; i < store->thingCount; i++)
    {
        if ((store->things[i].flags & (STORE_THING_LIVE | STORE_THING_REMOVED)) !=
            (STORE_THING_LIVE | STORE_THING_REMOVED))
            continue;
        uint32_t c = store->things[i].firstChild;
        while (c != STORE_NO_INDEX)
        {
            uint32_t next = store->things[c].nextSibling;
            if (Visible(&store->things[c]) && (store->things[c].flags & STORE_THING_GUEST))
            {
                if (store->hooks.orphan)
                    store->hooks.orphan(store->hooks.user, IdOf(store, c));
                DetachIndex(store, c);
            }
            c = next;
        }
    }
    for (uint32_t i = 0; i < store->thingCount; i++)
        if ((store->things[i].flags & (STORE_THING_LIVE | STORE_THING_REMOVED)) ==
            (STORE_THING_LIVE | STORE_THING_REMOVED))
        {
            if (store->hooks.removed)
                store->hooks.removed(store->hooks.user, IdOf(store, i));
            FreeThing(store, i);
        }
    store->pendingRemovals = 0;
    store->phase = phase;
    store->current = current;
    store->currentOwner = currentOwner;
}

// Rules 1 and 5 for changing a thing's shared state (its tree, timers, stream), not a field.
static bool CheckSharedChange(const Store *store, const StoreThing *t, const char *what)
{
    if (store->phase == STORE_PHASE_PRESENTATION)
        return StoreFail(store, true,
                         "shared-write: %s on %s #%u changes shared state, which a presentation "
                         "handler can't do",
                         what, KindNameOf(store, t), (unsigned)(t - store->things));
    if (store->phase == STORE_PHASE_GAMEPLAY && t->owner != store->currentOwner)
        return StoreFail(store, true,
                         "not-owner: %s on %s #%u: it belongs to player %d, and this handler runs "
                         "for player %d",
                         what, KindNameOf(store, t), (unsigned)(t - store->things), t->owner,
                         store->currentOwner);
    return true;
}

bool StoreRemove(Store *store, StoreId id)
{
    StoreThing *t = Thing(store, id);
    if (!t)
        return store ? StaleFail(store, id) : false;
    if (store->phase == STORE_PHASE_GAMEPLAY && t->owner != store->currentOwner)
        return StoreFail(store, true,
                         "not-owner: remove on %s #%u: it belongs to player %d, and this handler "
                         "runs for player %d",
                         KindNameOf(store, t), id.index, t->owner, store->currentOwner);
    MarkRemoved(store, id.index);
    if (!store->ticking && !store->framing)
        ApplyRemovals(store);
    return true;
}

bool StoreAlive(const Store *store, StoreId id) { return Thing(store, id) != NULL; }

StoreKind StoreKindOf(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? t->kind : -1;
}

int StoreOwner(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? t->owner : -1;
}

int StoreSpawner(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? t->spawner : -1;
}

StoreId StoreParent(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? IdOf(store, t->parent) : STORE_NULL;
}

static StoreId FirstVisible(const Store *store, uint32_t index)
{
    for (; index != STORE_NO_INDEX; index = store->things[index].nextSibling)
        if (Visible(&store->things[index]))
            return IdOf(store, index);
    return STORE_NULL;
}

StoreId StoreFirstChild(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? FirstVisible(store, t->firstChild) : STORE_NULL;
}

StoreId StoreNextSibling(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t ? FirstVisible(store, t->nextSibling) : STORE_NULL;
}

StoreId StoreChildNamed(const Store *store, StoreId id, StoreSymbol name)
{
    const StoreThing *t = Thing(store, id);
    if (!t || name == STORE_NO_SYMBOL)
        return STORE_NULL;
    for (uint32_t c = t->firstChild; c != STORE_NO_INDEX; c = store->things[c].nextSibling)
        if (Visible(&store->things[c]) && !(store->things[c].flags & STORE_THING_GUEST) &&
            store->things[c].childName == name)
            return IdOf(store, c);
    return STORE_NULL;
}

StoreSymbol StoreChildName(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t && !(t->flags & STORE_THING_GUEST) ? t->childName : STORE_NO_SYMBOL;
}

bool StoreIsGuest(const Store *store, StoreId id)
{
    const StoreThing *t = Thing(store, id);
    return t && (t->flags & STORE_THING_GUEST);
}

bool StoreAttach(Store *store, StoreId thing, StoreId parent)
{
    StoreThing *t = Thing(store, thing);
    if (!t)
        return store ? StaleFail(store, thing) : false;
    StoreThing *p = Thing(store, parent);
    if (!p)
        return StaleFail(store, parent);
    if (!CheckSharedChange(store, t, "attach"))
        return false;
    for (uint32_t a = parent.index; a != STORE_NO_INDEX; a = store->things[a].parent)
        if (a == thing.index)
            return StoreFail(store, false, "cycle: %s #%u can't hang under itself or its own child",
                             KindNameOf(store, t), thing.index);
    Unlink(store, thing.index);
    StoreLinkChild(store, thing.index, parent.index, STORE_NO_SYMBOL, true);
    SetOwnerTree(store, thing.index, p->owner);
    return true;
}

bool StoreDetach(Store *store, StoreId thing)
{
    StoreThing *t = Thing(store, thing);
    if (!t)
        return store ? StaleFail(store, thing) : false;
    if (!CheckSharedChange(store, t, "detach"))
        return false;
    if (t->parent == STORE_NO_INDEX)
        return StoreFail(store, false, "detach: %s #%u is not attached to anything",
                         KindNameOf(store, t), thing.index);
    DetachIndex(store, thing.index);
    return true;
}

int StoreThings(const Store *store, StoreKind kindOrDerived, StoreId *out, int max)
{
    int n = 0;
    for (uint32_t i = 0; store && i < store->thingCount; i++)
    {
        const StoreThing *t = &store->things[i];
        if (!Visible(t) || (kindOrDerived >= 0 && !StoreKindIs(store, t->kind, kindOrDerived)))
            continue;
        if (out)
        {
            if (n >= max)
                break;
            out[n] = IdOf(store, i);
        }
        n++;
    }
    return n;
}

uint32_t StoreCount(const Store *store)
{
    uint32_t n = 0;
    for (uint32_t i = 0; store && i < store->thingCount; i++)
        n += Visible(&store->things[i]);
    return n;
}

// ---- fields -----------------------------------------------------------------------------------
typedef struct Access
{
    StoreThing *thing;
    StoreKindData *kind;
    StoreFieldDecl *decl;
    unsigned char *p;
} Access;

static bool Resolve(const Store *store, StoreId id, int field, Access *a)
{
    if (!store || !store->error)
        return false;
    a->thing = Thing(store, id);
    if (!a->thing)
        return StaleFail(store, id);
    a->kind = store->kinds[a->thing->kind];
    if (field < 0 || field >= a->kind->fieldCount)
        return StoreFail(store, false, "field: %s has no field %d", a->kind->name, field);
    a->decl = &a->kind->fields[field];
    bool local = a->decl->flags & STORE_LOCAL;
    a->p = (local ? a->kind->local + (size_t)a->thing->row * (size_t)a->kind->localSize
                  : a->kind->shared + (size_t)a->thing->row * (size_t)a->kind->sharedSize) +
           a->kind->offsets[field];
    return true;
}

static bool CheckRead(const Store *store, const Access *a)
{
    if (store->phase == STORE_PHASE_GAMEPLAY && (a->decl->flags & STORE_LOCAL))
        return StoreFail(store, true,
                         "local-read: %s on %s #%u is a local field; a gameplay handler can't "
                         "read it",
                         a->decl->name, a->kind->name, (unsigned)(a->thing - store->things));
    return true;
}

static bool CheckWrite(const Store *store, const Access *a)
{
    unsigned index = (unsigned)(a->thing - store->things);
    bool local = a->decl->flags & STORE_LOCAL;
    if (store->phase == STORE_PHASE_PRESENTATION && !local)
        return StoreFail(store, true,
                         "shared-write: %s on %s #%u is shared state; a presentation handler "
                         "can't write it",
                         a->decl->name, a->kind->name, index);
    if (store->phase == STORE_PHASE_GAMEPLAY && !local && a->thing->owner != store->currentOwner)
        return StoreFail(store, true,
                         "not-owner: %s on %s #%u belongs to player %d, and this handler runs "
                         "for player %d",
                         a->decl->name, a->kind->name, index, a->thing->owner,
                         store->currentOwner);
    if (store->phase != STORE_PHASE_NONE && (a->decl->flags & STORE_ENGINE))
        return StoreFail(store, true, "engine-field: %s on %s #%u is written by the engine",
                         a->decl->name, a->kind->name, index);
    return true;
}

static bool ResolveKind(const Store *store, const Access *a, StoreType type)
{
    if (a->decl->type == type)
        return true;
    return StoreFail(store, true, "type: %s on %s is a %s, not a %s", a->decl->name, a->kind->name,
                     StoreTypeName(a->decl->type), StoreTypeName(type));
}

bool StoreGet(const Store *store, StoreId id, int field, StoreValue *out)
{
    Access a;
    if (!out || !Resolve(store, id, field, &a) || !CheckRead(store, &a))
        return false;
    if (IsCollection(a.decl->type))
        return StoreFail(store, true, "type: %s on %s is a %s; read its elements", a.decl->name,
                         a.kind->name, StoreTypeName(a.decl->type));
    ReadScalar(a.p, a.decl->type, out);
    return true;
}

static bool SetScalar(Store *store, StoreId id, int field, const StoreValue *value, bool engine)
{
    Access a;
    if (!Resolve(store, id, field, &a) || (!engine && !CheckWrite(store, &a)))
        return false;
    if (IsCollection(a.decl->type) || !WriteScalar(a.p, a.decl->type, value))
        return StoreFail(store, true, "type: %s on %s holds %s, not %s", a.decl->name,
                         a.kind->name, StoreTypeName(a.decl->type),
                         StoreTypeName(value ? value->type : STORE_NONE));
    return true;
}

bool StoreSet(Store *store, StoreId id, int field, const StoreValue *value)
{
    return SetScalar(store, id, field, value, false);
}

bool StoreSetEngine(Store *store, StoreId id, int field, const StoreValue *value)
{
    return SetScalar(store, id, field, value, true);
}

int StoreCountOf(const Store *store, StoreId id, int field)
{
    Access a;
    if (!Resolve(store, id, field, &a) || !CheckRead(store, &a))
        return -1;
    switch (a.decl->type)
    {
    case STORE_LIST:
    case STORE_SET:
    case STORE_MAP:
        return ReadCount(a.p);
    case STORE_GRID:
        return a.decl->max * a.decl->height;
    default:
        StoreFail(store, true, "type: %s on %s is a %s, not a collection", a.decl->name,
                  a.kind->name, StoreTypeName(a.decl->type));
        return -1;
    }
}

bool StoreGetAt(const Store *store, StoreId id, int field, int index, StoreValue *key,
                StoreValue *value)
{
    Access a;
    if (!Resolve(store, id, field, &a) || !CheckRead(store, &a))
        return false;
    const StoreFieldDecl *d = a.decl;
    int es = ElementSize(d->element);
    int count = d->type == STORE_GRID ? d->max * d->height
                : IsCollection(d->type)  ? ReadCount(a.p)
                                         : -1;
    if (count < 0)
        return StoreFail(store, true, "type: %s on %s is a %s, not a collection", d->name,
                         a.kind->name, StoreTypeName(d->type));
    if (index < 0 || index >= count)
        return StoreFail(store, false, "range: %s on %s has %d elements, and %d is not one", d->name,
                         a.kind->name, count, index);
    if (key)
    {
        memset(key, 0, sizeof *key);
        if (d->type == STORE_MAP)
            ReadScalar(a.p + 4 + index * (ElementSize(d->key) + es), d->key, key);
    }
    if (value)
    {
        const unsigned char *e = d->type == STORE_GRID  ? a.p + index * es
                                 : d->type == STORE_MAP ? a.p + 4 + index * (ElementSize(d->key) + es) +
                                                              ElementSize(d->key)
                                                        : a.p + 4 + index * es;
        ReadScalar(e, d->element, value);
    }
    return true;
}

static bool ResolveWrite(Store *store, StoreId id, int field, Access *a)
{
    return Resolve(store, id, field, a) && CheckWrite(store, a);
}

bool StoreSetList(Store *store, StoreId id, int field, const StoreValue *items, int count)
{
    Access a;
    if (!ResolveWrite(store, id, field, &a))
        return false;
    if (a.decl->type != STORE_LIST && a.decl->type != STORE_SET)
        return StoreFail(store, true, "type: %s on %s is a %s, not a list or set", a.decl->name,
                         a.kind->name, StoreTypeName(a.decl->type));
    return ListPut(store, a.decl, a.p, items, count, a.decl->name);
}

bool StoreMapGet(const Store *store, StoreId id, int field, const StoreValue *key, StoreValue *out)
{
    Access a;
    if (!Resolve(store, id, field, &a) || !CheckRead(store, &a) ||
        !ResolveKind(store, &a, STORE_MAP))
        return false;
    unsigned char k[12];
    memset(k, 0, sizeof k);
    if (!WriteScalar(k, a.decl->key, key))
        return StoreFail(store, true, "type: the keys of %s are %s, not %s", a.decl->name,
                         StoreTypeName(a.decl->key), StoreTypeName(key ? key->type : STORE_NONE));
    bool found;
    int at = MapFind(store, a.decl, a.p, k, &found);
    if (!found)
        return StoreFail(store, false, "missing: %s on %s has no such key", a.decl->name,
                         a.kind->name);
    if (out)
        ReadScalar(a.p + 4 + at * (ElementSize(a.decl->key) + ElementSize(a.decl->element)) +
                       ElementSize(a.decl->key),
                   a.decl->element, out);
    return true;
}

bool StoreMapSet(Store *store, StoreId id, int field, const StoreValue *key, const StoreValue *value)
{
    Access a;
    if (!ResolveWrite(store, id, field, &a) || !ResolveKind(store, &a, STORE_MAP))
        return false;
    return MapPut(store, a.decl, a.p, key, value, a.decl->name);
}

bool StoreMapRemove(Store *store, StoreId id, int field, const StoreValue *key)
{
    Access a;
    if (!ResolveWrite(store, id, field, &a) || !ResolveKind(store, &a, STORE_MAP))
        return false;
    unsigned char k[12];
    memset(k, 0, sizeof k);
    if (!WriteScalar(k, a.decl->key, key))
        return StoreFail(store, true, "type: the keys of %s are %s, not %s", a.decl->name,
                         StoreTypeName(a.decl->key), StoreTypeName(key ? key->type : STORE_NONE));
    bool found;
    int at = MapFind(store, a.decl, a.p, k, &found), count = ReadCount(a.p);
    if (!found)
        return StoreFail(store, false, "missing: %s on %s has no such key", a.decl->name,
                         a.kind->name);
    int pair = ElementSize(a.decl->key) + ElementSize(a.decl->element);
    unsigned char *slot = a.p + 4 + at * pair;
    memmove(slot, slot + pair, (size_t)(count - at - 1) * (size_t)pair);
    memset(a.p + 4 + (count - 1) * pair, 0, (size_t)pair);
    WriteCount(a.p, count - 1);
    return true;
}

static bool GridRect(const Store *store, const Access *a, int x, int y, int w, int h)
{
    if (x < 0 || y < 0 || w < 0 || h < 0 || x > a->decl->max - w || y > a->decl->height - h)
        return StoreFail(store, false, "range: %s on %s is %d by %d cells", a->decl->name,
                         a->kind->name, a->decl->max, a->decl->height);
    return true;
}

bool StoreGridGet(const Store *store, StoreId id, int field, int x, int y, StoreValue *out)
{
    Access a;
    if (!out || !Resolve(store, id, field, &a) || !CheckRead(store, &a) ||
        !ResolveKind(store, &a, STORE_GRID) || !GridRect(store, &a, x, y, 1, 1))
        return false;
    ReadScalar(a.p + (y * a.decl->max + x) * ElementSize(a.decl->element), a.decl->element, out);
    return true;
}

bool StoreGridFill(Store *store, StoreId id, int field, int x, int y, int w, int h,
                   const StoreValue *value)
{
    Access a;
    if (!ResolveWrite(store, id, field, &a) || !ResolveKind(store, &a, STORE_GRID) ||
        !GridRect(store, &a, x, y, w, h))
        return false;
    int es = ElementSize(a.decl->element);
    unsigned char e[12];
    memset(e, 0, sizeof e);
    if (!WriteScalar(e, a.decl->element, value))
        return StoreFail(store, true, "type: %s on %s holds %s cells, not %s", a.decl->name,
                         a.kind->name, StoreTypeName(a.decl->element),
                         StoreTypeName(value ? value->type : STORE_NONE));
    for (int row = y; row < y + h; row++)
        for (int column = x; column < x + w; column++)
            memcpy(a.p + (row * a.decl->max + column) * es, e, (size_t)es);
    return true;
}

bool StoreGridSet(Store *store, StoreId id, int field, int x, int y, const StoreValue *value)
{
    return StoreGridFill(store, id, field, x, y, 1, 1, value);
}

void *StoreSharedBlock(Store *store, StoreId id)
{
    StoreThing *t = Thing(store, id);
    if (!t || !store->kinds[t->kind]->sharedSize)
        return NULL;
    StoreKindData *k = store->kinds[t->kind];
    return k->shared + (size_t)t->row * (size_t)k->sharedSize;
}

void *StoreLocalBlock(Store *store, StoreId id)
{
    StoreThing *t = Thing(store, id);
    if (!t || !store->kinds[t->kind]->localSize)
        return NULL;
    StoreKindData *k = store->kinds[t->kind];
    return k->local + (size_t)t->row * (size_t)k->localSize;
}

const char *StoreLastError(const Store *store)
{
    return store && store->error ? store->error : "";
}

// ---- lifetime ---------------------------------------------------------------------------------
bool StoreInit(Store *store, uint64_t seed)
{
    if (!store)
        return false;
    memset(store, 0, sizeof *store);
    store->worldRandom = seed;
    store->localRandom = seed ^ LOCAL_STREAM_SALT;
    store->localOwners = ~0ull;
    store->dt = 1.0f / 60.0f;
    store->current = STORE_NULL;
    store->currentOwner = -1;
    store->error = calloc(ERROR_MAX, 1);
    if (!store->error)
        return false;
    // Fixed ids for the engine's own events, the same in every store.
    return StoreIntern(store, "start") == SYMBOL_START && StoreIntern(store, "tick") == SYMBOL_TICK &&
           StoreIntern(store, "frame") == SYMBOL_FRAME &&
           StoreIntern(store, "draw-hud") == SYMBOL_DRAW_HUD;
}

void StoreFree(Store *store)
{
    if (!store)
        return;
    for (int i = 0; i < store->kindCount; i++)
        FreeKind(store->kinds[i]);
    for (int32_t i = 0; i < store->symbolCount; i++)
        free(store->symbols[i]);
    free(store->kinds);
    free(store->things);
    free(store->symbols);
    free(store->symbolTable);
    free(store->queue);
    free(store->commands);
    free(store->timers);
    free(store->starts);
    free(store->systems);
    free(store->error);
    memset(store, 0, sizeof *store);
}

void StoreClearWorld(Store *store)
{
    store->thingCount = 0;
    store->firstFree = 0;
    store->pendingRemovals = 0;
    for (int i = 0; i < store->kindCount; i++)
    {
        StoreKindData *k = store->kinds[i];
        k->rows = k->freeCount = 0;
        for (uint32_t r = 0; r < k->shadowCapacity; r++)
            k->shadowThing[r] = STORE_NULL; // everything loaded is new to the next frame
    }
    store->queueHead = store->queueCount = 0;
    store->commandCount = 0;
    store->timerCount = 0;
    store->startCount = 0;
}

void StoreSetClock(Store *store, uint64_t tickCount, uint64_t worldRandom)
{
    store->tickCount = tickCount;
    store->worldRandom = worldRandom;
}

void StoreSetHooks(Store *store, const StoreHooks *hooks)
{
    if (!store)
        return;
    if (hooks)
        store->hooks = *hooks;
    else
        memset(&store->hooks, 0, sizeof store->hooks);
}

void StoreSetLocalOwners(Store *store, const int *owners, int count)
{
    if (!store)
        return;
    store->localOwners = 0;
    for (int i = 0; owners && i < count; i++)
        if (owners[i] >= 0 && owners[i] < 64)
            store->localOwners |= 1ull << owners[i];
}

// ---- handlers, messages, timers ---------------------------------------------------------------
void StoreKindSetHandler(Store *store, StoreKind kind, StoreHandlerFn handler, void *user)
{
    StoreKindData *k = Kind(store, kind);
    if (!k)
        return;
    k->handler = handler;
    k->user = user;
    RecomputeKinds(store);
}

void StoreKindHandles(Store *store, StoreKind kind, StoreSymbol event, bool handles)
{
    StoreKindData *k = Kind(store, kind);
    if (!k || event < 0 || event >= store->symbolCount || OwnHandles(k, event) == handles)
        return;
    if (handles)
    {
        StoreSymbol *events = Grow(k->events, &k->eventCapacity, k->eventCount + 1, sizeof *events);
        if (!events)
            return;
        k->events = events;
        events[k->eventCount++] = event;
    }
    else
    {
        int kept = 0;
        for (int i = 0; i < k->eventCount; i++)
            if (k->events[i] != event)
                k->events[kept++] = k->events[i];
        k->eventCount = kept;
    }
    RecomputeKinds(store);
}

bool StoreAddSystem(Store *store, StoreSystemFn system, void *user)
{
    if (!store || !system)
        return false;
    StoreSystem *systems = Grow(store->systems, &store->systemCapacity, store->systemCount + 1,
                                sizeof *systems);
    if (!systems)
        return false;
    store->systems = systems;
    systems[store->systemCount++] = (StoreSystem){system, user};
    return true;
}

static bool MakeMessage(const Store *store, StoreMessage *m, StoreId target, StoreSymbol event,
                        const StoreValue *args, int count)
{
    if (count < 0 || count > STORE_MAX_ARGS || (count && !args))
        return StoreFail(store, true, "type: a message carries 0 to %d arguments, not %d",
                         STORE_MAX_ARGS, count);
    if (event < 0 || event >= store->symbolCount)
        return StoreFail(store, false, "event: %d is not a symbol", event);
    memset(m, 0, sizeof *m);
    m->target = target;
    m->event = event;
    m->count = count;
    for (int i = 0; i < count; i++)
    {
        StoreType type = args[i].type;
        if (type == STORE_NONE)
            continue;
        unsigned char bytes[STORE_STRING_MAX + 1];
        if (!IsScalar(type) || !WriteScalar(bytes, type, &args[i]))
            return StoreFail(store, true, "type: argument %d of %s is a %s; messages carry data",
                             i, SymbolText(store, event), StoreTypeName(type));
        ReadScalar(bytes, type, &m->args[i]);
    }
    return true;
}

static bool Push(StoreMessage **items, int *count, int *capacity, const StoreMessage *m)
{
    StoreMessage *grown = Grow(*items, capacity, *count + 1, sizeof *grown);
    if (!grown)
        return false;
    *items = grown;
    grown[(*count)++] = *m;
    return true;
}

bool StoreSend(Store *store, StoreId target, StoreSymbol event, const StoreValue *args, int count)
{
    if (!store || !store->error)
        return false;
    if (store->phase == STORE_PHASE_PRESENTATION)
        return StoreFail(store, true,
                         "shared-write: a presentation handler can't send %s; send a player "
                         "command instead",
                         SymbolText(store, event));
    if (!Thing(store, target))
        return StaleFail(store, target);
    StoreMessage m;
    if (!MakeMessage(store, &m, target, event, args, count))
        return false;
    bool ok = store->ticking ? Push(&store->queue, &store->queueCount, &store->queueCapacity, &m)
                             : Push(&store->commands, &store->commandCount,
                                    &store->commandCapacity, &m);
    return ok || StoreFail(store, false, "memory: out of memory sending %s", SymbolText(store, event));
}

bool StoreCommand(Store *store, StoreId target, StoreSymbol event, const StoreValue *args,
                  int count)
{
    if (!store || !store->error)
        return false;
    if (!Thing(store, target))
        return StaleFail(store, target);
    StoreMessage m;
    if (!MakeMessage(store, &m, target, event, args, count))
        return false;
    return Push(&store->commands, &store->commandCount, &store->commandCapacity, &m) ||
           StoreFail(store, false, "memory: out of memory queueing %s", SymbolText(store, event));
}

bool StoreInsertTimer(Store *store, const StoreTimer *timer)
{
    StoreTimer *timers = Grow(store->timers, &store->timerCapacity, store->timerCount + 1,
                              sizeof *timers);
    if (!timers)
        return false;
    store->timers = timers;
    int at = store->timerCount;
    while (at > 0 && (timers[at - 1].due > timer->due ||
                      (timers[at - 1].due == timer->due && timers[at - 1].sequence > timer->sequence)))
        at--;
    memmove(&timers[at + 1], &timers[at], (size_t)(store->timerCount - at) * sizeof *timers);
    timers[at] = *timer;
    store->timerCount++;
    if (timer->sequence >= store->timerSequence)
        store->timerSequence = timer->sequence + 1;
    return true;
}

bool StoreAfter(Store *store, StoreId self, float seconds, StoreSymbol event,
                const StoreValue *args, int count)
{
    if (!store || !store->error)
        return false;
    StoreThing *t = Thing(store, self);
    if (!t)
        return StaleFail(store, self);
    if (!CheckSharedChange(store, t, "after"))
        return false;
    if (!isfinite(seconds) || seconds < 0.0f)
        return StoreFail(store, true, "type: a timer needs a time of 0 seconds or more");
    StoreMessage m;
    if (!MakeMessage(store, &m, self, event, args, count))
        return false;
    double ticks = floor((double)seconds / (double)store->dt + 0.5);
    StoreTimer timer;
    memset(&timer, 0, sizeof timer);
    timer.due = store->tickCount + (ticks < 1.0 ? 1u : (uint64_t)ticks);
    timer.sequence = store->timerSequence;
    timer.target = self;
    timer.event = event;
    timer.count = count;
    memcpy(timer.args, m.args, sizeof timer.args);
    return StoreInsertTimer(store, &timer) ||
           StoreFail(store, false, "memory: out of memory setting a timer");
}

static void Call(Store *store, uint32_t index, StoreSymbol event, const StoreValue *args, int count,
                 StorePhase phase)
{
    StoreThing *t = &store->things[index];
    StoreKindData *k = store->kinds[t->kind];
    if (!k->callHandler)
        return;
    StorePhase oldPhase = store->phase;
    StoreId oldCurrent = store->current;
    int oldOwner = store->currentOwner;
    StoreId id = {index, t->generation};
    store->phase = phase;
    store->current = id;
    store->currentOwner = t->owner;
    k->callHandler(store, id, event, args, count, k->callUser);
    store->phase = oldPhase;
    store->current = oldCurrent;
    store->currentOwner = oldOwner;
}

static void WarnUnhandled(Store *store, StoreKindData *k, uint32_t index, StoreSymbol event)
{
    for (int i = 0; i < k->warnedCount; i++)
        if (k->warned[i] == event)
            return;
    StoreSymbol *warned = Grow(k->warned, &k->warnedCapacity, k->warnedCount + 1, sizeof *warned);
    if (warned)
    {
        k->warned = warned;
        warned[k->warnedCount++] = event;
    }
    TraceLog(LOG_WARNING, "STORE: %s #%u was sent %s, which %s has no handler for", k->name, index,
             SymbolText(store, event), k->name);
}

// Steps 5 and 6: first in, first out, until the queue is empty or the tick's cap is reached.
static void Deliver(Store *store)
{
    while (store->queueHead < store->queueCount)
    {
        StoreMessage m = store->queue[store->queueHead++];
        StoreThing *t = Thing(store, m.target);
        if (!t || !IsLocalOwner(store, t->owner))
            continue;
        if (store->deliveries >= DELIVERY_CAP)
        {
            if (!store->dropped)
                TraceLog(LOG_WARNING,
                         "STORE: %d messages delivered this tick; dropping the rest, starting "
                         "with %s to %s #%u",
                         DELIVERY_CAP, SymbolText(store, m.event), KindNameOf(store, t),
                         m.target.index);
            store->dropped++;
            continue;
        }
        store->deliveries++;
        if (StoreKindHandlesEvent(store, t->kind, m.event))
            Call(store, m.target.index, m.event, m.args, m.count, STORE_PHASE_GAMEPLAY);
        else if (m.event != SYMBOL_START)
            WarnUnhandled(store, store->kinds[t->kind], m.target.index, m.event);
    }
    store->queueHead = store->queueCount = 0;
}

void StoreTick(Store *store, float dt)
{
    if (!store || !store->error || store->ticking || store->framing)
        return;
    if (dt > 0.0f)
        store->dt = dt;
    store->ticking = true;
    store->deliveries = store->dropped = 0;
    store->queueHead = store->queueCount = 0;
    // Things spawned outside a tick start first, in spawn order, before commands and timers.
    // From here until step 7 a spawn queues its start at the tail of the queue instead.
    store->queueOpen = true;
    StoreMessage m;
    for (int i = 0; i < store->startCount; i++)
        if (MakeMessage(store, &m, store->starts[i], SYMBOL_START, NULL, 0))
            Push(&store->queue, &store->queueCount, &store->queueCapacity, &m);
    store->startCount = 0;
    Deliver(store);
    // Step 2: the players' commands.
    for (int i = 0; i < store->commandCount; i++)
        Push(&store->queue, &store->queueCount, &store->queueCapacity, &store->commands[i]);
    store->commandCount = 0;
    // Step 3: due timers, in due-tick then creation order.
    int due = 0;
    while (due < store->timerCount && store->timers[due].due <= store->tickCount)
    {
        const StoreTimer *timer = &store->timers[due++];
        memset(&m, 0, sizeof m);
        m.target = timer->target;
        m.event = timer->event;
        m.count = timer->count;
        memcpy(m.args, timer->args, sizeof m.args);
        Push(&store->queue, &store->queueCount, &store->queueCapacity, &m);
    }
    if (due)
    {
        memmove(store->timers, store->timers + due,
                (size_t)(store->timerCount - due) * sizeof *store->timers);
        store->timerCount -= due;
    }
    // Step 4: tick handlers of locally owned things, in id order.
    StoreValue argument;
    memset(&argument, 0, sizeof argument);
    argument.type = STORE_FLOAT;
    argument.as.f = dt;
    uint32_t count = store->thingCount;
    for (uint32_t i = 0; i < count; i++)
    {
        const StoreThing *t = &store->things[i];
        if (Visible(t) && store->kinds[t->kind]->handlesTick && IsLocalOwner(store, t->owner))
            Call(store, i, SYMBOL_TICK, &argument, 1, STORE_PHASE_GAMEPLAY);
    }
    Deliver(store); // step 5
    for (int i = 0; i < store->systemCount; i++) // step 6
        store->systems[i].run(store, dt, store->systems[i].user);
    Deliver(store);
    store->queueOpen = false; // a spawn from a removal hook starts next tick
    ApplyRemovals(store); // step 7
    store->tickCount++;
    store->ticking = false;
}

// ---- the frame --------------------------------------------------------------------------------
static bool ReserveShadow(StoreKindData *k)
{
    if (k->shadowCapacity >= k->rowCapacity)
        return true;
    uint32_t cap = k->rowCapacity;
    unsigned char *shadow = realloc(k->shadow, (size_t)cap * (size_t)(k->sharedSize ? k->sharedSize : 1));
    if (!shadow)
        return false;
    k->shadow = shadow;
    StoreId *owners = realloc(k->shadowThing, (size_t)cap * sizeof *owners);
    if (!owners)
        return false;
    k->shadowThing = owners;
    for (uint32_t r = k->shadowCapacity; r < cap; r++)
        owners[r] = (StoreId){UINT32_MAX, 0};
    k->shadowCapacity = cap;
    return true;
}

static void ValueAt(const StoreFieldDecl *d, const unsigned char *p, StoreValue *out)
{
    if (IsCollection(d->type))
    {
        memset(out, 0, sizeof *out);
        out->type = d->type;
    }
    else
        ReadScalar(p, d->type, out);
}

// -changed for every watched field that differs from the shadow; everything, on a new thing.
static void Changes(Store *store, uint32_t index)
{
    StoreThing *t = &store->things[index];
    StoreKindData *k = store->kinds[t->kind];
    if (!k->watches || !ReserveShadow(k))
        return;
    StoreId id = {index, t->generation};
    uint32_t row = t->row;
    StoreId seen = k->shadowThing[row];
    bool appeared = seen.index != id.index || seen.generation != id.generation;
    for (int f = 0; f < k->fieldCount; f++)
    {
        if (k->changed[f] == STORE_NO_SYMBOL)
            continue;
        if (!Visible(&store->things[index]) || store->things[index].generation != id.generation)
            return; // a handler removed it
        size_t at = (size_t)row * (size_t)k->sharedSize + (size_t)k->offsets[f];
        const unsigned char *now = k->shared + at, *was = k->shadow + at;
        if (!appeared && !memcmp(now, was, (size_t)k->sizes[f]))
            continue;
        StoreValue args[2];
        memset(args, 0, sizeof args);
        if (!appeared)
            ValueAt(&k->fields[f], was, &args[0]);
        ValueAt(&k->fields[f], now, &args[1]);
        Call(store, index, k->changed[f], args, 2, STORE_PHASE_PRESENTATION);
    }
}

void StoreFrame(Store *store, float dt)
{
    if (!store || !store->error || store->ticking || store->framing)
        return;
    store->framing = true;
    StoreValue argument;
    memset(&argument, 0, sizeof argument);
    argument.type = STORE_FLOAT;
    argument.as.f = dt;
    uint32_t count = store->thingCount;
    for (uint32_t i = 0; i < count; i++)
        if (Visible(&store->things[i]) && store->kinds[store->things[i].kind]->handlesFrame)
            Call(store, i, SYMBOL_FRAME, &argument, 1, STORE_PHASE_PRESENTATION);
    count = store->thingCount;
    for (uint32_t i = 0; i < count; i++)
        if (Visible(&store->things[i]) && store->kinds[store->things[i].kind]->watches)
            Changes(store, i);
    count = store->thingCount;
    for (uint32_t i = 0; i < count; i++)
        if (Visible(&store->things[i]) && store->kinds[store->things[i].kind]->handlesDrawHud)
            Call(store, i, SYMBOL_DRAW_HUD, NULL, 0, STORE_PHASE_PRESENTATION);
    for (uint32_t i = 0; i < store->thingCount; i++)
    {
        const StoreThing *t = &store->things[i];
        if (!Visible(t))
            continue;
        StoreKindData *k = store->kinds[t->kind];
        if (!k->watches || !ReserveShadow(k))
            continue;
        size_t at = (size_t)t->row * (size_t)k->sharedSize;
        memcpy(k->shadow + at, k->shared + at, (size_t)k->sharedSize);
        k->shadowThing[t->row] = (StoreId){i, t->generation};
    }
    ApplyRemovals(store);
    store->framing = false;
}

uint64_t StoreTickCount(const Store *store) { return store ? store->tickCount : 0; }

float StoreTickTime(const Store *store)
{
    return store ? (float)((double)store->tickCount * (double)store->dt) : 0.0f;
}

StorePhase StorePhaseNow(const Store *store) { return store ? store->phase : STORE_PHASE_NONE; }
StoreId StoreCurrent(const Store *store) { return store ? store->current : STORE_NULL; }
int StoreCurrentOwner(const Store *store) { return store ? store->currentOwner : -1; }

// ---- randomness -------------------------------------------------------------------------------
uint32_t StoreRandom(Store *store, StoreId thing, uint32_t n)
{
    if (!store || !store->error || !n)
        return 0;
    if (thing.index == UINT32_MAX)
    {
        if (store->phase == STORE_PHASE_PRESENTATION)
            return StoreFail(store, true,
                             "shared-write: the world's random stream is shared state; a "
                             "presentation handler draws from the local one"),
                   0;
        return Uniform(&store->worldRandom, n);
    }
    StoreThing *t = Thing(store, thing);
    if (!t)
        return StaleFail(store, thing), 0;
    if (!CheckSharedChange(store, t, "random"))
        return 0;
    return Uniform(&t->random, n);
}

uint32_t StoreRandomLocal(Store *store, uint32_t n)
{
    return store ? Uniform(&store->localRandom, n) : 0;
}

// ---- snapshot ---------------------------------------------------------------------------------
void StoreSnapshotFree(StoreSnapshot *snapshot)
{
    if (!snapshot)
        return;
    for (int i = 0; snapshot->pools && i < snapshot->kindCount; i++)
    {
        free(snapshot->pools[i].shared);
        free(snapshot->pools[i].freeRows);
    }
    free(snapshot->pools);
    free(snapshot->things);
    free(snapshot->timers);
    free(snapshot->starts);
    free(snapshot);
}

StoreSnapshot *StoreSnapshotTake(const Store *store)
{
    if (!store || !store->error || store->ticking || store->framing)
        return NULL;
    StoreSnapshot *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->kindCount = store->kindCount;
    s->symbolCount = store->symbolCount;
    s->thingCount = store->thingCount;
    s->firstFree = store->firstFree;
    s->timerCount = store->timerCount;
    s->timerSequence = store->timerSequence;
    s->startCount = store->startCount;
    s->tickCount = store->tickCount;
    s->worldRandom = store->worldRandom;
    s->dt = store->dt;
    s->things = Duplicate(store->things, (size_t)store->thingCount * sizeof *store->things);
    s->timers = Duplicate(store->timers, (size_t)store->timerCount * sizeof *store->timers);
    s->starts = Duplicate(store->starts, (size_t)store->startCount * sizeof *store->starts);
    s->pools = calloc((size_t)store->kindCount + 1, sizeof *s->pools);
    bool ok = s->things && s->timers && s->starts && s->pools;
    for (int i = 0; ok && i < store->kindCount; i++)
    {
        const StoreKindData *k = store->kinds[i];
        SnapshotPool *pool = &s->pools[i];
        pool->rows = k->rows;
        pool->freeCount = k->freeCount;
        pool->sharedSize = k->sharedSize;
        pool->shared = Duplicate(k->shared, (size_t)k->rows * (size_t)k->sharedSize);
        pool->freeRows = Duplicate(k->freeRows, (size_t)k->freeCount * sizeof *k->freeRows);
        ok = pool->shared && pool->freeRows;
    }
    if (!ok)
    {
        StoreSnapshotFree(s);
        return NULL;
    }
    return s;
}

// Makes a kind's pools hold at least rows rows, keeping their contents.
static bool ReserveRows(StoreKindData *k, uint32_t rows)
{
    while (k->rowCapacity < rows)
    {
        uint32_t saveRows = k->rows, saveFree = k->freeCount, row;
        k->rows = k->rowCapacity;
        k->freeCount = 0;
        bool ok = AllocRow(k, &row);
        k->rows = saveRows;
        k->freeCount = saveFree;
        if (!ok)
            return false;
    }
    return true;
}

bool StoreSnapshotRestore(Store *store, const StoreSnapshot *snapshot)
{
    if (!store || !store->error || !snapshot)
        return false;
    if (store->ticking || store->framing)
        return StoreFail(store, false, "restore: not inside a tick or a frame");
    if (snapshot->kindCount > store->kindCount || snapshot->symbolCount > store->symbolCount)
        return StoreFail(store, false, "restore: the snapshot has kinds or symbols this store lacks");
    for (int i = 0; i < snapshot->kindCount; i++)
        if (snapshot->pools[i].sharedSize != store->kinds[i]->sharedSize)
            return StoreFail(store, false, "restore: kind %s is laid out differently",
                             store->kinds[i]->name);
    // Everything that can fail happens before anything changes.
    uint32_t *reset = malloc(((size_t)snapshot->thingCount + 1) * sizeof *reset);
    if (!reset)
        goto oom;
    if (snapshot->thingCount > store->thingCapacity)
    {
        StoreThing *things = realloc(store->things, (size_t)snapshot->thingCount * sizeof *things);
        if (!things)
            goto oom;
        store->things = things;
        store->thingCapacity = snapshot->thingCount;
    }
    for (int i = 0; i < snapshot->kindCount; i++)
        if (!ReserveRows(store->kinds[i], snapshot->pools[i].rows))
            goto oom;
    StoreTimer *timers = Grow(store->timers, &store->timerCapacity, snapshot->timerCount + 1,
                              sizeof *timers);
    if (!timers)
        goto oom;
    store->timers = timers;
    StoreId *starts = Grow(store->starts, &store->startCapacity, snapshot->startCount + 1,
                           sizeof *starts);
    if (!starts)
        goto oom;
    store->starts = starts;
    // A thing that is the same before and after keeps its local block; others start fresh.
    uint32_t resets = 0;
    for (uint32_t i = 0; i < snapshot->thingCount; i++)
    {
        const StoreThing *after = &snapshot->things[i];
        if (!(after->flags & STORE_THING_LIVE))
            continue;
        const StoreThing *before = i < store->thingCount ? &store->things[i] : NULL;
        if (!before || !(before->flags & STORE_THING_LIVE) || before->generation != after->generation ||
            before->kind != after->kind)
            reset[resets++] = i;
    }
    if (snapshot->thingCount)
        memcpy(store->things, snapshot->things, (size_t)snapshot->thingCount * sizeof *store->things);
    store->thingCount = snapshot->thingCount;
    store->firstFree = snapshot->firstFree;
    store->pendingRemovals = 0;
    for (int i = 0; i < store->kindCount; i++)
    {
        StoreKindData *k = store->kinds[i];
        if (i >= snapshot->kindCount)
        {
            k->rows = k->freeCount = 0;
            continue;
        }
        const SnapshotPool *pool = &snapshot->pools[i];
        if (pool->rows && pool->sharedSize)
            memcpy(k->shared, pool->shared, (size_t)pool->rows * (size_t)pool->sharedSize);
        if (pool->freeCount)
            memcpy(k->freeRows, pool->freeRows, (size_t)pool->freeCount * sizeof *k->freeRows);
        k->rows = pool->rows;
        k->freeCount = pool->freeCount;
    }
    for (uint32_t i = 0; i < resets; i++)
        ResetBlocks(store->kinds[store->things[reset[i]].kind], store->things[reset[i]].row, false,
                    true);
    free(reset);
    memcpy(store->timers, snapshot->timers, (size_t)snapshot->timerCount * sizeof *store->timers);
    store->timerCount = snapshot->timerCount;
    store->timerSequence = snapshot->timerSequence;
    memcpy(store->starts, snapshot->starts, (size_t)snapshot->startCount * sizeof *store->starts);
    store->startCount = snapshot->startCount;
    store->tickCount = snapshot->tickCount;
    store->worldRandom = snapshot->worldRandom;
    store->dt = snapshot->dt;
    store->queueHead = store->queueCount = 0;
    return true;
oom:
    free(reset);
    return StoreFail(store, false, "restore: out of memory");
}

// ---- hash -------------------------------------------------------------------------------------
static uint64_t HashBytes(uint64_t h, const void *data, size_t size)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < size; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static uint64_t HashU32(uint64_t h, uint32_t v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    return HashBytes(h, b, 4);
}

static uint64_t HashU64(uint64_t h, uint64_t v)
{
    return HashU32(HashU32(h, (uint32_t)v), (uint32_t)(v >> 32));
}

// Symbols by name, so a world rebuilt from a save hashes as the one that wrote it.
static uint64_t HashSymbol(uint64_t h, const Store *store, StoreSymbol symbol)
{
    const char *name = StoreSymbolName(store, symbol);
    if (!name)
        return HashU32(h, UINT32_MAX);
    return HashBytes(h, name, strlen(name) + 1);
}

static uint64_t HashElement(uint64_t h, const Store *store, StoreType type, const unsigned char *p)
{
    if (type == STORE_SYMBOL)
    {
        StoreSymbol symbol;
        memcpy(&symbol, p, 4);
        return HashSymbol(h, store, symbol);
    }
    return HashBytes(h, p, (size_t)ElementSize(type));
}

static uint64_t HashValue(uint64_t h, const Store *store, const StoreValue *v)
{
    unsigned char bytes[STORE_STRING_MAX + 1];
    h = HashU32(h, (uint32_t)v->type);
    if (!IsScalar(v->type) || !WriteScalar(bytes, v->type, v))
        return h;
    return HashElement(h, store, v->type, bytes);
}

static uint64_t HashField(uint64_t h, const Store *store, const StoreFieldDecl *d,
                          const unsigned char *p, int size)
{
    bool symbols = d->element == STORE_SYMBOL || (d->type == STORE_MAP && d->key == STORE_SYMBOL);
    switch (d->type)
    {
    case STORE_SYMBOL:
        return HashElement(h, store, STORE_SYMBOL, p);
    case STORE_LIST:
    case STORE_SET:
    case STORE_MAP:
    {
        if (!symbols)
            return HashBytes(h, p, (size_t)size);
        int count = ReadCount(p), es = ElementSize(d->element);
        int ks = d->type == STORE_MAP ? ElementSize(d->key) : 0;
        h = HashU32(h, (uint32_t)count);
        for (int i = 0; i < count; i++)
        {
            const unsigned char *e = p + 4 + i * (ks + es);
            if (ks)
                h = HashElement(h, store, d->key, e);
            h = HashElement(h, store, d->element, e + ks);
        }
        return h;
    }
    case STORE_GRID:
        if (!symbols)
            return HashBytes(h, p, (size_t)size);
        for (int i = 0; i < d->max * d->height; i++)
            h = HashElement(h, store, STORE_SYMBOL, p + i * 4);
        return h;
    default:
        return HashBytes(h, p, (size_t)size);
    }
}

uint64_t StoreHash(const Store *store)
{
    uint64_t h = 0xCBF29CE484222325ull;
    if (!store)
        return h;
    for (uint32_t i = 0; i < store->thingCount; i++)
    {
        const StoreThing *t = &store->things[i];
        if (!Visible(t))
            continue;
        const StoreKindData *k = store->kinds[t->kind];
        h = HashU32(h, i);
        h = HashU32(h, t->generation);
        h = HashBytes(h, k->name, strlen(k->name) + 1);
        h = HashU32(h, t->parent);
        h = HashSymbol(h, store, t->childName);
        h = HashU32(h, (uint32_t)t->owner);
        h = HashU32(h, (uint32_t)t->spawner);
        h = HashU64(h, t->random);
        if (!k->sharedSize)
            continue;
        const unsigned char *block = k->shared + (size_t)t->row * (size_t)k->sharedSize;
        for (int f = 0; f < k->fieldCount; f++)
            if (!(k->fields[f].flags & STORE_LOCAL))
                h = HashField(h, store, &k->fields[f], block + k->offsets[f], k->sizes[f]);
    }
    h = HashU32(h, (uint32_t)store->timerCount);
    for (int i = 0; i < store->timerCount; i++)
    {
        const StoreTimer *timer = &store->timers[i];
        h = HashU64(h, timer->due);
        h = HashU32(h, timer->target.index);
        h = HashU32(h, timer->target.generation);
        h = HashSymbol(h, store, timer->event);
        h = HashU32(h, (uint32_t)timer->count);
        for (int a = 0; a < timer->count; a++)
            h = HashValue(h, store, &timer->args[a]);
    }
    return HashU64(h, store->tickCount);
}

uint64_t StoreKindsHash(const Store *store)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (int kind = 0; store && kind < store->kindCount; kind++)
    {
        const StoreKindData *k = store->kinds[kind];
        const StoreKindData *b = Kind(store, k->base);
        h = HashBytes(h, k->name, strlen(k->name) + 1);
        h = HashBytes(h, b ? b->name : "", b ? strlen(b->name) + 1 : 1);
        // Own fields only: inherited ones are hashed with the kind that declares them.
        for (int f = b ? b->fieldCount : 0; f < k->fieldCount; f++)
        {
            const StoreFieldDecl *d = &k->fields[f];
            h = HashBytes(h, d->name, strlen(d->name) + 1);
            h = HashU32(h, (uint32_t)d->type);
            h = HashU32(h, (uint32_t)d->element);
            h = HashU32(h, (uint32_t)d->key);
            h = HashU32(h, (uint32_t)d->max);
            h = HashU32(h, (uint32_t)d->height);
            h = HashU32(h, d->flags);
        }
    }
    return h;
}
