/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The project runner (gameplay/game.c) and its recordings (core/replay.h), docs/developer/store.md
// §6: a headless bot session of tests/regression/runner_game.scm recorded, replayed to the same
// hash, a different seed reaching a different one, and what must be refused.
#include "checks.h"
#include "core/replay.h"
#include "core/store.h"
#include "gameplay/game.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: runner: %s\n", what);
        failures++;
    }
}

static int Run(char **argv)
{
    int argc = 0;
    while (argv[argc])
        argc++;
    return GameRun(argc, argv);
}

// The store copies out the commands queued for the next tick, and none once the tick took them.
static void PendingChecks(void)
{
    Store store;
    Expect(StoreInit(&store, 1), "a store starts");
    StoreKind kind = StoreDeclareKind(&store, "target", -1, NULL, 0, NULL);
    StoreId id = StoreSpawn(&store, kind, 1, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue arg = {STORE_INT, {.i = 42}};
    StoreSymbol poke = StoreIntern(&store, "poke");
    Expect(StoreCommand(&store, id, poke, &arg, 1), "a command is queued");
    StoreId targets[4];
    StoreSymbol events[4];
    StoreValue args[4][STORE_MAX_ARGS];
    int counts[4];
    int n = StoreCommandsPending(&store, targets, events, args, counts, 4);
    Expect(n == 1 && targets[0].index == id.index && events[0] == poke && counts[0] == 1 &&
               args[0][0].as.i == 42,
           "StoreCommandsPending copies the queued command");
    Expect(StoreCommandsPending(&store, NULL, NULL, NULL, NULL, 0) == 1, "with no room it still counts");
    StoreTick(&store, 1.0f / 60.0f);
    Expect(StoreCommandsPending(&store, targets, events, args, counts, 4) == 0,
           "a tick takes the commands, leaving none pending");
    StoreFree(&store);
}

// A tick with a command survives the file; a file that is not a recording is refused.
static void FileChecks(const char *path)
{
    Replay replay;
    ReplayCommand command = {{3, 2}, "poke", {{STORE_INT, {.i = -7}}, {STORE_VEC3, {.v = {1, 2, 3}}}}, 2};
    ReplayTick tick = {5, 1, 1.5f, -2.0f, 1};
    Expect(ReplayOpenWrite(&replay, path, 99, "test", 1234) && ReplayWriteTick(&replay, &tick, &command),
           "a recording is written");
    ReplayClose(&replay);
    uint64_t seed = 0, kinds = 0;
    ReplayTick back;
    ReplayCommand read;
    Expect(ReplayOpenRead(&replay, path, &seed, &kinds) && seed == 99 && kinds == 1234,
           "the header gives back the seed and the kinds hash");
    Expect(ReplayReadTick(&replay, &back, &read, 1) && back.actions == 5 && back.pressed == 1 &&
               back.mouseDx == 1.5f && back.mouseDy == -2.0f && back.commandCount == 1 &&
               read.target.index == 3 && read.target.generation == 2 && !strcmp(read.event, "poke") &&
               read.count == 2 && read.args[0].as.i == -7 && read.args[1].as.v.z == 3.0f,
           "a tick and its command read back as written");
    Expect(!ReplayReadTick(&replay, &back, &read, 1), "the end of the file ends the replay");
    ReplayClose(&replay);
    FILE *junk = fopen(path, "wb");
    if (junk)
        fputs("not a recording at all, just text", junk), fclose(junk);
    Expect(!ReplayOpenRead(&replay, path, &seed, &kinds), "a file without the magic is refused");
}

int GameRunnerChecks(void)
{
    failures = 0;
    PendingChecks();
    FILE *probe = fopen("build/core/.runner_probe", "w");
    if (probe)
        fclose(probe), remove("build/core/.runner_probe");
    const char *recording = probe ? "build/core/runner_check.replay" : "runner_check.replay";
    const char *wrong = probe ? "build/core/runner_wrong.replay" : "runner_wrong.replay";
    FileChecks(wrong);

    char *record[] = {"trench", "run", "tests/regression", "--headless", "--bot", "--ticks", "600",
                      "--record", (char *)recording, "--hash-every", "600", "--seed", "7", NULL};
    Expect(Run(record) == 0, "a headless bot session of 600 ticks runs and records");
    uint64_t recorded = GameLastHash();
    char *replay[] = {"trench", "run", "tests/regression", "--headless", "--replay", (char *)recording,
                      "--ticks", "600", NULL};
    Expect(Run(replay) == 0, "the recording replays");
    Expect(recorded != 0 && GameLastHash() == recorded, "the replay reaches the recorded hash at tick 600");
    char *other[] = {"trench", "tests/regression", "--headless", "--bot", "--ticks", "600", "--seed", "8", NULL};
    Expect(Run(other) == 0 && GameLastHash() != recorded, "another seed reaches another hash");

    Replay file;
    ReplayTick tick = {0};
    Expect(ReplayOpenWrite(&file, wrong, 7, "test", 0x1234) && ReplayWriteTick(&file, &tick, NULL),
           "a recording against other kinds is written");
    ReplayClose(&file);
    char *mismatch[] = {"trench", "run", "tests/regression", "--headless", "--replay", (char *)wrong, NULL};
    Expect(Run(mismatch) != 0, "a recording whose kinds hash differs is refused");
    char *missing[] = {"trench", "run", "tests/no-such-project", "--headless", "--ticks", "1", NULL};
    Expect(Run(missing) == 1, "a directory without engine.project is refused");
    char *flag[] = {"trench", "run", "tests/regression", "--frobnicate", NULL};
    Expect(Run(flag) == 2, "an unknown flag is refused");
    remove(recording);
    remove(wrong);
    return failures;
}
