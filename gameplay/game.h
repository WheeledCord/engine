/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_GAME_H
#define GAMEPLAY_GAME_H

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

#endif
