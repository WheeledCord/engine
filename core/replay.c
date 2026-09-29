/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "replay.h"
#include <string.h>

#define REPLAY_MAGIC "TRENCHREPLAY"
#define REPLAY_VERSION 1u
#define REPLAY_REVISION 64
#define REPLAY_SLOT 64

static void Put32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint32_t Get32(const unsigned char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void Put64(unsigned char *p, uint64_t v)
{
    Put32(p, (uint32_t)v);
    Put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t Get64(const unsigned char *p) { return Get32(p) | (uint64_t)Get32(p + 4) << 32; }

static uint32_t FloatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float BitsFloat(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

static bool Write(Replay *r, const void *bytes, size_t size)
{
    return r && r->file && r->writing && fwrite(bytes, 1, size, r->file) == size;
}

static bool Read(Replay *r, void *bytes, size_t size)
{
    return r && r->file && !r->writing && fread(bytes, 1, size, r->file) == size;
}

// One argument as a u32 type and a 64-byte slot; scalars are little-endian, strings are raw.
static void EncodeValue(unsigned char *out, const StoreValue *v)
{
    memset(out, 0, 4 + REPLAY_SLOT);
    Put32(out, (uint32_t)v->type);
    unsigned char *slot = out + 4;
    switch (v->type)
    {
    case STORE_INT: Put32(slot, (uint32_t)v->as.i); break;
    case STORE_SYMBOL: Put32(slot, (uint32_t)v->as.sym); break;
    case STORE_FLOAT: Put32(slot, FloatBits(v->as.f)); break;
    case STORE_BOOL: slot[0] = v->as.b ? 1 : 0; break;
    case STORE_VEC3:
        Put32(slot, FloatBits(v->as.v.x));
        Put32(slot + 4, FloatBits(v->as.v.y));
        Put32(slot + 8, FloatBits(v->as.v.z));
        break;
    case STORE_REF:
        Put32(slot, v->as.ref.index);
        Put32(slot + 4, v->as.ref.generation);
        break;
    case STORE_STRING: memcpy(slot, v->as.str, STORE_STRING_MAX); break;
    default: break;
    }
}

static void DecodeValue(const unsigned char *in, StoreValue *v)
{
    memset(v, 0, sizeof *v);
    v->type = (StoreType)Get32(in);
    const unsigned char *slot = in + 4;
    switch (v->type)
    {
    case STORE_INT: v->as.i = (int32_t)Get32(slot); break;
    case STORE_SYMBOL: v->as.sym = (StoreSymbol)Get32(slot); break;
    case STORE_FLOAT: v->as.f = BitsFloat(Get32(slot)); break;
    case STORE_BOOL: v->as.b = slot[0] != 0; break;
    case STORE_VEC3:
        v->as.v = (Vector3){BitsFloat(Get32(slot)), BitsFloat(Get32(slot + 4)), BitsFloat(Get32(slot + 8))};
        break;
    case STORE_REF: v->as.ref = (StoreId){Get32(slot), Get32(slot + 4)}; break;
    case STORE_STRING: memcpy(v->as.str, slot, STORE_STRING_MAX); break;
    default: v->type = STORE_NONE; break;
    }
}

bool ReplayOpenWrite(Replay *replay, const char *path, uint64_t seed, const char *engineRevision,
                     uint64_t kindsHash)
{
    if (!replay || !path)
        return false;
    memset(replay, 0, sizeof *replay);
    replay->file = fopen(path, "wb");
    if (!replay->file)
        return false;
    replay->writing = true;
    unsigned char header[16 + 8 + 8 + REPLAY_REVISION] = {0};
    memcpy(header, REPLAY_MAGIC, 12);
    Put32(header + 12, REPLAY_VERSION);
    Put64(header + 16, seed);
    Put64(header + 24, kindsHash);
    if (engineRevision)
        strncpy((char *)header + 32, engineRevision, REPLAY_REVISION - 1);
    if (!Write(replay, header, sizeof header))
    {
        ReplayClose(replay);
        return false;
    }
    return true;
}

bool ReplayWriteTick(Replay *replay, const ReplayTick *tick, const ReplayCommand *commands)
{
    if (!replay || !tick || (tick->commandCount && !commands))
        return false;
    unsigned char head[18];
    Put32(head, tick->actions);
    Put32(head + 4, tick->pressed);
    Put32(head + 8, FloatBits(tick->mouseDx));
    Put32(head + 12, FloatBits(tick->mouseDy));
    head[16] = (unsigned char)(tick->commandCount & 0xff);
    head[17] = (unsigned char)(tick->commandCount >> 8);
    if (!Write(replay, head, sizeof head))
        return false;
    for (int i = 0; i < tick->commandCount; i++)
    {
        const ReplayCommand *c = &commands[i];
        unsigned char bytes[8 + 32 + 4 + REPLAY_MAX_ARGS * (4 + REPLAY_SLOT)] = {0};
        Put32(bytes, c->target.index);
        Put32(bytes + 4, c->target.generation);
        strncpy((char *)bytes + 8, c->event, 31);
        int count = c->count < 0 ? 0 : c->count > REPLAY_MAX_ARGS ? REPLAY_MAX_ARGS : c->count;
        Put32(bytes + 40, (uint32_t)count);
        for (int a = 0; a < count; a++)
            EncodeValue(bytes + 44 + a * (4 + REPLAY_SLOT), &c->args[a]);
        if (!Write(replay, bytes, sizeof bytes))
            return false;
    }
    replay->ticks++;
    return true;
}

bool ReplayOpenRead(Replay *replay, const char *path, uint64_t *seed, uint64_t *kindsHash)
{
    if (!replay || !path)
        return false;
    memset(replay, 0, sizeof *replay);
    replay->file = fopen(path, "rb");
    if (!replay->file)
        return false;
    unsigned char header[16 + 8 + 8 + REPLAY_REVISION];
    if (!Read(replay, header, sizeof header) || memcmp(header, REPLAY_MAGIC, 12) ||
        Get32(header + 12) != REPLAY_VERSION)
    {
        ReplayClose(replay);
        return false;
    }
    if (seed)
        *seed = Get64(header + 16);
    if (kindsHash)
        *kindsHash = Get64(header + 24);
    return true;
}

bool ReplayReadTick(Replay *replay, ReplayTick *tick, ReplayCommand *commands, int max)
{
    unsigned char head[18];
    if (!tick || !Read(replay, head, sizeof head))
        return false;
    tick->actions = Get32(head);
    tick->pressed = Get32(head + 4);
    tick->mouseDx = BitsFloat(Get32(head + 8));
    tick->mouseDy = BitsFloat(Get32(head + 12));
    tick->commandCount = (uint16_t)(head[16] | head[17] << 8);
    for (int i = 0; i < tick->commandCount; i++)
    {
        unsigned char bytes[8 + 32 + 4 + REPLAY_MAX_ARGS * (4 + REPLAY_SLOT)];
        if (!Read(replay, bytes, sizeof bytes))
            return false;
        if (i >= max || !commands)
            continue;
        ReplayCommand *c = &commands[i];
        memset(c, 0, sizeof *c);
        c->target = (StoreId){Get32(bytes), Get32(bytes + 4)};
        memcpy(c->event, bytes + 8, 31);
        uint32_t count = Get32(bytes + 40);
        c->count = count > REPLAY_MAX_ARGS ? REPLAY_MAX_ARGS : (int)count;
        for (int a = 0; a < c->count; a++)
            DecodeValue(bytes + 44 + a * (4 + REPLAY_SLOT), &c->args[a]);
    }
    replay->ticks++;
    return true;
}

void ReplayClose(Replay *replay)
{
    if (!replay)
        return;
    if (replay->file)
        fclose(replay->file);
    memset(replay, 0, sizeof *replay);
}
