/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/animation.h"
#include "core/engine.h"
#include "core/file.h"
#include "core/fps_camera.h"
#include "core/frame_uniforms.h"
#include "core/math.h"
#include "core/mesh_builder.h"
#include "core/render_target.h"
#include "core/shader.h"
#include "core/ui.h"
#include "core/ui_containers.h"
#include "core/ui_menu.h"
#include "core/viewmodel.h"
#include "core/world_draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
    SKIN,
    TEXTURED,
    DEPTH,
    SHADER_COUNT
};
typedef struct Project
{
    Shader shaders[SHADER_COUNT];
    FrameUniforms uniforms;
    Model character, ground;
    ModelAnimation *animations;
    ActorClip *clips;
    int animationCount;
    Actor actor;
    ActorAimJoint aimJoint;
    FpsCamera camera;
    FpsCameraConfig cameraConfig;
    RenderTexture scene, depthView;
    float nearPlane, farPlane;
    int width, height, draws, updates, failures, activeClip;
    bool smoke, showDepth, capture, showViewmodel, look, hide, fix, noclip;
    unsigned char *firstImage;
    float *originalVertices;
    size_t vertexBytes;
    UiContext ui;
    UiPanel panel;
    UiDock dock;
    UiDockTile dockTiles[2];
    EngineInput uiInput;
    float uiSlider;
    bool uiCheckbox, uiPaused;
} Project;
static void Check(Project *p, bool ok, const char *message)
{
    TraceLog(ok ? LOG_INFO : LOG_ERROR, "CHECK %s: %s", message, ok ? "PASS" : "FAIL");
    if (!ok)
        p->failures++;
}

static void MenuToggleDepth(void *user)
{
    Project *p = user;
    p->showDepth = !p->showDepth;
    p->uiCheckbox = p->showDepth;
}

static void MenuPlayIdle(void *user)
{
    Project *p = user;
    p->activeClip = 1;
    p->uiPaused = false;
    ActorPlay(&p->actor, 1, true);
}

static void MenuPlayMove(void *user)
{
    Project *p = user;
    p->activeClip = 2;
    p->uiPaused = false;
    ActorPlay(&p->actor, 2, true);
}

static void MenuPlayAttack(void *user)
{
    Project *p = user;
    p->activeClip = 3;
    p->uiPaused = false;
    ActorPlay(&p->actor, 3, false);
}

static void MenuResetPanel(void *user)
{
    Project *p = user;
    p->panel.rect.x = 24;
    p->panel.rect.y = 64;
}

static const UiMenuItem animationMenuItems[] = {
    {"Idle", true, NULL, MenuPlayIdle},
    {"Move", true, NULL, MenuPlayMove},
    {"Attack once", true, NULL, MenuPlayAttack},
};
static const UiMenu animationMenu = {animationMenuItems, 3, 150, NULL};
static const UiMenuItem displayMenuItems[] = {
    {"Depth preview", true, NULL, MenuToggleDepth},
    {"Animation", true, &animationMenu, NULL},
};
static const UiMenu displayMenu = {displayMenuItems, 2, 170, NULL};
static const UiMenuItem rootMenuItems[] = {
    {"Display", true, &displayMenu, NULL},
    {"Reset panel", true, NULL, MenuResetPanel},
    {"Unavailable", false, NULL, NULL},
};
static const UiMenu rootMenu = {rootMenuItems, 3, 170, NULL};
static void InputCheck(Project *p)
{
    EngineInput pending = {0}, frame = {0};
    frame.mouseDelta = (Vector2){3, 4};
    frame.pressed[KEY_SPACE] = true;
    frame.down[KEY_W] = true;
    EngineInputAccumulate(&pending, &frame);
    frame.mouseDelta = (Vector2){2, -1};
    frame.pressed[KEY_SPACE] = false;
    EngineInputAccumulate(&pending, &frame);
    Check(p, pending.mouseDelta.x == 5 && pending.mouseDelta.y == 3 && pending.pressed[KEY_SPACE],
          "input accumulated through zero-update frames");
    EngineInputDrain(&pending);
    Check(p,
          !pending.pressed[KEY_SPACE] && pending.mouseDelta.x == 0 && pending.mouseDelta.y == 0 &&
              pending.down[KEY_W],
          "later updates see held keys, no repeated edge or mouse delta");
}

