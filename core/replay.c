/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "replay.h"
#include <stdlib.h>
#include <string.h>

#define REPLAY_MAGIC "TRENCHREPLAY"
#define REPLAY_VERSION 3u /* 1: no role, no packets; 2: no command kinds (no REPL lines) */
#define REPLAY_REVISION 64
#define REPLAY_SLOT 64
#define REPLAY_HEADER (16 + 8 + 8 + REPLAY_REVISION)
#define REPLAY_ROLE_AT REPLAY_HEADER /* version 2: role and player, u32 each */
#define REPLAY_MAX_PACKET (1024u * 1024u)

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
    replay->version = REPLAY_VERSION;
    unsigned char header[REPLAY_HEADER + 8] = {0};
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

bool ReplaySetRole(Replay *replay, ReplayRole role, int player)
{
    if (!replay || !replay->file || !replay->writing)
        return false;
    long at = ftell(replay->file);
    unsigned char bytes[8];
    Put32(bytes, (uint32_t)role);
    Put32(bytes + 4, (uint32_t)player);
    bool ok = at >= 0 && !fseek(replay->file, REPLAY_ROLE_AT, SEEK_SET) && Write(replay, bytes, sizeof bytes);
    ok = at >= 0 && !fseek(replay->file, at, SEEK_SET) && ok;
    if (ok)
        replay->role = role, replay->player = player;
    return ok;
}

bool ReplayWriteTick(Replay *replay, const ReplayTick *tick, const ReplayCommand *commands,
                     const ReplayPacket *packets)
{
    if (!replay || !tick || (tick->commandCount && !commands) || (tick->packetCount && !packets))
        return false;
    unsigned char head[20];
    Put32(head, tick->actions);
    Put32(head + 4, tick->pressed);
    Put32(head + 8, FloatBits(tick->mouseDx));
    Put32(head + 12, FloatBits(tick->mouseDy));
    head[16] = (unsigned char)(tick->commandCount & 0xff);
    head[17] = (unsigned char)(tick->commandCount >> 8);
    head[18] = (unsigned char)(tick->packetCount & 0xff);
    head[19] = (unsigned char)(tick->packetCount >> 8);
    if (!Write(replay, head, sizeof head))
        return false;
    for (int i = 0; i < tick->commandCount; i++)
    {
        const ReplayCommand *c = &commands[i];
        unsigned char kind[8];
        Put32(kind, (uint32_t)c->kind);
        if (c->kind == REPLAY_COMMAND_REPL)
        {
            size_t length = c->text ? strlen(c->text) : 0;
            if (length > REPLAY_MAX_TEXT)
                return false;
            Put32(kind + 4, (uint32_t)length);
            if (!Write(replay, kind, 8) || (length && !Write(replay, c->text, length)))
                return false;
            continue;
        }
        if (!Write(replay, kind, 4))
            return false;
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
    for (int i = 0; i < tick->packetCount; i++)
    {
        const ReplayPacket *p = &packets[i];
        unsigned char bytes[9];
        if (p->size > REPLAY_MAX_PACKET || (p->size && !p->data))
            return false;
        Put32(bytes, (uint32_t)p->peer);
        bytes[4] = (unsigned char)p->channel;
        Put32(bytes + 5, (uint32_t)p->size);
        if (!Write(replay, bytes, sizeof bytes) || (p->size && !Write(replay, p->data, p->size)))
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
    unsigned char header[REPLAY_HEADER + 8];
    uint32_t version = 0;
    if (!Read(replay, header, REPLAY_HEADER) || memcmp(header, REPLAY_MAGIC, 12) ||
        (version = Get32(header + 12)) < 1 || version > REPLAY_VERSION ||
        (version >= 2 && !Read(replay, header + REPLAY_HEADER, 8)))
    {
        ReplayClose(replay);
        return false;
    }
    replay->version = version;
    if (version >= 2)
    {
        uint32_t role = Get32(header + REPLAY_ROLE_AT);
        replay->role = role <= REPLAY_ROLE_CLIENT ? (ReplayRole)role : REPLAY_ROLE_NONE;
        replay->player = (int)Get32(header + REPLAY_ROLE_AT + 4);
    }
    if (seed)
        *seed = Get64(header + 16);
    if (kindsHash)
        *kindsHash = Get64(header + 24);
    return true;
}

bool ReplayReadTick(Replay *replay, ReplayTick *tick, ReplayCommand *commands, int max)
{
    unsigned char head[20];
    ReplayPacket skipped;
    while (replay && replay->unread > 0)
    {
        if (!ReplayReadPacket(replay, &skipped))
            return false;
        free(skipped.data);
    }
    if (!tick || !Read(replay, head, replay && replay->version >= 2 ? 20 : 18))
        return false;
    tick->actions = Get32(head);
    tick->pressed = Get32(head + 4);
    tick->mouseDx = BitsFloat(Get32(head + 8));
    tick->mouseDy = BitsFloat(Get32(head + 12));
    tick->commandCount = (uint16_t)(head[16] | head[17] << 8);
    tick->packetCount = replay->version >= 2 ? (uint16_t)(head[18] | head[19] << 8) : 0;
    for (int i = 0; i < tick->commandCount; i++)
    {
        unsigned char bytes[8 + 32 + 4 + REPLAY_MAX_ARGS * (4 + REPLAY_SLOT)];
        uint32_t kind = REPLAY_COMMAND_PLAYER;
        if (replay->version >= 3)
        {
            if (!Read(replay, bytes, 4))
                return false;
            kind = Get32(bytes);
        }
        if (kind == REPLAY_COMMAND_REPL)
        {
            if (!Read(replay, bytes, 4))
                return false;
            uint32_t length = Get32(bytes);
            char *text = length <= REPLAY_MAX_TEXT ? malloc((size_t)length + 1) : NULL;
            if (!text || (length && !Read(replay, text, length)))
            {
                free(text);
                return false;
            }
            text[length] = 0;
            if (i >= max || !commands)
            {
                free(text);
                continue;
            }
            memset(&commands[i], 0, sizeof commands[i]);
            commands[i].target = STORE_NULL;
            commands[i].kind = REPLAY_COMMAND_REPL;
            commands[i].text = text;
            continue;
        }
        if (kind != REPLAY_COMMAND_PLAYER || !Read(replay, bytes, sizeof bytes))
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
    replay->unread = tick->packetCount;
    replay->ticks++;
    return true;
}

bool ReplayReadPacket(Replay *replay, ReplayPacket *packet)
{
    unsigned char bytes[9];
    if (!packet)
        return false;
    memset(packet, 0, sizeof *packet);
    if (!replay || replay->unread <= 0 || !Read(replay, bytes, sizeof bytes))
        return false;
    replay->unread--;
    packet->peer = (int)Get32(bytes);
    packet->channel = bytes[4];
    packet->size = Get32(bytes + 5);
    if (packet->size > REPLAY_MAX_PACKET)
        return false;
    if (packet->size && (!(packet->data = malloc(packet->size)) || !Read(replay, packet->data, packet->size)))
    {
        free(packet->data);
        packet->data = NULL;
        return false;
    }
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
