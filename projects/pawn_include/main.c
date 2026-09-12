/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Writes the native declarations a Pawn script includes, from the binding table. The build needs
// them before it can compile any Pawn, so this is a program of the engine's own rather than
// something a game happens to provide.
#include "gameplay/script/script_pawn.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <engine.inc>\n", argv[0]);
        return 1;
    }
    return ScriptPawnWriteInclude(argv[1]) ? 0 : 1;
}
