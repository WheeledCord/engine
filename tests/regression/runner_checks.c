/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The project runner (gameplay/game.c) and its recordings (core/replay.h), docs/developer/store.md
// §6: a headless bot session of tests/regression/runner_game.scm recorded, replayed to the same
// hash, a different seed reaching a different one, and what must be refused.
#define _POSIX_C_SOURCE 200809L /* dup, dup2 and fileno, to capture a run's output */
#include "checks.h"
#include "core/input_map.h"
#include "core/network.h"
#include "core/replay.h"
#include "core/store.h"
#include "core/store_net.h"
#include "core/world3d.h"
#include "gameplay/game.h"
#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
    ReplayTick tick = {5, 1, 1.5f, -2.0f, 1, 0};
    Expect(ReplayOpenWrite(&replay, path, 99, "test", 1234) && ReplayWriteTick(&replay, &tick, &command, NULL),
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

    // Version 2: the role in the header, and a tick's packets after its commands.
    unsigned char bytes[3] = {7, 8, 9};
    ReplayPacket packets[2] = {{2, 1, 3, bytes}, {3, REPLAY_PEER_LEFT, 0, NULL}};
    ReplayTick netTick = {1, 0, 0, 0, 1, 2};
    Expect(ReplayOpenWrite(&replay, path, 5, "test", 6) && ReplaySetRole(&replay, REPLAY_ROLE_CLIENT, 0) &&
               ReplayWriteTick(&replay, &netTick, &command, packets) && ReplaySetRole(&replay, REPLAY_ROLE_CLIENT, 4) &&
               ReplayWriteTick(&replay, &tick, &command, NULL),
           "a recording with packets is written, its player set after the first tick");
    ReplayClose(&replay);
    ReplayPacket got = {0};
    bool opened = ReplayOpenRead(&replay, path, &seed, &kinds);
    Expect(opened && replay.version == 2 && replay.role == REPLAY_ROLE_CLIENT && replay.player == 4,
           "the header gives back the role and the player");
    Expect(ReplayReadTick(&replay, &back, &read, 1) && back.packetCount == 2 && ReplayReadPacket(&replay, &got) &&
               got.peer == 2 && got.channel == 1 && got.size == 3 && got.data && got.data[2] == 9,
           "a tick's packet reads back with its peer, channel and bytes");
    free(got.data);
    Expect(ReplayReadTick(&replay, &back, &read, 1) && back.actions == 5 && back.packetCount == 0 &&
               !ReplayReadPacket(&replay, &got),
           "a packet left unread is skipped by the next tick");
    ReplayClose(&replay);

    // Version 1 files, from before packets, still read: no role, no packets.
    FILE *old = fopen(path, "wb");
    unsigned char header[96] = "TRENCHREPLAY", head[18] = {3, 0, 0, 0};
    header[12] = 1;
    header[16] = 42;
    if (old)
        fwrite(header, 1, sizeof header, old), fwrite(head, 1, sizeof head, old), fclose(old);
    Expect(ReplayOpenRead(&replay, path, &seed, &kinds) && seed == 42 && replay.role == REPLAY_ROLE_NONE &&
               ReplayReadTick(&replay, &back, &read, 1) && back.actions == 3 && back.packetCount == 0 &&
               !ReplayReadTick(&replay, &back, &read, 1),
           "a version 1 recording still reads, as a session with no packets");
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

// Runs the runner with its stdout and stderr in log; answers its result.
static int RunLogged(char **argv, const char *log)
{
    fflush(stdout);
    fflush(stderr);
    int out = dup(1), err = dup(2);
    FILE *capture = fopen(log, "w");
    if (!capture || out < 0 || err < 0)
    {
        if (capture)
            fclose(capture);
        return -1;
    }
    dup2(fileno(capture), 1);
    dup2(fileno(capture), 2);
    int result = Run(argv);
    fflush(stdout);
    fflush(stderr);
    dup2(out, 1);
    dup2(err, 2);
    close(out);
    close(err);
    fclose(capture);
    return result;
}

/* What carrying needs from the Scheme layer on one machine (proposal B1, B3.7, B6, C4):
   tests/regression/carry prints "check NAME #t|#f" for each call it tries, with its expected
   failures, and "event ..." for each parent-changed and orphaned it hears. Every check must say #t,
   and the events must come in this order: the soldier's socket and the crates appear (was and now
   #f), the host attaches the crate (#f -> socket), the holder drops it (socket -> #f), it is
   attached again and let go, attached a third time, and the holder is removed: the crate is
   detached where the hand was (its old parent is gone, so was reads #f) and its orphaned runs on the
   host, once. */
static void CarryChecks(void)
{
    static const char *const names[] = {
        "socket-of-names", "socket-of-given-away", "spawn-list-given-away", "spawn-list-over-capacity-refused",
        "draw-ring-fill", "socket-of-not-owner-refused", "first-child-empty",
        "aimed-at-needs-a-camera", "raycast-ignore-list-hits", "raycast-ignore-list-skips",
        "raycast-ignore-list-refuses-a-number", "draw-ring-fill-refuses-a-symbol", "aimed-at-in-reach",
        "aimed-at-out-of-reach", "aimed-at-with-camera", "first-child-holds", "socket-local-transform",
        "dropped-at", "detach-root-no-placement-refused", "detach-root-not-owner-refused", "detach-root-placed",
        "detach-held-not-owner-refused", "keep-world", "orphan-detached", "orphaned-seated",
        "parent-changed-is-presentation", "box-kind-setting", "box-spawn-setting", "shape-unknown-refused",
        "box-area-touched", "box-area-untouched"};
    static const char *const events[] = {
        "event parent-changed #f #f", "event parent-changed #f #f", "event parent-changed #f socket",
        "event parent-changed socket #f", "event parent-changed #f socket", "event parent-changed socket #f",
        "event parent-changed #f socket", "event parent-changed #f #f", "event orphaned #f owner-root 1.5"};
    const char *log = "build/core/runner_carry.log";
    char *argv[] = {"trench", "run", "tests/regression/carry", "--headless", "--present", "--ticks", "30", NULL};
    int result = RunLogged(argv, log);
    Expect(result == 0 && !FileHasLine(log, "ERROR", true), "the carry project runs 30 ticks with no ERROR");
    FILE *file = fopen(log, "r");
    char line[512];
    int seen[sizeof names / sizeof names[0]] = {0}, next = 0, eventCount = 0;
    bool inOrder = true;
    while (file && fgets(line, sizeof line, file))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "check ", 6))
        {
            char name[128], value[8];
            if (sscanf(line, "check %127s %7s", name, value) != 2 || strcmp(value, "#t"))
            {
                printf("FAIL: runner: carry: %s\n", line);
                failures++;
                continue;
            }
            for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
                if (!strcmp(name, names[i]))
                    seen[i]++;
        }
        else if (!strncmp(line, "event ", 6))
        {
            eventCount++;
            inOrder = inOrder && next < (int)(sizeof events / sizeof events[0]) && !strcmp(line, events[next]);
            next++;
        }
    }
    if (file)
        fclose(file);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!seen[i])
        {
            printf("FAIL: runner: carry: check %s never ran\n", names[i]);
            failures++;
        }
    Expect(inOrder && eventCount == (int)(sizeof events / sizeof events[0]),
           "parent-changed fires on attach, drop and orphaning with was and now, and orphaned runs once on the host");

    // The example itself loads and runs, presenting, with bot input on its keys (grab and drop).
    const char *exampleLog = "build/core/runner_carried_item.log";
    char *example[] = {"trench", "run", "examples/carried-item", "--headless", "--present", "--bot", "--ticks", "600", NULL};
    Expect(RunLogged(example, exampleLog) == 0 && FileHasLine(exampleLog, "tick 600 hash ", false) &&
               !FileHasLine(exampleLog, "ERROR", true),
           "examples/carried-item runs 600 presented ticks with bot input and no ERROR");
}

