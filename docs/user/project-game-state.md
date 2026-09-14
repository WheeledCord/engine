# Project game state and systems

The engine owns the application loop. Your game owns its state: rules, score, current level, transient entities, and persistence. Put that state behind the `EngineApplication.context` pointer (or `GameplayProject.context` if using the gameplay adapter), rather than in a global or an engine singleton.

This complete small arena pattern shows the intended boundary. It uses a collision-query world to find pickups, but the project decides what collection means.

```c
typedef struct ArenaState {
    GameplayWorld entities;          /* transient actors for this session */
    Collision2DWorld queries;        /* transient index, rebuilt from actors */
    CoreCamera2D camera;
    int score;
    bool paused;
    char savePath[256];              /* persistent-data boundary, not entity state */
} ArenaState;

static bool ArenaInit(void *context) {
    ArenaState *game = context;
    *game = (ArenaState){.camera = CoreCamera2DDefault()};
    return GameplayWorldInit(&game->entities, (GameplayWorldConfig){128, 1.0 / 60.0}) &&
           Collision2DWorldInit(&game->queries, 128, 64.0f) && ArenaSpawnRound(game);
}

static bool ArenaUpdate(void *context, double dt, const EngineInput *input) {
    ArenaState *game = context;
    if (input->pressed[KEY_R]) ArenaResetRound(game);       /* destroys only transient round data */
    if (game->paused) return true;
    ArenaMoveActors(game, dt, input);                       /* updates Collision2DWorldSetShape */
    Collision2DHit collected[8];
    int count = Collision2DQueryCircle(&game->queries, ArenaPlayerPosition(game), 12,
                                       LAYER_PICKUP, collected, 8);
    for (int i = 0; i < count; i++) ArenaCollectPickup(game, collected[i].user, &game->score);
    GameplayWorldStepInput(&game->entities, input);
    CoreCamera2DFollow(&game->camera, ArenaPlayerPosition(game), (float)dt);
    return true;
}
```

Attach `ArenaState` as the application context. `BuildUi` runs before updates, so its capture result prevents UI clicks and typing from reaching `ArenaUpdate`; keep UI policy there, not in entity callbacks.

```c
static void ArenaBuildUi(void *context, UiContext *ui) {
    ArenaState *game = context;
    UiLabel(ui, (UiRect){12, 12, 180, 24}, TextFormat("Score: %d", game->score));
    if (UiButton(ui, (UiRect){12, 44, 120, 28}, "Pause")) game->paused = !game->paused;
}

EngineApplication app = EngineApplicationDefault();
app.context = &arena;
app.callbacks = (EngineProject){.Init = ArenaInit, .Update = ArenaUpdate,
                                .Draw = ArenaDraw, .Shutdown = ArenaShutdown};
app.ui = &ui;
app.BuildUi = ArenaBuildUi;
```

Treat a save file as a deliberate boundary. Serialize durable facts such as level identifier, score, unlocked items, and player checkpoint. On load, validate the file, set those facts, then call the same round-spawn function used for a reset. Do not serialize raw entity addresses, `Collision2DHandle` values, queued UI state, or the spatial hash; all are transient implementation details.

```text
load durable facts -> validate -> ArenaSpawnRound -> register collision proxies
reset round        -> clear actors -> ArenaSpawnRound -> preserve score if rules say so
shutdown           -> save durable facts -> Collision2DWorldFree -> GameplayWorldFree
```

The integration test at `tests/integration/gameplay/main.c` exercises the associated entity/system, input-capture, camera, and query contracts. This is intentionally a documented pattern instead of a bundled game project: games belong outside the engine repository.
