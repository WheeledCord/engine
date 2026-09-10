/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Cube Collector: a minimal 3D game exercising only core/engine.h's loop, input
   buffering and fixed-timestep interpolation. No models, shaders or core render
   helpers are used deliberately, to see what a bare-bones 3D game looks like on
   top of just the engine's window/loop/input layer. */
#include "core/engine.h"
#include "raymath.h"
#include <stdio.h>

#define COIN_COUNT 8

typedef struct Game
{
    Vector3 playerPrev, player;
    Vector3 coins[COIN_COUNT];
    bool coinAlive[COIN_COUNT];
    int score;
    double timeLeft;
    bool won, lost;
} Game;

static bool Init(void *context)
{
    Game *g = context;
    g->player = g->playerPrev = (Vector3){0, 0.5f, 0};
    g->timeLeft = 30.0;
    Vector3 spots[COIN_COUNT] = {
        {5, 0.5f, 5},   {-5, 0.5f, 5},  {5, 0.5f, -5},  {-5, 0.5f, -5},
        {8, 0.5f, 0},   {-8, 0.5f, 0},  {0, 0.5f, 8},   {0, 0.5f, -8},
    };
    for (int i = 0; i < COIN_COUNT; i++)
    {
        g->coins[i] = spots[i];
        g->coinAlive[i] = true;
    }
    return true;
}

static void FrameInput(void *context, const EngineInput *frame)
{
    (void)context;
    (void)frame;
}

static bool Update(void *context, double dt, const EngineInput *in)
{
    Game *g = context;
    if (in->pressed[KEY_ESCAPE])
        return false;
    if (g->won || g->lost)
        return true;
    g->timeLeft -= dt;
    if (g->timeLeft <= 0)
    {
        g->timeLeft = 0;
        g->lost = true;
    }
    Vector3 move = {(float)(in->down[KEY_D] - in->down[KEY_A]), 0,
                    (float)(in->down[KEY_S] - in->down[KEY_W])};
    if (Vector3LengthSqr(move) > 0)
        move = Vector3Scale(Vector3Normalize(move), 4.0f * (float)dt);
    g->playerPrev = g->player;
    g->player = Vector3Add(g->player, move);
    g->player.x = Clamp(g->player.x, -9.5f, 9.5f);
    g->player.z = Clamp(g->player.z, -9.5f, 9.5f);
    int remaining = 0;
    for (int i = 0; i < COIN_COUNT; i++)
    {
        if (!g->coinAlive[i])
            continue;
        remaining++;
        if (Vector3Distance(g->player, g->coins[i]) < 0.8f)
        {
            g->coinAlive[i] = false;
            g->score++;
            remaining--;
        }
    }
    if (remaining == 0)
        g->won = true;
    return true;
}

static void Draw(void *context, float alpha)
{
    Game *g = context;
    Vector3 player = Vector3Lerp(g->playerPrev, g->player, alpha);
    ClearBackground((Color){20, 24, 30, 255});
    Camera3D camera = {
        .position = (Vector3){0, 14, 14},
        .target = (Vector3){0, 0, 0},
        .up = (Vector3){0, 1, 0},
        .fovy = 45,
        .projection = CAMERA_PERSPECTIVE,
    };
    BeginMode3D(camera);
    DrawPlane((Vector3){0, 0, 0}, (Vector2){20, 20}, (Color){40, 60, 40, 255});
    DrawGrid(20, 1);
    for (int i = 0; i < COIN_COUNT; i++)
        if (g->coinAlive[i])
            DrawSphere(g->coins[i], 0.4f, (Color){240, 200, 40, 255});
    DrawCube(player, 0.9f, 0.9f, 0.9f, (Color){60, 140, 240, 255});
    DrawCubeWires(player, 0.9f, 0.9f, 0.9f, WHITE);
    EndMode3D();
    char hud[64];
    snprintf(hud, sizeof hud, "Score: %d/%d   Time: %.1f", g->score, COIN_COUNT, g->timeLeft);
    DrawText(hud, 16, 12, 20, WHITE);
    DrawText("WASD move | Esc quit", 16, GetScreenHeight() - 28, 16, LIGHTGRAY);
    if (g->won)
        DrawText("YOU WIN", GetScreenWidth() / 2 - 90, GetScreenHeight() / 2 - 20, 40, (Color){240, 200, 40, 255});
    if (g->lost)
        DrawText("TIME UP", GetScreenWidth() / 2 - 90, GetScreenHeight() / 2 - 20, 40, (Color){220, 80, 80, 255});
}

static void Shutdown(void *context)
{
    (void)context;
}

int main(void)
{
    Game game = {0};
    EngineConfig config = {
        .title = "Cube Collector (3D)",
        .width = 960,
        .height = 640,
        .targetFps = 60,
        .fixed_dt = 1.0 / 60.0,
        .max_frame_dt = 0.25,
        .windowFlags = 0,
    };
    EngineProject hooks = {Init, FrameInput, Update, Draw, Shutdown};
    return EngineRun(&config, &hooks, &game);
}
