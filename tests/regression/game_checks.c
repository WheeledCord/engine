/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The store's Scheme frontend (gameplay/script/game_s7.c): define-kind's code walk and the calls,
// from tests/regression/kinds_walk.scm; the five rule errors, one file each under
// tests/regression/rules/; and what must be refused. docs/developer/store.md §5 and §8.
#include "checks.h"
#include "core/store.h"
#include "gameplay/script/game_s7.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static char lastError[2048];
static int errorCount;

static void Expect(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL: game: %s\n", what);
        failures++;
    }
}

static void Sink(const char *message)
{
    snprintf(lastError, sizeof lastError, "%s", message);
    errorCount++;
}

static bool Get(Store *s, StoreId id, const char *field, StoreValue *out)
{
    int f = StoreFieldIndex(s, StoreKindOf(s, id), field);
    return f >= 0 && StoreGet(s, id, f, out);
}

static int Int(Store *s, StoreId id, const char *field)
{
    StoreValue v;
    return Get(s, id, field, &v) && v.type == STORE_INT ? v.as.i : -12345;
}

static bool True(Store *s, StoreId id, const char *field)
{
    StoreValue v;
    return Get(s, id, field, &v) && v.type == STORE_BOOL && v.as.b;
}

static bool IsSymbol(Store *s, StoreId id, const char *field, const char *name)
{
    StoreValue v;
    return Get(s, id, field, &v) && v.type == STORE_SYMBOL && v.as.sym >= 0 &&
           !strcmp(StoreSymbolName(s, v.as.sym), name);
}

static StoreId First(Store *s, const char *kind, int which)
{
    StoreId ids[8];
    int count = StoreThings(s, StoreKindNamed(s, kind), ids, 8);
    return which < count ? ids[which] : STORE_NULL;
}

// Evaluates at the REPL; answers whether it succeeded, with the answer kept for inspection.
static char answer[2048];
static bool Eval(const char *text)
{
    char *result = NULL;
    bool ok = GameS7Eval(text, &result);
    snprintf(answer, sizeof answer, "%s", result ? result : "");
    free(result);
    return ok;
}

static bool Answer(const char *text, const char *expected)
{
    bool ok = Eval(text) && !strcmp(answer, expected);
    if (!ok)
        printf("  %s answered %s\n", text, answer);
    return ok;
}

static bool Refused(const char *text, const char *containing)
{
    bool ok = !Eval(text) && strstr(answer, containing);
    if (!ok)
        printf("  %s answered %s\n", text, answer);
    return ok;
}

// The walk file, loaded into a fresh store: 10 ticks, a frame, a tick, a frame.
static bool Walk(Store *store, uint64_t seed)
{
    if (!StoreInit(store, seed))
        return false;
    if (!GameS7Open(store, "core/scheme/kinds.scm"))
        return StoreFree(store), false;
    if (!GameS7LoadGame("tests/regression/kinds_walk.scm"))
        return true;
    for (int i = 0; i < 10; i++)
        StoreTick(store, 1.0f / 60.0f);
    StoreFrame(store, 1.0f / 60.0f);
    StoreTick(store, 1.0f / 60.0f);
    StoreFrame(store, 1.0f / 60.0f);
    return true;
}

