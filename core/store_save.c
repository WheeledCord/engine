/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "store_internal.h"

#include "file.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The text form of a world (docs/developer/store.md §2.9): a header line, one stanza per thing in
   tree order (roots in id order, each followed by its children in sibling order, so loading the
   links in file order rebuilds the sibling order), then the timers in firing order. */

#define NAME_MAX_TEXT 256

// ---- writing ----------------------------------------------------------------------------------
static void WriteFloat(FILE *f, float value)
{
    char text[64];
    snprintf(text, sizeof text, "%.9g", (double)value);
    // A float always reads back as a float, even where nothing else says which it was.
    fputs(text, f);
    if (!strpbrk(text, ".eEni"))
        fputs(".0", f);
}

static void WriteString(FILE *f, const char *text)
{
    fputc('"', f);
    for (; *text; text++)
    {
        if (*text == '"' || *text == '\\')
            fputc('\\', f);
        if (*text == '\n')
            fputs("\\n", f);
        else
            fputc(*text, f);
    }
    fputc('"', f);
}

static void WriteValue(FILE *f, const Store *store, const StoreValue *v)
{
    switch (v->type)
    {
    case STORE_INT:
        fprintf(f, "%" PRId32, v->as.i);
        break;
    case STORE_FLOAT:
        WriteFloat(f, v->as.f);
        break;
    case STORE_BOOL:
        fputs(v->as.b ? "#t" : "#f", f);
        break;
    case STORE_SYMBOL:
    {
        const char *name = StoreSymbolName(store, v->as.sym);
        fputs(name ? name : "#f", f);
        break;
    }
    case STORE_STRING:
        WriteString(f, v->as.str);
        break;
    case STORE_VEC3:
        fputc('(', f);
        WriteFloat(f, v->as.v.x);
        fputc(' ', f);
        WriteFloat(f, v->as.v.y);
        fputc(' ', f);
        WriteFloat(f, v->as.v.z);
        fputc(')', f);
        break;
    case STORE_REF:
        if (v->as.ref.index == UINT32_MAX)
            fputs("#f", f);
        else
            fprintf(f, "#%" PRIu32 ":%" PRIu32, v->as.ref.index, v->as.ref.generation);
        break;
    default:
        fputs("#none", f);
        break;
    }
}

static void WriteField(FILE *f, const Store *store, StoreId id, int field, const StoreFieldDecl *d)
{
    StoreValue key, value;
    fprintf(f, "  %s ", d->name);
    if (d->type < STORE_LIST)
    {
        if (StoreGet(store, id, field, &value))
            WriteValue(f, store, &value);
        fputc('\n', f);
        return;
    }
    int count = StoreCountOf(store, id, field);
    fputc('(', f);
    for (int i = 0; i < count; i++)
    {
        if (!StoreGetAt(store, id, field, i, &key, &value))
            break;
        if (i)
            fputc(' ', f);
        if (d->type == STORE_MAP)
        {
            fputc('(', f);
            WriteValue(f, store, &key);
            fputc(' ', f);
            WriteValue(f, store, &value);
            fputc(')', f);
        }
        else
            WriteValue(f, store, &value);
    }
    fputs(")\n", f);
}

static void WriteThing(FILE *f, const Store *store, uint32_t index)
{
    const StoreThing *t = &store->things[index];
    const StoreKindData *k = store->kinds[t->kind];
    StoreId id = {index, t->generation};
    fprintf(f, "thing %" PRIu32 " gen %" PRIu32 " kind %s parent ", index, t->generation, k->name);
    if (t->parent == STORE_NO_INDEX)
        fputs("-", f);
    else
        fprintf(f, "%" PRIu32, t->parent);
    const char *name = StoreSymbolName(store, t->childName);
    fprintf(f, " name %s owner %d spawner %d rng %" PRIu64 "%s\n", name ? name : "-", t->owner,
            t->spawner, t->random, (t->flags & STORE_THING_GUEST) ? " guest" : "");
    for (int field = 0; field < k->fieldCount; field++)
        if (!(k->fields[field].flags & STORE_LOCAL))
            WriteField(f, store, id, field, &k->fields[field]);
    for (uint32_t c = t->firstChild; c != STORE_NO_INDEX; c = store->things[c].nextSibling)
        if ((store->things[c].flags & (STORE_THING_LIVE | STORE_THING_REMOVED | STORE_THING_LOCAL)) ==
            STORE_THING_LIVE)
            WriteThing(f, store, c);
}