/* A floor of swat-tower can end (playtest 2 froze there: populate's do loop drew a new random count
   in its end test, and on floor 2 it could run forever). The save of a short bot run is edited so
   that the enemies are gone and the soldier stands on the stairs; loaded, the stairs send the game
   to floor 2 within a few ticks. A save is text: a `thing N gen G kind K parent P ...` line, then
   its fields indented. */
static void FloorChecks(void)
{
    const char *saved = "build/core/floor_start.sav", *edited = "build/core/floor_edited.sav",
               *after = "build/core/floor_after.sav", *log = "build/core/floor_run.log";
    bool hadProfile = FileHasLine("examples/swat-tower/profile.txt", "", true);
    char *start[] = {"trench", "run", "examples/swat-tower", "--headless", "--bot", "--seed", "7",
                     "--ticks", "120", "--save", (char *)saved, NULL};
    Expect(RunLogged(start, log) == 0, "a bot run of swat-tower saves its world");
    enum { LINES = 4096 };
    static char lines[LINES][256];
    static int owner[LINES]; /* the thing index each line belongs to, or -1 before the first */
    int count = 0, current = -1, soldier = -1, enemies[256], enemyCount = 0;
    char stairsAt[256] = "";
    FILE *file = fopen(saved, "r");
    while (file && count < LINES && fgets(lines[count], sizeof lines[count], file))
    {
        char kind[64], parent[32];
        int index;
        if (sscanf(lines[count], "thing %d gen %*d kind %63s parent %31s", &index, kind, parent) == 3)
        {
            current = index;
            if (!strcmp(kind, "soldier"))
                soldier = index;
            bool enemy = !strcmp(kind, "rusher") || !strcmp(kind, "shooter");
            for (int e = 0; e < enemyCount && !enemy; e++)
                enemy = atoi(parent) == enemies[e] && parent[0] != '-'; /* an enemy's child */
            if (enemy && enemyCount < 256)
                enemies[enemyCount++] = index;
            if (!strcmp(kind, "stairs"))
                current = -2 - index; /* marks the stairs' own lines below */
        }
        else if (current <= -2 && !strncmp(lines[count], "  position ", 11))
            snprintf(stairsAt, sizeof stairsAt, "%s", lines[count]);
        owner[count] = current <= -2 ? -2 - current : current;
        count++;
    }
    if (file)
        fclose(file);
    Expect(soldier >= 0 && stairsAt[0] && enemyCount > 0, "the save holds a soldier, the stairs and enemies");
    FILE *out = fopen(edited, "w");
    for (int i = 0; out && i < count; i++)
    {
        bool drop = false;
        for (int e = 0; e < enemyCount && !drop; e++)
            drop = owner[i] == enemies[e];
        if (drop)
            continue;
        fputs(owner[i] == soldier && !strncmp(lines[i], "  position ", 11) ? stairsAt : lines[i], out);
    }
    if (out)
        fclose(out);
    char *load[] = {"trench", "run", "examples/swat-tower", "--load", (char *)edited, "--headless",
                    "--ticks", "120", "--save", (char *)after, NULL};
    Expect(RunLogged(load, log) == 0, "the edited save loads and runs 120 ticks");
    int floor = -1;
    bool inGame = false;
    file = fopen(after, "r");
    char line[256];
    while (file && fgets(line, sizeof line, file))
    {
        char kind[64];
        if (!strncmp(line, "thing ", 6))
            inGame = sscanf(line, "thing %*d gen %*d kind %63s", kind) == 1 && !strcmp(kind, "game");
        else if (inGame)
            sscanf(line, "  floor %d", &floor);
    }
    if (file)
        fclose(file);
    printf("runner floor: %d enemy things removed, soldier #%d put on the stairs at%s  floor after 120 ticks: %d\n",
           enemyCount, soldier, stairsAt + 10, floor);
    Expect(floor == 2, "with the enemies gone and the soldier on the stairs, the game reaches floor 2");
    Expect(!FileHasLine(log, "ERROR", false) && !FileHasLine(log, "ran for over", true),
           "reaching floor 2 prints no ERROR and stops no handler");
    if (!hadProfile)
        remove("examples/swat-tower/profile.txt");
    remove(saved);
    remove(edited);
    remove(after);
}