static void WalkChecks(void)
{
    Store store;
    int32_t rolls[2][3] = {{0}};
    uint64_t hashes[2] = {0, 1};
    for (int run = 0; run < 2; run++)
    {
        errorCount = 0;
        lastError[0] = 0;
        if (!Walk(&store, 1234))
        {
            Expect(false, "the prelude opens");
            return;
        }
        Expect(errorCount == 0, "kinds_walk.scm loads and runs 12 ticks without an error");
        if (errorCount)
            printf("  first error: %s\n", lastError);
        StoreId a = First(&store, "walker", 0), b = First(&store, "walker", 1);
        StoreValue v;
        for (int i = 0; i < 3; i++)
        {
            int f = StoreFieldIndex(&store, StoreKindOf(&store, a), "rolls");
            rolls[run][i] = StoreGetAt(&store, a, f, i, NULL, &v) ? v.as.i : -1;
        }
        hashes[run] = StoreHash(&store);
        if (run == 1)
            break;

        // Field reads and set!, shadowing, helpers, children, views, the tree.
        Expect(Int(&store, a, "health") == 89 && Int(&store, b, "health") == 39,
               "(set! health (- health 1)) writes the field, and :health 50 on spawn set b's");
        Expect(Int(&store, a, "ticks") == 11, "a tick handler ran once per tick");
        Expect(True(&store, a, "shadows-ok"),
               "lambda, let, let*, letrec, do, named let, inner define, quote and case datums shadow "
               "or hide a field name");
        Expect(True(&store, a, "helpers-ok") && Int(&store, a, "hits") == 5,
               "a helper inherited from the base, called and passed as a value, writes the field");
        Expect(True(&store, a, "children-ok"),
               "child names, nested children, (set! (eye 'power) ...), :at, a positional setting and "
               "the tree calls");
        Expect(True(&store, a, "map-ok"), "a map field's view reads, writes, removes and is replaced by an alist");
        Expect(True(&store, a, "grid-ok"), "a grid field's view reads, writes and fills, and :init gave its rows");
        Expect(True(&store, a, "tree-ok"), "(game), (things 'kind), (local-player), (players), clamp");
        StoreId eye = StoreChildNamed(&store, a, StoreIntern(&store, "eye"));
        Expect(Int(&store, eye, "power") == 4, "the child's field, written from its parent's handler, reads back in C");
        Expect(Int(&store, a, "pinged") == 7, "(after 0.05 'ping 7) fired with its argument");
        Expect(Int(&store, b, "poked") == 22 && Int(&store, a, "poked") == 0,
               "(send friend 'poke 2) is delivered each tick, to the friend only");
        Expect(IsSymbol(&store, a, "seen-was", "false") && Int(&store, a, "frames") == 2,
               "health-changed ran on the first frame with was #f, and again after a change");
        StoreId guard = First(&store, "guard", 0);
        Expect(IsSymbol(&store, guard, "state", "idle") && Int(&store, guard, "count") == 8 &&
                   Int(&store, guard, "entered") == 1 && Int(&store, guard, "exited") == 1 &&
                   Int(&store, guard, "alert-ticks") == 3,
               "states: tick per state, go, then exit and enter");
        Expect(True(&store, First(&store, "remover", 0), "victim-gone") &&
                   Int(&store, First(&store, "game", 0), "notes") == 0,
               "remove drops the messages queued for the thing");

        // A :local child.
        StoreId trooper = First(&store, "trooper", 0);
        StoreId gun = StoreChildNamed(&store, trooper, StoreIntern(&store, "gun"));
        StoreId flash = StoreChildNamed(&store, gun, StoreIntern(&store, "flash"));
        Expect(StoreIsLocal(&store, gun) && StoreIsLocal(&store, flash) && !StoreIsLocal(&store, trooper),
               "a child declared :local #t is local, and so is its own child");
        Expect(Get(&store, gun, "mesh", &v) && v.type == STORE_STRING && !strcmp(v.as.str, "pistol.glb"),
               "a -changed handler writes a field of a :local child");
        Expect(True(&store, trooper, "gun-refused"),
               "a tick handler reading a :local child's field gets rule 1's error naming the child");

        // The REPL, input, vectors.
        Expect(Answer("(length (things 'walker))", "2"), "the REPL evaluates in the game environment");
        Expect(Eval("(inspect (game))") && strstr(answer, "(notes . 0)"), "(inspect thing) lists its fields");
        Expect(GameS7ActionCount() == 6 && !strcmp(GameS7ActionName(1), "jump") &&
                   !strcmp(GameS7ActionKey(0), "Mouse1"),
               "define-actions declares actions in order with their keys");
        GameInput input = {(1u << 1) | (1u << 2) | (1u << 5), 1u << 1, 3.0f, -2.0f};
        GameS7SetInput(&input);
        Expect(Answer("(list (held? 'fire) (held? 'jump) (pressed? 'jump) (vx (mouse-motion)))", "(#f #t #t 3.0)"),
               "held?, pressed? and mouse-motion read the tick's input");
        Expect(Answer("(< (vdistance (input-vector 'left 'right 'forward 'back) (vec3 0.7071067811865476 0 -0.7071067811865476)) 1e-9)", "#t"),
               "input-vector is normalised, with forward -z");
        GameS7SetInput(NULL);
        Expect(Answer("(< (vdistance (rotate-y (vec3 0 0 -1) 0.5) (aim 0.5 0)) 1e-9)", "#t") &&
                   Answer("(< (abs (- (heading (rotate-y (vec3 0 0 1) 0.5)) 0.5)) 1e-9)", "#t") &&
                   Answer("(< (abs (- (vlength (spread (vec3 0 0 -2) 0.1)) 2)) 1e-9)", "#t") &&
                   Answer("(vcross (vec3 1 0 0) (vec3 0 1 0))", "#r(0.0 0.0 1.0)"),
               "rotate-y, aim, heading, spread and vcross agree");
        Expect(Eval("(real-time)"), "the clock is there outside gameplay handlers");

        // What must be refused.
        Expect(Refused("(define-kind bad (is nosuch))", "no kind named nosuch"),
               "define-kind with an unknown base names the base");
        Expect(Refused("(spawn 'nothing)", "no kind named nothing"), "spawning an unknown kind is refused");
        Expect(Refused("(spawn 'probe :colour 3)", "probe has no field for :colour"),
               "spawn with an unknown keyword names the kind and the keyword");
        Expect(Refused("(let ((t (spawn 'probe))) (remove t) (t 'power))", "has been removed") &&
                   Answer("(let ((t (spawn 'probe))) (remove t) (object->string t))", "\"#<removed probe>\""),
               "a removed thing refuses access and prints as removed");
        Expect(Refused("(set! ((car (things 'walker)) 'health) 'x)", "health on walker holds int, not symbol"),
               "a symbol into an int field is a type error");
        Expect(Refused("(set! ((car (things 'walker)) 'carried) '(a b c d e))", "carried holds 4 items at most"),
               "a list longer than its capacity is refused");
        Expect(Refused("(set! limit 4)", "can't set! limit: top-level definitions are frozen"),
               "top-level definitions are frozen after load");
        Expect(Refused("(host-game 7777)", "networking comes in phase 2"), "host-game says when");
        Expect(Refused("(define-kind probe (is body) (field label \"\") (field power 0) (field extra 1))",
                       "changed field extra"),
               "declaring a kind again with other fields is refused, naming the field");
        Expect(Eval("(define-kind probe (is body) (field label \"\") (field power 5))") &&
                   Answer("((spawn 'probe) 'power)", "5"),
               "declaring a kind again with the same fields takes its new defaults");

        // A save leaves the local gun out; load-game brings it back from the declaration.
        FILE *probe = fopen("build/core/.game_probe", "w");
        if (probe)
            fclose(probe), remove("build/core/.game_probe");
        const char *path = probe ? "build/core/regression_game.sav" : "regression_game.sav";
        char command[128];
        snprintf(command, sizeof command, "(save-game \"%s\")", path);
        Expect(Eval(command), "(save-game path) saves");
        FILE *saved = fopen(path, "r");
        char text[16384] = "";
        if (saved)
            text[fread(text, 1, sizeof text - 1, saved)] = 0, fclose(saved);
        Expect(saved && strstr(text, "kind trooper") && !strstr(text, "kind gun-model"),
               "a save leaves local children out");
        snprintf(command, sizeof command, "(load-game \"%s\")", path);
        trooper = First(&store, "trooper", 0);
        Expect(Answer(command, "#t") && StoreAlive(&store, trooper) &&
                   StoreIsLocal(&store, gun = StoreChildNamed(&store, trooper, StoreIntern(&store, "gun"))) &&
                   StoreIsLocal(&store, StoreChildNamed(&store, gun, StoreIntern(&store, "flash"))),
               "load-game spawns the missing :local children again");
        GameS7Close();
        StoreFree(&store);
    }
    Expect(rolls[0][0] == rolls[1][0] && rolls[0][1] == rolls[1][1] && rolls[0][2] == rolls[1][2] &&
               !(rolls[0][0] == rolls[0][1] && rolls[0][1] == rolls[0][2]),
           "random draws the same numbers from the same seed in two runs");
    Expect(hashes[0] == hashes[1], "two runs of the walk reach the same store hash");
    GameS7Close();
    StoreFree(&store);
}

