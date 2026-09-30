/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_REPLAY_H
#define CORE_REPLAY_H

#include "store.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* A recording of a session (docs/developer/store.md §6.3, §9.6): the world seed, the hash of the
   kinds the game declared, the engine revision and the network role, then per tick the input, the
   player commands queued for it and the packets that arrived for it. Replaying it into the same
   build and game reaches the same StoreHash every tick.

   The file is binary and little-endian: a 16-byte header (the 12 bytes "TRENCHREPLAY" and a u32
   version), the seed (u64), the kinds hash (u64), the revision (64 bytes, NUL-padded), and from
   version 2 the role (u32, a ReplayRole) and the player id (u32); then per tick the held and
   pressed action bits (u32 each), the mouse delta (two f32), the command count (u16), from version
   2 the packet count (u16), that many commands: target index and generation (u32 each), event name
   (32 bytes), argument count (u32) and four argument slots, each a u32 type and 64 bytes of value;
   then that many packets: transport peer (u32), channel (u8), byte count (u32) and the bytes.
   From version 3 each command starts with its kind (u32, a ReplayCommandKind); a REPL command is
   then its text's byte count (u32) and the bytes, in place of the target, event and arguments.
   Version 1 files (no role, no packets) and version 2 files (no REPL commands) still read. */

#define REPLAY_MAX_ARGS 4     /* arguments a recorded command keeps; later ones are dropped */
#define REPLAY_MAX_TEXT 65536 /* bytes of a recorded REPL line, at most */

/* A packet's channel above the transport's three names a transport event instead, with no bytes. */
#define REPLAY_PEER_CONNECTED 255 /* the host: peer (a player id) connected */
#define REPLAY_PEER_LEFT 254      /* peer went away */
#define REPLAY_NET_HOSTED 253     /* networking began at this tick: the recorder hosted */
#define REPLAY_NET_JOINED 252     /* networking began at this tick: the recorder joined */

typedef enum ReplayRole
{
    REPLAY_ROLE_NONE,   /* single player, or networking began mid-run (see REPLAY_NET_*) */
    REPLAY_ROLE_HOST,   /* hosted from the first tick */
    REPLAY_ROLE_CLIENT  /* joined from the first tick; the ticks start at the welcome */
} ReplayRole;

typedef struct ReplayTick
{
    uint32_t actions;       /* held action bits */
    uint32_t pressed;       /* actions that went down this tick */
    float mouseDx, mouseDy;
    uint16_t commandCount;  /* commands follow in the file */
    uint16_t packetCount;   /* packets follow the commands */
} ReplayTick;

typedef enum ReplayCommandKind
{
    REPLAY_COMMAND_PLAYER, /* a player command (StoreCommand): target, event, arguments */
    REPLAY_COMMAND_REPL    /* a developer command: a REPL line, evaluated again on replay (§5.7) */
} ReplayCommandKind;

/* One command of a tick, in the order they reached the store. */
typedef struct ReplayCommand
{
    StoreId target;
    char event[32];
    StoreValue args[REPLAY_MAX_ARGS];
    int count;
    ReplayCommandKind kind; /* REPLAY_COMMAND_PLAYER unless set */
    char *text;             /* a REPL command's line: written, borrowed; read, malloced (free it) */
} ReplayCommand;

/* One packet (or transport event) that arrived for a tick. */
typedef struct ReplayPacket
{
    int peer;            /* the store_net peer number (docs/developer/store.md §9.5) */
    int channel;         /* 0-2, or a REPLAY_PEER_* / REPLAY_NET_* event */
    size_t size;
    unsigned char *data; /* written: borrowed; read: malloced, release with free */
} ReplayPacket;

/* One open recording, for writing or for reading. Zero it or open it; release with ReplayClose. */
typedef struct Replay
{
    FILE *file;
    bool writing;
    uint64_t ticks;   /* ticks written or read so far */
    uint32_t version; /* of the file */
    ReplayRole role;  /* from the header */
    int player;       /* from the header: 1 for a host, the welcome's id for a client */
    int unread;       /* packets of the last tick read that ReplayReadPacket has not taken */
} Replay;

/** @brief Creates a recording and writes its header.
 * @param replay Replay to open; any file it had is not closed.
 * @param path Destination file, replaced.
 * @param seed World seed.
 * @param engineRevision Engine revision text, cut to 63 bytes; NULL writes an empty one.
 * @param kindsHash StoreKindsHash of the loaded game.
 * @return True when the header was written; false leaves replay closed. */
bool ReplayOpenWrite(Replay *replay, const char *path, uint64_t seed, const char *engineRevision,
                     uint64_t kindsHash);

/** @brief Rewrites the role and player id in a recording's header, where the file is now.
 * @param replay Replay open for writing.
 * @param role Role.
 * @param player Player id (0 while a client waits for its welcome).
 * @return True when written. */
bool ReplaySetRole(Replay *replay, ReplayRole role, int player);

/** @brief Appends one tick, its commands and its packets.
 * @param replay Replay open for writing.
 * @param tick The tick's input; its commandCount and packetCount say what follows.
 * @param commands tick->commandCount commands; may be NULL when there are none.
 * @param packets tick->packetCount packets; may be NULL when there are none.
 * @return True when written. */
bool ReplayWriteTick(Replay *replay, const ReplayTick *tick, const ReplayCommand *commands,
                     const ReplayPacket *packets);

/** @brief Opens a recording and reads its header (version 1, 2 or 3); the role and player land in
 * replay. The caller compares the kinds hash with StoreKindsHash of the loaded game and refuses a
 * mismatch.
 * @param replay Replay to open.
 * @param path Recording.
 * @param seed Receives the world seed; may be NULL.
 * @param kindsHash Receives the recorded kinds hash; may be NULL.
 * @return True when the file is a recording of a version this build reads; false leaves replay
 * closed. */
bool ReplayOpenRead(Replay *replay, const char *path, uint64_t *seed, uint64_t *kindsHash);

/** @brief Reads the next tick and its commands; its packets follow through ReplayReadPacket.
 *
 * Packets of the previous tick that were not read are skipped first. A REPL command's text is
 * malloced; the caller frees each one it received (commands past max are skipped unallocated).
 * @param replay Replay open for reading.
 * @param tick Receives the tick; commandCount and packetCount are what was recorded.
 * @param commands Receives up to max commands; later ones are skipped. May be NULL when max is 0.
 * @param max Room in commands.
 * @return True when a whole tick was read; false at the end of the file or on a short read. */
bool ReplayReadTick(Replay *replay, ReplayTick *tick, ReplayCommand *commands, int max);

/** @brief Reads the next packet of the tick ReplayReadTick last read.
 * @param replay Replay open for reading.
 * @param packet Receives the packet; its data is malloced (NULL for none), release with free.
 * @return True when read; false when the tick has no more, or on a short read. */
bool ReplayReadPacket(Replay *replay, ReplayPacket *packet);

/** @brief Closes the file, flushing a recording, and zeroes replay.
 * @param replay Replay; NULL or closed is allowed.
 * @return No value. */
void ReplayClose(Replay *replay);

#endif