bool StoreSave(const Store *store, const char *path)
{
    if (!store || !store->error)
        return false;
    if (!path || store->ticking || store->framing)
        return StoreFail(store, false, "save: needs a path, between ticks");
    CoreAtomicFile atomic;
    FILE *f = CoreAtomicBegin(&atomic, path);
    if (!f)
        return StoreFail(store, false, "save: can't write %s", path);
    fprintf(f, "store 1 tick %" PRIu64 " seed %" PRIu64 "\n", store->tickCount, store->worldRandom);
    for (uint32_t i = 0; i < store->thingCount; i++)
    {
        const StoreThing *t = &store->things[i];
        // Local things (and the declared children under them) are presentation: never saved.
        if ((t->flags & (STORE_THING_LIVE | STORE_THING_REMOVED | STORE_THING_LOCAL)) == STORE_THING_LIVE &&
            t->parent == STORE_NO_INDEX)
            WriteThing(f, store, i);
    }
    for (int i = 0; i < store->timerCount; i++)
    {
        const StoreTimer *timer = &store->timers[i];
        if (StoreLocalTimer(store, timer))
            continue;
        const char *event = StoreSymbolName(store, timer->event);
        fprintf(f, "timer %" PRIu32 " after %" PRIu64 " %s (", timer->target.index, timer->due,
                event ? event : "#f");
        for (int a = 0; a < timer->count; a++)
        {
            if (a)
                fputc(' ', f);
            WriteValue(f, store, &timer->args[a]);
        }
        fputs(")\n", f);
    }
    bool ok = !ferror(f);
    if (!CoreAtomicCommit(&atomic, ok))
        return StoreFail(store, false, "save: writing %s failed; the previous file is intact", path);
    return true;
}

// ---- reading ----------------------------------------------------------------------------------
typedef struct Reader
{
    const char *p;
} Reader;

static void SkipSpace(Reader *r)
{
    while (*r->p == ' ' || *r->p == '\t')
        r->p++;
}

// A bare token: anything up to whitespace or a parenthesis.
static bool Atom(Reader *r, char *out, size_t size)
{
    SkipSpace(r);
    size_t n = 0;
    while (*r->p && *r->p != ' ' && *r->p != '\t' && *r->p != '(' && *r->p != ')')
    {
        if (n + 1 >= size)
            return false;
        out[n++] = *r->p++;
    }
    out[n] = 0;
    return n > 0;
}

static bool Expect(Reader *r, char c)
{
    SkipSpace(r);
    if (*r->p != c)
        return false;
    r->p++;
    return true;
}

static bool ParseFloat(const char *text, float *out)
{
    char *end;
    double d = strtod(text, &end);
    if (end == text || *end)
        return false;
    *out = (float)d;
    return true;
}

/* One value. With want STORE_NONE (timer arguments) the text says the type; otherwise it is read
   as want, converting INT<->FLOAT as StoreSet does. */
static bool ParseValue(Store *store, Reader *r, StoreType want, StoreValue *out)
{
    char atom[NAME_MAX_TEXT];
    memset(out, 0, sizeof *out);
    SkipSpace(r);
    if (*r->p == '"')
    {
        size_t n = 0;
        for (r->p++; *r->p && *r->p != '"'; r->p++)
        {
            char c = *r->p;
            if (c == '\\' && r->p[1])
            {
                r->p++;
                c = *r->p == 'n' ? '\n' : *r->p;
            }
            if (n >= STORE_STRING_MAX)
                return false;
            out->as.str[n++] = c;
        }
        if (*r->p != '"')
            return false;
        r->p++;
        out->type = STORE_STRING;
    }
    else if (*r->p == '(')
    {
        r->p++;
        float v[3];
        for (int i = 0; i < 3; i++)
            if (!Atom(r, atom, sizeof atom) || !ParseFloat(atom, &v[i]))
                return false;
        if (!Expect(r, ')'))
            return false;
        out->type = STORE_VEC3;
        out->as.v = (Vector3){v[0], v[1], v[2]};
    }
    else if (!Atom(r, atom, sizeof atom))
        return false;
    else if (!strcmp(atom, "#f") && (want == STORE_REF || want == STORE_SYMBOL))
    {
        out->type = want;
        if (want == STORE_REF)
            out->as.ref = STORE_NULL;
        else
            out->as.sym = STORE_NO_SYMBOL;
    }
    else if (!strcmp(atom, "#t") || !strcmp(atom, "#f"))
    {
        out->type = STORE_BOOL;
        out->as.b = atom[1] == 't';
    }
    else if (!strcmp(atom, "#none"))
        out->type = STORE_NONE;
    else if (atom[0] == '#')
    {
        unsigned long index, generation;
        char *end;
        index = strtoul(atom + 1, &end, 10);
        if (end == atom + 1 || *end != ':')
            return false;
        const char *g = end + 1;
        generation = strtoul(g, &end, 10);
        if (end == g || *end || index >= UINT32_MAX || generation > UINT32_MAX)
            return false;
        out->type = STORE_REF;
        out->as.ref = (StoreId){(uint32_t)index, (uint32_t)generation};
    }
    else if (want == STORE_SYMBOL)
    {
        out->type = STORE_SYMBOL;
        out->as.sym = StoreIntern(store, atom);
    }
    else
    {
        char *end;
        long i = strtol(atom, &end, 10);
        if (end != atom && !*end && i >= INT32_MIN && i <= INT32_MAX)
        {
            out->type = STORE_INT;
            out->as.i = (int32_t)i;
        }
        else if (ParseFloat(atom, &out->as.f))
            out->type = STORE_FLOAT;
        else
        {
            out->type = STORE_SYMBOL;
            out->as.sym = StoreIntern(store, atom);
        }
    }
    if (want == STORE_NONE || out->type == want)
        return true;
    if (want == STORE_FLOAT && out->type == STORE_INT)
    {
        float f = (float)out->as.i;
        out->type = STORE_FLOAT;
        out->as.f = f;
        return true;
    }
    return want == STORE_INT && out->type == STORE_FLOAT; // StoreSet decides whether it is whole
}

