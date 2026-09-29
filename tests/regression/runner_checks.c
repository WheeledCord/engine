/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The project runner (gameplay/game.c) and its recordings (core/replay.h), docs/developer/store.md
// §6: a headless bot session of tests/regression/runner_game.scm recorded, replayed to the same
// hash, a different seed reaching a different one, and what must be refused.
#define _POSIX_C_SOURCE 200809L /* dup, dup2 and fileno, to capture a run's output */
#include "checks.h"
#include "core/replay.h"
#include "core/store.h"
#include "gameplay/game.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

// Whether a file holds a line starting with prefix (or containing it, when anywhere is set).
static bool FileHasLine(const char *path, const char *prefix, bool anywhere)
{
    FILE *file = fopen(path, "r");
    char line[1024];
    bool found = false;
    while (file && !found && fgets(line, sizeof line, file))
        found = anywhere ? strstr(line, prefix) != NULL : !strncmp(line, prefix, strlen(prefix));
    if (file)
        fclose(file);
    return found;
}

// A headless run that also presents (--present): the frame, -changed and draw-hud handlers of
// examples/swat-tower run every tick and report no error. The runner's errors reach stdout through
// TraceLog and its own complaints stderr, so both are captured. Seed 3 reaches a real alpha in
// (rgba 200 0 0 (* 300 hurt)) and a one-argument (teleport! where) within 300 ticks.
static void PresentChecks(void)
{
    static const char *const seeds[] = {"7", "3"};
    const char *log = "build/core/runner_present.log";
    bool hadProfile = FileHasLine("examples/swat-tower/profile.txt", "", true);
    for (int s = 0; s < 2; s++)
    {
        fflush(stdout);
        fflush(stderr);
        int out = dup(1), err = dup(2);
        FILE *capture = fopen(log, "w");
        if (!capture || out < 0 || err < 0)
        {
            Expect(false, "the --present run's output can be captured");
            return;
        }
        dup2(fileno(capture), 1);
        dup2(fileno(capture), 2);
        char *argv[] = {"trench", "run", "examples/swat-tower", "--headless", "--present", "--bot",
                        "--ticks", "300", "--seed", (char *)seeds[s], "--bench", NULL};
        int result = Run(argv);
        fflush(stdout);
        fflush(stderr);
        dup2(out, 1);
        dup2(err, 2);
        close(out);
        close(err);
        fclose(capture);
        char what[160];
        snprintf(what, sizeof what, "a --present bot run of swat-tower (seed %s) ends cleanly", seeds[s]);
        Expect(result == 0 && FileHasLine(log, "tick 300 ", false), what);
        snprintf(what, sizeof what, "a --present bot run of swat-tower (seed %s) prints no ERROR line", seeds[s]);
        Expect(!FileHasLine(log, "ERROR", false), what);
        snprintf(what, sizeof what, "a --present run (seed %s) counts its HUD calls", seeds[s]);
        Expect(FileHasLine(log, "bench hud calls per frame p50 ", false) &&
                   !FileHasLine(log, "bench hud calls per frame p50 0", false),
               what);
    }
    if (!hadProfile)
        remove("examples/swat-tower/profile.txt"); /* the game writes its high score there */
    char *windowed[] = {"trench", "run", "examples/swat-tower", "--present", NULL};
    Expect(Run(windowed) == 2, "--present without --headless is refused");
    char *shots[] = {"trench", "run", "examples/swat-tower", "--headless", "--shot-every", "10", NULL};
    Expect(Run(shots) == 2, "--shot-every without a window is refused");
}

static float Luminance(Color c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }

// The built runner in a window of its own: it captures the pointer, reaches tick 240 without an
// error, and its screenshot shows a lit world (not flat, not empty) and HUD text in the engine font.
static void WindowedChecks(void)
{
    if (!getenv("DISPLAY"))
    {
        printf("note: runner: no DISPLAY; the windowed runner checks are skipped\n");
        return;
    }
    const char *log = "build/core/trench_win.log", *shot = "build/core/shots/shot_240.png";
    remove(shot);
    bool hadProfile = FileHasLine("examples/swat-tower/profile.txt", "", true);
    int status = system("./build/core/trench run examples/swat-tower --bot --seed 7 --ticks 240 --shot-every 240 "
                        "--shot-dir build/core/shots > build/core/trench_win.log 2>&1");
    if (!hadProfile)
        remove("examples/swat-tower/profile.txt");
    Expect(status == 0, "the windowed runner exits cleanly");
    Expect(FileHasLine(log, "tick 240 ", false), "the windowed runner reaches tick 240");
    Expect(!FileHasLine(log, "ERROR", false), "the windowed runner prints no ERROR line");
    if (!FileHasLine(log, "run: cursor captured yes", false))
        printf("note: runner: the display did not grant pointer capture (%s)\n",
               FileHasLine(log, "run: cursor captured no", false) ? "run: cursor captured no" : "no line");
    Expect(FileHasLine(log, "shot 240 ", false), "the windowed runner reports its screenshot");
    Image image = LoadImage(shot);
    Expect(image.data != NULL, "the screenshot at tick 240 is written");
    if (!image.data)
        return;
    // The runner names the monitor it fitted the window to.
    int monitorW = 0, monitorH = 0, windowW = 0, windowH = 0;
    FILE *file = fopen(log, "r");
    char line[1024];
    while (file && fgets(line, sizeof line, file))
        if (sscanf(line, "run: window %dx%d on monitor %dx%d", &windowW, &windowH, &monitorW, &monitorH) == 4)
            break;
    if (file)
        fclose(file);
    Expect((image.width == 1024 && image.height == 576) ||
               (monitorW > 0 && image.width < monitorW && image.height < monitorH),
           "the window is 1024x576 or smaller than the monitor");
    Color *pixels = LoadImageColors(image);
    long total = (long)image.width * image.height, notClear = 0, bins[8] = {0}, bright = 0;
    for (long i = 0; pixels && i < total; i++)
    {
        Color c = pixels[i];
        notClear += !(c.r == 30 && c.g == 32 && c.b == 36);
        int bin = (int)(Luminance(c) / 32.0f);
        bins[bin > 7 ? 7 : bin]++;
        int x = (int)(i % image.width), y = (int)(i / image.width);
        bright += x < 400 && y < 40 && Luminance(c) > 180;
    }
    int fullBins = 0;
    for (int b = 0; b < 8; b++)
        fullBins += bins[b] * 50 > total;
    printf("runner shot: %dx%d (monitor %dx%d), not clear colour %.1f%%, luminance bins over 2%%: %d "
           "[%ld %ld %ld %ld %ld %ld %ld %ld], bright HUD pixels top-left %ld\n",
           image.width, image.height, monitorW, monitorH, total ? 100.0 * notClear / total : 0.0, fullBins, bins[0],
           bins[1], bins[2], bins[3], bins[4], bins[5], bins[6], bins[7], bright);
    Expect(notClear * 5 >= total, "at least 20% of the screenshot is not the clear colour");
    Expect(fullBins >= 3, "the screenshot's luminance spreads over at least 3 of 8 bins (a lit world)");
    Expect(bright >= 50, "the top-left of the screenshot holds the HUD's text");
    UnloadImageColors(pixels);
    UnloadImage(image);
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
    PresentChecks();
    WindowedChecks();
    return failures;
}
