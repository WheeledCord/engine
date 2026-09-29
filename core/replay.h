/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_REPLAY_H
#define CORE_REPLAY_H

#include "store.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* A recording of a session (docs/developer/store.md §6.3): the world seed, the hash of the kinds
   the game declared and the engine revision, then per tick the input and the player commands
   queued for it. Replaying it into the same build and game reaches the same StoreHash every tick.

   The file is binary and little-endian: a 16-byte header (the 12 bytes "TRENCHREPLAY" and a u32
   version), the seed (u64), the kinds hash (u64), the revision (64 bytes, NUL-padded); then per
   tick the held and pressed action bits (u32 each), the mouse delta (two f32), the command count
   (u16) and that many commands: target index and generation (u32 each), event name (32 bytes),
   argument count (u32) and four argument slots, each a u32 type and 64 bytes of value. */

#define REPLAY_MAX_ARGS 4 /* arguments a recorded command keeps; later ones are dropped */

typedef struct ReplayTick
{
    uint32_t actions;       /* held action bits */
    uint32_t pressed;       /* actions that went down this tick */
    float mouseDx, mouseDy;
    uint16_t commandCount;  /* commands follow in the file */
} ReplayTick;

typedef struct ReplayCommand
{
    StoreId target;
    char event[32];
    StoreValue args[REPLAY_MAX_ARGS];
    int count;
} ReplayCommand;

/* One open recording, for writing or for reading. Zero it or open it; release with ReplayClose. */
typedef struct Replay
{
    FILE *file;
    bool writing;
    uint64_t ticks; /* ticks written or read so far */
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

/** @brief Appends one tick and its commands.
 * @param replay Replay open for writing.
 * @param tick The tick's input; its commandCount says how many commands follow.
 * @param commands tick->commandCount commands; may be NULL when there are none.
 * @return True when written. */
bool ReplayWriteTick(Replay *replay, const ReplayTick *tick, const ReplayCommand *commands);

/** @brief Opens a recording and reads its header. The caller compares the kinds hash with
 * StoreKindsHash of the loaded game and refuses a mismatch.
 * @param replay Replay to open.
 * @param path Recording.
 * @param seed Receives the world seed; may be NULL.
 * @param kindsHash Receives the recorded kinds hash; may be NULL.
 * @return True when the file is a recording of this version; false leaves replay closed. */
bool ReplayOpenRead(Replay *replay, const char *path, uint64_t *seed, uint64_t *kindsHash);

/** @brief Reads the next tick and its commands.
 * @param replay Replay open for reading.
 * @param tick Receives the tick; commandCount is how many were recorded.
 * @param commands Receives up to max commands; later ones are skipped. May be NULL when max is 0.
 * @param max Room in commands.
 * @return True when a whole tick was read; false at the end of the file or on a short read. */
bool ReplayReadTick(Replay *replay, ReplayTick *tick, ReplayCommand *commands, int max);

/** @brief Closes the file, flushing a recording, and zeroes replay.
 * @param replay Replay; NULL or closed is allowed.
 * @return No value. */
void ReplayClose(Replay *replay);

#endif