typedef struct Loader
{
    Store *store;
    int line;
    char (*warned)[2 * NAME_MAX_TEXT];
    int warnedCount, warnedCapacity;
    StoreValue *items;
    int itemCapacity;
} Loader;

static void Warn(Loader *l, const char *kind, const char *field, const char *why)
{
    char key[2 * NAME_MAX_TEXT];
    snprintf(key, sizeof key, "%s %s", kind, field);
    for (int i = 0; i < l->warnedCount; i++)
        if (!strcmp(l->warned[i], key))
            return;
    if (l->warnedCount == l->warnedCapacity)
    {
        int cap = l->warnedCapacity ? l->warnedCapacity * 2 : 8;
        void *grown = realloc(l->warned, (size_t)cap * sizeof *l->warned);
        if (grown)
        {
            l->warned = grown;
            l->warnedCapacity = cap;
        }
    }
    if (l->warnedCount < l->warnedCapacity)
        memcpy(l->warned[l->warnedCount++], key, sizeof key);
    TraceLog(LOG_WARNING, "STORE: loading %s: %s (line %d)", key, why, l->line);
}

static bool PushItem(Loader *l, int count, const StoreValue *v)
{
    if (count >= l->itemCapacity)
    {
        int cap = l->itemCapacity ? l->itemCapacity * 2 : 16;
        void *grown = realloc(l->items, (size_t)cap * sizeof *l->items);
        if (!grown)
            return false;
        l->items = grown;
        l->itemCapacity = cap;
    }
    l->items[count] = *v;
    return true;
}

// One field line. A value that does not fit the field as declared now keeps the default.
static void LoadField(Loader *l, StoreId id, const char *text)
{
    Store *store = l->store;
    StoreKind kind = StoreKindOf(store, id);
    const char *kindName = StoreKindName(store, kind);
    Reader r = {text};
    char name[NAME_MAX_TEXT];
    if (!Atom(&r, name, sizeof name))
        return;
    int field = StoreFieldIndex(store, kind, name);
    const StoreFieldDecl *d = StoreFieldAt(store, kind, field);
    if (!d || (d->flags & STORE_LOCAL))
    {
        Warn(l, kindName, name, "the kind has no such shared field; skipped");
        return;
    }
    StoreValue key, value;
    bool ok = true;
    if (d->type < STORE_LIST)
        ok = ParseValue(store, &r, d->type, &value) && StoreSet(store, id, field, &value);
    else if (!Expect(&r, '('))
        ok = false;
    else if (d->type == STORE_MAP)
    {
        // Start from an empty map: the save lists every key.
        while (StoreCountOf(store, id, field) > 0 && StoreGetAt(store, id, field, 0, &key, NULL))
            StoreMapRemove(store, id, field, &key);
        while (ok && !Expect(&r, ')'))
            ok = Expect(&r, '(') && ParseValue(store, &r, d->key, &key) &&
                 ParseValue(store, &r, d->element, &value) && Expect(&r, ')') &&
                 StoreMapSet(store, id, field, &key, &value);
    }
    else
    {
        int count = 0;
        while (ok && !Expect(&r, ')'))
            ok = ParseValue(store, &r, d->element, &value) && PushItem(l, count++, &value);
        if (ok && d->type == STORE_GRID)
        {
            if (count != d->max * d->height)
                ok = false;
            for (int i = 0; ok && i < count; i++)
                ok = StoreGridSet(store, id, field, i % d->max, i / d->max, &l->items[i]);
        }
        else if (ok)
            ok = StoreSetList(store, id, field, l->items, count);
    }
    if (!ok)
        Warn(l, kindName, name, "the saved value does not fit the field; it keeps its default");
}

