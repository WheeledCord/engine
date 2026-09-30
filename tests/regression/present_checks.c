/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// What the runner draws and plays beyond the models themselves (docs/developer/store.md §3.1):
// static batching, point lights, the viewmodel pass and sound emitters. Each project under
// tests/regression runs ./build/core/trench in a window (skipped without DISPLAY) and the checks
// read its printed lines and its screenshots back as numbers.
#include "checks.h"
#include "core/viewmodel.h"
#include "gameplay/game.h"
#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int failures;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: present: %s\n", what);
        failures++;
    }
}

/* The first line of a log that contains text, copied into out; false when there is none. */
static bool LineWith(const char *log, const char *text, char *out, size_t size)
{
    FILE *file = fopen(log, "r");
    char line[1024];
    bool found = false;
    while (file && !found && fgets(line, sizeof line, file))
        if (strstr(line, text))
        {
            snprintf(out, size, "%s", line);
            found = true;
        }
    if (file)
        fclose(file);
    return found;
}

/* A run that does not exit cleanly prints how it ended and the end of its log, so a rare failure
   leaves its evidence in the suite's output instead of in a log the next run overwrites. */
static int Trench(const char *arguments, const char *log)
{
    char command[1024];
    snprintf(command, sizeof command, "./build/core/trench run %s < /dev/null > %s 2>&1", arguments, log);
    int status = system(command);
    if (status != 0)
    {
        if (WIFSIGNALED(status))
            printf("present: run %s ended by signal %d\n", arguments, WTERMSIG(status));
        else
            printf("present: run %s exited %d\n", arguments, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        snprintf(command, sizeof command, "grep -v '^INFO' %s | tail -12 | sed 's/^/  | /'", log);
        fflush(stdout);
        if (system(command) != 0)
            printf("  | (no log)\n");
    }
    return status;
}

/* Mean absolute difference per channel between two screenshots of one size, over all but the bottom
   `skip` rows (where the runner prints errors); -1 when either is missing or they differ in size. */
static double MeanDifference(const char *a, const char *b, int skip)
{
    Image one = LoadImage(a), two = LoadImage(b);
    double result = -1;
    if (one.data && two.data && one.width == two.width && one.height == two.height)
    {
        Color *p = LoadImageColors(one), *q = LoadImageColors(two);
        long n = (long)one.width * (one.height > skip ? one.height - skip : 0);
        double sum = 0;
        for (long i = 0; p && q && i < n; i++)
            sum += abs(p[i].r - q[i].r) + abs(p[i].g - q[i].g) + abs(p[i].b - q[i].b);
        result = p && q && n ? sum / (3.0 * (double)n) : -1;
        UnloadImageColors(p);
        UnloadImageColors(q);
    }
    UnloadImage(one);
    UnloadImage(two);
    return result;
}

typedef struct Bench
{
    int items, visible, draws, models, regions, batches, materials;
} Bench;

static Bench ReadBench(const char *log)
{
    Bench b = {-1, -1, -1, -1, -1, -1, -1};
    char line[1024];
    if (LineWith(log, "bench draw items", line, sizeof line))
        sscanf(line, "bench draw items %d visible %d draws %d", &b.items, &b.visible, &b.draws);
    if (LineWith(log, "bench static models", line, sizeof line))
        sscanf(line, "bench static models %d regions %d batches %d materials %d", &b.models, &b.regions, &b.batches,
               &b.materials);
    return b;
}

/* ---- static batching ------------------------------------------------------------------------- */

/* Pixels of the green crate (tests/regression/statics) in the left and right halves of a shot, above
   the bottom `skip` rows (where the runner prints errors); -1 each when the shot is missing. */
static void Green(const char *path, int skip, long *left, long *right)
{
    Image image = LoadImage(path);
    Color *p = image.data ? LoadImageColors(image) : NULL;
    *left = *right = p ? 0 : -1;
    long n = p ? (long)image.width * (image.height > skip ? image.height - skip : 0) : 0;
    for (long i = 0; i < n; i++)
        if (p[i].g > 100 && p[i].r < p[i].g / 2 && p[i].b < p[i].g / 2)
            *(i % image.width < image.width / 2 ? left : right) += 1;
    UnloadImageColors(p);
    UnloadImage(image);
}

/* tests/regression/statics: 200 static crates of two tints on a 3x3-region tilemap, batched and not.
   By tick 60 one was moved (tick 45) and one removed (tick 50), so both shots compare what the
   batches draw before and after a rebuild. At tick 45 a green static crate owned by player 2 (not a
   local owner here) moves from the left of the view to the right, and a plain node carrying a static
   crate moves. */
static void StaticChecks(void)
{
    const char *on = "build/core/statics_on.log", *off = "build/core/statics_off.log";
    remove("build/core/statics_on/shot_30.png");
    remove("build/core/statics_on/shot_60.png");
    remove("build/core/statics_off/shot_30.png");
    remove("build/core/statics_off/shot_60.png");
    int statusOn = Trench("tests/regression/statics --ticks 60 --shot-every 30 --shot-dir build/core/statics_on --bench", on);
    int statusOff = Trench("tests/regression/statics --ticks 60 --shot-every 30 --shot-dir build/core/statics_off --bench "
                           "--no-static-batch",
                           off);
    Bench a = ReadBench(on), b = ReadBench(off);
    /* The batched run shows the moved crates' errors along the bottom from tick 45; the world above
       them is what is compared. */
    double at30 = MeanDifference("build/core/statics_on/shot_30.png", "build/core/statics_off/shot_30.png", 0);
    double at60 = MeanDifference("build/core/statics_on/shot_60.png", "build/core/statics_off/shot_60.png", 80);
    char line[1024] = "", ancestor[1024] = "";
    bool error = LineWith(on, "position on crate #", line, sizeof line) &&
                 strstr(line, "changed, but crate is :static; remove :static if it moves");
    bool carried = LineWith(on, "an ancestor of crate #", ancestor, sizeof ancestor) &&
                   strstr(ancestor, "moved it, but crate is :static; remove :static if it moves");
    long green[4][2]; /* batched tick 30, 60; unbatched tick 30, 60: left, right */
    Green("build/core/statics_on/shot_30.png", 0, &green[0][0], &green[0][1]);
    Green("build/core/statics_on/shot_60.png", 80, &green[1][0], &green[1][1]);
    Green("build/core/statics_off/shot_30.png", 0, &green[2][0], &green[2][1]);
    Green("build/core/statics_off/shot_60.png", 80, &green[3][0], &green[3][1]);
    int nonStatic = a.items - a.batches;
    printf("present statics: batched: items %d draws %d, %d models in %d regions as %d batches of %d materials "
           "(bound regions x materials + non-static items = %d); unbatched: items %d draws %d; mean pixel "
           "difference tick 30 %.4f, tick 60 %.4f\n",
           a.items, a.draws, a.models, a.regions, a.batches, a.materials, a.regions * a.materials + nonStatic, b.items,
           b.draws, at30, at60);
    printf("present statics: the moved crate: %s", error ? line : "(no error line)\n");
    printf("present statics: the crate whose parent moved: %s", carried ? ancestor : "(no error line)\n");
    printf("present statics: player 2's green crate (left, right pixels): batched tick 30 %ld %ld, tick 60 %ld %ld; "
           "unbatched tick 30 %ld %ld, tick 60 %ld %ld\n",
           green[0][0], green[0][1], green[1][0], green[1][1], green[2][0], green[2][1], green[3][0], green[3][1]);
    Expect(statusOn == 0 && statusOff == 0, "statics: both runs exit cleanly");
    Expect(a.models == 199 && a.regions == 9 && a.materials == 3,
           "statics: 199 crates (202, one moved, one carried off by its parent and one removed; the green one "
           "batched again) ride in batches in 9 regions of 3 materials");
    Expect(a.draws >= 0 && a.draws <= a.regions * a.materials + nonStatic && a.draws * 4 < 200,
           "statics: draws <= regions x materials + non-static items, and far fewer than the crates");
    Expect(b.models == 0 && b.draws >= 200, "statics: --no-static-batch draws every crate as itself");
    Expect(at30 >= 0 && at30 < 1.0 && at60 >= 0 && at60 < 1.0,
           "statics: batched and unbatched shots differ by under 1.0 per channel, before and after a rebuild");
    Expect(error, "statics: moving a batched static crate is the error naming :static");
    Expect(carried, "statics: moving the parent of a batched static crate is the error naming the crate, saying an "
                    "ancestor moved it");
    Expect(!LineWith(on, "loose-crate #", line, sizeof line) && !LineWith(off, "ERROR", line, sizeof line),
           "statics: moving a loose crate, or a static one that was never batched, is no error");
    Expect(!LineWith(on, "green-crate #", line, sizeof line),
           "statics: a batched static crate owned elsewhere that moves is no error here");
    bool moved = true;
    for (int r = 0; r < 4; r += 2)
        moved = moved && green[r][0] > 50 && green[r][1] == 0 && green[r + 1][0] == 0 && green[r + 1][1] > 50;
    Expect(moved, "statics: a batched static crate owned elsewhere draws at its new place after it moves, batched "
                  "or not");
    int refused = Trench("tests/regression/statics --headless --ticks 1 --no-static-batch", "build/core/statics_headless.log");
    Expect(WIFEXITED(refused) && WEXITSTATUS(refused) == 2, "statics: --no-static-batch is refused headless");
}

/* ---- point lights ---------------------------------------------------------------------------- */

typedef struct Shot
{
    Image image;
    Color *pixels;
} Shot;

static Shot LoadShot(const char *path)
{
    Shot s = {LoadImage(path), NULL};
    if (s.image.data)
        s.pixels = LoadImageColors(s.image);
    return s;
}

static void UnloadShot(Shot *s)
{
    UnloadImageColors(s->pixels);
    UnloadImage(s->image);
}

/* Mean luminance of the 5x5 pixels around a floor point, seen from a camera 12 m straight above
   (16, 0, 16) with a 60 degree vertical field of view (tests/regression/lights): +x is screen right
   and +z screen down. -1 off the shot. */
static float FloorLuminance(const Shot *s, float x, float z)
{
    if (!s->pixels)
        return -1;
    float scale = (float)s->image.height * 0.5f / (12.0f * tanf(30.0f * DEG2RAD));
    int cx = (int)lroundf((float)s->image.width * 0.5f + (x - 16.0f) * scale);
    int cy = (int)lroundf((float)s->image.height * 0.5f + (z - 16.0f) * scale);
    if (cx < 2 || cy < 2 || cx >= s->image.width - 2 || cy >= s->image.height - 2)
        return -1;
    float sum = 0;
    for (int y = cy - 2; y <= cy + 2; y++)
        for (int x2 = cx - 2; x2 <= cx + 2; x2++)
        {
            Color c = s->pixels[y * s->image.width + x2];
            sum += 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        }
    return sum / 25.0f;
}

/* Pixels of the magenta-tinted rig lit well above the dark room's ambient. */
static long LitMagenta(const Shot *s)
{
    long n = 0;
    for (long i = 0; s->pixels && i < (long)s->image.width * s->image.height; i++)
        n += s->pixels[i].r > 100 && s->pixels[i].b > 100 && s->pixels[i].g < s->pixels[i].r / 2;
    return n;
}

/* tests/regression/lights: under light a against a floor point twice its range away, with the
   lights on (tick 30) and at energy 0 (tick 60); the skinned rig under light b; and light e, the
   fifth nearest the camera, which is left out. */
static void LightChecks(void)
{
    const char *log = "build/core/lights.log";
    remove("build/core/lights/shot_30.png");
    remove("build/core/lights/shot_60.png");
    int status = Trench("tests/regression/lights --ticks 60 --shot-every 30 --shot-dir build/core/lights", log);
    Shot on = LoadShot("build/core/lights/shot_30.png"), off = LoadShot("build/core/lights/shot_60.png");
    float under = FloorLuminance(&on, 16, 16), away = FloorLuminance(&on, 20, 16), fifth = FloorLuminance(&on, 27, 21);
    float underOff = FloorLuminance(&off, 16, 16), awayOff = FloorLuminance(&off, 20, 16),
          fifthOff = FloorLuminance(&off, 27, 21);
    long rigOn = LitMagenta(&on), rigOff = LitMagenta(&off);
    char line[1024];
    printf("present lights: luminance under light a %.1f, 4 m (2 x range) away %.1f (x%.1f), under the fifth light "
           "%.1f; at energy 0: %.1f, %.1f and %.1f; lit rig pixels %ld, at energy 0 %ld\n",
           (double)under, (double)away, away > 0 ? (double)(under / away) : 0.0, (double)fifth, (double)underOff,
           (double)awayOff, (double)fifthOff, rigOn, rigOff);
    Expect(status == 0 && on.pixels && off.pixels && !LineWith(log, "ERROR", line, sizeof line),
           "lights: the run exits cleanly with both shots and no error");
    Expect(under > 0 && away > 0 && under >= 4 * away, "lights: the floor under a point light is at least 4x brighter "
                                                       "than twice its range away");
    Expect(underOff >= 0 && awayOff >= 0 && fabsf(underOff - awayOff) <= 3,
           "lights: at energy 0 the two floor points are equally dark (within 3 levels)");
    Expect(rigOn > 100 && rigOff < rigOn / 20, "lights: a skinned model is lit by a point light, and dark without it");
    Expect(fifth >= 0 && fifthOff >= 0 && fabsf(fifth - fifthOff) <= 3,
           "lights: a fifth point light, farther from the camera than four others, lights nothing");
    UnloadShot(&on);
    UnloadShot(&off);
}

/* ---- the viewmodel pass ---------------------------------------------------------------------- */

/* The runner's magenta stand-in cube, lit: pixels in the left and right halves of a shot. */
static void Magenta(const Shot *s, long *left, long *right)
{
    *left = *right = 0;
    for (long i = 0; s->pixels && i < (long)s->image.width * s->image.height; i++)
    {
        Color c = s->pixels[i];
        if (c.r > 60 && c.b > 60 && c.g < 30)
            *(i % s->image.width < s->image.width / 2 ? left : right) += 1;
    }
}

static int viewmodelCalls;
static void CountCall(void *context)
{
    (void)context;
    viewmodelCalls++;
}

/* DrawViewmodelCleared refuses what it cannot draw before it touches GL: no field of view, clip
   planes not 0 < near < far, no callback. */
static void ViewmodelArgumentChecks(void)
{
    ViewmodelProjection good = {60, 16.0f / 9.0f, 1}, flat = {0, 16.0f / 9.0f, 1};
    viewmodelCalls = 0;
    bool refused = !DrawViewmodelCleared(flat, 0.01f, 10, CountCall, NULL) &&
                   !DrawViewmodelCleared(good, 0, 10, CountCall, NULL) &&
                   !DrawViewmodelCleared(good, 1, 0.5f, CountCall, NULL) &&
                   !DrawViewmodelCleared(good, 0.01f, 10, NULL, NULL);
    Expect(refused && viewmodelCalls == 0,
           "viewmodel: the cleared pass refuses a field of view of 0, planes not 0 < near < far, and no callback");
}

/* tests/regression/viewmodel: a box 0.4 m ahead of the eye behind a wall 0.2 m ahead, as a model
   (tick 20, hidden) and as a viewmodel (tick 40, drawn); then player 2's viewmodel boxes, the
   :for-owner one not drawn here and the other drawn like any model (tick 60). */
static void ViewmodelChecks(void)
{
    const char *log = "build/core/viewmodel.log";
    const char *paths[3] = {"build/core/viewmodel/shot_20.png", "build/core/viewmodel/shot_40.png",
                            "build/core/viewmodel/shot_60.png"};
    for (int i = 0; i < 3; i++)
        remove(paths[i]);
    int status = Trench("tests/regression/viewmodel --ticks 60 --shot-every 20 --shot-dir build/core/viewmodel", log);
    long left[3], right[3];
    for (int i = 0; i < 3; i++)
    {
        Shot s = LoadShot(paths[i]);
        Magenta(&s, &left[i], &right[i]);
        if (!s.pixels)
            left[i] = right[i] = -1;
        UnloadShot(&s);
    }
    char line[1024];
    printf("present viewmodel: box pixels (left + right) behind the wall as a model %ld, as a viewmodel %ld; player "
           "2's boxes: :for-owner (left) %ld, not (right) %ld\n",
           left[0] + right[0], left[1] + right[1], left[2], right[2]);
    Expect(status == 0 && !LineWith(log, "ERROR", line, sizeof line), "viewmodel: the run exits cleanly with no error");
    Expect(left[0] == 0 && right[0] == 0, "viewmodel: as an ordinary model, the box behind the wall is hidden");
    Expect(left[1] + right[1] > 2000, "viewmodel: as a viewmodel, the box 0.4 m ahead is drawn over the wall 0.2 m ahead");
    Expect(left[2] == 0, "viewmodel: another player's :for-owner viewmodel is not drawn here");
    Expect(right[2] > 200, "viewmodel: another player's viewmodel without :for-owner is drawn like any model");
}

/* ---- sound emitters ------------------------------------------------------------------------- */

/* What a playing `sound` thing is heard at (GameSoundHeard, the runner's own computation): its
   volume at the camera, half at 15 m, nothing from 30 m, and panned to the side it is on. */
static void EmitterMixChecks(void)
{
    Vector3 eye = {0, 1.6f, 0}, ahead = {0, 0, -1}, up = {0, 1, 0};
    const float distances[5] = {0, 15, 30, 45, 7.5f};
    float gains[5], pans[4];
    for (int i = 0; i < 5; i++)
        GameSoundHeard(eye, ahead, up, (Vector3){0, 1.6f, -distances[i]}, 0.8f, &gains[i], NULL);
    GameSoundHeard(eye, ahead, up, (Vector3){5, 1.6f, -5}, 0.8f, NULL, &pans[0]);  /* right */
    GameSoundHeard(eye, ahead, up, (Vector3){-5, 1.6f, -5}, 0.8f, NULL, &pans[1]); /* left */
    GameSoundHeard(eye, (Vector3){1, 0, 0}, up, (Vector3){0, 1.6f, 5}, 0.8f, NULL, &pans[2]); /* facing +x: +z is right */
    GameSoundHeard(eye, ahead, up, (Vector3){0, 1.6f, -10}, 0.8f, NULL, &pans[3]); /* dead ahead */
    printf("present emitters: volume 0.8 heard at 0 m %.3f, 15 m %.3f, 30 m %.3f, 45 m %.3f, 7.5 m %.3f; pan (0.5 "
           "centred, above it left) right %.3f, left %.3f, right of a camera facing +x %.3f, ahead %.3f\n",
           (double)gains[0], (double)gains[1], (double)gains[2], (double)gains[3], (double)gains[4], (double)pans[0],
           (double)pans[1], (double)pans[2], (double)pans[3]);
    Expect(fabsf(gains[0] - 0.8f) < 1e-5f && fabsf(gains[1] - 0.4f) < 1e-5f && gains[2] == 0 && gains[3] == 0 &&
               fabsf(gains[4] - 0.6f) < 1e-5f,
           "emitters: the gain is volume x 1, 0.5 and 0 at 0, 15 and 30 m, 0 beyond, linear between");
    Expect(pans[0] < 0.5f && pans[1] > 0.5f && pans[2] < 0.5f && fabsf(pans[3] - 0.5f) < 1e-5f,
           "emitters: the pan follows the side of the camera the sound is on, and centres dead ahead");
}

/* tests/regression/emitters in a window, where this container has no audio device, and headless:
   both end cleanly; the missing file is one warning. */
static void EmitterRunChecks(void)
{
    const char *log = "build/core/emitters.log", *quiet = "build/core/emitters_headless.log";
    int status = Trench("tests/regression/emitters --ticks 50", log);
    int headless = Trench("tests/regression/emitters --headless --ticks 50", quiet);
    int missing = 0, silent = 0;
    char line[1024];
    FILE *file = fopen(log, "r");
    while (file && fgets(line, sizeof line, file))
    {
        missing += strstr(line, "RUN: no sound missing.wav") != NULL;
        silent += strstr(line, "RUN: sound ") && strstr(line, "hum.wav could not be loaded or no audio device");
    }
    if (file)
        fclose(file);
    printf("present emitters: windowed run exit %d (no audio device here: %s), headless exit %d; missing-file "
           "warnings %d\n",
           status, silent ? "yes" : "no, the hum played", headless, missing);
    Expect(status == 0 && !LineWith(log, "ERROR", line, sizeof line),
           "emitters: a windowed run with sound things ends cleanly (with or without an audio device)");
    Expect(missing == 1 && silent <= 1, "emitters: a missing stream, and a hum without a device, warn once each");
    Expect(headless == 0 && !LineWith(quiet, "RUN: ", line, sizeof line),
           "emitters: headless plays nothing and says nothing about sound");
}

int PresentationChecks(void)
{
    failures = 0;
    EmitterMixChecks();
    ViewmodelArgumentChecks();
    if (!getenv("DISPLAY"))
    {
        printf("note: present: no DISPLAY; the static batching, light, viewmodel and emitter runs are skipped\n");
        return failures;
    }
    StaticChecks();
    LightChecks();
    ViewmodelChecks();
    EmitterRunChecks();
    return failures;
}