static void DrawPlayButton(UiContext *ui, UiRect content, void *user)
{
    Project *p = user;
    if (UiButtonBare(ui, content, "Play move"))
    {
        p->activeClip = 2;
        ActorPlay(&p->actor, 2, true);
    }
}

static void DrawPanelContents(UiContext *ui, UiRect content, void *user)
{
    Project *p = user;
    int gap = ui->theme.containerGap;
    UiRect row = {content.x + gap, content.y + gap, content.width - gap * 2,
                  ui->theme.itemHeight};
    UiLabel(ui, row, "Immediate widgets");
    row.y += row.height + gap;
    UiRect buttonWell = {row.x, row.y, row.width, ui->theme.itemHeight + ui->theme.indentWidth * 2};
    UiIndent(ui, buttonWell, DrawPlayButton, p);
    row.y += buttonWell.height + gap;
    UiSlider(ui, row, &p->uiSlider, 0.0f, 1.0f);
    row.y += row.height + gap;
    if (UiCheckbox(ui, row, "Show depth", &p->uiCheckbox))
        p->showDepth = p->uiCheckbox;
}

static void DrawStatusBar(UiContext *ui, UiRect content, void *user)
{
    Project *p = user;
    int filled = (int)lroundf(content.width * p->uiSlider);
    DrawRectangle(content.x, content.y, content.width, content.height, ui->theme.darkGrey);
    DrawRectangle(content.x, content.y, filled, content.height, ui->theme.black);
}

static void DrawStatusTile(UiContext *ui, UiRect content, void *user)
{
    Project *p = user;
    UiLabel(ui, (UiRect){content.x + 5, content.y + 4, content.width - 10, ui->theme.itemHeight},
            "GL 2.1");
    int gap = ui->theme.containerGap;
    UiRect well = {content.x + gap, content.y + content.height - gap - 9,
                   content.width - gap * 2, 9};
    UiIndent(ui, well, DrawStatusBar, p);
}

static void DrawTransportButtons(UiContext *ui, UiRect content, void *user)
{
    Project *p = user;
    int side = content.height;
    UiRect button = {content.x, content.y, side, side};
    if (UiButtonBare(ui, button, "<"))
    {
        p->activeClip = (p->activeClip + p->animationCount - 1) % p->animationCount;
        p->uiPaused = false;
        ActorPlay(&p->actor, p->activeClip, true);
    }
    button.x += side;
    if (UiButtonBare(ui, button, "||"))
        p->uiPaused = !p->uiPaused;
    button.x += side;
    if (UiButtonBare(ui, button, ">"))
    {
        p->activeClip = (p->activeClip + 1) % p->animationCount;
        p->uiPaused = false;
        ActorPlay(&p->actor, p->activeClip, true);
    }
}

static void DrawTransportTile(UiContext *ui, UiRect content, void *user)
{
    UiLabel(ui, (UiRect){content.x + 5, content.y + 3, content.width - 10, ui->theme.itemHeight},
            "ANIM");
    int gap = ui->theme.containerGap;
    int available = content.width - gap * 2;
    int side = (available - ui->theme.indentWidth * 2) / 3;
    UiRect well = {content.x + gap,
                   content.y + content.height - gap - side - ui->theme.indentWidth * 2,
                   side * 3 + ui->theme.indentWidth * 2,
                   side + ui->theme.indentWidth * 2};
    UiIndent(ui, well, DrawTransportButtons, user);
}