typedef struct Saved
{
    uint32_t index, generation, parent;
    StoreKind kind;
    char name[NAME_MAX_TEXT];
    int owner, spawner;
    uint64_t random;
    bool guest;
} Saved;

static bool ParseThing(Store *store, const char *line, Saved *s)
{
    char kind[NAME_MAX_TEXT], parent[32];
    int used = 0;
    memset(s, 0, sizeof *s);
    if (sscanf(line,
               "thing %" SCNu32 " gen %" SCNu32 " kind %255s parent %31s name %255s owner %d "
               "spawner %d rng %" SCNu64 "%n",
               &s->index, &s->generation, kind, parent, s->name, &s->owner, &s->spawner,
               &s->random, &used) < 8 ||
        !used || s->index == UINT32_MAX || !s->generation)
        return StoreFail(store, false, "load: a thing line is damaged");
    s->guest = !strncmp(line + used, " guest", 6);
    s->kind = StoreKindNamed(store, kind);
    if (s->kind < 0)
        return StoreFail(store, false, "load: the save has a kind %s this game does not declare",
                         kind);
    s->parent = STORE_NO_INDEX;
    if (strcmp(parent, "-"))
    {
        char *end;
        unsigned long p = strtoul(parent, &end, 10);
        if (*end || p >= UINT32_MAX)
            return StoreFail(store, false, "load: thing %" PRIu32 " has a damaged parent",
                             s->index);
        s->parent = (uint32_t)p;
    }
    return true;
}

static bool LoadTimer(Loader *l, const char *line, uint64_t sequence)
{
    Store *store = l->store;
    char event[NAME_MAX_TEXT];
    uint32_t index;
    uint64_t due;
    int used = 0;
    if (sscanf(line, "timer %" SCNu32 " after %" SCNu64 " %255s%n", &index, &due, event, &used) < 3 ||
        !used)
        return StoreFail(store, false, "load: line %d: a timer line is damaged", l->line);
    StoreTimer timer;
    memset(&timer, 0, sizeof timer);
    Reader r = {line + used};
    if (!Expect(&r, '('))
        return StoreFail(store, false, "load: line %d: a timer has no argument list", l->line);
    while (!Expect(&r, ')'))
    {
        if (timer.count == STORE_MAX_ARGS ||
            !ParseValue(store, &r, STORE_NONE, &timer.args[timer.count++]))
            return StoreFail(store, false, "load: line %d: a timer argument is damaged", l->line);
    }
    StoreId target = {index, index < store->thingCount ? store->things[index].generation : 0};
    if (!StoreAlive(store, target))
    {
        TraceLog(LOG_WARNING, "STORE: loading: timer %s for missing thing #%" PRIu32 " skipped",
                 event, index);
        return true;
    }
    timer.due = due;
    timer.sequence = sequence;
    timer.target = target;
    timer.event = StoreIntern(store, event);
    return StoreInsertTimer(store, &timer) || StoreFail(store, false, "load: out of memory");
}

