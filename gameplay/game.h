/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_GAME_H
#define GAMEPLAY_GAME_H

#include "raylib.h"
#include <stdint.h>

/* The runner of a Scheme-only project (docs/developer/store.md §6): reads `engine.project`, loads
   the game file into a store with the built-in 3D kinds, and runs it headless or in a window, with
   recording, replay, a bot, hashes and timing on request. Flags are listed in docs/user/kinds.md. */

/** @brief Runs a project: `[run] <dir> [--headless] [--ticks N] [--seed S] [--record FILE]
 * [--replay FILE] [--hash-every N] [--bot] [--bench] [--save FILE] [--load FILE] [--host PORT]
 * [--join ADDRESS:PORT] [--bot-until N] [--print-field KIND FIELD] [--print-count KIND]`.
 *
 * Prints `tick T hash H` at the end (and every N ticks with --hash-every), and in a session `net
 * state hash H`. Tears everything down before returning, so a process may call it again.
 * @param argc Argument count, argv[0] being the program.
 * @param argv Arguments; borrowed for the call.
 * @return 0 after a normal run; 1 when the project, the game or a recording is refused, or a
 * session ended early (refused, no welcome, the host left); 2 for bad arguments. */
int GameRun(int argc, char **argv);

/** @brief Answers the store hash the last GameRun printed at its end.
 * @return The hash, or 0 when no run reached its end. */
uint64_t GameLastHash(void);

/* A `sound` thing that is playing falls off linearly to silence at this distance from the camera
   (docs/developer/store.md §3.1). */
#define GAME_SOUND_RANGE 30.0f

/** @brief How a `sound` emitter is heard from a camera: what the runner gives core/audio.h's held
 * voice each frame, computed by the same CoreAudioGainAt and CoreAudioPanAt.
 * @param camera Where the camera is.
 * @param forward The way it looks; with up, the side a sound is on.
 * @param up Its up.
 * @param at The emitter's world position.
 * @param volume The thing's `volume`.
 * @param gain Receives volume times max(0, 1 - distance / GAME_SOUND_RANGE); may be NULL.
 * @param pan Receives raylib's pan: 0.5 centred, above it the camera's left, below it its right;
 * may be NULL.
 * @return Nothing. */
void GameSoundHeard(Vector3 camera, Vector3 forward, Vector3 up, Vector3 at, float volume, float *gain, float *pan);

#endif