// With no display the windowed runner exits with a message, not a crash (playtest 2, over ssh).
static void NoDisplayChecks(void)
{
    int status = system("env -u DISPLAY -u WAYLAND_DISPLAY ./build/core/trench run examples/swat-tower "
                        "--ticks 1 > build/core/trench_nodisplay.log 2>&1");
    printf("runner no display: exit %d%s\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? ", killed by a signal" : "");
    Expect(status != -1 && !WIFSIGNALED(status) && WIFEXITED(status) && WEXITSTATUS(status) != 0,
           "with no display the runner exits nonzero, not by a signal");
    Expect(FileHasLine("build/core/trench_nodisplay.log", "Engine: no display; run with --headless", true),
           "with no display the runner says so");
}

// The three words of the `rebind:` line a run of the rebind game printed; false when it printed none.
static bool RebindLine(const char *log, char *was, char *now, char *clash)
{
    FILE *file = fopen(log, "r");
    char line[256];
    bool found = false;
    while (file && !found && fgets(line, sizeof line, file))
        found = sscanf(line, "rebind: %31s %31s %31s", was, now, clash) == 3;
    if (file)
        fclose(file);
    return found;
}

// rebind! and binding (docs/user/kinds.md): the game's first frame reads fire's key, gives fire the key
// K and then jump the same key. The first run saves input.map with K for fire and is told fire has K; a
// second run of the same project starts with K.
static void RebindChecks(void)
{
    char dir[] = "build/core/rebind_XXXXXX";
    if (!mkdtemp(dir))
    {
        Expect(false, "a temporary project directory can be made under build/core");
        return;
    }
    char path[300], log[300], want[64];
    snprintf(path, sizeof path, "%s/engine.project", dir);
    FILE *file = fopen(path, "w");
    if (file)
        fputs("name rebind-check\ngame game.scm\n", file), fclose(file);
    snprintf(path, sizeof path, "%s/game.scm", dir);
    file = fopen(path, "w");
    if (file)
        fputs("(define-actions (fire \"Space\") (jump \"J\"))\n"
              "(define-kind game\n"
              "  (field k0 \"\" :local) (field k1 \"\" :local) (field clash 'none :local) (field done #f :local)\n"
              "  (on (frame dt)\n"
              "    (unless done\n"
              "      (set! done #t) (set! k0 (binding 'fire)) (rebind! 'fire \"K\") (set! k1 (binding 'fire))\n"
              "      (set! clash (rebind! 'jump \"K\"))\n"
              "      (format #t \"rebind: ~A ~A ~A~%\" k0 k1 clash)))\n"
              "  (on (draw-hud) #f))\n",
              file),
            fclose(file);
    snprintf(log, sizeof log, "%s/run.log", dir);
    char *argv[] = {"trench", "run", dir, "--headless", "--present", "--ticks", "3", NULL};
    char was[32] = "", now[32] = "", clash[32] = "", second[32] = "", again[32] = "", secondClash[32] = "";
    Expect(RunLogged(argv, log) == 0 && !FileHasLine(log, "ERROR", false), "the rebind game runs its first time");
    Expect(RebindLine(log, was, now, clash) && !strcmp(was, "Space") && !strcmp(now, "K"),
           "rebind! gives fire K, which binding reads back (it began as Space)");
    snprintf(path, sizeof path, "%s/input.map", dir);
    snprintf(want, sizeof want, "fire 0 %d %d", INPUT_KEY, KEY_K);
    char mapLine[64] = "no such line";
    file = fopen(path, "r");
    for (char line[128]; file && fgets(line, sizeof line, file);)
        if (!strncmp(line, "fire ", 5))
            snprintf(mapLine, sizeof mapLine, "%.*s", (int)strcspn(line, "\r\n"), line);
    if (file)
        fclose(file);
    Expect(!strcmp(mapLine, want), "input.map is saved beside the project with K for fire");
    snprintf(want, sizeof want, "jump 0 %d %d", INPUT_KEY, KEY_J);
    Expect(FileHasLine(path, want, false), "the refused rebind left jump on J in input.map");
    Expect(!strcmp(clash, "fire"), "rebind! answers the action that has the key when another action wants it");
    Expect(RunLogged(argv, log) == 0 && !FileHasLine(log, "ERROR", false), "the rebind game runs a second time");
    Expect(RebindLine(log, second, again, secondClash) && !strcmp(second, "K"),
           "the second run reads input.map: fire is K on the first frame, before any rebind");
    printf("runner rebind: input.map line \"%s\", second run's first-frame binding of fire %s, rebinding jump to K "
           "answered %s\n",
           mapLine, second, clash);
    snprintf(path, sizeof path, "%s/engine.project", dir);
    remove(path);
    snprintf(path, sizeof path, "%s/game.scm", dir);
    remove(path);
    snprintf(path, sizeof path, "%s/input.map", dir);
    remove(path);
    remove(log);
    rmdir(dir);
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

/* ---- animation and sockets in drawing (docs/developer/store.md §3) ----------------------------- */

/* Which pixels of a shot are grey: the test rigs are drawn untextured and white-tinted, so lit they
   are grey, while the clear colour is darker and the box in the hand magenta. */
static unsigned char *GreyMask(const char *path, long *size, long *grey)
{
    Image image = LoadImage(path);
    Color *pixels = image.data ? LoadImageColors(image) : NULL;
    *size = pixels ? (long)image.width * image.height : 0;
    *grey = 0;
    unsigned char *mask = *size ? calloc((size_t)*size, 1) : NULL;
    for (long i = 0; mask && i < *size; i++)
    {
        Color c = pixels[i];
        mask[i] = c.r > 60 && abs(c.r - c.g) < 25 && abs(c.g - c.b) < 25;
        *grey += mask[i];
    }
    UnloadImageColors(pixels);
    UnloadImage(image);
    return mask;
}

/* tests/regression/anim in a window, 70 ticks: the rig waves, so the box in the socket on its hand.R
   is drawn somewhere else at tick 60 than at tick 30, and the rig's arm is drawn over other pixels; gameplay's
   (world-position box) is the socket's rest pose both times. Once on the GPU path and once with
   --skin-on-cpu. */
static void AnimChecks(void)
{
    char *headless[] = {"trench", "run", "tests/regression/anim", "--headless", "--print-draw-position", "box", NULL};
    Expect(Run(headless) == 2, "--print-draw-position is refused headless, where nothing is drawn");
    if (!getenv("DISPLAY"))
    {
        printf("note: runner: no DISPLAY; the animation and socket drawing checks are skipped\n");
        return;
    }
    static const char *const modes[2][3] = {{"gpu", "", "build/core/anim_gpu"}, {"cpu", "--skin-on-cpu", "build/core/anim_cpu"}};
    for (int k = 0; k < 2; k++)
    {
        char command[512], log[128], shot30[128], shot60[128], line[256];
        snprintf(log, sizeof log, "%s.log", modes[k][2]);
        snprintf(shot30, sizeof shot30, "%s/shot_30.png", modes[k][2]);
        snprintf(shot60, sizeof shot60, "%s/shot_60.png", modes[k][2]);
        remove(shot30);
        remove(shot60);
        snprintf(command, sizeof command,
                 "./build/core/trench run tests/regression/anim --ticks 70 --shot-every 30 --shot-dir %s "
                 "--print-draw-position box %s > %s 2>&1",
                 modes[k][2], modes[k][1], log);
        int status = system(command);
        Vector3 drawn[2] = {{0}}, gameplay[2] = {{0}};
        int seenDrawn = 0, seenGameplay = 0, clipWarnings = 0;
        FILE *file = fopen(log, "r");
        while (file && fgets(line, sizeof line, file))
        {
            int tick;
            Vector3 v;
            if (sscanf(line, "draw-position box %d %f %f %f", &tick, &v.x, &v.y, &v.z) == 4 && (tick == 30 || tick == 60))
                drawn[tick / 60] = v, seenDrawn++;
            else if (sscanf(line, "gameplay-position box %d %f %f %f", &tick, &v.x, &v.y, &v.z) == 4 &&
                     (tick == 30 || tick == 60))
                gameplay[tick / 60] = v, seenGameplay++;
            clipWarnings += strstr(line, "has no clip dance") != NULL;
        }
        if (file)
            fclose(file);
        long size30, size60, grey30, grey60, moved = 0;
        unsigned char *mask30 = GreyMask(shot30, &size30, &grey30), *mask60 = GreyMask(shot60, &size60, &grey60);
        for (long i = 0; mask30 && mask60 && size30 == size60 && i < size30; i++)
            moved += mask30[i] != mask60[i];
        free(mask30);
        free(mask60);
        printf("runner anim (%s): box drawn at tick 30 (%.3f %.3f %.3f), tick 60 (%.3f %.3f %.3f), %.3f m apart; "
               "gameplay (%.3f %.3f %.3f) and (%.3f %.3f %.3f); rig pixels %ld then %ld, %ld changed\n",
               modes[k][0], drawn[0].x, drawn[0].y, drawn[0].z, drawn[1].x, drawn[1].y, drawn[1].z,
               (double)Vector3Distance(drawn[0], drawn[1]), gameplay[0].x, gameplay[0].y, gameplay[0].z, gameplay[1].x,
               gameplay[1].y, gameplay[1].z, grey30, grey60, moved);
        char what[160];
        snprintf(what, sizeof what, "anim (%s): the run exits cleanly with no ERROR and both positions printed twice",
                 modes[k][0]);
        Expect(status == 0 && !FileHasLine(log, "ERROR", true) && seenDrawn == 2 && seenGameplay == 2, what);
        snprintf(what, sizeof what, "anim (%s): the box in the hand socket is drawn over 0.2 m apart at ticks 30 and 60",
                 modes[k][0]);
        Expect(Vector3Distance(drawn[0], drawn[1]) > 0.2f, what);
        snprintf(what, sizeof what, "anim (%s): gameplay's world-position of the box is the rest pose at both ticks",
                 modes[k][0]);
        Expect(!memcmp(&gameplay[0], &gameplay[1], sizeof gameplay[0]) &&
                   Vector3Distance(gameplay[0], (Vector3){0, 2, -2}) < 1e-4f,
               what);
        snprintf(what, sizeof what, "anim (%s): the skinned rig is drawn over other pixels at tick 60 than at 30",
                 modes[k][0]);
        Expect(grey30 > 100 && grey60 > 100 && moved > 100, what);
        snprintf(what, sizeof what, "anim (%s): a missing clip name is warned about once in 70 ticks", modes[k][0]);
        Expect(clipWarnings == 1, what);
    }
}

/* ---- networking (docs/developer/store.md §9.6) ------------------------------------------------ */

// Things store_net creates on a client reach world3d's spawned hook (and the runner's behind it),
// so their transforms are cached and they draw: a host and a client store with world3d each, joined
// by a queue in memory.
typedef struct HookSide
{
    Store store;
    World3D world;
    StoreNet net;
    int spawned; /* calls of the hook chained behind world3d's */
} HookSide;

static HookSide sides[2];
static struct
{
    int to, channel;
    size_t size;
    unsigned char data[4096];
} queued[64];
static int queuedCount;

static bool QueueSend(void *user, int peer, int channel, bool reliable, const void *data, size_t size)
{
    (void)reliable;
    (void)peer;
    int from = (int)((HookSide *)user - sides);
    if (queuedCount == 64 || size > sizeof queued[0].data)
        return false;
    queued[queuedCount].to = 1 - from;
    queued[queuedCount].channel = channel;
    queued[queuedCount].size = size;
    memcpy(queued[queuedCount++].data, data, size);
    return true;
}

static void CountSpawned(void *user, StoreId thing)
{
    (void)thing;
    ((HookSide *)user)->spawned++;
}

static void NetHookChecks(void)
{
    memset(sides, 0, sizeof sides);
    queuedCount = 0;
    for (int i = 0; i < 2; i++)
    {
        StoreInit(&sides[i].store, 5);
        World3DInit(&sides[i].world, &sides[i].store);
        StoreHooks hooks = {0};
        hooks.user = &sides[i];
        hooks.spawned = CountSpawned;
        World3DHooks(&sides[i].world, &hooks);
        StoreSetHooks(&sides[i].store, &hooks);
    }
    StoreNetConfig config = {0};
    config.send = QueueSend;
    config.game = "hook-check";
    config.user = &sides[0];
    bool hosting = StoreNetHost(&sides[0].net, &sides[0].store, &config);
    config.user = &sides[1];
    bool joining = StoreNetJoin(&sides[1].net, &sides[1].store, &config);
    StoreId node = StoreSpawn(&sides[0].store, sides[0].world.node, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue at = {STORE_VEC3, {.v = {3, 4, 5}}};
    StoreSetEngine(&sides[0].store, node, sides[0].world.position, &at);
    StoreNetPeerConnected(&sides[0].net, 2);
    int before = sides[1].spawned;
    for (int tick = 0; tick < 12; tick++)
    {
        int n = queuedCount;
        queuedCount = 0;
        for (int i = 0; i < n; i++)
            StoreNetReceive(&sides[queued[i].to].net, queued[i].to == 0 ? 2 : 0, queued[i].channel,
                            queued[i].data, queued[i].size);
        for (int i = 0; i < 2; i++)
        {
            StoreNetBeforeTick(&sides[i].net);
            World3DBeginTick(&sides[i].world);
            StoreTick(&sides[i].store, 1.0f / 60.0f);
            StoreNetAfterTick(&sides[i].net);
        }
    }
    StoreId arrived[4];
    int count = StoreThings(&sides[1].store, sides[1].world.node, arrived, 4);
    Matrix m = {0};
    World3DUpdateTransforms(&sides[1].world, 1.0f);
    bool placed = count == 1 && World3DWorldMatrix(&sides[1].world, arrived[0], &m) && m.m12 == 3 && m.m13 == 4 &&
                  m.m14 == 5;
    printf("runner net hooks: the client holds %d node(s); the hook behind world3d's ran %d time(s) for them; "
           "drawn at %.0f %.0f %.0f\n",
           count, sides[1].spawned - before, m.m12, m.m13, m.m14);
    Expect(hosting && joining && sides[1].net.joined, "an in-memory host and client join");
    Expect(sides[1].spawned - before == 1, "a thing the network creates goes through world3d's spawned hook");
    Expect(placed, "world3d draws the arrived node where the host put it");
    for (int i = 1; i >= 0; i--)
    {
        StoreNetFree(&sides[i].net);
        World3DFree(&sides[i].world);
        StoreFree(&sides[i].store);
    }
}

// The value after prefix on the last line of log that starts with it, or "" when none does.
static const char *LineValue(const char *log, const char *prefix, char *out, size_t size)
{
    FILE *file = fopen(log, "r");
    char line[512];
    out[0] = 0;
    while (file && fgets(line, sizeof line, file))
        if (!strncmp(line, prefix, strlen(prefix)))
            snprintf(out, size, "%.*s", (int)strcspn(line + strlen(prefix), "\r\n"), line + strlen(prefix));
    if (file)
        fclose(file);
    return out;
}

// The exit status a process of the three wrote, or -1.
static int Status(const char *dir, const char *name)
{
    char path[512];
    int status = -1;
    snprintf(path, sizeof path, "%s/%s.status", dir, name);
    FILE *file = fopen(path, "r");
    if (file && fscanf(file, "%d", &status) != 1)
        status = -1;
    if (file)
        fclose(file);
    return status;
}

/* A host and two clients of project as three processes over loopback: the host starts, the clients
   0.2 s later. Each writes DIR/<h|c2|c3>.log, its pid to .pid and its exit status to .status.
   lastFlags go to the second client only, after flags (a later --ticks wins). */
static void RunThree(const char *dir, const char *project, int port, const char *flags, const char *lastFlags)
{
    char script[4096];
    snprintf(script, sizeof script,
             "D=%s; T='timeout 120 ./build/core/trench run %s --headless'; F='%s';"
             "( $T --host %d --seed 1 --record $D/H.rec $F > $D/h.log 2>&1; echo $? > $D/h.status ) &"
             " echo $! > $D/h.pid; sleep 0.2;"
             "( $T --join 127.0.0.1:%d --seed 2 --record $D/C2.rec $F > $D/c2.log 2>&1; echo $? > $D/c2.status ) &"
             " echo $! > $D/c2.pid;"
             "( $T --join 127.0.0.1:%d --seed 3 --record $D/C3.rec $F %s > $D/c3.log 2>&1; echo $? > $D/c3.status ) &"
             " echo $! > $D/c3.pid; wait",
             dir, project, flags, port, port, port, lastFlags);
    fflush(stdout);
    if (system(script) == -1)
        Expect(false, "the three processes start");
}

/* The carried item of proposal C4 over the network (tests/regression/net_carry): the example's
   carryable, and a carrier per player that walks to the nearest free carryable, grabs it within
   1.5 m and drops it 3 m ahead after 2 s. A host and two clients for 2,400 ticks, the last 120 still:
   every machine's last report names the same holder for each item and the same counts, the grants
   add up to at least 5, every grab was answered (sent = got + grab-failed), the net state hashes are
   equal, and nothing prints ERROR. Then a leave: carriers that never drop, and the second client
   run for 1,200 ticks only. On the host, the item that client's last `hand` line named runs its
   orphaned there (once, on player 1's machine), hangs from nothing at floor height, and is on the
   floor (holder 0) when player-left runs; the host and the client that stayed agree at the end. */
static void CarryNetChecks(const char *dir, int port)
{
    static const char *const names[3] = {"h", "c2", "c3"};
    char log[3][300], holders[3][64], counts[3][3][96], hash[3][64];
    for (int i = 0; i < 3; i++)
        snprintf(log[i], sizeof log[i], "%s/%s.log", dir, names[i]);
    RunThree(dir, "tests/regression/net_carry", port, "--ticks 2400 --present", "");
    bool clean = true, exited = true, agree = true, answered = true;
    int grants = 0;
    for (int i = 0; i < 3; i++)
    {
        LineValue(log[i], "carry holders ", holders[i], sizeof holders[i]);
        LineValue(log[i], "net state hash ", hash[i], sizeof hash[i]);
        for (int p = 0; p < 3; p++)
        {
            char prefix[32];
            snprintf(prefix, sizeof prefix, "carry player %d ", p + 1);
            LineValue(log[i], prefix, counts[i][p], sizeof counts[i][p]);
            agree = agree && counts[i][p][0] && !strcmp(counts[i][p], counts[0][p]);
        }
        agree = agree && holders[i][0] && !strcmp(holders[i], holders[0]);
        clean = clean && !FileHasLine(log[i], "ERROR", true);
        exited = exited && Status(dir, names[i]) == 0;
    }
    for (int p = 0; p < 3; p++)
    {
        int sent = 0, got = 0, failed = 0, drops = 0;
        answered = answered && sscanf(counts[0][p], "sent %d got %d failed %d drops %d", &sent, &got, &failed,
                                      &drops) == 4 && sent == got + failed;
        grants += got;
        printf("runner net carry: player %d: grabs sent %d, got %d, grab-failed %d, drops %d\n", p + 1, sent, got,
               failed, drops);
    }
    printf("runner net carry: holders of items 1-3: host %s, client 2 %s, client 3 %s; net state hashes %s %s %s; "
           "exits %d %d %d\n",
           holders[0], holders[1], holders[2], hash[0], hash[1], hash[2], Status(dir, "h"), Status(dir, "c2"),
           Status(dir, "c3"));
    Expect(agree, "the host and both clients agree on who holds each carryable, and on every player's counts");
    Expect(grants >= 5, "the carriers are granted at least 5 grabs in all");
    Expect(answered, "every grab sent is answered with got or grab-failed");
    Expect(hash[0][0] && !strcmp(hash[0], hash[1]) && !strcmp(hash[0], hash[2]),
           "a still carry session ends with the same net state hash on all three");
    Expect(clean, "no process of the carry session prints ERROR");
    Expect(exited, "the three processes of the carry session exit 0");

    // The leave: the project with carriers that keep what they get, and a report from 23 s.
    char project[300], path[340];
    snprintf(project, sizeof project, "%s/leave", dir);
    mkdir(project, 0755);
    static const char *const files[2] = {"engine.project", "net_carry.scm"};
    for (int f = 0; f < 2; f++)
    {
        char from[128];
        snprintf(from, sizeof from, "tests/regression/net_carry/%s", files[f]);
        snprintf(path, sizeof path, "%s/%s", project, files[f]);
        FILE *in = fopen(from, "r"), *out = fopen(path, "w");
        for (int c; in && out && (c = fgetc(in)) != EOF;)
            fputc(c, out);
        if (out && f == 1)
            fputs("(define hold-for 1000.0)\n(define still-at 23.0)\n", out);
        if (in)
            fclose(in);
        if (out)
            fclose(out);
    }
    RunThree(dir, project, port + 1, "--ticks 1500 --present", "--ticks 1200");
    char joined[16], hand[16], orphaned[128], after[128], prefix[96];
    LineValue(log[2], "net: joined as player ", joined, sizeof joined);
    snprintf(prefix, sizeof prefix, "hand player %s item ", joined);
    LineValue(log[2], prefix, hand, sizeof hand);
    snprintf(prefix, sizeof prefix, "orphaned item %s ", hand);
    LineValue(log[0], prefix, orphaned, sizeof orphaned);
    snprintf(prefix, sizeof prefix, "after player-left %s item %s ", joined, hand);
    LineValue(log[0], prefix, after, sizeof after);
    int orphanings = 0;
    FILE *file = fopen(log[0], "r");
    char line[512];
    while (file && fgets(line, sizeof line, file))
        orphanings += !strncmp(line, "orphaned item ", 14);
    if (file)
        fclose(file);
    float y = -1;
    bool seated = sscanf(orphaned, "on player 1 parent #f y %f", &y) == 1 && fabsf(y) < 0.01f;
    LineValue(log[0], "carry holders ", holders[0], sizeof holders[0]);
    LineValue(log[1], "carry holders ", holders[1], sizeof holders[1]);
    printf("runner net carry leave: player %s left holding item %s; on the host: orphaned %d time(s), \"%s\"; "
           "at player-left \"%s\"; holders at the end host %s, client %s; exits %d %d %d\n",
           joined, hand, orphanings, orphaned, after, holders[0], holders[1], Status(dir, "h"), Status(dir, "c2"),
           Status(dir, "c3"));
    Expect(joined[0] && hand[0] && strcmp(hand, "0"), "the client that leaves is holding an item when it goes");
    Expect(orphanings == 1 && seated,
           "the leaver's item runs orphaned once, on the host, and ends hanging from nothing on the floor");
    Expect(!strcmp(after, "holder 0 parent #f"), "when player-left runs the leaver's item is on the floor");
    Expect(!FileHasLine(log[1], "orphaned item ", false), "orphaned does not run on the client that stayed");
    Expect(holders[0][0] && !strcmp(holders[0], holders[1]),
           "after the leave the host and the client that stayed agree on every holder");
    clean = true;
    for (int i = 0; i < 3; i++)
        clean = clean && !FileHasLine(log[i], "ERROR", true);
    Expect(clean && Status(dir, "h") == 0 && Status(dir, "c2") == 0 && Status(dir, "c3") == 0,
           "the leave session prints no ERROR and its three processes exit 0");
}

static void NetRunChecks(void)
{
    int port = 20000 + (int)(getpid() % 20000);
    CoreNetEndpoint probe = {0};
    if (!CoreNetOpenServer(&probe, (uint16_t)port, 1, 3))
    {
        printf("note: runner: ENet cannot bind UDP port %d on this machine; the networked runner checks are "
               "skipped\n",
               port);
        return;
    }
    CoreNetClose(&probe);
    char dir[] = "build/core/net_XXXXXX";
    if (!mkdtemp(dir))
    {
        Expect(false, "a temporary directory can be made under build/core");
        return;
    }
    static const char *const names[3] = {"h", "c2", "c3"};
    char log[3][300], value[256];
    for (int i = 0; i < 3; i++)
        snprintf(log[i], sizeof log[i], "%s/%s.log", dir, names[i]);

    // The net test game: 1500 ticks, bots for the first 600, then still.
    RunThree(dir, "tests/regression/net_game", port,
             "--ticks 1500 --bot --bot-until 600 --hash-every 1500 --print-field game hellos --bench", "");
    char netHash[3][64], liveHash[3][64], hellos[3][32];
    bool clean = true, exited = true;
    for (int i = 0; i < 3; i++)
    {
        LineValue(log[i], "net state hash ", netHash[i], sizeof netHash[i]);
        LineValue(log[i], "tick 1500 hash ", liveHash[i], sizeof liveHash[i]);
        LineValue(log[i], "field game hellos ", hellos[i], sizeof hellos[i]);
        clean = clean && !FileHasLine(log[i], "ERROR", true);
        exited = exited && Status(dir, names[i]) == 0;
    }
    printf("runner net: net state hashes host %s client 2 %s client 3 %s; hellos on the host %s; "
           "exits %d %d %d; host %s\n",
           netHash[0], netHash[1], netHash[2], hellos[0], Status(dir, "h"), Status(dir, "c2"), Status(dir, "c3"),
           LineValue(log[0], "net sent ", value, sizeof value));
    Expect(netHash[0][0] && !strcmp(netHash[0], netHash[1]) && !strcmp(netHash[0], netHash[2]),
           "a host and two clients of net_game end with the same net state hash");
    Expect(clean, "no process of the net_game session prints ERROR");
    Expect(exited, "the three processes of the net_game session exit 0");
    Expect(!strcmp(hellos[0], "3"), "the host counts three hellos: its walker's and one from each client");

    // Each side's recording replays, with no socket, to its live run's tick 1500 hash.
    static const char *const recordings[2] = {"H.rec", "C2.rec"};
    for (int i = 0; i < 2; i++)
    {
        char path[300], replayLog[300], replayed[64];
        snprintf(path, sizeof path, "%s/%s", dir, recordings[i]);
        snprintf(replayLog, sizeof replayLog, "%s/replay_%s.log", dir, names[i]);
        char *argv[] = {"trench", "run", "tests/regression/net_game", "--headless", "--replay", path,
                        "--hash-every", "1500", i ? "--join" : "--host", i ? "127.0.0.1:1" : "1", NULL};
        int result = RunLogged(argv, replayLog);
        LineValue(replayLog, "tick 1500 hash ", replayed, sizeof replayed);
        printf("runner net replay: %s live tick 1500 hash %s, replayed %s (exit %d)\n", recordings[i], liveHash[i],
               replayed, result);
        Expect(result == 0 && liveHash[i][0] && !strcmp(replayed, liveHash[i]),
               i ? "the client's recording replays to its live tick 1500 hash"
                 : "the host's recording replays to its live tick 1500 hash");
        Expect(FileHasLine(replayLog, "run: --host and --join are ignored while replaying", false),
               "replaying says the --host and --join flags are ignored");
    }

    // SWAT Tower in co-op: bots throughout, 1200 ticks; every machine holds three soldiers.
    RunThree(dir, "examples/swat-tower", port + 1, "--ticks 1200 --bot --print-count soldier --bench", "");
    char soldiers[3][32], sent[256];
    clean = true;
    exited = true;
    for (int i = 0; i < 3; i++)
    {
        LineValue(log[i], "count soldier ", soldiers[i], sizeof soldiers[i]);
        clean = clean && !FileHasLine(log[i], "ERROR", true);
        exited = exited && Status(dir, names[i]) == 0;
    }
    double up = 0, down = 0;
    LineValue(log[0], "net sent ", sent, sizeof sent);
    sscanf(sent, "%lf B/s received %lf", &up, &down);
    printf("runner net swat-tower: soldiers host %s client 2 %s client 3 %s; host sends %.0f B/s per client, "
           "receives %.0f B/s per client; exits %d %d %d\n",
           soldiers[0], soldiers[1], soldiers[2], up / 2, down / 2, Status(dir, "h"), Status(dir, "c2"),
           Status(dir, "c3"));
    Expect(!strcmp(soldiers[0], "3") && !strcmp(soldiers[1], "3") && !strcmp(soldiers[2], "3"),
           "every machine of a three-player swat-tower session ends with three soldiers");
    Expect(clean, "no process of the swat-tower session prints ERROR");
    Expect(exited, "the three processes of the swat-tower session exit 0");
    Expect(up > 0, "the host's --bench prints the bytes it sent per second");
    if (!FileHasLine("examples/swat-tower/profile.txt", "", true))
        remove("examples/swat-tower/profile.txt");

    CarryNetChecks(dir, port + 6);

    // What must fail: a join nobody answers, and a client whose kinds differ from the host's.
    char failLog[300];
    snprintf(failLog, sizeof failLog, "%s/nohost.log", dir);
    char joinTo[64];
    snprintf(joinTo, sizeof joinTo, "127.0.0.1:%d", port + 2);
    char *lonely[] = {"trench", "run", "tests/regression/net_game", "--headless", "--join", joinTo, "--ticks", "60", NULL};
    Expect(RunLogged(lonely, failLog) == 1 && FileHasLine(failLog, "run: the session ended: no welcome from", false),
           "a join nobody answers gives up after 5 s, says so and exits 1");
    char project[64], path[300];
    snprintf(project, sizeof project, "%s/other", dir);
    mkdir(project, 0755);
    snprintf(path, sizeof path, "%s/engine.project", project);
    FILE *file = fopen(path, "w");
    if (file)
        fputs("name net-game\ngame net_game.scm\n", file), fclose(file);
    snprintf(path, sizeof path, "%s/net_game.scm", project);
    FILE *in = fopen("tests/regression/net_game/net_game.scm", "r");
    file = fopen(path, "w");
    for (int c; in && file && (c = fgetc(in)) != EOF;)
        fputc(c, file);
    if (file)
        fputs("(define-kind crate (is node) (field weight 1))\n", file), fclose(file);
    if (in)
        fclose(in);
    char script[1024];
    snprintf(script, sizeof script,
             "timeout 60 ./build/core/trench run tests/regression/net_game --headless --host %d --ticks 120 > "
             "%s/refuse_host.log 2>&1 & sleep 0.2; timeout 60 ./build/core/trench run %s --headless --join "
             "127.0.0.1:%d --ticks 60 > %s/refused.log 2>&1; echo $? > %s/refused.status; wait",
             port + 3, dir, project, port + 3, dir, dir);
    fflush(stdout);
    if (system(script) == -1)
        Expect(false, "the refused client runs");
    snprintf(failLog, sizeof failLog, "%s/refused.log", dir);
    LineValue(failLog, "run: the session ended: ", value, sizeof value);
    printf("runner net refused: \"%s\" (exit %d)\n", value, Status(dir, "refused"));
    Expect(Status(dir, "refused") == 1 && strstr(value, "kinds differ:") && strstr(value, "crate"),
           "a client whose kinds differ is refused, names the kind and exits 1");

    // Ctrl+C on a client leaves cleanly (the host hears at once); a client outlives a host that
    // ends, is told the host left and exits 1.
    snprintf(script, sizeof script,
             "D=%s; T='timeout 60 ./build/core/trench run tests/regression/net_game --headless';"
             "$T --host %d --ticks 300 > $D/int_host.log 2>&1 & sleep 0.2;"
             "$T --join 127.0.0.1:%d --ticks 3000 > $D/int.log 2>&1 & C=$!; sleep 2; kill -INT $C;"
             "wait $C; echo $? > $D/int.status;"
             "$T --host %d --ticks 60 > $D/gone_host.log 2>&1 & sleep 0.2;"
             "$T --join 127.0.0.1:%d --ticks 3000 > $D/gone.log 2>&1; echo $? > $D/gone.status; wait",
             dir, port + 4, port + 4, port + 5, port + 5);
    fflush(stdout);
    if (system(script) == -1)
        Expect(false, "the Ctrl+C and host-leaving runs start");
    snprintf(failLog, sizeof failLog, "%s/int_host.log", dir);
    char intLog[300], goneLog[300];
    snprintf(intLog, sizeof intLog, "%s/int.log", dir);
    snprintf(goneLog, sizeof goneLog, "%s/gone.log", dir);
    printf("runner net leaving: Ctrl+C'd client exit %d, host saw it leave: %s; client of an ending host exit %d\n",
           Status(dir, "int"), FileHasLine(failLog, "net: player 2 left", false) ? "yes" : "no", Status(dir, "gone"));
    Expect(Status(dir, "int") == 0 && FileHasLine(intLog, "run: interrupted", false) &&
               FileHasLine(failLog, "net: player 2 left", false),
           "Ctrl+C on a client ends it cleanly and the host hears it leave");
    Expect(Status(dir, "gone") == 1 && FileHasLine(goneLog, "run: the session ended: the host left the game", false),
           "a client whose host ends says the host left and exits 1");
    snprintf(script, sizeof script, "rm -rf %s", dir);
    if (system(script) == -1)
        printf("note: runner: could not remove %s\n", dir);
}

// (host-game port) from a frame handler hosts at run time; local-player and players answer for the
// host; the recording notes where the session began and replays to the same hash.
static void RuntimeHostChecks(void)
{
    char dir[] = "build/core/hostcall_XXXXXX";
    if (!mkdtemp(dir))
    {
        Expect(false, "a temporary project directory can be made under build/core");
        return;
    }
    int port = 20000 + (int)((getpid() + 7) % 20000);
    char path[300], log[300], recording[300], text[512];
    snprintf(path, sizeof path, "%s/engine.project", dir);
    FILE *file = fopen(path, "w");
    if (file)
        fputs("name host-call\ngame game.scm\n", file), fclose(file);
    snprintf(path, sizeof path, "%s/game.scm", dir);
    snprintf(text, sizeof text,
             "(define-kind game\n"
             "  (field n 0) (field hosted #f :local)\n"
             "  (on (tick dt) (set! n (+ n 1)))\n"
             "  (on (frame dt)\n"
             "    (unless hosted\n"
             "      (set! hosted #t)\n"
             "      (format #t \"host-game: ~A local-player ~A players ~A~%%\" (host-game %d) (local-player) (players)))))\n",
             port);
    file = fopen(path, "w");
    if (file)
        fputs(text, file), fclose(file);
    snprintf(log, sizeof log, "%s/run.log", dir);
    snprintf(recording, sizeof recording, "%s/run.rec", dir);
    char *argv[] = {"trench", "run", dir, "--headless", "--present", "--ticks", "30", "--record", recording, NULL};
    int result = RunLogged(argv, log);
    uint64_t live = GameLastHash();
    char said[128];
    LineValue(log, "host-game: ", said, sizeof said);
    printf("runner host-game: \"%s\" (exit %d)\n", said, result);
    Expect(result == 0 && !strcmp(said, "#t local-player 1 players (1)") &&
               FileHasLine(log, "net: hosting on UDP port", false),
           "(host-game port) from a frame handler hosts; local-player is 1 and players (1)");
    char *again[] = {"trench", "run", dir, "--headless", "--replay", recording, NULL};
    Expect(RunLogged(again, log) == 0 && live && GameLastHash() == live && !FileHasLine(log, "ERROR", true),
           "a recording that began hosting mid-run replays to the same hash");
    snprintf(text, sizeof text, "rm -rf %s", dir);
    if (system(text) == -1)
        printf("note: runner: could not remove %s\n", dir);
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
    Expect(ReplayOpenWrite(&file, wrong, 7, "test", 0x1234) && ReplayWriteTick(&file, &tick, NULL, NULL),
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
    CarryChecks();
    FloorChecks();
    RebindChecks();
    NoDisplayChecks();
    WindowedChecks();
    AnimChecks();
    NetHookChecks();
    RuntimeHostChecks();
    NetRunChecks();
    return failures;
}
