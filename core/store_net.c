/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "store_net.h"

#include "store_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Networking on the store, docs/developer/store.md §9. Every few ticks a machine captures what it
   holds (each thing's network id, header and shared block) into a ring. To each peer it sends the
   latest capture as a delta against the capture that peer last acknowledged, as Quake 3 does [N16];
   the receiver rebuilds the sender's full state from its copy of that baseline, keeps it, and each
   tick writes the sender's things as they stood 100 ms behind the newest (renderTick): the
   difference from the last state written, with registered fields blended towards the next. Per
   peer the sender remembers which things each sent state held, and whether the peer's copy of each
   is current or was left behind because the peer owns it; a thing leaving a peer's view is sent as
   leaving (the peer drops it from what this machine vouches for, without removing it), and a thing
   coming back into it is sent whole.

   Wire format, little-endian, every packet starting with its type byte:
     hello    protocol u32, game text, kinds hash u64, kind count u16, (name text, hash u64)...
     welcome  player u8, host tick u32, players u32
     refused  why (u16 length + bytes)
     state    tick u32, ack u32, baseline tick u32 (0: full), players u32 (host only),
              message count u16, messages..., entry count u32, entries... in network id order
     effect   name text, what text, x y z f32
   A text is a u8 length and its bytes. An entry is a network id u32 and an op u8: spawn (kind u16,
   header, spawner u8, every shared field), update (header flag u8 [header], field count u8,
   (field u8, value)...), remove, left (no longer sent to this peer: not a removal). A header is the parent's network id u32, the child name text, a
   guest u8 and the root owner u8. Values follow §9.1. A message is its target's network id u32,
   the event text, a bounced u8, an argument count u8 and each argument as a type u8 and a value. */

#define RING 32                        /* captures, frames and rebuilt states kept */
#define SEND_EVERY 3                   /* a state every third tick: 20 Hz */
#define DELAY_TICKS 6                  /* other machines' things are shown 100 ms behind */
#define LARGE_STATE 1200               /* a state packet over this goes reliable (B3.2) */
#define PEERS (STORE_NET_PLAYERS + 1)  /* transport peers 0 to 16 */
#define TEXT_MAX 255
#define FNV_OFFSET 0xCBF29CE484222325ull

enum { PACKET_HELLO = 1, PACKET_WELCOME, PACKET_REFUSED, PACKET_STATE, PACKET_EFFECT };
enum { ENTRY_SPAWN, ENTRY_UPDATE, ENTRY_REMOVE, ENTRY_LEFT };

typedef struct Entry
{
    uint32_t netId, parent; /* the parent's network id; 0 for a root */
    StoreSymbol name;       /* child name */
    int32_t kind;
    uint8_t guest, owner, spawner;
    uint8_t left;   /* rebuilt states: its sender stopped sending it, so this is not its word now */
    uint32_t block; /* offset of its shared block in the state's bytes */
} Entry;

/* A capture of this machine, or a state rebuilt from a peer's packets. A capture's blocks are the
   store's own bytes; a rebuilt state's blocks hold each REF as {network id, 0}. */
typedef struct State
{
    uint32_t tick; /* 0: unused */
    uint32_t ack;  /* a received state: the newest of our captures its sender had rebuilt */
    Entry *entries; /* in network id order */
    int count, capacity;
    unsigned char *bytes;
    size_t used, size;
} State;

/* What one peer was sent at a tick, per entry of that tick's capture: 0 not in its view, 1 in it
   and current, 2 told it left the view (the peer owns it; the peer keeps it, marked left). */
typedef struct Frame
{
    uint32_t tick;
    unsigned char *view;
    int capacity;
} Frame;

typedef struct Message
{
    int peer;      /* where it goes, or where it came from */
    uint32_t tick; /* received: the sender tick of the state it came with */
    uint32_t target;
    StoreSymbol event;
    uint8_t bounced;
    int count;
    StoreValue args[STORE_MAX_ARGS]; /* a REF holds a network id in as.ref.index */
} Message;

typedef struct Peer
{
    bool accepted;
    uint32_t acked;   /* newest of our states it has rebuilt */
    uint32_t newest;  /* newest of its states we have rebuilt */
    uint64_t arrival; /* our tick count when that one arrived */
    uint32_t seen;    /* newest of its states looked at on arrival for things becoming ours */
    uint32_t applied; /* the state last written (the earlier one at renderTick) */
    bool arrived;     /* a state was rebuilt since StoreNetBeforeTick last looked */
    int64_t render;   /* renderTick last used; it never goes back */
    unsigned frameNext;
    Frame frames[RING];
    State states[RING];
} Peer;

typedef struct Lerp
{
    StoreKind kind;
    int field;
} Lerp;

/* A thing this machine stopped owning by its own write, with the first capture that shows it. */
typedef struct Release
{
    uint32_t netId, tick;
} Release;

typedef struct Pair
{
    uint32_t netId;
    StoreId id;
} Pair;

/* An update entry already written this send, at its offset in StoreNetData.encoded; size -1: not
   written yet, 0: nothing changed. */
typedef struct Encoded
{
    uint32_t at;
    int32_t size;
} Encoded;

typedef struct Action
{
    const Entry *was, *now;
    bool fresh;
} Action;

/* Bytes being written, or with hashing set, fed to 64-bit FNV-1a instead. */
typedef struct Buf
{
    unsigned char *data;
    size_t size, capacity;
    bool hashing, failed;
    uint64_t hash;
} Buf;

typedef struct Reader
{
    const unsigned char *p, *end;
    bool bad;
} Reader;

struct StoreNetData
{
    uint32_t machine, counter;
    uint32_t *keys; /* network id -> thing, open addressing; ids are never reused, so never removed */
    StoreId *ids;
    uint32_t tableSize, tableUsed;
    uint32_t *slotGeneration, *slotNet; /* thing index -> the network id of the thing mapped there */
    uint32_t slots;
    State captures[RING];
    unsigned captureNext;
    Peer peers[PEERS];
    Message *out, *in;
    int outCount, outCapacity, inCount, inCapacity;
    Lerp *lerps;
    int lerpCount, lerpCapacity;
    Buf packet;
    unsigned char *scratch;
    int scratchCapacity;
    Action *actions;
    int actionCapacity;
    StoreId *created;
    int createdCapacity;
    const Entry **taken; /* scratch: entries being written, with whether each was created */
    int takenCapacity;
    bool *fresh;
    int freshCapacity;
    Release *releases;
    int releaseCount, releaseCapacity;
    uint32_t *owned; /* thing index -> its generation + 1 while owned here, as last looked */
    int ownedCapacity;
    /* Update entries of this send's capture against one baseline, per capture entry: every peer
       with that baseline is sent the same bytes for a thing it keeps seeing (SendState). */
    Buf encoded;
    Encoded *encodedAt;
    int encodedCapacity;
    uint64_t sends, encodedSend; /* StoreNetAfterTick calls that sent; the one encoded belongs to */
    uint32_t encodedBase;
    bool moved; /* this StoreNetBeforeTick created, linked, moved or removed a thing */
};

// ---- small helpers ----------------------------------------------------------------------------
static void *Grow(void *items, int *capacity, int need, size_t size)
{
    if (need <= *capacity)
        return items;
    int cap = *capacity ? *capacity : 16;
    while (cap < need)
        cap *= 2;
    void *grown = realloc(items, (size_t)cap * size);
    if (grown)
        *capacity = cap;
    return grown;
}

static void PutSlow(Buf *b, const void *data, size_t n)
{
    const unsigned char *c = data;
    if (b->hashing)
    {
        for (size_t i = 0; i < n; i++)
            b->hash = (b->hash ^ c[i]) * 0x100000001B3ull;
        return;
    }
    if (!n || b->failed)
        return;
    if (b->size + n > b->capacity)
    {
        size_t cap = b->capacity ? b->capacity : 1024;
        while (cap < b->size + n)
            cap *= 2;
        unsigned char *grown = realloc(b->data, cap);
        if (!grown)
        {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->capacity = cap;
    }
    memcpy(b->data + b->size, c, n);
    b->size += n;
}

// Most writes fit what the buffer already holds: those take no call.
static inline void Put(Buf *b, const void *data, size_t n)
{
    if (!b->hashing && !b->failed && b->size + n <= b->capacity)
    {
        memcpy(b->data + b->size, data, n);
        b->size += n;
    }
    else
        PutSlow(b, data, n);
}

static void PutU8(Buf *b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    Put(b, &c, 1);
}

static void PutU16(Buf *b, unsigned v)
{
    unsigned char c[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    Put(b, c, 2);
}

static inline void PutU32(Buf *b, uint32_t v)
{
    unsigned char c[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16),
                          (unsigned char)(v >> 24)};
    Put(b, c, 4);
}

static void PutU64(Buf *b, uint64_t v)
{
    PutU32(b, (uint32_t)v);
    PutU32(b, (uint32_t)(v >> 32));
}

static void PutF32(Buf *b, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    PutU32(b, v);
}

static void PutText(Buf *b, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    if (n > TEXT_MAX)
        n = TEXT_MAX;
    PutU8(b, (unsigned)n);
    Put(b, text, n);
}

static bool Take(Reader *r, void *out, size_t n)
{
    if (r->bad || (size_t)(r->end - r->p) < n)
    {
        r->bad = true;
        memset(out, 0, n);
        return false;
    }
    memcpy(out, r->p, n);
    r->p += n;
    return true;
}

static unsigned GetU8(Reader *r)
{
    unsigned char c;
    Take(r, &c, 1);
    return c;
}

static unsigned GetU16(Reader *r)
{
    unsigned char c[2];
    Take(r, c, 2);
    return c[0] | (unsigned)c[1] << 8;
}

static uint32_t GetU32(Reader *r)
{
    unsigned char c[4];
    Take(r, c, 4);
    return c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
}

static uint64_t GetU64(Reader *r)
{
    uint64_t low = GetU32(r);
    return low | (uint64_t)GetU32(r) << 32;
}

static float GetF32(Reader *r)
{
    uint32_t v = GetU32(r);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void GetText(Reader *r, char out[TEXT_MAX + 1])
{
    unsigned n = GetU8(r);
    Take(r, out, n);
    out[r->bad ? 0 : n] = 0;
}

static void Send(StoreNet *net, int peer, int channel, const Buf *b)
{
    if (!b->failed)
        net->config.send(net->config.user, peer, channel, channel == 0, b->data, b->size);
}

static Buf *StartPacket(StoreNet *net, unsigned type)
{
    Buf *b = &net->data->packet;
    b->size = 0;
    b->failed = false;
    PutU8(b, type);
    return b;
}

static int ElementSize(StoreType type)
{
    switch (type)
    {
    case STORE_VEC3:
        return 12;
    case STORE_REF:
        return 8;
    case STORE_STRING:
        return STORE_STRING_MAX + 1;
    default:
        return 4;
    }
}

static const StoreKindData *KindData(const Store *store, StoreKind kind) { return store->kinds[kind]; }

static size_t BlockSize(const Store *store, StoreKind kind)
{
    return (size_t)KindData(store, kind)->sharedSize;
}

static bool Shared(const StoreKindData *k, int field) { return !(k->fields[field].flags & STORE_LOCAL); }

// ---- network ids ------------------------------------------------------------------------------
static uint32_t Mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

static StoreId *Lookup(const StoreNetData *d, uint32_t netId)
{
    if (!d->tableSize || !netId)
        return NULL;
    uint32_t mask = d->tableSize - 1;
    for (uint32_t i = Mix(netId) & mask; d->keys[i]; i = (i + 1) & mask)
        if (d->keys[i] == netId)
            return &d->ids[i];
    return NULL;
}

static StoreId LocalOf(const StoreNet *net, uint32_t netId)
{
    const StoreId *id = Lookup(net->data, netId);
    return id ? *id : STORE_NULL;
}

// The network id of the thing a handle names, while that thing is the one mapped in its slot.
static uint32_t NetIdOf(const StoreNet *net, StoreId id)
{
    const StoreNetData *d = net->data;
    return id.index < d->slots && d->slotGeneration[id.index] == id.generation ? d->slotNet[id.index]
                                                                               : 0;
}

static bool Bind(StoreNet *net, uint32_t netId, StoreId id)
{
    StoreNetData *d = net->data;
    if ((d->tableUsed + 1) * 2 > d->tableSize)
    {
        uint32_t size = d->tableSize ? d->tableSize * 2 : 256;
        uint32_t *keys = calloc(size, sizeof *keys);
        StoreId *ids = calloc(size, sizeof *ids);
        if (!keys || !ids)
        {
            free(keys);
            free(ids);
            return false;
        }
        for (uint32_t i = 0; i < d->tableSize; i++)
        {
            if (!d->keys[i])
                continue;
            uint32_t j = Mix(d->keys[i]) & (size - 1);
            while (keys[j])
                j = (j + 1) & (size - 1);
            keys[j] = d->keys[i];
            ids[j] = d->ids[i];
        }
        free(d->keys);
        free(d->ids);
        d->keys = keys;
        d->ids = ids;
        d->tableSize = size;
    }
    if (id.index >= d->slots)
    {
        uint32_t slots = d->slots ? d->slots : 256;
        while (slots <= id.index)
            slots *= 2;
        uint32_t *generations = realloc(d->slotGeneration, slots * sizeof *generations);
        if (generations)
            d->slotGeneration = generations;
        uint32_t *nets = generations ? realloc(d->slotNet, slots * sizeof *nets) : NULL;
        if (!nets)
            return false;
        d->slotNet = nets;
        memset(generations + d->slots, 0, (slots - d->slots) * sizeof *generations);
        memset(nets + d->slots, 0, (slots - d->slots) * sizeof *nets);
        d->slots = slots;
    }
    StoreId *existing = Lookup(d, netId);
    if (existing)
        *existing = id;
    else
    {
        uint32_t mask = d->tableSize - 1, i = Mix(netId) & mask;
        while (d->keys[i])
            i = (i + 1) & mask;
        d->keys[i] = netId;
        d->ids[i] = id;
        d->tableUsed++;
    }
    d->slotGeneration[id.index] = id.generation;
    d->slotNet[id.index] = netId;
    return true;
}

static bool Replicated(const Store *store, StoreId id)
{
    return StoreAlive(store, id) && !StoreIsLocal(store, id);
}

// Replicated for the thing in a slot, read from the table: live, not removed, not local.
static bool LiveShared(const StoreThing *t)
{
    return (t->flags & (STORE_THING_LIVE | STORE_THING_REMOVED | STORE_THING_LOCAL)) == STORE_THING_LIVE;
}

// A thing gets its network id when this machine first sees it: here, when it is first captured or
// named in a message. Local things never get one.
static uint32_t Assign(StoreNet *net, StoreId id)
{
    StoreNetData *d = net->data;
    uint32_t netId = NetIdOf(net, id);
    if (netId || !Replicated(net->store, id) || d->counter >= (1u << 27) - 1)
        return netId;
    netId = d->machine << 27 | ++d->counter;
    return Bind(net, netId, id) ? netId : 0;
}

static void AssignAll(StoreNet *net)
{
    const Store *s = net->store;
    for (uint32_t i = 0; i < s->thingCount; i++)
        Assign(net, (StoreId){i, s->things[i].generation});
}

static int ComparePairs(const void *a, const void *b)
{
    uint32_t x = ((const Pair *)a)->netId, y = ((const Pair *)b)->netId;
    return (x > y) - (x < y);
}

// Every live replicated thing that has a network id, in network id order; the caller frees *out.
static int Collect(const StoreNet *net, Pair **out)
{
    const Store *s = net->store;
    Pair *pairs = malloc(((size_t)s->thingCount + 1) * sizeof *pairs);
    int n = 0;
    for (uint32_t i = 0; pairs && i < s->thingCount; i++)
    {
        StoreId id = {i, s->things[i].generation};
        uint32_t netId = NetIdOf(net, id);
        if (netId && Replicated(s, id))
            pairs[n++] = (Pair){netId, id};
    }
    if (pairs)
        qsort(pairs, (size_t)n, sizeof *pairs, ComparePairs);
    *out = pairs;
    return n;
}

// ---- values on the wire (§9.1) ----------------------------------------------------------------
// From a block of this store's own: a REF becomes its network id (0 for a removed thing).
static void PutElement(const StoreNet *net, Buf *b, StoreType type, const unsigned char *p)
{
    uint32_t v;
    switch (type)
    {
    case STORE_SYMBOL:
    {
        StoreSymbol symbol;
        memcpy(&symbol, p, 4);
        PutText(b, StoreSymbolName(net->store, symbol));
        return;
    }
    case STORE_STRING:
        PutText(b, (const char *)p);
        return;
    case STORE_REF:
    {
        StoreId id;
        memcpy(&id, p, 8);
        PutU32(b, StoreAlive(net->store, id) ? NetIdOf(net, id) : 0);
        return;
    }
    case STORE_VEC3:
        for (int i = 0; i < 3; i++)
        {
            memcpy(&v, p + 4 * i, 4);
            PutU32(b, v);
        }
        return;
    default:
        memcpy(&v, p, 4);
        PutU32(b, v);
        return;
    }
}

// Into a rebuilt state's block: a REF stays a network id, a symbol is interned here.
static void GetElement(StoreNet *net, Reader *r, StoreType type, unsigned char *p)
{
    char text[TEXT_MAX + 1];
    uint32_t v;
    switch (type)
    {
    case STORE_SYMBOL:
    {
        GetText(r, text);
        StoreSymbol symbol = text[0] ? StoreIntern(net->store, text) : STORE_NO_SYMBOL;
        memcpy(p, &symbol, 4);
        return;
    }
    case STORE_STRING:
        GetText(r, text);
        if (strlen(text) > STORE_STRING_MAX)
            r->bad = true;
        memset(p, 0, STORE_STRING_MAX + 1);
        memcpy(p, text, strlen(text) > STORE_STRING_MAX ? STORE_STRING_MAX : strlen(text));
        return;
    case STORE_REF:
    {
        uint32_t ref[2] = {GetU32(r), 0};
        memcpy(p, ref, 8);
        return;
    }
    case STORE_VEC3:
        for (int i = 0; i < 3; i++)
        {
            v = GetU32(r);
            memcpy(p + 4 * i, &v, 4);
        }
        return;
    case STORE_BOOL:
        v = GetU32(r) != 0;
        memcpy(p, &v, 4);
        return;
    default:
        v = GetU32(r);
        memcpy(p, &v, 4);
        return;
    }
}

static void PutField(const StoreNet *net, Buf *b, const StoreFieldDecl *d, const unsigned char *p)
{
    int es = ElementSize(d->element), ks = d->type == STORE_MAP ? ElementSize(d->key) : 0;
    int32_t count;
    switch (d->type)
    {
    case STORE_LIST:
    case STORE_SET:
    case STORE_MAP:
        memcpy(&count, p, 4);
        PutU32(b, (uint32_t)count);
        for (int i = 0; i < count; i++)
        {
            const unsigned char *e = p + 4 + i * (ks + es);
            if (ks)
                PutElement(net, b, d->key, e);
            PutElement(net, b, d->element, e + ks);
        }
        return;
    case STORE_GRID:
        for (int i = 0; i < d->max * d->height; i++)
            PutElement(net, b, d->element, p + i * es);
        return;
    default:
        PutElement(net, b, d->type, p);
    }
}

static void GetField(StoreNet *net, Reader *r, const StoreFieldDecl *d, unsigned char *p, int size)
{
    int es = ElementSize(d->element), ks = d->type == STORE_MAP ? ElementSize(d->key) : 0;
    memset(p, 0, (size_t)size);
    switch (d->type)
    {
    case STORE_LIST:
    case STORE_SET:
    case STORE_MAP:
    {
        uint32_t count = GetU32(r);
        if (count > (uint32_t)d->max)
        {
            r->bad = true;
            return;
        }
        int32_t n = (int32_t)count;
        memcpy(p, &n, 4);
        for (int i = 0; i < n; i++)
        {
            unsigned char *e = p + 4 + i * (ks + es);
            if (ks)
                GetElement(net, r, d->key, e);
            GetElement(net, r, d->element, e + ks);
        }
        return;
    }
    case STORE_GRID:
        for (int i = 0; i < d->max * d->height; i++)
            GetElement(net, r, d->element, p + i * es);
        return;
    default:
        GetElement(net, r, d->type, p);
    }
}

static void FixRef(const StoreNet *net, unsigned char *p)
{
    uint32_t netId;
    memcpy(&netId, p, 4);
    StoreId id = netId ? LocalOf(net, netId) : STORE_NULL;
    memcpy(p, &id, 8);
}

// A rebuilt field's REFs, from network ids to this store's handles.
static void RefsToLocal(const StoreNet *net, const StoreFieldDecl *d, unsigned char *p)
{
    if (d->type == STORE_REF)
        FixRef(net, p);
    if (d->type < STORE_LIST || (d->element != STORE_REF && d->key != STORE_REF))
        return;
    int es = ElementSize(d->element), ks = d->type == STORE_MAP ? ElementSize(d->key) : 0;
    int32_t n = d->max * d->height;
    unsigned char *first = p;
    if (d->type != STORE_GRID)
    {
        memcpy(&n, p, 4);
        first = p + 4;
    }
    for (int i = 0; i < n; i++)
    {
        unsigned char *e = first + i * (ks + es);
        if (ks && d->key == STORE_REF)
            FixRef(net, e);
        if (d->element == STORE_REF)
            FixRef(net, e + ks);
    }
}

static void ValueOf(StoreType type, const unsigned char *p, StoreValue *out)
{
    memset(out, 0, sizeof *out);
    out->type = type;
    switch (type)
    {
    case STORE_BOOL:
    {
        int32_t b;
        memcpy(&b, p, 4);
        out->as.b = b != 0;
        break;
    }
    case STORE_STRING:
        memcpy(out->as.str, p, STORE_STRING_MAX + 1);
        break;
    case STORE_VEC3:
    {
        float v[3];
        memcpy(v, p, 12);
        out->as.v = (Vector3){v[0], v[1], v[2]};
        break;
    }
    case STORE_REF:
        memcpy(&out->as.ref, p, 8);
        break;
    default:
        memcpy(&out->as.i, p, 4); // int, float and symbol are all four bytes
        break;
    }
}

// A message argument: its type, then its value; a REF carries a network id.
static void PutValue(const StoreNet *net, Buf *b, const StoreValue *v)
{
    PutU8(b, (unsigned)v->type);
    switch (v->type)
    {
    case STORE_NONE:
        return;
    case STORE_SYMBOL:
        PutText(b, StoreSymbolName(net->store, v->as.sym));
        return;
    case STORE_STRING:
        PutText(b, v->as.str);
        return;
    case STORE_REF:
        PutU32(b, v->as.ref.index);
        return;
    case STORE_VEC3:
        PutF32(b, v->as.v.x);
        PutF32(b, v->as.v.y);
        PutF32(b, v->as.v.z);
        return;
    case STORE_BOOL:
        PutU32(b, v->as.b ? 1u : 0u);
        return;
    case STORE_FLOAT:
        PutF32(b, v->as.f);
        return;
    default:
        PutU32(b, (uint32_t)v->as.i);
        return;
    }
}

static bool GetValue(StoreNet *net, Reader *r, StoreValue *v)
{
    char text[TEXT_MAX + 1];
    memset(v, 0, sizeof *v);
    v->type = (StoreType)GetU8(r);
    switch (v->type)
    {
    case STORE_NONE:
        break;
    case STORE_SYMBOL:
        GetText(r, text);
        v->as.sym = text[0] ? StoreIntern(net->store, text) : STORE_NO_SYMBOL;
        break;
    case STORE_STRING:
        GetText(r, text);
        if (strlen(text) > STORE_STRING_MAX)
            r->bad = true;
        memcpy(v->as.str, text, STORE_STRING_MAX);
        v->as.str[STORE_STRING_MAX] = 0;
        break;
    case STORE_REF:
        v->as.ref = (StoreId){GetU32(r), 0};
        break;
    case STORE_VEC3:
        v->as.v.x = GetF32(r);
        v->as.v.y = GetF32(r);
        v->as.v.z = GetF32(r);
        break;
    case STORE_BOOL:
        v->as.b = GetU32(r) != 0;
        break;
    case STORE_INT:
    case STORE_FLOAT:
        v->as.i = (int32_t)GetU32(r);
        break;
    default:
        r->bad = true;
    }
    return !r->bad;
}

static void PutMessage(const StoreNet *net, Buf *b, const Message *m)
{
    PutU32(b, m->target);
    PutText(b, StoreSymbolName(net->store, m->event));
    PutU8(b, m->bounced);
    PutU8(b, (unsigned)m->count);
    for (int i = 0; i < m->count; i++)
        PutValue(net, b, &m->args[i]);
}

static bool GetMessage(StoreNet *net, Reader *r, Message *m)
{
    char text[TEXT_MAX + 1];
    memset(m, 0, sizeof *m);
    m->target = GetU32(r);
    GetText(r, text);
    m->event = StoreIntern(net->store, text);
    m->bounced = (uint8_t)GetU8(r);
    m->count = (int)GetU8(r);
    if (m->count > STORE_MAX_ARGS || m->event == STORE_NO_SYMBOL)
        r->bad = true;
    for (int i = 0; i < m->count && !r->bad; i++)
        GetValue(net, r, &m->args[i]);
    return !r->bad;
}

static void Queue(StoreNet *net, bool outgoing, const Message *m)
{
    StoreNetData *d = net->data;
    Message **items = outgoing ? &d->out : &d->in;
    int *count = outgoing ? &d->outCount : &d->inCount;
    Message *grown = Grow(*items, outgoing ? &d->outCapacity : &d->inCapacity, *count + 1,
                          sizeof *grown);
    if (!grown)
        return;
    *items = grown;
    grown[(*count)++] = *m;
}

// ---- states -----------------------------------------------------------------------------------
static State *FindState(State *ring, uint32_t tick)
{
    for (int i = 0; tick && i < RING; i++)
        if (ring[i].tick == tick)
            return &ring[i];
    return NULL;
}

static const Entry *FindEntry(const State *s, uint32_t netId)
{
    int low = 0, high = s->count;
    while (low < high)
    {
        int mid = (low + high) / 2;
        if (s->entries[mid].netId == netId)
            return &s->entries[mid];
        if (s->entries[mid].netId < netId)
            low = mid + 1;
        else
            high = mid;
    }
    return NULL;
}

static void ClearState(State *s, uint32_t tick)
{
    s->tick = tick;
    s->ack = 0;
    s->count = 0;
    s->used = 0;
}

static Entry *AddEntry(State *s, size_t blockSize)
{
    Entry *entries = Grow(s->entries, &s->capacity, s->count + 1, sizeof *entries);
    if (!entries)
        return NULL;
    s->entries = entries;
    if (s->used + blockSize > s->size)
    {
        size_t size = s->size ? s->size : 4096;
        while (size < s->used + blockSize)
            size *= 2;
        unsigned char *bytes = realloc(s->bytes, size);
        if (!bytes)
            return NULL;
        s->bytes = bytes;
        s->size = size;
    }
    Entry *e = &entries[s->count++];
    memset(e, 0, sizeof *e);
    e->block = (uint32_t)s->used;
    s->used += blockSize;
    return e;
}

static bool CopyEntry(const Store *store, State *to, const State *from, const Entry *e)
{
    size_t size = BlockSize(store, e->kind);
    Entry *copy = AddEntry(to, size);
    if (!copy)
        return false;
    uint32_t block = copy->block;
    *copy = *e;
    copy->block = block;
    memcpy(to->bytes + block, from->bytes + e->block, size);
    return true;
}

static bool SameHeader(const Entry *a, const Entry *b)
{
    return a->parent == b->parent && a->name == b->name && a->guest == b->guest &&
           a->owner == b->owner;
}

static void PutHeader(const StoreNet *net, Buf *b, const Entry *e)
{
    PutU32(b, e->parent);
    PutText(b, StoreSymbolName(net->store, e->name));
    PutU8(b, e->guest);
    PutU8(b, e->owner);
}

static void GetHeader(StoreNet *net, Reader *r, Entry *e)
{
    char text[TEXT_MAX + 1];
    e->parent = GetU32(r);
    GetText(r, text);
    e->name = text[0] ? StoreIntern(net->store, text) : STORE_NO_SYMBOL;
    e->guest = GetU8(r) != 0;
    e->owner = (uint8_t)GetU8(r);
    if (e->owner > 63)
        r->bad = true;
}

static int CompareEntries(const void *a, const void *b)
{
    uint32_t x = ((const Entry *)a)->netId, y = ((const Entry *)b)->netId;
    return (x > y) - (x < y);
}

static bool Ignored(const StoreNet *net, uint32_t netId, const State *st);

/* The host relays a client's thing as it last received it, whole (header and every field from the
   newest state from that client), not the store's copy running 100 ms behind: the other clients see
   it 100 ms behind its owner and not 200 (B3.5), and still from one tick. A thing that state does
   not hold yet (just given to the client) goes as the store has it. */
static void RelayReceived(StoreNet *net, State *c, Entry *e)
{
    const Peer *peer = &net->data->peers[e->owner];
    const State *newest = NULL;
    for (int i = 0; i < RING && peer->newest; i++)
        if (peer->states[i].tick == peer->newest)
            newest = &peer->states[i];
    const Entry *received = newest ? FindEntry(newest, e->netId) : NULL;
    if (!received || received->left || received->kind != e->kind || Ignored(net, e->netId, newest))
        return; // not the client's word now, or made before it knew the thing was given to it
    e->parent = received->parent;
    e->name = received->name;
    e->guest = received->guest;
    e->owner = received->owner;
    e->spawner = received->spawner;
    const StoreKindData *k = KindData(net->store, e->kind);
    for (int f = 0; f < k->fieldCount; f++)
        if (Shared(k, f))
        {
            unsigned char *slot = c->bytes + e->block + k->offsets[f];
            memcpy(slot, newest->bytes + received->block + k->offsets[f], (size_t)k->sizes[f]);
            RefsToLocal(net, &k->fields[f], slot); // back to this store's own form
        }
}

/* In network id order. A capture is in slot order, which is network id order but for a few things
   (one given an id early, by a message, or put in a freed slot): an insertion sort puts those in
   place in a few moves, and hands over to qsort when there are many. */
static void SortEntries(Entry *entries, int count)
{
    long budget = 4L * count + 64;
    for (int i = 1; i < count; i++)
    {
        Entry e = entries[i];
        int j = i;
        for (; j > 0 && entries[j - 1].netId > e.netId && budget > 0; j--, budget--)
            entries[j] = entries[j - 1];
        entries[j] = e;
        if (budget <= 0)
        {
            qsort(entries, (size_t)count, sizeof *entries, CompareEntries);
            return;
        }
    }
}

// What this machine holds now (§9.2), taken at most once a tick.
static const State *Capture(StoreNet *net)
{
    StoreNetData *d = net->data;
    const Store *s = net->store;
    uint32_t tick = (uint32_t)StoreTickCount(s);
    State *c = FindState(d->captures, tick);
    if (c)
        return c;
    c = &d->captures[d->captureNext++ % RING];
    ClearState(c, tick);
    AssignAll(net);
    bool sorted = true; // things are mostly in network id order already; sort only when not
    for (uint32_t i = 0; i < s->thingCount; i++)
    {
        const StoreThing *t = &s->things[i];
        StoreId id = {i, t->generation};
        uint32_t netId = NetIdOf(net, id);
        if (!netId || !LiveShared(t))
            continue;
        const StoreKindData *k = KindData(s, t->kind);
        Entry *e = AddEntry(c, (size_t)k->sharedSize);
        if (!e)
        {
            c->tick = 0;
            return NULL;
        }
        sorted = sorted && (c->count < 2 || c->entries[c->count - 2].netId < netId);
        e->netId = netId;
        e->kind = t->kind;
        e->parent = t->parent == STORE_NO_INDEX ? 0 : NetIdOf(net, (StoreId){t->parent, s->things[t->parent].generation});
        e->guest = (t->flags & STORE_THING_GUEST) != 0;
        e->name = e->guest ? STORE_NO_SYMBOL : t->childName;
        e->owner = (uint8_t)t->owner;
        e->spawner = (uint8_t)t->spawner;
        if (k->sharedSize)
            memcpy(c->bytes + e->block, k->shared + (size_t)t->row * (size_t)k->sharedSize,
                   (size_t)k->sharedSize);
        if (net->host && e->owner >= 2 && e->owner < PEERS && d->peers[e->owner].accepted)
            RelayReceived(net, c, e);
    }
    if (!sorted)
        SortEntries(c->entries, c->count);
    return c;
}

// ---- sending states ---------------------------------------------------------------------------
static bool LocalOwner(const StoreNet *net, int owner)
{
    return net->host ? owner == 0 || (owner == 1 && !net->config.dedicated) : owner == net->player;
}

// The first capture of ours that shows this machine letting go of a thing (§9.2); 0 for none known.
static uint32_t ReleasedAt(const StoreNetData *d, uint32_t netId)
{
    for (int i = 0; i < d->releaseCount; i++)
        if (d->releases[i].netId == netId)
            return d->releases[i].tick;
    return 0;
}

/* Whether a thing goes to a peer (§9.2): the host sends what the peer does not own, a client what
   it owns; both keep sending a thing whose ownership moved until the peer has acknowledged a state
   showing the move. A client letting go of a thing so soon that the host never acknowledged a
   capture where it held it counts: the thing goes until the host has a capture showing it let go
   (else the host, rebuilding from a baseline without it, would read its absence as a removal).
   seen is the thing's view in the baseline, base its entry there. */
static bool Include(const StoreNet *net, int peer, const Entry *e, const Entry *base, int seen)
{
    if (net->host)
    {
        if (e->owner != peer)
            return true;
        if (seen == 1)
            return base->owner != peer;
        return seen == 0 && (e->netId >> 27) != (uint32_t)peer; // made elsewhere, given to it
    }
    if (e->owner == net->player)
        return true;
    uint32_t released = ReleasedAt(net->data, e->netId);
    if (released && net->data->peers[peer].acked < released)
        return true;
    if (seen == 1)
        return base->owner == net->player;
    return seen == 0 && (e->netId >> 27) == net->data->machine; // made here, given away at once
}

static bool HasMessages(const StoreNetData *d, int peer)
{
    for (int i = 0; i < d->outCount; i++)
        if (d->out[i].peer == peer)
            return true;
    return false;
}

static bool FieldDiffers(const StoreKindData *k, int f, const unsigned char *a, const unsigned char *b)
{
    return Shared(k, f) && memcmp(a + k->offsets[f], b + k->offsets[f], (size_t)k->sizes[f]) != 0;
}

static void SendState(StoreNet *net, int peerNumber, const State *c)
{
    StoreNetData *d = net->data;
    const Store *s = net->store;
    Peer *peer = &d->peers[peerNumber];
    Frame *frame = &peer->frames[peer->frameNext % RING];
    // The baseline: the capture the peer last rebuilt, while both it and what it was sent are kept.
    const State *base = FindState(d->captures, peer->acked);
    const Frame *seen = NULL;
    for (int i = 0; base && i < RING; i++)
        if (peer->frames[i].tick == peer->acked && &peer->frames[i] != frame)
            seen = &peer->frames[i];
    if (!seen)
        base = NULL;
    unsigned char *view = Grow(frame->view, &frame->capacity, c->count + 1, 1);
    if (!view)
        return;
    frame->view = view;
    frame->tick = 0;
    Buf *b = StartPacket(net, PACKET_STATE);
    PutU32(b, c->tick);
    PutU32(b, peer->newest);
    PutU32(b, base ? base->tick : 0);
    PutU32(b, net->host ? net->players : 0);
    // Messages first, so they are read even when the state's baseline is gone at the other end.
    int messages = 0, kept = 0;
    for (int i = 0; i < d->outCount; i++)
        messages += d->out[i].peer == peerNumber;
    PutU16(b, (unsigned)messages);
    for (int i = 0; i < d->outCount; i++)
        if (d->out[i].peer == peerNumber)
            PutMessage(net, b, &d->out[i]);
        else
            d->out[kept++] = d->out[i];
    d->outCount = kept;
    size_t countAt = b->size;
    uint32_t entries = 0;
    PutU32(b, 0);
    // Update entries against this baseline written for another peer this send are copied, not
    // compared and written again.
    Encoded *memo = NULL;
    if (base && (d->encodedSend != d->sends || d->encodedBase != base->tick))
    {
        Encoded *grown = Grow(d->encodedAt, &d->encodedCapacity, c->count + 1, sizeof *grown);
        d->encodedSend = 0;
        if (grown)
        {
            d->encodedAt = grown;
            for (int i = 0; i < c->count; i++)
                grown[i].size = -1;
            d->encoded.size = 0;
            d->encoded.failed = false;
            d->encodedSend = d->sends;
            d->encodedBase = base->tick;
        }
    }
    if (base && d->encodedSend == d->sends)
        memo = d->encodedAt;
    int j = 0;
    for (int i = 0; i <= c->count; i++)
    {
        const Entry *e = i < c->count ? &c->entries[i] : NULL;
        // Things the peer holds from us that are gone here.
        for (; base && j < base->count && (!e || base->entries[j].netId < e->netId); j++)
            if (seen->view[j])
            {
                PutU32(b, base->entries[j].netId);
                PutU8(b, ENTRY_REMOVE);
                entries++;
            }
        if (!e)
            break;
        const Entry *was =
            base && j < base->count && base->entries[j].netId == e->netId ? &base->entries[j] : NULL;
        int before = was ? seen->view[j++] : 0;
        if (!Include(net, peerNumber, e, was, before))
        {
            view[i] = before ? 2 : 0;
            if (before == 1) // it leaves the peer's view: the peer stops taking our word on it
            {
                PutU32(b, e->netId);
                PutU8(b, ENTRY_LEFT);
                entries++;
            }
            continue;
        }
        view[i] = 1;
        const StoreKindData *k = KindData(s, e->kind);
        const unsigned char *now = c->bytes + e->block;
        if (before != 1) // new to it, or left behind while it owned it: whole
        {
            PutU32(b, e->netId);
            PutU8(b, ENTRY_SPAWN);
            PutU16(b, (unsigned)e->kind);
            PutHeader(net, b, e);
            PutU8(b, e->spawner);
            for (int f = 0; f < k->fieldCount; f++)
                if (Shared(k, f))
                    PutField(net, b, &k->fields[f], now + k->offsets[f]);
            entries++;
            continue;
        }
        if (memo && memo[i].size >= 0)
        {
            Put(b, d->encoded.data + memo[i].at, (size_t)memo[i].size);
            entries += memo[i].size > 0;
            continue;
        }
        const unsigned char *then = base->bytes + was->block;
        bool header = !SameHeader(e, was);
        unsigned char changed[256];
        int n = 0;
        if (header || memcmp(now, then, (size_t)k->sharedSize))
            for (int f = 0; f < k->fieldCount; f++)
                if (FieldDiffers(k, f, now, then))
                    changed[n++] = (unsigned char)f;
        size_t start = b->size;
        if (header || n)
        {
            PutU32(b, e->netId);
            PutU8(b, ENTRY_UPDATE);
            PutU8(b, header);
            if (header)
                PutHeader(net, b, e);
            PutU8(b, (unsigned)n);
            for (int f = 0; f < n; f++)
            {
                PutU8(b, changed[f]);
                PutField(net, b, &k->fields[changed[f]], now + k->offsets[changed[f]]);
            }
            entries++;
        }
        if (memo && !b->failed)
        {
            memo[i] = (Encoded){(uint32_t)d->encoded.size, (int32_t)(b->size - start)};
            Put(&d->encoded, b->data + start, b->size - start);
            if (d->encoded.failed)
                memo[i].size = -1;
        }
    }
    if (b->failed)
        return;
    for (int i = 0; i < 4; i++)
        b->data[countAt + (size_t)i] = (unsigned char)(entries >> (8 * i));
    frame->tick = c->tick;
    peer->frameNext++;
    Send(net, peerNumber, messages || b->size > LARGE_STATE ? 0 : 1, b); // large: reliable (B3.2)
}

// ---- receiving states -------------------------------------------------------------------------
// The sender's state at a tick: the named baseline with the packet's entries applied.
static bool Rebuild(StoreNet *net, Reader *r, const State *base, State *st)
{
    const Store *s = net->store;
    uint32_t count = GetU32(r), last = 0;
    int j = 0;
    for (uint32_t i = 0; i < count && !r->bad; i++)
    {
        uint32_t netId = GetU32(r);
        unsigned op = GetU8(r);
        if (r->bad || netId <= last)
            return false;
        last = netId;
        for (; base && j < base->count && base->entries[j].netId < netId; j++)
            if (!CopyEntry(s, st, base, &base->entries[j]))
                return false;
        const Entry *was =
            base && j < base->count && base->entries[j].netId == netId ? &base->entries[j++] : NULL;
        Entry *e;
        const StoreKindData *k;
        if (op == ENTRY_REMOVE)
            continue;
        if (op == ENTRY_LEFT) // kept, so a later removal is still one, but no longer the sender's word
        {
            if (!was || !CopyEntry(s, st, base, was))
                return false;
            st->entries[st->count - 1].left = 1;
            continue;
        }
        if (op == ENTRY_SPAWN)
        {
            StoreKind kind = (StoreKind)GetU16(r);
            if (r->bad || kind >= s->kindCount || !(e = AddEntry(st, BlockSize(s, kind))))
                return false;
            e->netId = netId;
            e->kind = kind;
            GetHeader(net, r, e);
            e->spawner = (uint8_t)GetU8(r);
            k = KindData(s, kind);
            for (int f = 0; f < k->fieldCount; f++)
                if (Shared(k, f))
                    GetField(net, r, &k->fields[f], st->bytes + e->block + k->offsets[f], k->sizes[f]);
            continue;
        }
        if (op != ENTRY_UPDATE || !was || was->left || !CopyEntry(s, st, base, was))
            return false;
        e = &st->entries[st->count - 1];
        if (GetU8(r))
            GetHeader(net, r, e);
        k = KindData(s, e->kind);
        unsigned n = GetU8(r);
        for (unsigned f = 0; f < n && !r->bad; f++)
        {
            unsigned field = GetU8(r);
            if ((int)field >= k->fieldCount || !Shared(k, (int)field))
                return false;
            GetField(net, r, &k->fields[field], st->bytes + e->block + k->offsets[field],
                     k->sizes[field]);
        }
    }
    for (; base && j < base->count; j++)
        if (!CopyEntry(s, st, base, &base->entries[j]))
            return false;
    return !r->bad;
}

static void ReceiveState(StoreNet *net, int from, Reader *r)
{
    StoreNetData *d = net->data;
    Peer *peer = &d->peers[from];
    uint32_t tick = GetU32(r), ack = GetU32(r), baseTick = GetU32(r), players = GetU32(r);
    if (r->bad || !tick)
        return;
    if (ack > peer->acked)
        peer->acked = ack;
    if (!net->host && players)
        net->players = players;
    unsigned messages = GetU16(r);
    for (unsigned i = 0; i < messages; i++)
    {
        Message m;
        if (!GetMessage(net, r, &m))
            return;
        m.peer = from;
        m.tick = tick; // delivered when renderTick reaches it (§9.3)
        Queue(net, false, &m);
    }
    if (FindState(peer->states, tick))
        return; // a duplicate
    State *base = FindState(peer->states, baseTick);
    if (baseTick && !base)
        return; // its baseline is gone; our ack stays older, so a later state names one we keep
    State *st = NULL;
    for (int i = 0; i < RING; i++)
        if (&peer->states[i] != base && (!st || peer->states[i].tick < st->tick))
            st = &peer->states[i];
    ClearState(st, 0);
    if (!Rebuild(net, r, base, st))
    {
        ClearState(st, 0);
        return;
    }
    st->tick = tick;
    st->ack = ack;
    peer->arrived = true;
    if (tick > peer->newest)
    {
        peer->newest = tick;
        peer->arrival = StoreTickCount(net->store);
    }
}

// ---- applying states --------------------------------------------------------------------------
// Whether a thing here takes what a peer says of it: on a client, when this machine does not own
// it; on the host, when the sending player does.
static bool Accept(const StoreNet *net, StoreId id, int from)
{
    int owner = StoreOwner(net->store, id);
    return net->host ? owner == from : owner != net->player;
}

static bool IsLerp(const StoreNet *net, StoreKind kind, int field)
{
    const StoreNetData *d = net->data;
    for (int i = 0; i < d->lerpCount; i++)
        if (d->lerps[i].field == field && StoreKindIs(net->store, kind, d->lerps[i].kind))
            return true;
    return false;
}

// Writes one rebuilt field into a thing when it differs from what the thing holds.
static void WriteField(StoreNet *net, StoreId id, int field, const unsigned char *slot)
{
    StoreNetData *nd = net->data;
    Store *s = net->store;
    const StoreKindData *k = KindData(s, StoreKindOf(s, id));
    const StoreFieldDecl *d = &k->fields[field];
    int size = k->sizes[field];
    unsigned char *want = Grow(nd->scratch, &nd->scratchCapacity, size + 1, 1);
    if (!want)
        return;
    nd->scratch = want;
    memcpy(want, slot, (size_t)size);
    RefsToLocal(net, d, want);
    unsigned char *have = (unsigned char *)StoreSharedBlock(s, id) + k->offsets[field];
    if (!memcmp(want, have, (size_t)size))
        return;
    StoreValue key, value;
    if (d->type < STORE_LIST)
    {
        ValueOf(d->type, want, &value);
        StoreSetEngine(s, id, field, &value);
    }
    else if ((d->type == STORE_SET && d->element == STORE_REF) ||
             (d->type == STORE_MAP && d->key == STORE_REF))
    {
        // Sorted by handle, which differs between machines: the store sorts them again.
        int32_t n;
        memcpy(&n, want, 4);
        int es = ElementSize(d->element), ks = d->type == STORE_MAP ? ElementSize(d->key) : 0;
        StoreValue *items = malloc(((size_t)n + 1) * sizeof *items);
        if (!items)
            return;
        for (int i = 0; i < n; i++)
            ValueOf(d->element, want + 4 + i * (ks + es) + ks, &items[i]);
        if (d->type == STORE_SET)
            StoreSetList(s, id, field, items, n);
        else
        {
            while (StoreCountOf(s, id, field) > 0 && StoreGetAt(s, id, field, 0, &key, NULL))
                StoreMapRemove(s, id, field, &key);
            for (int i = 0; i < n; i++)
            {
                ValueOf(d->key, want + 4 + i * (ks + es), &key);
                StoreMapSet(s, id, field, &key, &items[i]);
            }
        }
        free(items);
    }
    else
        memcpy(have, want, (size_t)size);
}

// Every shared field that changed between two rebuilt states (all of them without an earlier one).
static void WriteFields(StoreNet *net, StoreId id, const State *wasState, const Entry *was,
                        const State *nowState, const Entry *now, bool lerps)
{
    const Store *s = net->store;
    if (StoreKindOf(s, id) != now->kind || (was && was->kind != now->kind))
        was = NULL;
    if (StoreKindOf(s, id) != now->kind)
        return;
    const StoreKindData *k = KindData(s, now->kind);
    for (int f = 0; f < k->fieldCount; f++)
    {
        const unsigned char *slot = nowState->bytes + now->block + k->offsets[f];
        if (!Shared(k, f) || (!lerps && IsLerp(net, now->kind, f)) ||
            (was && !memcmp(slot, wasState->bytes + was->block + k->offsets[f], (size_t)k->sizes[f])))
            continue;
        WriteField(net, id, f, slot);
    }
}

// A thing from the network, raw: no declared children, no start. It is a root until linked.
static StoreId Create(StoreNet *net, const Entry *e)
{
    Store *s = net->store;
    uint32_t index = s->firstFree;
    while (index < s->thingCount && (s->things[index].flags & STORE_THING_LIVE))
        index++;
    if (index == s->thingCount && !StoreReserveSlots(s, index + 1, 1))
        return STORE_NULL;
    StoreId id = {index, s->things[index].generation};
    if (!StorePlaceThing(s, id, e->kind, e->owner, e->spawner, e->netId) || !Bind(net, e->netId, id))
        return STORE_NULL;
    net->data->moved = true;
    if (s->hooks.spawned)
        s->hooks.spawned(s->hooks.user, id);
    return id;
}

static void Link(StoreNet *net, StoreId id, const Entry *e)
{
    Store *s = net->store;
    StoreId parent = LocalOf(net, e->parent);
    if (!e->parent || !StoreAlive(s, parent))
        return;
    for (StoreId a = parent; a.index != UINT32_MAX; a = StoreParent(s, a))
        if (a.index == id.index)
            return; // it would hang under itself
    net->data->moved = true;
    StoreLinkChild(s, id.index, parent.index, e->name, e->guest);
}

static void Reparent(StoreNet *net, StoreId id, const Entry *e)
{
    Store *s = net->store;
    if (NetIdOf(net, StoreParent(s, id)) == e->parent)
        return;
    StoreId to = LocalOf(net, e->parent);
    net->data->moved = true;
    if (!e->parent)
        StoreDetach(s, id);
    else if (StoreAlive(s, to))
        StoreAttach(s, id, to);
}

// ---- letting go (§9.2) ------------------------------------------------------------------------
/* Whether a state says nothing this machine should take about a thing: it let go of the thing by its
   own write at its tick D, and the state's sender had not yet rebuilt a capture of ours from D on.
   Such a state was made before its sender knew, and would give the thing back (§20 item 4). */
static bool Ignored(const StoreNet *net, uint32_t netId, const State *st)
{
    return st->ack < ReleasedAt(net->data, netId);
}

/* Looks at which replicated things this machine owns. With release set, a thing owned here at the
   last look, alive and owned elsewhere now was let go by this machine's own write since then, and
   the first capture that can show it is at tick release. With 0, it only looks (after the net's own
   writes, which are not letting go). */
static void LookAtOwners(StoreNet *net, uint32_t release)
{
    StoreNetData *d = net->data;
    Store *s = net->store;
    if ((int)s->thingCount > d->ownedCapacity)
    {
        int cap = d->ownedCapacity ? d->ownedCapacity : 256;
        while (cap < (int)s->thingCount)
            cap *= 2;
        uint32_t *grown = realloc(d->owned, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        memset(grown + d->ownedCapacity, 0, (size_t)(cap - d->ownedCapacity) * sizeof *grown);
        d->owned = grown;
        d->ownedCapacity = cap;
    }
    // Every tick, three times, over every slot: the thing table is read directly (Replicated and
    // StoreOwner, without a call and a handle check per thing).
    for (uint32_t i = 0; i < s->thingCount; i++)
    {
        const StoreThing *t = &s->things[i];
        bool replicated = LiveShared(t), here = replicated && LocalOwner(net, t->owner);
        if (!release || here || !replicated || d->owned[i] != t->generation + 1)
        {
            d->owned[i] = here ? t->generation + 1 : 0;
            continue;
        }
        uint32_t netId = NetIdOf(net, (StoreId){i, t->generation});
        if (netId)
        {
            int at = 0;
            while (at < d->releaseCount && d->releases[at].netId != netId)
                at++;
            if (at == d->releaseCount)
            {
                Release *grown = Grow(d->releases, &d->releaseCapacity, at + 1, sizeof *grown);
                if (!grown)
                    continue;
                d->releases = grown;
                d->releaseCount++;
            }
            d->releases[at] = (Release){netId, release};
        }
        d->owned[i] = 0;
    }
}

// Whether every sender's state written here was made after it had rebuilt our capture at tick.
static bool SeenByAll(const StoreNet *net, uint32_t tick)
{
    for (int p = 0; p < PEERS; p++)
    {
        const Peer *peer = &net->data->peers[p];
        if (!peer->accepted)
            continue;
        const State *written = NULL;
        for (int j = 0; peer->applied && j < RING; j++)
            if (peer->states[j].tick == peer->applied)
                written = &peer->states[j];
        if (!written || written->ack < tick)
            return false;
    }
    return true;
}

/* A release is forgotten once no state left to write can be older than the sender's knowing of it
   (acknowledgements only grow with the sender's ticks, and older states are never written). */
static void ForgetReleases(StoreNet *net)
{
    StoreNetData *d = net->data;
    int kept = 0;
    for (int i = 0; i < d->releaseCount; i++)
        if (StoreAlive(net->store, LocalOf(net, d->releases[i].netId)) &&
            !SeenByAll(net, d->releases[i].tick))
            d->releases[kept++] = d->releases[i];
    d->releaseCount = kept;
}

// ---- writing states ---------------------------------------------------------------------------
// Room for n entries and their flags in the scratch lists.
static bool Room(StoreNetData *d, int n)
{
    const Entry **taken = Grow(d->taken, &d->takenCapacity, n + 1, sizeof *taken);
    if (taken)
        d->taken = taken;
    bool *fresh = taken ? Grow(d->fresh, &d->freshCapacity, n + 1, sizeof *fresh) : NULL;
    if (fresh)
        d->fresh = fresh;
    StoreId *made = fresh ? Grow(d->created, &d->createdCapacity, n + 1, sizeof *made) : NULL;
    if (made)
        d->created = made;
    return made != NULL;
}

// Creates the things of d->taken not seen here before (a removed thing stays removed); fresh[k]
// says which. Returns how many, listed in d->created.
static int CreateAll(StoreNet *net, int n)
{
    StoreNetData *d = net->data;
    int created = 0;
    for (int k = 0; k < n; k++)
    {
        d->fresh[k] = false;
        if (!d->taken[k] || Lookup(d, d->taken[k]->netId))
            continue;
        StoreId id = Create(net, d->taken[k]);
        if (id.index != UINT32_MAX)
        {
            d->fresh[k] = true;
            d->created[created++] = id;
        }
    }
    return created;
}

static void TellArrived(StoreNet *net, int created)
{
    for (int k = 0; k < created && net->config.onArrived; k++)
        if (StoreAlive(net->store, net->data->created[k]))
            net->config.onArrived(net->config.user, net->data->created[k]);
}

/* A thing that a newly arrived state makes this machine's own stops being remote on arrival: that
   state's header and every field apply now (§9.2), and it is never interpolated while it is ours. */
static void TakeOwned(StoreNet *net, int from, const State *st)
{
    StoreNetData *d = net->data;
    Store *s = net->store;
    int n = 0;
    if (!Room(d, st->count))
        return;
    // Decided for all before any is written: a root's move changes its children's owner.
    for (int j = 0; j < st->count; j++)
    {
        const Entry *e = &st->entries[j];
        if (e->left || !LocalOwner(net, e->owner) || Ignored(net, e->netId, st))
            continue;
        StoreId *known = Lookup(d, e->netId);
        if (known && (!StoreAlive(s, *known) || LocalOwner(net, StoreOwner(s, *known)) ||
                      !Accept(net, *known, from)))
            continue;
        d->taken[n++] = e;
    }
    int created = CreateAll(net, n);
    for (int k = 0; k < n; k++)
    {
        StoreId id = LocalOf(net, d->taken[k]->netId);
        if (!StoreAlive(s, id))
            continue;
        if (d->fresh[k])
            Link(net, id, d->taken[k]);
        else
            Reparent(net, id, d->taken[k]);
        WriteFields(net, id, NULL, NULL, st, d->taken[k], true);
    }
    TellArrived(net, created);
}

// Applies what changed from the state last written from a peer to the next, in network id order.
static void Apply(StoreNet *net, int from, const State *was, const State *now)
{
    StoreNetData *d = net->data;
    Store *s = net->store;
    int wasCount = was ? was->count : 0, n = 0, i = 0, j = 0;
    Action *actions = Grow(d->actions, &d->actionCapacity, wasCount + now->count + 1, sizeof *actions);
    if (!actions)
        return;
    d->actions = actions;
    if (!Room(d, wasCount + now->count))
        return;
    while (i < wasCount || j < now->count)
    {
        const Entry *a = i < wasCount ? &was->entries[i] : NULL;
        const Entry *b = j < now->count ? &now->entries[j] : NULL;
        if (a && b && a->netId == b->netId)
            i++, j++;
        else if (a && (!b || a->netId < b->netId))
            b = NULL, i++;
        else
            a = NULL, j++;
        if (b && (b->left || Ignored(net, b->netId, now)))
            continue; // no longer sent to us, or let go of here and its sender did not know yet
        if (a && b && (a->left || Ignored(net, a->netId, was)))
            a = NULL; // left alone until now: whole
        if (a && b && a->kind == b->kind && SameHeader(a, b) &&
            !memcmp(was->bytes + a->block, now->bytes + b->block, BlockSize(s, b->kind)))
            continue;
        actions[n] = (Action){a, b, false};
        d->taken[n++] = b;
    }
    // Things new here first, so parents and references among them resolve.
    int created = CreateAll(net, n);
    for (int k = 0; k < n; k++)
    {
        const Entry *a = actions[k].was, *b = actions[k].now;
        StoreId id = LocalOf(net, (a ? a : b)->netId);
        if (!StoreAlive(s, id))
            continue;
        if (d->fresh[k])
        {
            Link(net, id, b);
            WriteFields(net, id, NULL, NULL, now, b, true);
        }
        else if (!Accept(net, id, from))
            continue;
        else if (!b)
        {
            d->moved = true;
            StoreRemove(s, id);
        }
        else
        {
            Reparent(net, id, b);
            // Registered fields are the interpolation's to write, unless the thing just became ours.
            WriteFields(net, id, a ? was : NULL, a, now, b, !Accept(net, id, from));
        }
    }
    TellArrived(net, created);
}

// Two equal states give the later one's exact bits: a -0.0 at rest stays -0.0 (-0.0 + 0 * t is +0.0),
// so a still game hashes the same everywhere.
static float Blend(float a, float b, float t) { return a == b ? b : a + (b - a) * t; }

/* A peer's things' registered fields at renderTick, between the state written (a) and the first
   received after renderTick (b; none: hold a, no extrapolation). Across a parent change there is no
   blend: a's value, until renderTick reaches b and it jumps to b's. */
static void Interpolate(StoreNet *net, int from, const State *a, const State *b, int64_t render)
{
    StoreNetData *d = net->data;
    Store *s = net->store;
    if (!d->lerpCount || !a)
        return;
    float t = b ? (float)(render - (int64_t)a->tick) / (float)(b->tick - a->tick) : 0.0f;
    for (int j = 0; j < a->count; j++)
    {
        const Entry *ea = &a->entries[j];
        StoreId id = LocalOf(net, ea->netId);
        if (ea->left || !StoreAlive(s, id) || StoreKindOf(s, id) != ea->kind || !Accept(net, id, from) ||
            Ignored(net, ea->netId, a))
            continue;
        const Entry *eb = b ? FindEntry(b, ea->netId) : NULL;
        if (eb && (eb->left || eb->parent != ea->parent || eb->kind != ea->kind ||
                   Ignored(net, eb->netId, b)))
            eb = NULL;
        const StoreKindData *k = KindData(s, ea->kind);
        for (int l = 0; l < d->lerpCount; l++)
        {
            if (!StoreKindIs(s, ea->kind, d->lerps[l].kind))
                continue;
            int f = d->lerps[l].field;
            const unsigned char *pa = a->bytes + ea->block + k->offsets[f];
            StoreValue va, vb;
            ValueOf(k->fields[f].type, pa, &va);
            ValueOf(k->fields[f].type, eb ? b->bytes + eb->block + k->offsets[f] : pa, &vb);
            if (k->fields[f].type == STORE_FLOAT)
                va.as.f = Blend(va.as.f, vb.as.f, t);
            else
                va.as.v = (Vector3){Blend(va.as.v.x, vb.as.v.x, t), Blend(va.as.v.y, vb.as.v.y, t),
                                    Blend(va.as.v.z, vb.as.v.z, t)};
            StoreSetEngine(s, id, f, &va);
        }
    }
}

/* One sender's things as they stood at renderTick (§9.2): renderTick = newest tick received + ticks
   since it arrived - 6, never going back. The latest state at or before it is written (when newer
   than the one written), then the registered fields are blended towards the next. */
static void Render(StoreNet *net, int from)
{
    Peer *peer = &net->data->peers[from];
    if (!peer->newest)
        return;
    int64_t render =
        (int64_t)peer->newest + (int64_t)(StoreTickCount(net->store) - peer->arrival) - DELAY_TICKS;
    if (render < peer->render)
        render = peer->render;
    peer->render = render;
    const State *a = NULL, *b = NULL;
    for (int i = 0; i < RING; i++)
    {
        const State *st = &peer->states[i];
        if (st->tick && (int64_t)st->tick <= render && (!a || st->tick > a->tick))
            a = st;
        if (st->tick && (int64_t)st->tick > render && (!b || st->tick < b->tick))
            b = st;
    }
    if (a && a->tick > peer->applied)
    {
        Apply(net, from, FindState(peer->states, peer->applied), a);
        peer->applied = a->tick;
    }
    Interpolate(net, from, FindState(peer->states, peer->applied), b, render);
}

/* Messages whose sender tick renderTick has reached, after this tick's states are written:
   delivered, forwarded or bounced (§9.3). The rest wait; a message from a peer that left goes now. */
static void DeliverMessages(StoreNet *net)
{
    StoreNetData *d = net->data;
    Store *s = net->store;
    int count = d->inCount, kept = 0;
    for (int i = 0; i < count; i++)
    {
        Message m = d->in[i];
        const Peer *sender = m.peer >= 0 && m.peer < PEERS ? &d->peers[m.peer] : NULL;
        if (sender && sender->accepted && (int64_t)m.tick > sender->render)
        {
            d->in[kept++] = m;
            continue;
        }
        StoreId target = LocalOf(net, m.target);
        bool alive = StoreAlive(s, target);
        int owner = alive ? StoreOwner(s, target) : -1;
        if (alive && LocalOwner(net, owner))
        {
            StoreValue args[STORE_MAX_ARGS];
            for (int a = 0; a < m.count; a++)
            {
                args[a] = m.args[a];
                if (args[a].type == STORE_REF)
                    args[a].as.ref = LocalOf(net, m.args[a].as.ref.index);
            }
            StoreSend(s, target, m.event, args, m.count);
            continue;
        }
        if (net->host && alive && owner >= 2 && owner < PEERS && d->peers[owner].accepted &&
            !(m.bounced && owner == m.peer))
        {
            m.peer = owner; // to its owner, with the host's own state
            Queue(net, true, &m);
            continue;
        }
        if (!net->host && !m.bounced)
        {
            m.peer = 0; // not ours any more: back to the host, once
            m.bounced = 1;
            Queue(net, true, &m);
            continue;
        }
        TraceLog(LOG_WARNING, "STORE NET: %s for %s (network id %u) missed its owner twice; dropped",
                 StoreSymbolName(s, m.event),
                 alive ? StoreKindName(s, StoreKindOf(s, target)) : "a thing", (unsigned)m.target);
    }
    d->inCount = kept;
}

// ---- store hooks ------------------------------------------------------------------------------
static void HookError(void *user, const char *message)
{
    StoreNet *net = user;
    net->chained.error(net->chained.user, message);
}

static void HookOrphan(void *user, StoreId guest)
{
    StoreNet *net = user;
    net->chained.orphan(net->chained.user, guest);
}

static void HookSpawned(void *user, StoreId thing)
{
    StoreNet *net = user;
    net->chained.spawned(net->chained.user, thing);
}

static void HookRemoved(void *user, StoreId thing)
{
    StoreNet *net = user;
    net->chained.removed(net->chained.user, thing);
}

// A message for a thing owned on another machine: it leaves with this tick's state (§9.3).
static void HookOutgoing(void *user, StoreId target, StoreSymbol event, const StoreValue *args,
                         int count)
{
    StoreNet *net = user;
    Store *s = net->store;
    if (!net->joined || net->ended || StoreIsLocal(s, target))
        return;
    Message m;
    memset(&m, 0, sizeof m);
    if (net->host)
    {
        m.peer = StoreOwner(s, target);
        if (m.peer < 2 || m.peer >= PEERS || !net->data->peers[m.peer].accepted)
        {
            TraceLog(LOG_WARNING, "STORE NET: %s for %s #%u: its owner, player %d, is not in the game",
                     StoreSymbolName(s, event), StoreKindName(s, StoreKindOf(s, target)),
                     target.index, m.peer);
            return;
        }
    }
    m.target = Assign(net, target);
    m.event = event;
    m.count = count;
    for (int i = 0; i < count; i++)
    {
        m.args[i] = args[i];
        if (args[i].type == STORE_REF)
            m.args[i].as.ref = (StoreId){Assign(net, args[i].as.ref), 0};
    }
    if (m.target)
        Queue(net, true, &m);
}

// ---- joining and leaving ----------------------------------------------------------------------
// FNV-1a over what StoreKindsHash covers for one kind: its name, its base's, its own fields.
static uint64_t KindHash(const Store *s, StoreKind kind)
{
    Buf b = {NULL, 0, 0, true, false, FNV_OFFSET};
    StoreKind base = StoreKindBase(s, kind);
    const char *name = StoreKindName(s, kind), *baseName = base >= 0 ? StoreKindName(s, base) : "";
    Put(&b, name, strlen(name) + 1);
    Put(&b, baseName, strlen(baseName) + 1);
    for (int f = base >= 0 ? StoreFieldCount(s, base) : 0; f < StoreFieldCount(s, kind); f++)
    {
        const StoreFieldDecl *d = StoreFieldAt(s, kind, f);
        Put(&b, d->name, strlen(d->name) + 1);
        PutU32(&b, (uint32_t)d->type);
        PutU32(&b, (uint32_t)d->element);
        PutU32(&b, (uint32_t)d->key);
        PutU32(&b, (uint32_t)d->max);
        PutU32(&b, (uint32_t)d->height);
        PutU32(&b, d->flags);
    }
    return b.hash;
}

static StoreId GameRoot(const Store *s)
{
    StoreKind game = StoreKindNamed(s, "game");
    for (uint32_t i = 0; game >= 0 && i < s->thingCount; i++)
    {
        StoreId id = {i, s->things[i].generation};
        if (StoreAlive(s, id) && StoreKindIs(s, StoreKindOf(s, id), game) &&
            StoreParent(s, id).index == UINT32_MAX)
            return id;
    }
    return STORE_NULL;
}

static void TellGame(Store *s, const char *event, int player)
{
    StoreId game = GameRoot(s);
    StoreValue who;
    memset(&who, 0, sizeof who);
    who.type = STORE_INT;
    who.as.i = player;
    if (StoreAlive(s, game))
        StoreSend(s, game, StoreIntern(s, event), &who, 1);
}

static void ResetPeer(Peer *p)
{
    p->accepted = false;
    p->acked = p->newest = p->seen = p->applied = 0;
    p->arrived = false;
    p->arrival = 0;
    p->render = 0;
    for (int i = 0; i < RING; i++)
    {
        p->frames[i].tick = 0;
        ClearState(&p->states[i], 0);
    }
}

static void DropMessagesFor(StoreNetData *d, int peer)
{
    int kept = 0;
    for (int i = 0; i < d->outCount; i++)
        if (d->out[i].peer != peer)
            d->out[kept++] = d->out[i];
    d->outCount = kept;
}

static void End(StoreNet *net, const char *why)
{
    net->ended = true;
    net->joined = false;
    if (net->config.onEnded)
        net->config.onEnded(net->config.user, why);
}

static void Append(char *why, size_t size, int *listed, const char *name, const char *reason)
{
    size_t n = strlen(why);
    if (n < size)
        snprintf(why + n, size - n, "%s %s (%s)", *listed ? "," : "", name, reason);
    (*listed)++;
}

static void ReceiveHello(StoreNet *net, int peer, Reader *r)
{
    Store *s = net->store;
    StoreNetData *d = net->data;
    char game[TEXT_MAX + 1], name[TEXT_MAX + 1], why[1024];
    uint32_t protocol = GetU32(r);
    GetText(r, game);
    uint64_t kinds = GetU64(r);
    unsigned count = GetU16(r);
    if (r->bad)
        return;
    why[0] = 0;
    if (peer < 2 || peer > STORE_NET_PLAYERS || d->peers[peer].accepted)
        snprintf(why, sizeof why, "the game has no room: players are 2 to %d", STORE_NET_PLAYERS);
    else if (protocol != STORE_NET_PROTOCOL)
        snprintf(why, sizeof why, "the host speaks store protocol %u, and this client %u",
                 STORE_NET_PROTOCOL, (unsigned)protocol);
    else if (strcmp(game, net->config.game ? net->config.game : ""))
        snprintf(why, sizeof why, "the host runs %s, not %s",
                 net->config.game ? net->config.game : "", game);
    else if (kinds != StoreKindsHash(s))
    {
        bool *matched = calloc((size_t)s->kindCount + 1, sizeof *matched);
        int listed = 0;
        snprintf(why, sizeof why, "kinds differ:");
        for (unsigned i = 0; matched && i < count && !r->bad; i++)
        {
            GetText(r, name);
            uint64_t hash = GetU64(r);
            StoreKind kind = StoreKindNamed(s, name);
            if (kind < 0)
                Append(why, sizeof why, &listed, name, "missing on the host");
            else
            {
                matched[kind] = true;
                if (hash != KindHash(s, kind))
                    Append(why, sizeof why, &listed, name, "fields");
            }
        }
        for (StoreKind kind = 0; matched && kind < s->kindCount; kind++)
            if (!matched[kind])
                Append(why, sizeof why, &listed, StoreKindName(s, kind), "missing on the client");
        if (!listed)
            Append(why, sizeof why, &listed, "the same kinds", "declared in another order");
        free(matched);
    }
    if (why[0])
    {
        TraceLog(LOG_WARNING, "STORE NET: refused peer %d: %s", peer, why);
        Buf *b = StartPacket(net, PACKET_REFUSED);
        size_t n = strlen(why);
        PutU16(b, (unsigned)n);
        Put(b, why, n);
        Send(net, peer, 0, b);
        return;
    }
    ResetPeer(&d->peers[peer]);
    DropMessagesFor(d, peer);
    d->peers[peer].accepted = true; // its first state is whole: nothing of it is acknowledged
    net->players |= 1u << peer;
    Buf *b = StartPacket(net, PACKET_WELCOME);
    PutU8(b, (unsigned)peer);
    PutU32(b, (uint32_t)StoreTickCount(s));
    PutU32(b, net->players); // the session's players, this one included (a dedicated host is none)
    Send(net, peer, 0, b);
    TellGame(s, "player-joined", peer);
}

static void ReceiveWelcome(StoreNet *net, Reader *r)
{
    Store *s = net->store;
    unsigned player = GetU8(r);
    uint32_t tick = GetU32(r), players = GetU32(r);
    if (r->bad || player < 2 || player > STORE_NET_PLAYERS)
        return;
    // The world is the host's from here: empty this one (guests of removed roots become roots,
    // so again until nothing is left) and take the host's clock.
    while (StoreCount(s))
        for (uint32_t i = 0; i < s->thingCount; i++)
        {
            StoreId id = {i, s->things[i].generation};
            if (StoreAlive(s, id) && StoreParent(s, id).index == UINT32_MAX)
                StoreRemove(s, id);
        }
    StoreSetClock(s, tick, s->worldRandom);
    int owners[1] = {(int)player};
    StoreSetLocalOwners(s, owners, 1);
    net->player = (int)player;
    net->data->machine = player;
    net->players = players | 1u << player;
    net->joined = true;
    net->data->releaseCount = 0;
    ResetPeer(&net->data->peers[0]);
    net->data->peers[0].accepted = true;
    if (net->config.onJoined)
        net->config.onJoined(net->config.user, (int)player);
}

// A player left the host (§9.4): guests under what they spawned are dropped where they are and
// told, their roots go (declared children with them), then the game hears of it.
static void Leave(StoreNet *net, int player)
{
    Store *s = net->store;
    AssignAll(net);
    Pair *things;
    int n = Collect(net, &things);
    Pair *guests = malloc(((size_t)n + 1) * sizeof *guests);
    int g = 0;
    for (int i = 0; guests && i < n; i++)
        if (StoreSpawner(s, things[i].id) == player)
            for (StoreId c = StoreFirstChild(s, things[i].id); c.index != UINT32_MAX;
                 c = StoreNextSibling(s, c))
                if (StoreIsGuest(s, c))
                    guests[g++] = (Pair){NetIdOf(net, c), c};
    if (guests)
        qsort(guests, (size_t)g, sizeof *guests, ComparePairs);
    StoreSymbol orphaned = StoreIntern(s, "orphaned");
    for (int i = 0; i < g; i++)
    {
        if (!StoreIsGuest(s, guests[i].id))
            continue;
        if (net->chained.orphan)
            net->chained.orphan(net->chained.user, guests[i].id); // writes its world transform
        StoreDetach(s, guests[i].id);
        StoreSend(s, guests[i].id, orphaned, NULL, 0);
    }
    for (int i = 0; i < n; i++)
        if (StoreAlive(s, things[i].id) && StoreParent(s, things[i].id).index == UINT32_MAX &&
            StoreSpawner(s, things[i].id) == player)
            StoreRemove(s, things[i].id);
    TellGame(s, "player-left", player);
    free(guests);
    free(things);
}

// ---- the calls --------------------------------------------------------------------------------
static bool Open(StoreNet *net, Store *store, const StoreNetConfig *config, bool host)
{
    if (!net)
        return false;
    memset(net, 0, sizeof *net);
    if (!store || !config || !config->send)
        return false;
    for (int k = 0; k < store->kindCount; k++)
        if (store->kinds[k]->fieldCount > 255)
        {
            TraceLog(LOG_WARNING, "STORE NET: kind %s has more than 255 fields", store->kinds[k]->name);
            return false;
        }
    net->data = calloc(1, sizeof *net->data);
    if (!net->data)
        return false;
    net->store = store;
    net->config = *config;
    net->host = host;
    net->chained = store->hooks;
    StoreHooks hooks = store->hooks;
    hooks.user = net;
    hooks.error = hooks.error ? HookError : NULL;
    hooks.orphan = hooks.orphan ? HookOrphan : NULL;
    hooks.spawned = hooks.spawned ? HookSpawned : NULL;
    hooks.removed = hooks.removed ? HookRemoved : NULL;
    hooks.outgoing = HookOutgoing;
    StoreSetHooks(store, &hooks);
    return true;
}

bool StoreNetHost(StoreNet *net, Store *store, const StoreNetConfig *config)
{
    if (!Open(net, store, config, true))
        return false;
    int owners[2] = {0, 1};
    StoreSetLocalOwners(store, owners, config->dedicated ? 1 : 2);
    net->joined = true;
    net->player = config->dedicated ? 0 : 1;
    net->players = config->dedicated ? 0 : 1u << 1;
    return true;
}

bool StoreNetJoin(StoreNet *net, Store *store, const StoreNetConfig *config)
{
    if (!Open(net, store, config, false))
        return false;
    Buf *b = StartPacket(net, PACKET_HELLO);
    PutU32(b, STORE_NET_PROTOCOL);
    PutText(b, config->game);
    PutU64(b, StoreKindsHash(store));
    PutU16(b, (unsigned)store->kindCount);
    for (StoreKind k = 0; k < store->kindCount; k++)
    {
        PutText(b, StoreKindName(store, k));
        PutU64(b, KindHash(store, k));
    }
    return !b->failed && config->send(config->user, 0, 0, true, b->data, b->size);
}

void StoreNetPeerConnected(StoreNet *net, int peer)
{
    if (net && net->data && net->host && peer >= 2 && peer < PEERS &&
        !net->data->peers[peer].accepted)
        ResetPeer(&net->data->peers[peer]);
}

void StoreNetPeerLeft(StoreNet *net, int peer)
{
    if (!net || !net->data || peer < 0 || peer >= PEERS)
        return;
    if (!net->host)
    {
        if (peer == 0 && !net->ended)
            End(net, "the host left the game");
        return;
    }
    if (net->data->peers[peer].accepted)
        Leave(net, peer);
    ResetPeer(&net->data->peers[peer]);
    DropMessagesFor(net->data, peer);
    net->players &= ~(1u << peer);
}

void StoreNetReceive(StoreNet *net, int peer, int channel, const void *data, size_t size)
{
    (void)channel; // the packet's type byte says what it is
    if (!net || !net->data || net->ended || !data || !size || peer < 0 || peer >= PEERS)
        return;
    StoreNetData *d = net->data;
    Reader r = {data, (const unsigned char *)data + size, false};
    char name[TEXT_MAX + 1], what[TEXT_MAX + 1], why[1024];
    switch (GetU8(&r))
    {
    case PACKET_HELLO:
        if (net->host)
            ReceiveHello(net, peer, &r);
        break;
    case PACKET_WELCOME:
        if (!net->host && peer == 0 && !net->joined)
            ReceiveWelcome(net, &r);
        break;
    case PACKET_REFUSED:
    {
        if (net->host || peer != 0)
            break;
        unsigned n = GetU16(&r);
        if (n >= sizeof why)
            n = sizeof why - 1;
        Take(&r, why, n);
        why[r.bad ? 0 : n] = 0;
        End(net, why);
        break;
    }
    case PACKET_STATE:
        if (d->peers[peer].accepted && (net->host ? peer >= 2 : peer == 0))
            ReceiveState(net, peer, &r);
        break;
    case PACKET_EFFECT:
    {
        if (!d->peers[peer].accepted)
            break;
        GetText(&r, name);
        GetText(&r, what);
        Vector3 at;
        at.x = GetF32(&r);
        at.y = GetF32(&r);
        at.z = GetF32(&r);
        if (r.bad)
            break;
        if (net->config.onEffect)
            net->config.onEffect(net->config.user, name, what, at);
        for (int p = 2; net->host && p < PEERS; p++) // the host relays to the others
            if (p != peer && d->peers[p].accepted)
                net->config.send(net->config.user, p, 2, false, data, size);
        break;
    }
    default:
        break;
    }
}

void StoreNetBeforeTick(StoreNet *net)
{
    if (!net || !net->data || !net->joined || net->ended)
        return;
    StoreNetData *d = net->data;
    // Let go of between ticks: the next capture is the first that can show it.
    LookAtOwners(net, (uint32_t)StoreTickCount(net->store) + 1);
    d->moved = false;
    for (int from = 0; from < PEERS; from++)
    {
        Peer *peer = &d->peers[from];
        bool arrived = peer->arrived;
        peer->arrived = false;
        while (peer->accepted && arrived)
        {
            // Every state that arrived since the last look, oldest first: what it makes ours.
            State *next = NULL;
            for (int i = 0; i < RING; i++)
                if (peer->states[i].tick > peer->seen && (!next || peer->states[i].tick < next->tick))
                    next = &peer->states[i];
            if (!next)
                break;
            peer->seen = next->tick;
            TakeOwned(net, from, next);
        }
    }
    for (int from = 0; from < PEERS; from++)
        if (d->peers[from].accepted)
            Render(net, from);
    DeliverMessages(net);
    if (d->moved) // the net's own writes are not letting go; with none, the first look stands
        LookAtOwners(net, 0);
    ForgetReleases(net);
}

void StoreNetAfterTick(StoreNet *net)
{
    if (!net || !net->data || !net->joined || net->ended)
        return;
    StoreNetData *d = net->data;
    bool sendTick = StoreTickCount(net->store) % SEND_EVERY == 0, any = false;
    for (int p = 0; p < PEERS; p++)
        any |= d->peers[p].accepted && (sendTick || HasMessages(d, p));
    if (!any)
        return; // no capture: the next look, at tick + 1, finds what this tick let go of, and no
                // capture comes between, so every acknowledgement compares with it the same way
    LookAtOwners(net, (uint32_t)StoreTickCount(net->store)); // let go of in this tick
    const State *c = Capture(net);
    d->sends++;
    for (int p = 0; c && p < PEERS; p++)
        if (d->peers[p].accepted && (sendTick || HasMessages(d, p)))
            SendState(net, p, c);
}

bool StoreNetEffect(StoreNet *net, const char *name, const char *what, Vector3 at)
{
    if (!net || !net->data || !net->joined || net->ended ||
        StorePhaseNow(net->store) == STORE_PHASE_PRESENTATION)
        return false;
    Buf *b = StartPacket(net, PACKET_EFFECT);
    PutText(b, name);
    PutText(b, what);
    PutF32(b, at.x);
    PutF32(b, at.y);
    PutF32(b, at.z);
    for (int p = 0; p < PEERS; p++)
        if (net->data->peers[p].accepted)
            Send(net, p, 2, b);
    return !b->failed;
}

bool StoreNetInterpolate(StoreNet *net, StoreKind kind, const char *field)
{
    if (!net || !net->data)
        return false;
    StoreNetData *d = net->data;
    int f = StoreFieldIndex(net->store, kind, field);
    const StoreFieldDecl *decl = StoreFieldAt(net->store, kind, f);
    if (!decl || (decl->flags & STORE_LOCAL) || (decl->type != STORE_FLOAT && decl->type != STORE_VEC3))
        return false;
    Lerp *lerps = Grow(d->lerps, &d->lerpCapacity, d->lerpCount + 1, sizeof *lerps);
    if (!lerps)
        return false;
    d->lerps = lerps;
    lerps[d->lerpCount++] = (Lerp){kind, f};
    return true;
}

int StoreNetPlayer(const StoreNet *net) { return net ? net->player : 0; }

int StoreNetPlayers(const StoreNet *net, int *out, int max)
{
    int n = 0;
    for (int p = 1; net && p <= STORE_NET_PLAYERS; p++)
        if (net->players >> p & 1u)
        {
            if (out && n < max)
                out[n] = p;
            n++;
        }
    return n;
}

uint64_t StoreNetStateHash(const StoreNet *net)
{
    Buf b = {NULL, 0, 0, true, false, FNV_OFFSET};
    if (!net || !net->data)
        return b.hash;
    Store *s = net->store;
    Pair *things;
    int n = Collect(net, &things);
    for (int i = 0; i < n; i++)
    {
        StoreId id = things[i].id;
        const StoreKindData *k = KindData(s, StoreKindOf(s, id));
        const unsigned char *block = StoreSharedBlock(s, id);
        PutU32(&b, things[i].netId);
        PutText(&b, k->name);
        PutU32(&b, NetIdOf(net, StoreParent(s, id)));
        PutText(&b, StoreSymbolName(s, StoreChildName(s, id)));
        PutU8(&b, StoreIsGuest(s, id));
        PutU8(&b, (unsigned)StoreOwner(s, id));
        PutU8(&b, (unsigned)StoreSpawner(s, id));
        for (int f = 0; f < k->fieldCount; f++)
            if (Shared(k, f))
                PutField(net, &b, &k->fields[f], block + k->offsets[f]);
    }
    free(things);
    return b.hash;
}

static void FreeState(State *s)
{
    free(s->entries);
    free(s->bytes);
}

void StoreNetFree(StoreNet *net)
{
    if (!net)
        return;
    StoreNetData *d = net->data;
    if (net->store && d)
        StoreSetHooks(net->store, &net->chained);
    if (d)
    {
        free(d->keys);
        free(d->ids);
        free(d->slotGeneration);
        free(d->slotNet);
        for (int i = 0; i < RING; i++)
            FreeState(&d->captures[i]);
        for (int p = 0; p < PEERS; p++)
            for (int i = 0; i < RING; i++)
            {
                free(d->peers[p].frames[i].view);
                FreeState(&d->peers[p].states[i]);
            }
        free(d->out);
        free(d->in);
        free(d->lerps);
        free(d->packet.data);
        free(d->scratch);
        free(d->actions);
        free(d->created);
        free(d->taken);
        free(d->fresh);
        free(d->releases);
        free(d->owned);
        free(d->encoded.data);
        free(d->encodedAt);
        free(d);
    }
    memset(net, 0, sizeof *net);
}
