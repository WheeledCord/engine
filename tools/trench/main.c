/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// trench run <dir> [flags]: runs a Scheme-only project (docs/user/kinds.md).
#include "gameplay/game.h"

int main(int argc, char **argv) { return GameRun(argc, argv); }
