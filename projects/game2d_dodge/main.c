/* Falling Dodge: a minimal 2D game exercising only core/engine.h. The engine
   has no 2D-specific module (no sprite batching, no 2D camera, no collision
   helpers) so this is built entirely on raylib's own 2D primitives plus the
   engine's loop/input layer. The game-over screen is a UiDocument authored in
   the core UI editor (projects/ui_tool) and saved as menu.ui; the game just
   loads and draws it with the core/ui runtime instead of hand-drawn text. */
#include "core/engine.h"
#include "core/ui.h"
#include "core/ui_document.h"
#include "raymath.h"
#include <stdio.h>
#include <stdlib.h>

#define SCREEN_W 640
#define SCREEN_H 480
#define MAX_BLOCKS 24
#define MENU_PATH "projects/game2d_dodge/menu.ui"

typedef struct Block
{
    Vector2 pos, prevPos;
    float speed;
    bool alive;
} Block;

typedef struct Game
{
    float playerX, playerPrevX;
    Block blocks[MAX_BLOCKS];
    double spawnTimer, spawnPeriod;
    double survived;
    int lives;
    bool over, restartRequested;
    UiContext ui;
    UiDocument menu;
    EngineInput frameInput;
} Game;

static void SpawnBlock(Game *g)
{
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (!g->blocks[i].alive)
        {
            float x = (float)(GetRandomValue(10, SCREEN_W - 30));
            g->blocks[i] = (Block){{x, -20}, {x, -20}, 120.0f + (float)GetRandomValue(0, 120), true};
            return;
        }
}

static void ResetGame(Game *g)
{
    UiContext ui = g->ui;
    UiDocument menu = g->menu;
    *g = (Game){0};
    g->ui = ui;
    g->menu = menu;
    g->playerX = g->playerPrevX = SCREEN_W / 2.0f;
    g->spawnPeriod = 0.6;
    g->lives = 3;
}

static bool Init(void *context)
{
    Game *g = context;
    if (!UiInit(&g->ui, UiThemeDefault()))
        return false;
    if (!UiDocumentLoad(&g->menu, MENU_PATH))
        return false;
    ResetGame(g);
    return true;
}

static void FrameInput(void *context, const EngineInput *frame)
{
    Game *g = context;
    g->frameInput = *frame;
}

static bool Update(void *context, double dt, const EngineInput *in)
{
    Game *g = context;
    if (in->pressed[KEY_ESCAPE])
        return false;
    if (g->over)
    {
        if (in->pressed[KEY_R] || g->restartRequested)
            ResetGame(g);
        return true;
    }
    g->survived += dt;
    g->playerPrevX = g->playerX;
    float move = (float)(in->down[KEY_D] - in->down[KEY_A] + in->down[KEY_RIGHT] - in->down[KEY_LEFT]);
    g->playerX += move * 260.0f * (float)dt;
    g->playerX = Clamp(g->playerX, 20, SCREEN_W - 20);
    g->spawnTimer += dt;
    if (g->spawnTimer >= g->spawnPeriod)
    {
        g->spawnTimer = 0;
        g->spawnPeriod = fmaxf(0.22f, (float)g->spawnPeriod - 0.01f);
        SpawnBlock(g);
    }
    Rectangle player = {g->playerX - 22, SCREEN_H - 40, 44, 18};
    for (int i = 0; i < MAX_BLOCKS; i++)
    {
        Block *b = &g->blocks[i];
        if (!b->alive)
            continue;
        b->prevPos = b->pos;
        b->pos.y += b->speed * (float)dt;
        Rectangle blockRect = {b->pos.x - 10, b->pos.y - 10, 20, 20};
        if (CheckCollisionRecs(player, blockRect))
        {
            b->alive = false;
            g->lives--;
            if (g->lives <= 0)
                g->over = true;
            continue;
        }
        if (b->pos.y > SCREEN_H + 20)
            b->alive = false;
    }
    return true;
}

static void Draw(void *context, float alpha)
{
    Game *g = context;
    ClearBackground((Color){18, 18, 24, 255});
    float playerX = Lerp(g->playerPrevX, g->playerX, alpha);
    for (int i = 0; i < MAX_BLOCKS; i++)
    {
        Block *b = &g->blocks[i];
        if (!b->alive)
            continue;
        Vector2 pos = Vector2Lerp(b->prevPos, b->pos, alpha);
        DrawRectangle((int)pos.x - 10, (int)pos.y - 10, 20, 20, (Color){220, 90, 70, 255});
    }
    DrawRectangle((int)playerX - 22, SCREEN_H - 40, 44, 18, (Color){90, 200, 230, 255});
    char hud[64];
    snprintf(hud, sizeof hud, "Lives: %d   Survived: %.1fs", g->lives, g->survived);
    DrawText(hud, 12, 10, 20, WHITE);
    DrawText("A/D or arrows move | Esc quit", 12, SCREEN_H - 22, 16, LIGHTGRAY);
    if (g->over)
    {
        DrawRectangle(0, 0, SCREEN_W, SCREEN_H, (Color){0, 0, 0, 140});
        UiRect screen = {0, 0, SCREEN_W, SCREEN_H};
        UiBeginFrame(&g->ui, &g->frameInput, screen);
        UiRect menu = UiDocumentOuterRect(&g->ui, &g->menu, (Vector2){0, 0});
        Vector2 origin = {(float)((SCREEN_W - menu.width) / 2), (float)((SCREEN_H - menu.height) / 2)};
        bool clicked = UiDocumentDraw(&g->ui, &g->menu, origin, true);
        UiEndFrame(&g->ui);
        if (clicked)
            g->restartRequested = true;
    }
}

static void Shutdown(void *context)
{
    Game *g = context;
    UiDocumentFree(&g->menu);
    UiFree(&g->ui);
}

int main(void)
{
    Game game = {0};
    EngineConfig config = {
        .title = "Falling Dodge (2D)",
        .width = SCREEN_W,
        .height = SCREEN_H,
        .targetFps = 60,
        .fixed_dt = 1.0 / 60.0,
        .max_frame_dt = 0.25,
        .windowFlags = 0,
    };
    EngineProject hooks = {Init, FrameInput, Update, Draw, Shutdown};
    return EngineRun(&config, &hooks, &game);
}