// Each rule file breaks one rule in a handler; the reported line carries A4's first sentence.
static void RuleChecks(void)
{
    static const struct
    {
        const char *file, *sentence;
    } rules[] = {
        {"tests/regression/rules/rule1_read.scm",
         "soldier #0 tick: hurt is a local field (this screen only). The tick handler of soldier "
         "can't read it, because other players and replays don't have it."},
        {"tests/regression/rules/rule1_write.scm",
         "soldier #0 frame: mesh on gun is shared state; a presentation handler can't write it."},
        {"tests/regression/rules/rule2.scm",
         "soldier #0 tick: can't set! score: top-level definitions are frozen once the game has "
         "loaded, because they aren't saved, sent to other players or replayed."},
        {"tests/regression/rules/rule3.scm", "soldier #0 tick: real-time is for presentation."},
        {"tests/regression/rules/rule4.scm",
         "grenade #0 tick: can't store a procedure in on-hit: fields hold data so they can be saved "
         "and sent."},
        {"tests/regression/rules/rule5.scm",
         "soldier #0 tick: health on soldier #1 belongs to player 2, and this handler runs for "
         "player 1, so it can't write it."},
    };
    for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++)
    {
        Store store;
        errorCount = 0;
        lastError[0] = 0;
        bool opened = StoreInit(&store, 7) && GameS7Open(&store, "core/scheme/kinds.scm");
        bool loaded = opened && GameS7LoadGame(rules[i].file);
        if (loaded)
        {
            StoreTick(&store, 1.0f / 60.0f);
            StoreFrame(&store, 1.0f / 60.0f);
        }
        char what[256];
        snprintf(what, sizeof what, "%s reports its rule's sentence", rules[i].file);
        bool ok = loaded && errorCount >= 1 && !strncmp(lastError, rules[i].sentence, strlen(rules[i].sentence)) &&
                  strstr(lastError, rules[i].file + strlen("tests/regression/rules/"));
        Expect(ok, what);
        if (!ok)
            printf("  got: %s\n", lastError);
        GameS7Close();
        StoreFree(&store);
    }
}

int GameChecks(void)
{
    failures = 0;
    GameS7SetErrorSink(Sink);
    WalkChecks();
    RuleChecks();
    GameS7SetErrorSink(NULL);
    Store store;
    Expect(StoreInit(&store, 1) && !GameS7Open(&store, "core/scheme/no-such-prelude.scm"),
           "a missing prelude is refused");
    GameS7Close();
    StoreFree(&store);
    return failures;
}