bool StoreLoad(Store *store, const char *path)
{
    if (!store || !store->error)
        return false;
    if (!path || store->ticking || store->framing)
        return StoreFail(store, false, "load: needs a path, between ticks");
    char *text = CoreReadFile(path);
    if (!text)
        return StoreFail(store, false, "load: can't read %s", path);
    // Lines, in place.
    int lineCount = 1, thingLines = 1;
    for (const char *c = text; *c; c++)
        if (*c == '\n')
        {
            lineCount++;
            thingLines += !strncmp(c + 1, "thing ", 6);
        }
    char **lines = malloc((size_t)lineCount * sizeof *lines);
    Saved *saved = malloc((size_t)thingLines * sizeof *saved);
    Loader l = {store, 0, NULL, 0, 0, NULL, 0};
    StorePhase phase = store->phase;
    bool ok = lines && saved;
    int n = 0, savedCount = 0;
    if (ok)
    {
        for (char *c = text; c;)
        {
            lines[n++] = c;
            c = strchr(c, '\n');
            if (c)
                *c++ = 0;
        }
        for (int i = 0; i < n; i++)
        {
            size_t length = strlen(lines[i]);
            if (length && lines[i][length - 1] == '\r')
                lines[i][length - 1] = 0;
        }
    }
    else
        StoreFail(store, false, "load: out of memory");
    uint64_t tick = 0, seed = 0;
    int version = 0;
    if (ok && (sscanf(lines[0], "store %d tick %" SCNu64 " seed %" SCNu64, &version, &tick, &seed) != 3 ||
               version != 1))
        ok = StoreFail(store, false, "load: %s is not a version 1 store save", path);
    // Every thing line is checked, and every kind found, before the world is touched.
    uint32_t slots = 0, generation = 0;
    for (int i = 1; ok && i < n; i++)
    {
        if (strncmp(lines[i], "thing ", 6))
        {
            if (lines[i][0] && lines[i][0] != ' ' && lines[i][0] != '\t' &&
                strncmp(lines[i], "timer ", 6))
                ok = StoreFail(store, false, "load: line %d of %s is not a thing, field or timer",
                               i + 1, path);
            continue;
        }
        Saved *s = &saved[savedCount];
        ok = ParseThing(store, lines[i], s);
        for (int j = 0; ok && j < savedCount; j++)
            if (saved[j].index == s->index)
                ok = StoreFail(store, false, "load: thing %" PRIu32 " is saved twice", s->index);
        if (!ok)
            break;
        savedCount++;
        if (s->index + 1 > slots)
            slots = s->index + 1;
        if (s->generation > generation)
            generation = s->generation;
    }
    if (ok)
    {
        store->phase = STORE_PHASE_NONE;
        StoreClearWorld(store);
        // Free slots take a generation no saved handle can carry.
        ok = StoreReserveSlots(store, slots, generation + 1 ? generation + 1 : 1) ||
             StoreFail(store, false, "load: out of memory");
        for (int i = 0; ok && i < savedCount; i++)
        {
            const Saved *s = &saved[i];
            ok = StorePlaceThing(store, (StoreId){s->index, s->generation}, s->kind, s->owner,
                                 s->spawner, s->random) ||
                 StoreFail(store, false, "load: out of memory");
        }
        store->firstFree = 0; // the next spawn looks up from here for the lowest free slot
        for (int i = 0; ok && i < savedCount; i++)
        {
            const Saved *s = &saved[i];
            if (s->parent == STORE_NO_INDEX)
                continue;
            bool cycle = false;
            for (uint32_t a = s->parent, steps = 0; a != STORE_NO_INDEX && a < store->thingCount &&
                                                    steps <= store->thingCount;
                 a = store->things[a].parent, steps++)
                cycle |= a == s->index;
            if (s->parent >= store->thingCount || !(store->things[s->parent].flags & STORE_THING_LIVE) ||
                cycle)
            {
                TraceLog(LOG_WARNING, "STORE: loading: thing %" PRIu32 " names a missing parent; "
                                      "it becomes a root", s->index);
                continue;
            }
            StoreSymbol name = strcmp(s->name, "-") ? StoreIntern(store, s->name) : STORE_NO_SYMBOL;
            StoreLinkChild(store, s->index, s->parent, name, s->guest);
        }
    }
    // Fields and timers, in file order.
    StoreId current = STORE_NULL;
    uint64_t sequence = 0;
    for (int i = 1; ok && i < n; i++)
    {
        l.line = i + 1;
        if (!strncmp(lines[i], "thing ", 6))
        {
            uint32_t index = (uint32_t)strtoul(lines[i] + 6, NULL, 10);
            current = (StoreId){index, store->things[index].generation};
        }
        else if (!strncmp(lines[i], "timer ", 6))
            ok = LoadTimer(&l, lines[i], sequence++);
        else if ((lines[i][0] == ' ' || lines[i][0] == '\t') && StoreAlive(store, current))
            LoadField(&l, current, lines[i]);
    }
    if (ok)
    {
        StoreSetClock(store, tick, seed);
        for (uint32_t i = 0; store->hooks.spawned && i < store->thingCount; i++)
            if (store->things[i].flags & STORE_THING_LIVE)
                store->hooks.spawned(store->hooks.user, (StoreId){i, store->things[i].generation});
    }
    store->phase = phase;
    free(l.warned);
    free(l.items);
    free(saved);
    free(lines);
    CoreFreeFile(text);
    return ok;
}
