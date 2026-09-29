/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Go/no-go condition 3 (docs/developer/store.md §8): what the store costs per tick when every
// field of every thing changes every tick. Each tick writes every field through StoreSet with a new
// value, then runs StoreTick with no handlers. Headless; opens no window. Run it pinned to one core,
// for instance `taskset -c 3 ./build/core/store_bench`.
#define _POSIX_C_SOURCE 199309L
#include "core/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TICKS 3600

static double Now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int Compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double Percentile(const double *sorted, int count, double p)
{
    int at = (int)(p * (count - 1) + 0.5);
    return sorted[at];
}

static void Report(const char *what, double *micros, int count)
{
    qsort(micros, (size_t)count, sizeof *micros, Compare);
    printf("  %-26s p50 %8.1f  p99 %8.1f  max %8.1f us\n", what, Percentile(micros, count, 0.5),
           Percentile(micros, count, 0.99), micros[count - 1]);
}

// One run: things of a kind with the given counts of int, float, vec3, bool and symbol fields.
static void Run(int things, int ints, int floats, int vecs, int bools, int symbols)
{
    Store store;
    if (!StoreInit(&store, 12345))
        return;
    StoreFieldDecl fields[16];
    char names[16][16];
    StoreType types[16];
    int count = 0;
    for (int i = 0; i < ints; i++)
        types[count++] = STORE_INT;
    for (int i = 0; i < floats; i++)
        types[count++] = STORE_FLOAT;
    for (int i = 0; i < vecs; i++)
        types[count++] = STORE_VEC3;
    for (int i = 0; i < bools; i++)
        types[count++] = STORE_BOOL;
    for (int i = 0; i < symbols; i++)
        types[count++] = STORE_SYMBOL;
    memset(fields, 0, sizeof fields);
    for (int f = 0; f < count; f++)
    {
        snprintf(names[f], sizeof names[f], "field%d", f);
        fields[f].name = names[f];
        fields[f].type = types[f];
    }
    const char *error = NULL;
    StoreKind kind = StoreDeclareKind(&store, "bench", -1, fields, count, &error);
    if (kind < 0)
    {
        printf("declare failed: %s\n", error ? error : "?");
        StoreFree(&store);
        return;
    }
    StoreSymbol moods[4] = {StoreIntern(&store, "idle"), StoreIntern(&store, "walk"),
                            StoreIntern(&store, "run"), StoreIntern(&store, "dead")};
    StoreId *ids = malloc((size_t)things * sizeof *ids);
    double *total = malloc(TICKS * sizeof *total), *tickOnly = malloc(TICKS * sizeof *tickOnly);
    if (!ids || !total || !tickOnly)
        return;
    for (int i = 0; i < things; i++)
        ids[i] = StoreSpawn(&store, kind, 0, STORE_NULL, STORE_NO_SYMBOL);
    StoreValue v;
    memset(&v, 0, sizeof v);
    int refused = 0;
    for (int tick = 0; tick < TICKS; tick++)
    {
        double start = Now();
        for (int i = 0; i < things; i++)
            for (int f = 0; f < count; f++)
            {
                v.type = types[f];
                switch (types[f])
                {
                case STORE_INT:
                    v.as.i = tick * 7 + i + f;
                    break;
                case STORE_FLOAT:
                    v.as.f = (float)tick * 0.25f + (float)(i + f);
                    break;
                case STORE_VEC3:
                    v.as.v = (Vector3){(float)tick, (float)i * 0.5f, (float)f};
                    break;
                case STORE_BOOL:
                    v.as.b = ((tick + i) & 1) != 0;
                    break;
                default:
                    v.as.sym = moods[(tick + i + f) & 3];
                    break;
                }
                refused += !StoreSet(&store, ids[i], f, &v);
            }
        double ticked = Now();
        StoreTick(&store, 1.0f / 60.0f);
        double end = Now();
        total[tick] = end - start;
        tickOnly[tick] = end - ticked;
    }
    double t0 = Now();
    StoreSnapshot *snapshot = StoreSnapshotTake(&store);
    double t1 = Now();
    bool restored = StoreSnapshotRestore(&store, snapshot);
    double t2 = Now();
    uint64_t hash = StoreHash(&store);
    double t3 = Now();
    printf("%d things, %d fields (%d int, %d float, %d vec3, %d bool, %d symbol), %d ticks, "
           "every field written every tick\n",
           things, count, ints, floats, vecs, bools, symbols, TICKS);
    Report("per tick (writes + tick)", total, TICKS);
    Report("StoreTick alone", tickOnly, TICKS);
    printf("  snapshot take %.1f us, restore %.1f us%s, hash %.1f us (%016llx)%s\n", t1 - t0,
           t2 - t1, restored ? "" : " (FAILED)", t3 - t2, (unsigned long long)hash,
           refused ? " (some writes refused)" : "");
    StoreSnapshotFree(snapshot);
    free(ids);
    free(total);
    free(tickOnly);
    StoreFree(&store);
}

int main(void)
{
    Run(1000, 4, 4, 2, 1, 1); // condition 3: 1,000 things of a 12-field kind
    Run(333, 3, 3, 1, 1, 0);  // Trenchfoot's size from E3: 333 things, 8 fields
    return 0;
}