static bool Resize(Project *p)
{
    int w = GetRenderWidth(), h = GetRenderHeight();
    if (w < 1 || h < 1)
        return true;
    if (w == p->width && h == p->height)
        return true;
    RenderTexture scene = {0}, depth = {0};
    if (!MakeRT(&scene, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, true))
        return false;
    if (!MakeRT(&depth, w, h, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false))
    {
        CoreUnloadRT(&scene);
        return false;
    }
    CoreUnloadRT(&p->scene);
    CoreUnloadRT(&p->depthView);
    p->scene = scene;
    p->depthView = depth;
    p->width = w;
    p->height = h;
    free(p->firstImage);
    p->firstImage = NULL;
    return true;
}
static void PoseCheck(Project *p)
{
    Actor *a = &p->actor;
    ActorPoseFrames(a, 2, 0, 0, 0);
    ActorUploadPose(a, 1);
    Matrix own[CORE_BONE_CAPACITY];
    memcpy(own, a->matrices, sizeof own);
    UpdateModelAnimationBones(p->character, p->animations[2], 0);
    float maximum = 0;
    for (int b = 0; b < a->skeleton.count; b++)
    {
        float16 x = MatrixToFloatV(own[b]), y = MatrixToFloatV(p->character.meshes[0].boneMatrices[b]);
        for (int k = 0; k < 16; k++)
            maximum = fmaxf(maximum, fabsf(x.v[k] - y.v[k]));
    }
    Check(p, maximum < 0.001f, "raymath pose matrices match raylib reference");
    ActorPlay(a, 2, true);
    ActorUpdate(a, 0.2f);
    ActorBlendToIdle(a, 0, 0, 0.1f);
    ActorUpdate(a, 0.2f);
    Transform held = a->current[0];
    ActorUpdate(a, 0.5f);
    Check(p, ActorAnimDone(a) && !memcmp(&held, &a->current[0], sizeof held),
          "blend holds explicitly selected destination frame");
    ActorPlay(a, 3, false);
    ActorUpdate(a, 20);
    Check(p, ActorAnimDone(a), "one-shot clip completion");
    ActorPlay(a, 2, true);
    Transform globals[CORE_BONE_CAPACITY];
    RigToGlobal(&a->skeleton, a->current, globals);
    int bone = ActorBone(a, "body_up");
    Vector3 pivot = globals[bone].translation;
    RigBoneLook(&a->skeleton, globals, bone, 0.5f, 0.2f, (Vector3){0, 1, 0}, (Vector3){1, 0, 0});
    Check(p, Vector3Distance(pivot, globals[bone].translation) < 0.0001f,
          "look rotation preserves posed pivot");
    RigBoneHide(&a->skeleton, globals, bone);
    bool collapsed = true;
    for (int b = 0; b < a->skeleton.count; b++)
        if (RigIsDescendant(&a->skeleton, b, bone))
            collapsed &= Vector3Distance(globals[b].translation, pivot) < 0.0001f &&
                         Vector3LengthSqr(globals[b].scale) == 0;
    Check(p, collapsed, "hide collapses selected bone chain");
}
static bool Init(void *context)
{
    Project *p = context;
    const ShaderFile shaders[] = {{"core/shaders/skinning.vs", "core/shaders/textured.fs"},
                                  {NULL, "core/shaders/textured.fs"},
                                  {NULL, "projects/core_test/shaders/depth_preview.fs"}};
    if (!CoreLoadShaders(shaders, SHADER_COUNT, p->shaders))
        return false;
    if (p->shaders[SKIN].locs[SHADER_LOC_BONE_MATRICES] < 0 ||
        p->shaders[SKIN].locs[SHADER_LOC_VERTEX_BONEIDS] < 0 ||
        p->shaders[SKIN].locs[SHADER_LOC_VERTEX_BONEWEIGHTS] < 0)
    {
        TraceLog(LOG_ERROR, "Required GPU skinning interface absent");
        return false;
    }
    FrameUniformDecl uniforms[] = {{"nearPlane", FRAME_FLOAT, 1},
                                   {"farPlane", FRAME_FLOAT, 1},
                                   {"unusedDeclaredUniform", FRAME_VEC3, 1}};
    if (!FrameUniformsInit(&p->uniforms, uniforms, 3))
        return false;
    for (int i = 0; i < SHADER_COUNT; i++)
        if (!FrameUniformsAdd(&p->uniforms, p->shaders[i]))
            return false;
    CoreDepthRange(&p->nearPlane, &p->farPlane);
    char asset[1024];
    const char *model = CoreResolvePath("projects/core_test/assets/greenman.glb", asset, sizeof asset);
    p->character = model ? LoadModel(model) : (Model){0};
    p->animations = model ? LoadModelAnimations(model, &p->animationCount) : NULL;
    if (p->character.meshCount < 1 || p->animationCount < 4)
        return false;
    p->clips = calloc((size_t)p->animationCount, sizeof(*p->clips));
    if (!p->clips)
        return false;
    /* Explicit rate: this raylib glTF loader samples clips every 17 milliseconds. No core format policy. */
    for (int i = 0; i < p->animationCount; i++)
        p->clips[i] = (ActorClip){&p->animations[i], 1000.0f / 17.0f};
    if (!ActorInit(&p->actor, &p->character, p->clips, p->animationCount))
        return false;
    for (int i = 0; i < p->character.materialCount; i++)
        p->character.materials[i].shader = p->shaders[SKIN];
    bool textured = false;
    for (int i = 0; i < p->character.materialCount; i++)
    {
        Texture t = p->character.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        if (t.width > 1 && t.height > 1)
            textured = true;
    }
    Check(p, textured, "native model contains an actual diffuse texture");
    p->aimJoint = (ActorAimJoint){ActorBone(&p->actor, "body_up"), 1};
    if (p->aimJoint.bone < 0)
        return false;
    p->actor.aim.joints = &p->aimJoint;
    p->actor.aim.count = 1;
    p->actor.corrections[p->aimJoint.bone] = QuaternionFromAxisAngle((Vector3){0, 0, 1}, 0.25f);
    MB mb;
    if (!MBInit(&mb, 0))
        return false;
    Vector3 positions[] = {{-12, -0.005f, -12}, {-12, -0.005f, 12}, {12, -0.005f, -12}, {12, -0.005f, 12}};
    Vector2 uv[] = {{0, 0}, {0, 12}, {12, 0}, {12, 12}};
    bool built = EmitQuadN(&mb, positions, (Vector3){0, 1, 0}, uv) && MBModel(&mb, &p->ground);
    MBFree(&mb);
    if (!built)
        return false;
    p->ground.materials[0].shader = p->shaders[TEXTURED];
    p->cameraConfig = FpsCameraDefaults();
    FpsCameraInit(&p->camera, (Vector3){4, 2.8f, 5}, -2.46685f, -0.27f, p->cameraConfig.fov);
    p->noclip = true;
    p->showDepth = true;
    p->capture = !p->smoke;
    if (p->capture)
        DisableCursor();
    if (!Resize(p))
        return false;
    RenderTexture onlyDepth = {0};
    Check(p, MakeRT(&onlyDepth, 32, 32, 0, true), "depth-only target completeness");
    CoreUnloadRT(&onlyDepth);
    p->activeClip = 2;
    ActorPlay(&p->actor, 2, true);
    p->uiSlider = 0.65f;
    p->uiCheckbox = p->showDepth;
    p->panel = (UiPanel){{24, 64, 260, 145}, true};
    p->dockTiles[0] = (UiDockTile){{0}, DrawStatusTile, p};
    p->dockTiles[1] = (UiDockTile){{0}, DrawTransportTile, p};
    p->dock = (UiDock){0, 232, 72, p->dockTiles, 2};
    if (!UiInit(&p->ui, UiThemeDefault()))
        return false;
    p->vertexBytes = (size_t)p->character.meshes[0].vertexCount * 3 * sizeof(float);
    p->originalVertices = malloc(p->vertexBytes);
    if (!p->originalVertices)
        return false;
    memcpy(p->originalVertices, p->character.meshes[0].animVertices, p->vertexBytes);
    if (p->smoke)
    {
        InputCheck(p);
        PoseCheck(p);
    }
    return p->failures == 0 && CoreCheckGraphicsErrors("project initialization");
}
static void FrameInput(void *context, const EngineInput *frame)
{
    Project *p = context;
    p->uiInput = *frame;
    if (frame->pressed[KEY_TAB])
    {
        p->capture = !p->capture;
        if (p->capture)
            DisableCursor();
        else
            EnableCursor();
    }
}
static float GroundHeight(void *context, float x, float z)
{
    (void)context;
    (void)x;
    (void)z;
    return 0;
}
static bool Update(void *context, double dt, const EngineInput *in)
{
    Project *p = context;
    p->updates++;
    if (p->failures || in->pressed[KEY_ESCAPE] || (p->smoke && p->draws >= 100))
        return false;
    if (in->pressed[KEY_F1])
        p->showDepth = !p->showDepth;
    if (in->pressed[KEY_V])
        p->showViewmodel = !p->showViewmodel;
    if (in->pressed[KEY_L])
        p->look = !p->look;
    if (in->pressed[KEY_H])
        p->hide = !p->hide;
    if (in->pressed[KEY_F])
        p->fix = !p->fix;
    if (in->pressed[KEY_N])
        p->noclip = !p->noclip;
    if (in->pressed[KEY_ONE])
    {
        p->activeClip = (p->activeClip + 1) % p->animationCount;
        ActorPlay(&p->actor, p->activeClip, true);
    }
    if (in->pressed[KEY_TWO])
        ActorPlay(&p->actor, 3, false);
    if (in->pressed[KEY_THREE])
        ActorBlendToIdle(&p->actor, 0, 0, 0.3f);
    FpsInput input = {0};
    input.noclip = p->noclip;
    if (!p->smoke)
    {
        input.move =
            (Vector3){in->down[KEY_D] - in->down[KEY_A], in->down[KEY_SPACE] - in->down[KEY_LEFT_CONTROL],
                      in->down[KEY_W] - in->down[KEY_S]};
        input.lookDelta = p->capture ? in->mouseDelta : (Vector2){0};
        input.sprint = in->down[KEY_LEFT_SHIFT];
        input.crouch = in->down[KEY_C];
    }
    FpsCameraUpdate(&p->camera, &p->cameraConfig, input, (FpsWorld){NULL, NULL, GroundHeight}, (float)dt);
    if (p->look)
        ActorLookAt(&p->actor, (Vector3){0, 1.2f, 0}, p->camera.position, 0);
    else
        p->actor.targetYaw = p->actor.targetPitch = 0;
    p->actor.hidden[p->aimJoint.bone] = p->hide;
    p->actor.corrected[p->aimJoint.bone] = p->fix;
    if (!p->uiPaused)
        ActorUpdate(&p->actor, (float)dt);
    return true;
}
static void DrawHeld(void *context)
{
    WorldSurf *held = context;
    DrawWorldSurfaces(held, 1, NULL);
}
static void Readback(Project *p)
{
    Image color = LoadImageFromTexture(p->scene.texture);
    ImageFlipVertical(&color);
    Color *pixels = LoadImageColors(color);
    size_t n = (size_t)color.width * (size_t)color.height;
    if (!p->firstImage)
    {
        p->firstImage = malloc(n * sizeof(Color));
        if (p->firstImage)
            memcpy(p->firstImage, pixels, n * sizeof(Color));
        ExportImage(color, "build/core/scene0.png");
    }
    else
    {
        int changed = 0;
        for (size_t i = 0; i < n; i++)
            if (p->firstImage && memcmp(p->firstImage + i * sizeof(Color), pixels + i, 3))
                changed++;
        TraceLog(LOG_INFO, "Animated scene changed pixels: %d", changed);
        Check(p, changed > 100, "textured animation changes rendered pixels");
        ExportImage(color, "build/core/scene1.png");
        Check(p, !memcmp(p->originalVertices, p->character.meshes[0].animVertices, p->vertexBytes),
              "GPU animation leaves CPU animated vertices unchanged");
        Image depth = LoadImageFromTexture(p->depthView.texture);
        ImageFlipVertical(&depth);
        Color *d = LoadImageColors(depth);
        int low = 255, high = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (d[i].r < low)
                low = d[i].r;
            if (d[i].r > high)
                high = d[i].r;
        }
        Check(p, high - low > 50, "sampled scene depth has foreground and background");
        ExportImage(depth, "build/core/depth.png");
        UnloadImageColors(d);
        UnloadImage(depth);
    }
    UnloadImageColors(pixels);
    UnloadImage(color);
}
static void Draw(void *context, float alpha)
{
    Project *p = context;
    if (!Resize(p))
    {
        p->failures++;
        return;
    }
    Vector3 unused = {1, 2, 3};
    const void *values[] = {&p->nearPlane, &p->farPlane, &unused};
    FrameUniformsBind(&p->uniforms, values);
    Camera camera = FpsCameraInterpolated(&p->camera, alpha);
    ActorUploadPose(&p->actor, alpha);
    BeginTextureMode(p->scene);
    ClearBackground((Color){24, 31, 45, 255});
    BeginMode3D(camera);
    WorldSurf surfaces[] = {{&p->ground, MatrixIdentity(), (Color){105, 122, 140, 255}},
                            {&p->character, MatrixIdentity(), WHITE}};
    DrawWorldSurfaces(surfaces, 2, NULL);
    DrawGrid(24, 1);
    if (p->showViewmodel)
    {
        Vector3 offset;
        Quaternion rotation;
        FpsViewmodelInterpolated(&p->camera, alpha, &offset, &rotation);
        offset = Vector3Add(offset, (Vector3){0.35f, -0.5f, 1.2f});
        Matrix transform = MatrixMultiply(
            MatrixMultiply(MatrixScale(0.3f, 0.3f, 0.3f), QuaternionToMatrix(rotation)),
            ViewmodelTransform(camera.position, Vector3Subtract(camera.target, camera.position), camera.up,
                               offset));
        WorldSurf held = {&p->character, transform, WHITE};
        DrawViewmodel((ViewmodelProjection){54, (float)p->width / p->height, 0.1f}, DrawHeld, &held);
    }
    EndMode3D();
    EndTextureMode();
    BeginTextureMode(p->depthView);
    ClearBackground(BLACK);
    BeginShaderMode(p->shaders[DEPTH]);
    DrawTexturePro(p->scene.depth, (Rectangle){0, 0, (float)p->width, -(float)p->height},
                   (Rectangle){0, 0, (float)p->width, (float)p->height}, (Vector2){0}, 0, WHITE);
    EndShaderMode();
    EndTextureMode();
    ClearBackground(BLACK);
    DrawTexturePro(p->scene.texture, (Rectangle){0, 0, (float)p->width, -(float)p->height},
                   (Rectangle){0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()}, (Vector2){0}, 0,
                   WHITE);
    if (p->showDepth)
    {
        DrawTexturePro(p->depthView.texture, (Rectangle){0, 0, (float)p->width, -(float)p->height},
                       (Rectangle){GetScreenWidth() - 270, 60, 250, 150}, (Vector2){0}, 0, WHITE);
        DrawText("Sampled depth", GetScreenWidth() - 270, 38, 18, WHITE);
    }
    DrawText("CORE TEST | Textured GPU skinning / sampleable depth", 16, 12, 20, WHITE);
    DrawText("WASD fly | Space/Ctrl up/down | Shift fast | Tab mouse | Esc exit", 16, GetScreenHeight() - 76,
             16, WHITE);
    DrawText("1 clip | 2 one-shot | 3 blend to frame 0 | L aim | H hide | F fix", 16, GetScreenHeight() - 54,
             16, WHITE);
    DrawText("V viewmodel | N ground/noclip | F1 depth", 16, GetScreenHeight() - 32, 16, WHITE);
    UiRect uiScreen = {0, 0, GetScreenWidth(), GetScreenHeight()};
    EngineInput uiInput = p->uiInput;
    if (p->smoke)
    {
        if (p->draws == 4)
        {
            uiInput.mousePosition = (Vector2){(float)(uiScreen.width - 1), (float)(uiScreen.height - 1)};
            uiInput.mousePressed[MOUSE_BUTTON_RIGHT] = true;
        }
        else if (p->draws == 5)
            uiInput.mousePosition =
                (Vector2){(float)(uiScreen.width - 160), (float)(uiScreen.height - 62)};
        else if (p->draws == 20)
            uiInput.mousePosition =
                (Vector2){(float)(uiScreen.width - 160), (float)(uiScreen.height - 38)};
    }
    UiBeginFrame(&p->ui, &uiInput, uiScreen);
    if (!p->capture)
        UiContextMenu(&p->ui, uiScreen, "Core test", &rootMenu, p);
    UiDrawPanel(&p->ui, &p->panel, "Core UI", DrawPanelContents, p);
    p->dock.x = GetScreenWidth() - p->dock.tileSize - 16;
    UiDrawDock(&p->ui, &p->dock);
    UiDrawMenus(&p->ui);
    UiEndFrame(&p->ui);
    if (p->smoke && (p->draws == 3 || p->draws == 6 || p->draws == 35))
    {
        Image uiCapture = LoadImageFromScreen();
        const char *path = p->draws == 3   ? "build/core/ui.png"
                           : p->draws == 6 ? "build/core/ui-menu-moving.png"
                                           : "build/core/ui-menu.png";
        ExportImage(uiCapture, path);
        UnloadImage(uiCapture);
    }
    if (p->smoke && (!p->firstImage || p->draws == 90))
        Readback(p);
    if (!CoreCheckGraphicsErrors("frame"))
        p->failures++;
    p->draws++;
}
static void Shutdown(void *context)
{
    Project *p = context;
    if (p->smoke)
    {
        Check(p, p->draws >= 100 && p->updates > 0, "engine loop completed smoke run");
        TraceLog(LOG_INFO, "CORE TEST failures=%d updates=%d draws=%d", p->failures, p->updates, p->draws);
    }
    free(p->firstImage);
    free(p->originalVertices);
    free(p->clips);
    UiFree(&p->ui);
    FrameUniformsFree(&p->uniforms);
    CoreUnloadRT(&p->scene);
    CoreUnloadRT(&p->depthView);
    /* raylib Model unload doesn't own textures. Release unique asset textures separately. */
    for (int i = 0; i < p->character.materialCount; i++)
    {
        Texture t = p->character.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        bool seen = false;
        for (int j = 0; j < i; j++)
            if (p->character.materials[j].maps[MATERIAL_MAP_DIFFUSE].texture.id == t.id)
                seen = true;
        if (!seen && t.width > 1)
            UnloadTexture(t);
    }
    if (p->character.meshCount)
        UnloadModel(p->character);
    if (p->ground.meshCount)
        UnloadModel(p->ground);
    if (p->animations)
        UnloadModelAnimations(p->animations, p->animationCount);
    CoreUnloadShaders(p->shaders, SHADER_COUNT);
}
int main(int argc, char **argv)
{
    Project project = {0};
    double fixed = 1.0 / 60.0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--smoke"))
            project.smoke = true;
        else if (!strcmp(argv[i], "--variable"))
            fixed = 0;
        else if (!strcmp(argv[i], "--slow-fixed"))
            fixed = 1.0 / 20.0;
        else
        {
            fprintf(stderr, "Usage: %s [--smoke] [--variable | --slow-fixed]\n", argv[0]);
            return 1;
        }
    }
    EngineConfig config = {.title = "Core test",
                           .width = 1100,
                           .height = 700,
                           .targetFps = 60,
                           .fixed_dt = fixed,
                           .max_frame_dt = 0.25,
                           .windowFlags = FLAG_WINDOW_RESIZABLE,
                           /* A skinned character drawn into a target whose depth it samples back. */
                           .requirements = {.gpuSkinning = true, .sampleableDepth = true}};
    EngineProject hooks = {Init, FrameInput, Update, Draw, Shutdown};
    int result = EngineRun(&config, &hooks, &project);
    return result || project.failures ? 1 : 0;
}
