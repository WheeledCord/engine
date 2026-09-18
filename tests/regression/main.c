/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One check per bug that has been fixed here, so none of them can come back unnoticed. Each one
// asserts on a value read back: a pixel, a rectangle, a return code. Run with no arguments for the
// checks that share one window, and with --runner for the ones that need the engine's own loop.
#define _DEFAULT_SOURCE
#include "core/engine.h"
#include "core/fps_camera.h"
#include "core/frame_uniforms.h"
#include "core/iso_grid.h"
#include "core/network.h"
#include "core/net_sync.h"
#include "core/sprite_sheet.h"
#include "core/texture.h"
#include "core/playback.h"
#include "core/file.h"
#include "gameplay/iso_move.h"
#include "core/transform.h"
#include "core/ui.h"
#include "core/ui_document.h"
#include "gameplay/runtime.h"
#include "gameplay/script/script_pawn.h"
#include "gameplay/script/script_s7.h"
#include "gameplay/scene.h"
#include "rlgl.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <math.h>
#include <time.h>

static int failures, checks;
static UiContext ui;
static RenderTexture2D scratch;

static void Check(bool ok, const char *what)
{
    checks++;
    if (!ok)
        printf("FAIL: %s\n", what);
    failures += !ok;
}

// Output lands beside the build when it is there, and in the working directory otherwise.
static const char *Scratch(const char *name)
{
    static char path[256];
    snprintf(path, sizeof path, DirectoryExists("build/core") ? "build/core/%s" : "%s", name);
    return path;
}

static Color Pixel(RenderTexture2D target, int x, int y)
{
    Image image = LoadImageFromTexture(target.texture);
    Color c = GetImageColor(image, x, target.texture.height - 1 - y);
    UnloadImage(image);
    return c;
}

static bool Same(Color a, Color b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static EngineInput Mouse(float x, float y, bool pressed, bool down, bool released)
{
    EngineInput in = {0};
    in.mousePosition = (Vector2){x, y};
    in.mousePressed[MOUSE_BUTTON_LEFT] = pressed;
    in.mouseDown[MOUSE_BUTTON_LEFT] = down;
    in.mouseReleased[MOUSE_BUTTON_LEFT] = released;
    return in;
}

static void Begin(EngineInput in)
{
    BeginTextureMode(scratch);
    UiBeginFrame(&ui, &in, (UiRect){0, 0, 320, 240});
}

static void End(void)
{
    UiEndFrame(&ui);
    EndTextureMode();
}

typedef bool (*ClickBody)(int frame);
static bool Click(ClickBody body, float x, float y)
{
    Begin(Mouse(x, y, true, true, false));
    body(0);
    End();
    Begin(Mouse(x, y, false, false, true));
    bool clicked = body(1);
    End();
    return clicked;
}

// ---- frame textures keep a texture unit of their own --------------------------------------------
static const char *vertexSource =
    "#version 120\n"
    "attribute vec3 vertexPosition; attribute vec2 vertexTexCoord;\n"
    "uniform mat4 mvp; varying vec2 uv;\n"
    "void main(){ uv = vertexTexCoord; gl_Position = mvp*vec4(vertexPosition,1.0); }\n";
static const char *fragmentSource = "#version 120\n"
                                    "uniform sampler2D frameTex;\n"
                                    "void main(){ gl_FragColor = texture2D(frameTex, vec2(0.5)); }\n";

static void FrameTextureChecks(void)
{
    Shader shader = LoadShaderFromMemory(vertexSource, fragmentSource);
    FrameUniforms uniforms = {0};
    FrameUniformDecl decl = {"frameTex", FRAME_TEXTURE, 1};
    if (!FrameUniformsInit(&uniforms, &decl, 1) || !FrameUniformsAdd(&uniforms, shader))
    {
        Check(false, "frame uniform registry accepts a texture declaration");
        return;
    }
    RenderTexture2D red = LoadRenderTexture(4, 4);
    BeginTextureMode(red);
    ClearBackground(RED);
    EndTextureMode();
    Texture2D green = LoadTextureFromImage(GenImageColor(4, 4, GREEN));
    Texture2D blue = LoadTextureFromImage(GenImageColor(4, 4, BLUE));
    const void *values[] = {&red.texture};
    RenderTexture2D target = LoadRenderTexture(64, 64);
    Camera camera = {{0, 0, 5}, {0, 0, 0}, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};
    Model model = LoadModelFromMesh(GenMeshCube(2, 2, 2));
    model.materials[0].shader = shader;

    // Another texture left on a low unit, which is what a previous draw would do.
    rlActiveTextureSlot(1);
    rlEnableTexture(green.id);
    rlActiveTextureSlot(0);
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode3D(camera);
    FrameUniformsBind(&uniforms, values);
    DrawModel(model, (Vector3){0}, 1, WHITE);
    EndMode3D();
    EndTextureMode();
    Check(Same(Pixel(target, 32, 32), RED), "a model samples the frame texture that was bound");

    // A material map of its own on the unit raylib would have used for the frame texture.
    model.materials[0].maps[MATERIAL_MAP_METALNESS].texture = blue;
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode3D(camera);
    FrameUniformsBind(&uniforms, values);
    DrawModel(model, (Vector3){0}, 1, WHITE);
    DrawModel(model, (Vector3){0}, 1, WHITE); // and again, in the same batch
    EndMode3D();
    EndTextureMode();
    Check(Same(Pixel(target, 32, 32), RED),
          "the model's own material maps cannot shadow the frame texture");
    model.materials[0].maps[MATERIAL_MAP_METALNESS].texture = (Texture2D){0};

    UnloadModel(model);
    FrameUniformsFree(&uniforms);
    UnloadRenderTexture(target);
    UnloadRenderTexture(red);
    UnloadTexture(green);
    UnloadTexture(blue);
}

// ---- input goes where it is visible --------------------------------------------------------------
static bool clipped;
static void OverflowingContent(UiContext *u, UiRect content, void *user)
{
    (void)content;
    *(bool *)user = UiButtonBare(u, (UiRect){11, 151, 60, 24}, "Hidden");
}

static bool ClippedButton(int frame)
{
    (void)frame;
    bool hit = false;
    if (clipped) // the indent's content ends at y 128, so the button is entirely out of sight
        UiIndent(&ui, (UiRect){10, 90, 100, 40}, OverflowingContent, &hit);
    else
        hit = UiButtonBare(&ui, (UiRect){11, 151, 60, 24}, "Hidden");
    return hit;
}

static UiButtonFlags releaseFlags;
static bool FlagButton(int frame)
{
    return UiButtonEx(&ui, (UiRect){10, 10, 80, 24}, "Go", frame ? releaseFlags : UI_BUTTON_DEFAULT);
}

static int identityCase;
static bool IdentityButton(int frame)
{
    UiRect rect = {10, 10, 80, 24};
    const char *label = frame && identityCase == 1 ? "B" : "A";
    if (frame && identityCase == 2)
        rect.x += 1;
    if (identityCase == 3)
        UiNextId(&ui, 0x51D); // the caller's own identity, whatever the label says
    return UiButtonEx(&ui, rect, label, UI_BUTTON_BARE);
}

static char fieldText[32] = "abc";
static void InputChecks(void)
{
    clipped = false;
    bool visible = Click(ClippedButton, 20, 160);
    clipped = true;
    Check(visible && !Click(ClippedButton, 20, 160),
          "a widget clipped out of sight takes no clicks where it would have been");

    releaseFlags = UI_BUTTON_DEFAULT;
    bool normal = Click(FlagButton, 40, 20);
    releaseFlags = UI_BUTTON_NO_INPUT;
    Check(normal && !Click(FlagButton, 40, 20),
          "a button that stops taking input mid-press reports no click");

    identityCase = 0;
    bool plain = Click(IdentityButton, 40, 20);
    identityCase = 1;
    bool relabelled = Click(IdentityButton, 40, 20);
    identityCase = 2;
    bool moved = Click(IdentityButton, 40, 20);
    identityCase = 3;
    bool ownId = Click(IdentityButton, 40, 20);
    Check(plain && moved && ownId, "a press survives the button moving, and an explicit id");
    (void)relabelled;

    // A focused field that the project stops drawing must not keep the keyboard forever.
    Begin(Mouse(20, 20, true, true, false));
    UiTextField(&ui, (UiRect){10, 10, 120, 24}, fieldText, sizeof fieldText);
    End();
    Begin(Mouse(20, 20, false, false, true));
    UiTextField(&ui, (UiRect){10, 10, 120, 24}, fieldText, sizeof fieldText);
    End();
    bool held = UiCapture(&ui).keyboard;
    for (int i = 0; i < 3; i++)
    {
        Begin(Mouse(300, 200, false, false, false));
        End();
    }
    Check(held && !UiCapture(&ui).keyboard && !UiTextFieldActive(&ui),
          "the keyboard is released when the focused field stops being drawn");

    // Captured input must not reach the game, and must not fire later either.
    EngineInput frame = {0}, pending = {0};
    frame.down[KEY_W] = frame.pressed[KEY_W] = true;
    frame.mouseDelta = (Vector2){2, 3};
    EngineInputRoute(&pending, &frame, (EngineInputCapture){.keyboard = true});
    bool blocked = !pending.pressed[KEY_W] && !pending.down[KEY_W] && pending.mouseDelta.y == 3;
    EngineInputDrain(&pending);
    EngineInput release = {0};
    release.down[KEY_W] = true;
    EngineInputRoute(&pending, &release, (EngineInputCapture){0});
    Check(blocked && pending.down[KEY_W] && !pending.pressed[KEY_W],
          "captured keys never reach the game, and do not fire once capture ends");
}

// ---- documents ------------------------------------------------------------------------------------
#define LRTB (UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP | UI_ANCHOR_BOTTOM)
#define LRT (UI_ANCHOR_LEFT | UI_ANCHOR_RIGHT | UI_ANCHOR_TOP)

static UiElement *Add(UiDocument *d, UiElementType type, UiRect r, unsigned anchors)
{
    UiElement *e = UiDocumentAdd(d, type, r, type == UI_ELEMENT_BUTTON ? "B" : "");
    e->anchors = anchors;
    return e;
}

static void DrawingChecks(void)
{
    RenderTexture2D target = LoadRenderTexture(200, 100);
    // A container stored after its contents must still be drawn under them.
    UiDocument d = {0};
    UiDocumentInit(&d, 200, 100, UI_SURFACE_SCREEN, "");
    UiRect well = {10, 10, 100, 40}, button = {11, 11, 98, 38};
    Add(&d, UI_ELEMENT_BUTTON, button, LRT);
    Add(&d, UI_ELEMENT_INDENT, well, LRTB);
    UiDocumentSetParent(&d, 0, 1);
    EngineInput idle = Mouse(190, 90, false, false, false);
    BeginTextureMode(target);
    ClearBackground(BLACK);
    UiBeginFrame(&ui, &idle, (UiRect){0, 0, 200, 100});
    UiDocumentDraw(&ui, &d, (Vector2){0, 0}, false);
    UiEndFrame(&ui);
    EndTextureMode();
    Check(Same(Pixel(target, 11, 11), WHITE), "contents are drawn over the container that holds them");
    UiDocumentFree(&d);

    // A child that cannot shrink with its container is clipped by it, not drawn across the surface.
    UiDocumentInit(&d, 200, 100, UI_SURFACE_SCREEN, "");
    UiElement *region = Add(&d, UI_ELEMENT_INDENT, (UiRect){10, 10, 100, 40}, LRTB);
    region->flow = UI_FLOW_FREE;
    region->maxWidth = 50; // narrower than the button it holds, which therefore has to be clipped
    Add(&d, UI_ELEMENT_BUTTON, (UiRect){11, 11, 98, 38}, UI_ANCHOR_LEFT | UI_ANCHOR_TOP);
    BeginTextureMode(target);
    ClearBackground(BLACK);
    UiBeginFrame(&ui, &idle, (UiRect){0, 0, 200, 100});
    UiDocumentDrawSized(&ui, &d, (Vector2){0, 0}, 200, 100, false);
    UiEndFrame(&ui);
    EndTextureMode();
    const UiRect *r = UiDocumentResolve(&ui, &d, 200, 100);
    Check(r[0].width == 50 && r[1].width > 50 && Same(Pixel(target, 80, 30), BLACK),
          "a container clips what it holds, however big the contents are");
    UiDocumentFree(&d);
    UnloadRenderTexture(target);
}

static void ResolveStack(bool bottomUp, UiRect out[3])
{
    UiDocument d = {0};
    UiDocumentInit(&d, 100, 90, UI_SURFACE_SCREEN, "");
    UiElement *well = Add(&d, UI_ELEMENT_INDENT, (UiRect){0, 0, 100, 90}, LRTB);
    well->flow = UI_FLOW_AUTO;
    int ys[3] = {1, 31, 61};
    for (int i = 0; i < 3; i++)
        Add(&d, UI_ELEMENT_BUTTON, (UiRect){1, ys[bottomUp ? 2 - i : i], 98, 28}, LRT);
    const UiRect *r = UiDocumentResolve(&ui, &d, 100, 150);
    for (int i = 0; i < 3; i++)
        out[i] = r[1 + i];
    UiDocumentFree(&d);
}

static void LayoutChecks(void)
{
    UiRect down[3], up[3];
    ResolveStack(false, down);
    ResolveStack(true, up);
    Check(down[0].x == down[1].x && down[0].y != down[1].y && up[0].x == up[1].x &&
              up[0].y != up[1].y,
          "a stack divides downwards whichever order its contents were added in");

    // Siblings that both stretch must never end up on top of each other.
    UiDocument d = {0};
    UiDocumentInit(&d, 210, 40, UI_SURFACE_SCREEN, "");
    UiElement *a = Add(&d, UI_ELEMENT_BUTTON, (UiRect){0, 0, 100, 24}, LRT);
    UiElement *b = Add(&d, UI_ELEMENT_BUTTON, (UiRect){110, 0, 100, 24}, LRT);
    a->minWidth = 60;
    b->minWidth = 60;
    int minWidth = 0, minHeight = 0;
    UiDocumentMinimumSize(&ui, &d, &minWidth, &minHeight);
    bool tidy = minWidth == 130; // 60 + the authored 10px gap + 60, and nothing kept in reserve
    for (int width = 210; width >= 40; width--)
    {
        const UiRect *r = UiDocumentResolve(&ui, &d, width, 40);
        tidy &= r[0].x + r[0].width <= r[1].x && r[0].width >= 60 && r[1].width >= 60;
    }
    Check(tidy, "shrinking stops at each minimum without the siblings overlapping");
    UiDocumentFree(&d);

    // Identical containers cannot adopt each other, and a move never changes a parent.
    UiDocumentInit(&d, 200, 100, UI_SURFACE_SCREEN, "");
    Add(&d, UI_ELEMENT_INDENT, (UiRect){10, 10, 100, 50}, LRTB);
    Add(&d, UI_ELEMENT_INDENT, (UiRect){10, 10, 100, 50}, LRTB);
    const UiRect *twin = UiDocumentResolve(&ui, &d, 300, 100);
    Check(UiDocumentContainerOf(&d, 0) == -1 && twin[0].width == 200,
          "two containers with the same rectangle do not contain each other");
    UiDocumentFree(&d);

    UiDocumentInit(&d, 200, 100, UI_SURFACE_SCREEN, "");
    Add(&d, UI_ELEMENT_INDENT, (UiRect){10, 10, 100, 50}, LRTB);
    Add(&d, UI_ELEMENT_BUTTON, (UiRect){12, 12, 60, 20}, UI_ANCHOR_LEFT | UI_ANCHOR_TOP);
    bool adopted = UiDocumentContainerOf(&d, 1) == 0;
    d.elements[1].rect.x = 150; // dragged out of the indent by hand
    Check(adopted && UiDocumentContainerOf(&d, 1) == 0 && !UiDocumentSetParent(&d, 0, 1),
          "parents are explicit: moving an element does not silently reparent it");
    UiDocumentFree(&d);

    // Resolving a document has to stay cheap enough to run every frame.
    UiDocumentInit(&d, 1000, 1000, UI_SURFACE_SCREEN, "");
    for (int i = 0; i < 200; i++)
        Add(&d, UI_ELEMENT_BUTTON, (UiRect){(i % 10) * 100, (i / 10) * 30, 90, 24}, LRT);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 10; i++)
        UiDocumentResolve(&ui, &d, 1100 + i, 1000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 10;
    printf("  resolve of 200 elements: %.3f ms\n", ms);
    Check(ms < 1.0, "a 200 element layout resolves in well under a frame");
    UiDocumentFree(&d);
}

static void ActivationChecks(void)
{
    UiDocument d = {0};
    UiDocumentInit(&d, 200, 60, UI_SURFACE_SCREEN, "");
    UiElement *first = Add(&d, UI_ELEMENT_BUTTON, (UiRect){10, 10, 80, 24}, UI_ANCHOR_LEFT | UI_ANCHOR_TOP);
    snprintf(first->name, sizeof first->name, "start");
    UiElement *second = Add(&d, UI_ELEMENT_BUTTON, (UiRect){10, 40, 80, 24}, UI_ANCHOR_LEFT | UI_ANCHOR_TOP);
    snprintf(second->name, sizeof second->name, "quit");
    for (int frame = 0; frame < 2; frame++)
    {
        Begin(Mouse(50, 52, frame == 0, frame == 0, frame == 1));
        UiDocumentDraw(&ui, &d, (Vector2){0, 0}, true);
        End();
    }
    const UiElement *hit = UiDocumentActivated(&d);
    Check(hit && !strcmp(hit->name, "quit") && UiDocumentFind(&d, "start") == &d.elements[0],
          "a document says which of its elements was used");
    UiDocumentFree(&d);
}

// ---- files ----------------------------------------------------------------------------------------
static long FileSize(const char *path)
{
    struct stat s;
    return stat(path, &s) ? -1 : (long)s.st_size;
}

static bool KeyValue(EntityContext *e, const char *k, const char *v)
{
    (void)e;
    (void)k;
    (void)v;
    return true;
}

static void FileChecks(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s", Scratch("regression_binary.dat"));
    FILE *f = fopen(path, "wb");
    const unsigned char expected[] = {0x00, 0x7f, 0xff, 0x31};
    fwrite(expected, 1, sizeof expected, f);
    fclose(f);
    size_t binarySize = 99;
    unsigned char *binary = CoreReadData(path, &binarySize);
    Check(binary && binarySize == sizeof expected && !memcmp(binary, expected, sizeof expected),
          "a binary file keeps embedded zeroes and reports its exact size");
    CoreFreeData(binary);
    binarySize = 99;
    binary = CoreReadData(Scratch("missing_binary.dat"), &binarySize);
    Check(!binary && binarySize == 0, "a missing binary file fails with an empty result");

    snprintf(path, sizeof path, "%s", Scratch("regression_malformed.ui"));
    f = fopen(path, "w");
    fprintf(f, "core_ui_document 6\nsurface 200 100 0\t\n"
               "0 10 10 80 24 0 0 7 0 0 0 0 0 1 1 -1 -\tGood\n"
               "this line is not an element at all\n");
    fclose(f);
    UiDocument d = {0};
    Check(!UiDocumentLoad(&d, path) && d.count == 0,
          "a layout with a line that cannot be read refuses to load");
    UiDocumentFree(&d);

    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){8, 0.1});
    EntityRegister(&world, (EntityClass){.classname = "thing", .size = 16, .KeyValue = KeyValue});
    snprintf(path, sizeof path, "%s", Scratch("regression_unterminated.scene"));
    f = fopen(path, "w");
    fprintf(f, "entity \"thing\" {\n    \"name\" \"one\"\n}\n\"unterminated\n");
    fclose(f);
    Check(!GameplaySceneLoad(&world, path, true),
          "a scene with an unterminated token fails instead of stopping early");
    GameplayWorldFree(&world);

    // A save that runs out of room must leave the file that was there.
    snprintf(path, sizeof path, "%s", Scratch("regression_layout.ui"));
    UiDocumentInit(&d, 400, 400, UI_SURFACE_SCREEN, "big");
    for (int i = 0; i < 300; i++)
        Add(&d, UI_ELEMENT_BUTTON, (UiRect){i % 300, i % 300, 50, 20}, LRT);
    bool first = UiDocumentSave(&d, path);
    long good = FileSize(path);
    signal(SIGXFSZ, SIG_IGN);
    struct rlimit old, limit;
    getrlimit(RLIMIT_FSIZE, &old);
    limit = old;
    limit.rlim_cur = 4096;
    setrlimit(RLIMIT_FSIZE, &limit);
    bool second = UiDocumentSave(&d, path);
    setrlimit(RLIMIT_FSIZE, &old);
    UiDocument reloaded = {0};
    bool back = UiDocumentLoad(&reloaded, path);
    Check(first && !second && FileSize(path) == good && back && reloaded.count == d.count,
          "a save that fails part way leaves the previous file untouched");
    UiDocumentFree(&reloaded);
    UiDocumentFree(&d);
}

static void TextureChecks(void)
{
    const char *path = Scratch("regression_texture.png");
    Image image = GenImageColor(4, 2, WHITE);
    Check(ExportImage(image, path), "texture fixture exports");
    UnloadImage(image);
    CoreTextureOptions options = CoreTextureOptionsDefault();
    options.mipmaps = true;
    options.filter = TEXTURE_FILTER_TRILINEAR;
    options.wrap = TEXTURE_WRAP_CLAMP;
    Texture2D texture = {0};
    Check(CoreLoadTexture(&texture, path, options) && texture.width == 4 && texture.height == 2 &&
              texture.mipmaps > 1,
          "texture loads with requested mipmaps through the data path");
    CoreUnloadTexture(&texture);
    Check(!texture.id, "texture unload clears its owning handle");
    Check(!CoreLoadTexture(&texture, Scratch("missing_texture.png"), options) && !texture.id,
          "missing texture is rejected and leaves an empty handle");
    Check(!CoreLoadTexture(NULL, path, options), "texture load rejects a NULL destination");
    remove(path);
}

static void ConventionChecks(void)
{
    FpsCamera camera;
    FpsCameraInit(&camera, (Vector3){0}, 0, 0, 60);
    Camera c = FpsCameraInterpolated(&camera, 1);
    Vector3 fps = Vector3Normalize(Vector3Subtract(c.target, c.position));
    Transform t = TransformIdentity();
    Check(Vector3Distance(fps, TransformForward(t)) < 1e-5f,
          "a zero yaw camera and an identity transform face the same way");
    TransformLookAt(&t, (Vector3){3, 0, 4}, (Vector3){0, 1, 0});
    Check(Vector3Distance(TransformForward(t), Vector3Normalize((Vector3){3, 0, 4})) < 1e-5f &&
              Vector3Distance(TransformRight(t), (Vector3){-0.8f, 0, 0.6f}) < 1e-5f,
          "look-at faces the target, with right where the camera would strafe");
}

// ---- the deferred UI, which only the application runner uses -------------------------------------
static char deferredText[32] = "hi";
static void SampleUi(UiContext *context)
{
    UiDrawFrame(context, (UiRect){0, 0, 200, 120});
    UiButtonEx(context, (UiRect){10, 10, 80, 24}, "Go", UI_BUTTON_DEFAULT);
    UiTextField(context, (UiRect){10, 44, 120, 24}, deferredText, sizeof deferredText);
    UiLabel(context, (UiRect){10, 80, 180, 20}, "Label");
}

static void DeferredChecks(void)
{
    RenderTexture2D immediate = LoadRenderTexture(200, 120);
    RenderTexture2D deferred = LoadRenderTexture(200, 120);
    EngineInput idle = Mouse(190, 110, false, false, false);
    BeginTextureMode(immediate);
    ClearBackground(BLACK);
    UiBeginFrame(&ui, &idle, (UiRect){0, 0, 200, 120});
    SampleUi(&ui);
    UiEndFrame(&ui);
    EndTextureMode();

    UiBeginDeferredFrame(&ui, &idle, (UiRect){0, 0, 200, 120});
    SampleUi(&ui);
    UiEndFrame(&ui);
    BeginTextureMode(deferred);
    ClearBackground(BLACK);
    bool rendered = UiRender(&ui);
    EndTextureMode();

    Image a = LoadImageFromTexture(immediate.texture);
    Image b = LoadImageFromTexture(deferred.texture);
    int differences = 0;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++)
            differences += !Same(GetImageColor(a, x, y), GetImageColor(b, x, y));
    UnloadImage(a);
    UnloadImage(b);
    Check(rendered && differences == 0, "a deferred frame draws exactly what an immediate one does");

    // Clicking the field captures the keyboard before the update sees the input.
    EngineInput click = Mouse(40, 56, true, true, false);
    UiBeginDeferredFrame(&ui, &click, (UiRect){0, 0, 200, 120});
    SampleUi(&ui);
    UiEndFrame(&ui);
    EngineInputCapture capture = UiCapture(&ui);
    BeginTextureMode(deferred);
    UiRender(&ui);
    EndTextureMode();
    Check(capture.keyboard && capture.mouse,
          "the UI reports what it took before the game is updated");
    UnloadRenderTexture(immediate);
    UnloadRenderTexture(deferred);
}

// ---- scripting: one table, and a language on top of it -----------------------------------------
static void ScriptChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptS7Open(&host), "the Scheme frontend registers the binding table");
    bool declared = ScriptS7Eval(
        "(begin"
        " (define (t-spawn) (think-next))"
        " (define (t-think) (move-world! (vec (* (get \"speed\") (dt)) 0)) (think-next))"
        " (define-entity \"tester\" '((\"speed\" \"float\") (\"target\" \"vector2\"))"
        "   '((\"spawn\" \"t-spawn\") (\"think\" \"t-think\"))))",
        NULL);
    EntityProperty properties[] = {{"position", "10 20"}, {"speed", "100"}};
    EntityHandle entity = EntitySpawnWith(&world, "tester", properties, 2);
    ScriptEntity *body = EntityData(&world, entity);
    bool placed = body && body->transform.translation.x == 10 && body->slots[0].as.number == 100;
    GameplayWorldStep(&world); // one tick of 0.1s at 100 a second
    Check(declared && placed && body && fabsf(body->transform.translation.x - 20) < 0.001f,
          "a scripted class takes its fields from a scene and runs its own think");

    char *answer = NULL;
    bool refused = !ScriptS7Eval("(rotate! \"sideways\")", &answer);
    free(answer);
    answer = NULL;
    bool arity = !ScriptS7Eval("(move-world!)", &answer);
    free(answer);
    Check(refused && arity, "the table checks a script's arguments instead of trusting them");

    EntityHandle second = EntitySpawnWith(&world, "tester", properties, 2);
    ScriptValue classArgument[] = {ScriptString("tester")};
    ScriptValue first = ScriptNone(), next = ScriptNone();
    const char *message = NULL;
    bool found = ScriptInvoke(&host, ScriptBindingNamed("find-first"), classArgument, 1, &first,
                              &message) &&
                 ScriptInvoke(&host, ScriptBindingNamed("find-next"),
                              (ScriptValue[]){first, ScriptString("tester")}, 2, &next, &message);
    ScriptValue setSpeed[] = {first, ScriptString("speed"), ScriptFloat(55)};
    ScriptValue setTarget[] = {next, ScriptString("target"), ScriptVector2((Vector2){7, 8})};
    ScriptValue speed = ScriptNone(), target = ScriptNone();
    bool coordinated = found && first.as.entity && next.as.entity && first.as.entity != next.as.entity &&
                       ScriptInvoke(&host, ScriptBindingNamed("entity-set-number!"), setSpeed, 3,
                                    NULL, &message) &&
                       ScriptInvoke(&host, ScriptBindingNamed("entity-set-vector!"), setTarget, 3,
                                    NULL, &message) &&
                       ScriptInvoke(&host, ScriptBindingNamed("entity-get"),
                                    (ScriptValue[]){first, ScriptString("speed")}, 2, &speed,
                                    &message) &&
                       ScriptInvoke(&host, ScriptBindingNamed("entity-get-vector"),
                                    (ScriptValue[]){next, ScriptString("target")}, 2, &target,
                                    &message) &&
                       speed.as.number == 55 && target.as.vector2.x == 7 && target.as.vector2.y == 8;
    Check(coordinated, "scripts can find scripted entities and read or write their declared fields");

    EntityDestroy(&world, second);
    ScriptValue stale = ScriptNone();
    bool safe = ScriptInvoke(&host, ScriptBindingNamed("entity-get"),
                             (ScriptValue[]){next, ScriptString("speed")}, 2, &stale, &message) &&
                stale.as.number == 0;
    Check(safe, "cross-entity script access rejects stale handles without reaching replacement data");

    Check(safe, "cross-entity script access rejects stale handles without reaching replacement data");

    // The small math and query rows: answers checked against raymath, and types still checked.
    ScriptValue length = ScriptNone(), distance = ScriptNone(), normalized = ScriptNone();
    bool math = ScriptInvoke(&host, ScriptBindingNamed("vec-length"),
                             (ScriptValue[]){ScriptVector2((Vector2){3, 4})}, 1, &length, &message) &&
                ScriptInvoke(&host, ScriptBindingNamed("vec-distance"),
                             (ScriptValue[]){ScriptVector2((Vector2){0, 0}),
                                             ScriptVector2((Vector2){3, 4})},
                             2, &distance, &message) &&
                ScriptInvoke(&host, ScriptBindingNamed("vec-normalize"),
                             (ScriptValue[]){ScriptVector2((Vector2){0, 2})}, 1, &normalized,
                             &message) &&
                fabsf(length.as.number - 5) < 0.001f && fabsf(distance.as.number - 5) < 0.001f &&
                fabsf(normalized.as.vector2.y - 1) < 0.001f;
    Check(math, "vector queries come from raymath through the one table");

    bool ranged = true;
    for (int i = 0; i < 32 && ranged; i++)
    {
        ScriptValue roll = ScriptNone();
        ranged = ScriptInvoke(&host, ScriptBindingNamed("random-int"),
                              (ScriptValue[]){ScriptInt(2), ScriptInt(5)}, 2, &roll, &message) &&
                 roll.as.integer >= 2 && roll.as.integer <= 5;
    }
    bool rolled = ranged && !ScriptInvoke(&host, ScriptBindingNamed("random-int"),
                                          (ScriptValue[]){ScriptFloat(2), ScriptInt(5)}, 2, NULL,
                                          &message);
    Check(rolled, "random-int stays inside its range and refuses a float for an int");

    ScriptValue wide = ScriptNone(), narrow = ScriptNone();
    bool measured = ScriptInvoke(&host, ScriptBindingNamed("text-width"),
                                 (ScriptValue[]){ScriptString("ww"), ScriptInt(10)}, 2, &wide,
                                 &message) &&
                    ScriptInvoke(&host, ScriptBindingNamed("text-width"),
                                 (ScriptValue[]){ScriptString("w"), ScriptInt(10)}, 2, &narrow,
                                 &message) &&
                    wide.as.integer > narrow.as.integer && narrow.as.integer > 0;
    Check(measured, "text-width measures what draw-text would draw");

    ScriptValue size = ScriptNone();
    bool unknown = ScriptInvoke(&host, ScriptBindingNamed("sprite-size"),
                                (ScriptValue[]){ScriptString("no-such-sheet")}, 1, &size, &message) &&
                   size.as.vector2.x == 0 && size.as.vector2.y == 0;
    Check(unknown, "sprite-size answers a zero size for a sheet nobody loaded");

    // Resource handle system: create, use, destroy, stale rejection, kind mismatch.
    ScriptValue camHandle = ScriptNone();
    bool camMade = ScriptInvoke(&host, ScriptBindingNamed("camera-create"),
                                (ScriptValue[]){ScriptNone()}, 0, &camHandle, &message) &&
                   camHandle.type == SCRIPT_RESOURCE && camHandle.as.integer != 0;
    Check(camMade, "camera-create returns a live resource handle");

    ScriptValue camPos = ScriptNone();
    bool camRead = ScriptInvoke(&host, ScriptBindingNamed("camera-position"),
                                (ScriptValue[]){camHandle}, 1, &camPos, &message) &&
                   camPos.type == SCRIPT_VECTOR2;
    Check(camRead, "camera-position reads from a valid handle");

    ScriptValue camGone = ScriptBool(true);
    bool camFreed = ScriptInvoke(&host, ScriptBindingNamed("camera-destroy"),
                                 (ScriptValue[]){camHandle}, 1, &camGone, &message) &&
                    camGone.as.boolean;
    Check(camFreed, "camera-destroy releases a live handle");

    ScriptValue stalePos = ScriptNone();
    bool staleRefused = ScriptInvoke(&host, ScriptBindingNamed("camera-position"),
                                     (ScriptValue[]){camHandle}, 1, &stalePos, &message) &&
                        stalePos.type == SCRIPT_VECTOR2 && stalePos.as.vector2.x == 0 &&
                        stalePos.as.vector2.y == 0;
    Check(staleRefused, "a destroyed camera handle gives a zero position");

    // Generational reuse: the next camera reuses the slot but gets a new generation.
    ScriptValue cam2Handle = ScriptNone();
    bool cam2Made = ScriptInvoke(&host, ScriptBindingNamed("camera-create"),
                                 (ScriptValue[]){ScriptNone()}, 0, &cam2Handle, &message) &&
                    cam2Handle.type == SCRIPT_RESOURCE && cam2Handle.as.integer != 0 &&
                    cam2Handle.as.integer != camHandle.as.integer;
    Check(cam2Made, "a recreated camera has a different handle than the destroyed one");

    // Camera follow and coordinate round-trip.
    ScriptValue cam3Handle = ScriptNone();
    ScriptInvoke(&host, ScriptBindingNamed("camera-create"),
                 (ScriptValue[]){ScriptNone()}, 0, &cam3Handle, &message);
    ScriptInvoke(&host, ScriptBindingNamed("camera-set-viewport!"),
                 (ScriptValue[]){cam3Handle, ScriptVector2((Vector2){960, 540})}, 2, NULL, &message);
    ScriptInvoke(&host, ScriptBindingNamed("camera-set-position!"),
                 (ScriptValue[]){cam3Handle, ScriptVector2((Vector2){100, 200})}, 2, NULL, &message);
    ScriptValue w2s = ScriptNone();
    bool roundTrip = ScriptInvoke(&host, ScriptBindingNamed("camera-world-to-screen"),
                                  (ScriptValue[]){cam3Handle, ScriptVector2((Vector2){100, 200})}, 2, &w2s, &message) &&
                     w2s.type == SCRIPT_VECTOR2;
    Check(roundTrip, "camera-world-to-screen converts a world point at the camera centre");
    ScriptInvoke(&host, ScriptBindingNamed("camera-destroy"),
                 (ScriptValue[]){cam3Handle}, 1, NULL, &message);

    bool kindMismatch = !ScriptInvoke(&host, ScriptBindingNamed("camera-position"),
                                      (ScriptValue[]){ScriptInt(42)}, 1, NULL, &message);
    Check(kindMismatch, "camera-position refuses an int where a resource is expected");

    // Collision world: create, add, query, destroy.
    ScriptValue colHandle = ScriptNone();
    bool colMade = ScriptInvoke(&host, ScriptBindingNamed("collision-create"),
                                (ScriptValue[]){ScriptInt(32), ScriptFloat(64.0f)}, 2, &colHandle, &message) &&
                   colHandle.type == SCRIPT_RESOURCE && colHandle.as.integer != 0;
    Check(colMade, "collision-create makes a query world");

    ScriptValue proxyId = ScriptNone();
    bool circleAdded = ScriptInvoke(&host, ScriptBindingNamed("collision-add-circle"),
                                    (ScriptValue[]){colHandle, ScriptVector2((Vector2){100, 100}),
                                                    ScriptFloat(10), ScriptInt(1), ScriptInt(1)},
                                    5, &proxyId, &message) && proxyId.as.integer != 0;
    Check(circleAdded, "collision-add-circle registers a proxy");

    ScriptValue overlap = ScriptNone();
    bool queryOk = ScriptInvoke(&host, ScriptBindingNamed("collision-query-circle"),
                                (ScriptValue[]){colHandle, ScriptVector2((Vector2){100, 100}),
                                                ScriptFloat(15), ScriptInt(1)},
                                4, &overlap, &message) && overlap.as.integer == 1;
    Check(queryOk, "collision-query-circle finds the registered circle");

    // AABB proxy: add and query
    ScriptValue aabbProxyId = ScriptNone();
    bool aabbAdded = ScriptInvoke(&host, ScriptBindingNamed("collision-add-aabb"),
                                  (ScriptValue[]){colHandle, ScriptVector2((Vector2){200, 200}),
                                                  ScriptVector2((Vector2){20, 20}), ScriptInt(2), ScriptInt(2)},
                                  5, &aabbProxyId, &message) && aabbProxyId.as.integer != 0;
    Check(aabbAdded, "collision-add-aabb registers an AABB proxy");

    ScriptValue aabbOverlap = ScriptNone();
    bool aabbQueryOk = ScriptInvoke(&host, ScriptBindingNamed("collision-query-aabb"),
                                    (ScriptValue[]){colHandle, ScriptVector2((Vector2){205, 205}),
                                                    ScriptVector2((Vector2){10, 10}), ScriptInt(2)},
                                    4, &aabbOverlap, &message) && aabbOverlap.as.integer == 1;
    Check(aabbQueryOk, "collision-query-aabb finds the registered box");

    ScriptValue colFreed = ScriptBool(false);
    bool colDestroyed = ScriptInvoke(&host, ScriptBindingNamed("collision-destroy"),
                                     (ScriptValue[]){colHandle}, 1, &colFreed, &message) &&
                        colFreed.as.boolean;
    Check(colDestroyed, "collision-destroy releases the world");

    // Pathfinder: create, block, query, destroy.
    ScriptValue pfHandle = ScriptNone();
    bool pfMade = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-create"),
                               (ScriptValue[]){ScriptInt(10), ScriptInt(10)}, 2, &pfHandle, &message) &&
                  pfHandle.type == SCRIPT_RESOURCE && pfHandle.as.integer != 0;
    Check(pfMade, "pathfinder-create makes an A* workspace");

    ScriptValue blocked = ScriptNone();
    bool blockOk = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-block!"),
                                (ScriptValue[]){pfHandle, ScriptVector2((Vector2){3, 3})}, 2, &blocked, &message) &&
                   blocked.as.boolean;
    ScriptValue isBlocked = ScriptNone();
    bool checkBlocked = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-blocked?"),
                                     (ScriptValue[]){pfHandle, ScriptVector2((Vector2){3, 3})}, 2, &isBlocked, &message) &&
                        isBlocked.as.boolean;
    Check(blockOk && checkBlocked, "pathfinder-block! marks a hex and pathfinder-blocked? sees it");

    ScriptValue pfFreed = ScriptBool(false);
    bool pfDestroyed = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-destroy"),
                                    (ScriptValue[]){pfHandle}, 1, &pfFreed, &message) && pfFreed.as.boolean;
    Check(pfDestroyed, "pathfinder-destroy releases the workspace");

    // Pathfinder solve: a route around a wall, and the same hexes read back in order.
    ScriptValue pf2Handle = ScriptNone();
    ScriptInvoke(&host, ScriptBindingNamed("pathfinder-create"),
                 (ScriptValue[]){ScriptInt(10), ScriptInt(10)}, 2, &pf2Handle, &message);
    for (int x = 0; x < 9; x++)
        ScriptInvoke(&host, ScriptBindingNamed("pathfinder-block!"),
                     (ScriptValue[]){pf2Handle, ScriptVector2((Vector2){x, 5})}, 2, NULL, &message);
    ScriptValue route = ScriptNone();
    bool solved = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-solve"),
                               (ScriptValue[]){pf2Handle, ScriptVector2((Vector2){4, 4}),
                                               ScriptVector2((Vector2){4, 6})},
                               3, &route, &message) && route.as.integer > 4;
    Check(solved, "pathfinder-solve routes around a wall of blocked hexes");

    ScriptValue routeFirst = ScriptNone(), routeLast = ScriptNone();
    bool endsRead = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-path-get"),
                                 (ScriptValue[]){pf2Handle, ScriptInt(0)}, 2, &routeFirst, &message) &&
                    ScriptInvoke(&host, ScriptBindingNamed("pathfinder-path-get"),
                                 (ScriptValue[]){pf2Handle, ScriptInt(route.as.integer - 1)}, 2,
                                 &routeLast, &message) &&
                    routeFirst.as.vector2.x == 4 && routeFirst.as.vector2.y == 4 &&
                    routeLast.as.vector2.x == 4 && routeLast.as.vector2.y == 6;
    Check(endsRead, "the solved route starts and ends where it was asked to");

    ScriptValue noRoute = ScriptNone();
    bool walled = ScriptInvoke(&host, ScriptBindingNamed("pathfinder-solve"),
                               (ScriptValue[]){pf2Handle, ScriptVector2((Vector2){4, 4}),
                                               ScriptVector2((Vector2){4, 5})},
                               3, &noRoute, &message) && noRoute.as.integer == 0;
    Check(walled, "a route onto a blocked hex answers zero");

    // Mover: plan a walk through the same pathfinder and advance it.
    ScriptValue mvHandle = ScriptNone();
    bool mvMade = ScriptInvoke(&host, ScriptBindingNamed("mover-create"),
                              (ScriptValue[]){ScriptVector2((Vector2){4, 4}), ScriptInt(64)}, 2,
                              &mvHandle, &message) &&
                  mvHandle.type == SCRIPT_RESOURCE && mvHandle.as.integer != 0;
    Check(mvMade, "mover-create starts a walker on a hex");

    ScriptValue walking = ScriptNone();
    bool went = ScriptInvoke(&host, ScriptBindingNamed("mover-go-to"),
                             (ScriptValue[]){mvHandle, pf2Handle, ScriptVector2((Vector2){4, 6})}, 3,
                             &walking, &message) && walking.as.boolean;
    ScriptValue moving = ScriptNone();
    bool isMoving = ScriptInvoke(&host, ScriptBindingNamed("mover-moving?"),
                                 (ScriptValue[]){mvHandle}, 1, &moving, &message) && moving.as.boolean;
    Check(went && isMoving, "mover-go-to plans a walk and mover-moving? sees it");

    ScriptValue remain = ScriptNone();
    bool stepsLeft = ScriptInvoke(&host, ScriptBindingNamed("mover-remaining"),
                                  (ScriptValue[]){mvHandle}, 1, &remain, &message) &&
                                  remain.as.integer > 0;
    Check(stepsLeft, "mover-remaining counts the hexes still to enter");

    for (int i = 0; i < 200; i++)
        ScriptInvoke(&host, ScriptBindingNamed("mover-update!"),
                     (ScriptValue[]){mvHandle, ScriptFloat(0.1f)}, 2, NULL, &message);
    ScriptValue done = ScriptNone();
    bool arrived = ScriptInvoke(&host, ScriptBindingNamed("mover-moving?"),
                                (ScriptValue[]){mvHandle}, 1, &done, &message) && !done.as.boolean;
    ScriptValue mvHex = ScriptNone();
    bool atHex = ScriptInvoke(&host, ScriptBindingNamed("mover-hex"),
                              (ScriptValue[]){mvHandle}, 1, &mvHex, &message) &&
                 mvHex.as.vector2.x == 4 && mvHex.as.vector2.y == 6;
    Check(arrived && atHex, "after enough updates a mover arrives at its target hex");

    ScriptValue mvFreed = ScriptBool(false);
    bool mvDestroyed = ScriptInvoke(&host, ScriptBindingNamed("mover-destroy"),
                                    (ScriptValue[]){mvHandle}, 1, &mvFreed, &message) && mvFreed.as.boolean;
    Check(mvDestroyed, "mover-destroy releases the walker");

    // Saved key/values: set, read, and round-trip through a file.
    bool setOk = ScriptInvoke(&host, ScriptBindingNamed("save-set-number!"),
                              (ScriptValue[]){ScriptString("gold"), ScriptFloat(42.5f)}, 2, NULL, &message) &&
                 ScriptInvoke(&host, ScriptBindingNamed("save-set-string!"),
                              (ScriptValue[]){ScriptString("hero"), ScriptString("Fionn")}, 2, NULL, &message);
    Check(setOk, "save-set-number! and save-set-string! remember values");

    ScriptValue gold = ScriptNone(), hero = ScriptNone();
    bool getOk = ScriptInvoke(&host, ScriptBindingNamed("save-get-number"),
                              (ScriptValue[]){ScriptString("gold")}, 1, &gold, &message) &&
                 gold.as.number == 42.5f &&
                 ScriptInvoke(&host, ScriptBindingNamed("save-get-string"),
                              (ScriptValue[]){ScriptString("hero")}, 1, &hero, &message) &&
                 !strcmp(hero.as.string, "Fionn");
    Check(getOk, "save-get-number and save-get-string read what was set");

    bool missing = ScriptInvoke(&host, ScriptBindingNamed("save-get-number"),
                                (ScriptValue[]){ScriptString("never-set")}, 1, &gold, &message) &&
                   gold.as.number == 0.0f;
    Check(missing, "a key nobody set reads as zero");

    const char *savePath = Scratch("regression_save.txt");
    bool wrote = ScriptInvoke(&host, ScriptBindingNamed("save-write"),
                              (ScriptValue[]){ScriptString(savePath)}, 1, NULL, &message) &&
                 ScriptInvoke(&host, ScriptBindingNamed("save-set-number!"),
                              (ScriptValue[]){ScriptString("gold"), ScriptFloat(1.0f)}, 2, NULL, &message) &&
                 ScriptInvoke(&host, ScriptBindingNamed("save-read"),
                              (ScriptValue[]){ScriptString(savePath)}, 1, NULL, &message) &&
                 ScriptInvoke(&host, ScriptBindingNamed("save-get-number"),
                              (ScriptValue[]){ScriptString("gold")}, 1, &gold, &message) &&
                 gold.as.number == 42.5f;
    Check(wrote, "save-write and save-read round-trip a file");

    // Debug queue: the first primitive makes the queue, and the binding reports it.
    ScriptValue queued = ScriptNone();
    bool debugOk = ScriptInvoke(&host, ScriptBindingNamed("debug-line"),
                                (ScriptValue[]){ScriptVector2((Vector2){0, 0}),
                                                ScriptVector2((Vector2){10, 10}),
                                                ScriptInt(0xffffffff), ScriptFloat(1.0f)},
                                4, &queued, &message) && queued.as.boolean;
    Check(debugOk, "debug-line queues a primitive on the host's queue");

    int count = 0;
    const ScriptBinding *table = ScriptBindings(&count);
    bool described = count > 20;
    for (int i = 0; i < count && described; i++)
        described = table[i].name && table[i].call && table[i].help &&
                    table[i].argumentCount >= 0 && table[i].argumentCount <= 8;
    Check(described, "every row of the table is complete enough for a frontend to register");
    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);

    // The other frontend, over the same table: the same entity, written in Pawn.
    char name[32];
    ScriptPawnName("move-world!", name, sizeof name);
    bool spelled = !strcmp(name, "move_world");
    GameplayWorld pawnWorld = {0};
    GameplayWorldInit(&pawnWorld, (GameplayWorldConfig){16, 0.1});
    ScriptHost pawnHost;
    ScriptHostInit(&pawnHost, &pawnWorld);
    bool opened = ScriptPawnOpen(&pawnHost, "tests/regression/tester.amx");
    EntityProperty pawnProperties[] = {{"position", "10 20"}, {"speed", "100"}};
    EntityHandle pawnEntity = EntitySpawnWith(&pawnWorld, "tester-pawn", pawnProperties, 2);
    ScriptEntity *pawnBody = EntityData(&pawnWorld, pawnEntity);
    GameplayWorldStep(&pawnWorld);
    Check(spelled && opened && pawnBody && pawnBody->slots[0].as.number == 100 &&
              fabsf(pawnBody->transform.translation.x - 20) < 0.001f,
          "the same entity in Pawn runs against the same table, with no second set of bindings");
    ScriptPawnClose();
    ScriptHostFree(&pawnHost);
    GameplayWorldFree(&pawnWorld);
}

// ---- a game's own calls, on the same table --------------------------------------------------
static float grappled;
SCRIPT_CALL(grapple, "grapple!", SCRIPT_FLOAT, "a call the engine knows nothing about",
            (SCRIPT_NONE, SCRIPT_VECTOR2))
{
    (void)host;
    grappled = a[0].as.vector2.x + a[0].as.vector2.y;
    return ScriptFloat(grappled);
}

static void GameCallChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){8, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    int engineRows = 0;
    ScriptBindings(&engineRows);
    bool added = ScriptAddBinding(&host, &grapple_binding);
    // A name the engine already uses cannot be taken, and neither can the same one twice.
    bool guarded = !ScriptAddBinding(&host, &grapple_binding) &&
                   ScriptBindingCount(&host) == engineRows + 1;

    ScriptS7Open(&host);
    char *answer = NULL;
    bool called = ScriptS7Eval("(grapple! (vec 2 3))", &answer);
    bool ran = called && grappled == 5;
    free(answer);
    answer = NULL;
    // And it is checked like any other row, from its declaration alone.
    bool checked = !ScriptS7Eval("(grapple! \"over there\")", &answer);
    free(answer);
    Check(added && guarded && ran && checked,
          "a game adds a call of its own, and Scheme gets it with the same checking");
    ScriptS7Close();

    // Pawn spells it its own way and declares it alongside the engine's, so a script can call it.
    char declarations[512];
    snprintf(declarations, sizeof declarations, "%s", Scratch("regression_engine.inc"));
    bool written = ScriptPawnWriteInclude(&host, declarations);
    char *text = LoadFileText(declarations);
    bool declared = text && strstr(text, "native Float:grapple(Float:a0x, Float:a0y);") != NULL;
    UnloadFileText(text);
    Check(written && declared, "the Pawn declarations the build writes include the game's calls");
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

// ---- per-class storage: no shared block to run over ---------------------------------------------
typedef struct Tiny
{
    int marker;
} Tiny;

static void StorageChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    ScriptS7Open(&host);

    // Registered first and tiny: this is what used to decide the stride for everybody.
    EntityRegister(&world, (EntityClass){.classname = "tiny",
                                         .size = sizeof(Tiny),
                                         .alignment = ENTITY_ALIGNMENT_OF(Tiny)});
    // A scripted class with every field a declaration allows, twenty times the size of the other.
    bool declared = ScriptS7Eval(
        "(define-entity \"big\""
        " '((\"a\" \"float\") (\"b\" \"float\") (\"c\" \"float\") (\"d\" \"float\")"
        "   (\"e\" \"float\") (\"f\" \"float\") (\"g\" \"float\") (\"h\" \"float\"))"
        " '())",
        NULL);

    EntityHandle before = EntitySpawn(&world, "tiny");
    Tiny *first = EntityData(&world, before);
    if (first)
        first->marker = 0x5a5a5a;
    EntityProperty properties[] = {{"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"},
                                   {"e", "5"}, {"f", "6"}, {"g", "7"}, {"h", "8"}};
    EntityHandle big = EntitySpawnWith(&world, "big", properties, 8);
    EntityHandle after = EntitySpawn(&world, "tiny");
    Tiny *second = EntityData(&world, after);
    if (second)
        second->marker = 0xa5a5a5;

    ScriptEntity *payload = EntityData(&world, big);
    bool whole = declared && payload && first && second;
    for (int i = 0; i < 8 && whole; i++)
        whole = payload->slots[i].as.number == (float)(i + 1);
    Check(whole && first->marker == 0x5a5a5a && second->marker == 0xa5a5a5 &&
              sizeof(ScriptEntity) > sizeof(Tiny) * 8,
          "a class far larger than its neighbours keeps every field, and touches nobody else's");

    // Spawning from C and from the REPL are the same path, so both agree about handles.
    char expression[64];
    snprintf(expression, sizeof expression, "(alive? %d)", ScriptHostIdOf(big));
    char *answer = NULL;
    ScriptS7Eval(expression, &answer);
    bool sees = answer && !strcmp(answer, "#t");
    free(answer);
    answer = NULL;
    EntityDestroy(&world, big);
    ScriptS7Eval(expression, &answer);
    bool gone = answer && !strcmp(answer, "#f");
    free(answer);
    Check(sees && gone && !EntityAlive(&world, big),
          "a handle means the same thing to C and to a script, before and after the entity goes");

    // A class that cannot describe its own payload is refused, loudly, at registration.
    bool refused = !EntityRegister(&world, (EntityClass){.classname = "no-size", .size = 0}) &&
                   !EntityRegister(&world, (EntityClass){.classname = "odd-alignment",
                                                         .size = 16, .alignment = 3}) &&
                   !EntityRegister(&world, (EntityClass){.classname = "ragged-size",
                                                         .size = 10, .alignment = 8});
    Check(refused, "a class whose size and alignment disagree cannot be registered");
    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

// ---- the runner: its own window, so it runs as a second pass -------------------------------------
static int updates;
static double engineStep, worldStep;
static bool CountingUpdate(GameplayRuntime *r, double dt, const EngineInput *input)
{
    (void)input;
    engineStep = dt;
    worldStep = r->world.tickInterval;
    return ++updates < 4;
}

static int RunnerChecks(void)
{
    static GameplayRuntime runtime;
    GameplayProject project = GameplayProjectDefault();
    project.config.windowFlags = FLAG_WINDOW_HIDDEN;
    project.config.targetFps = 0;
    project.Update = CountingUpdate;
    EngineApplication app = GameplayApplication(&runtime, project);
    app.config.fixed_dt = 1.0 / 30.0; // the caller edits the descriptor it was handed
    EngineRunApplication(&app);
    Check(updates > 0 && engineStep == worldStep,
          "gameplay ticks at the rate the engine was actually started with");
    printf("REGRESSION TEST (runner) failures=%d checks=%d\n", failures, checks);
    return failures ? 1 : 0;
}

// Fallout's own published conversions, kept here verbatim as the reference the grid is checked
// against: if core/iso_grid.c ever stops agreeing with these, it has stopped being that grid.
static void FalloutHexToScreen(int x, int y, int *sx, int *sy)
{
    *sx = 4816 - ((((x + 1) >> 1) << 5) + ((x >> 1) << 4) - (y << 4));
    *sy = ((12 * (x >> 1)) + (y * 12)) + 11;
}
static void FalloutTileToScreen(int x, int y, int *sx, int *sy)
{
    x = 99 - x;
    *sx = 4752 + (32 * y) - (48 * x);
    *sy = (24 * y) + (12 * x);
}
static bool GapInWall(void *user, IsoHex hex)
{
    (void)user;
    return hex.x == 5 && hex.y != 9; // a wall down one column with a single way through
}
static void IsoGridChecks(void)
{
    bool hexMatches = true, tileMatches = true, pairsUp = true, hexRoundTrips = true,
         tileRoundTrips = true;
    for (int x = 0; x < 40 && hexMatches; x++)
        for (int y = 0; y < 40; y++)
        {
            int sx, sy;
            FalloutHexToScreen(x, y, &sx, &sy);
            Vector2 ours = IsoHexToScreen((IsoHex){x, y});
            if ((int)ours.x + 4816 != sx || (int)ours.y + 11 != sy)
            {
                hexMatches = false;
                break;
            }
        }
    for (int x = 0; x < 40 && tileMatches; x++)
        for (int y = 0; y < 40; y++)
        {
            int sx, sy;
            FalloutTileToScreen(99 - x, y, &sx, &sy);
            Vector2 ours = IsoTileToScreen((IsoTile){x, y});
            if ((int)ours.x + 4752 != sx || (int)ours.y != sy)
            {
                tileMatches = false;
                break;
            }
        }
    Check(hexMatches, "hex projection agrees with Fallout's own hex conversion");
    Check(tileMatches, "tile projection agrees with Fallout's own tile conversion");
    // One tile is a 2x2 block of hexes, which is the whole reason the two grids can share ground.
    for (int x = 0; x < 40 && pairsUp; x++)
        for (int y = 0; y < 40; y++)
        {
            Vector2 tile = IsoTileToScreen((IsoTile){x, y});
            Vector2 hex = IsoHexToScreen(IsoTileHex((IsoTile){x, y}));
            IsoTile back = IsoHexTile(IsoTileHex((IsoTile){x, y}));
            if (tile.x != hex.x || tile.y != hex.y || back.x != x || back.y != y)
            {
                pairsUp = false;
                break;
            }
        }
    Check(pairsUp, "each tile is the 2x2 hex block at the same place on screen");
    for (int x = 0; x < 40 && hexRoundTrips; x++)
        for (int y = 0; y < 40; y++)
        {
            IsoHex hex = {x, y};
            IsoHex back = IsoScreenToHex(IsoHexToScreen(hex));
            if (back.x != x || back.y != y)
            {
                hexRoundTrips = false;
                break;
            }
        }
    for (int x = 0; x < 40 && tileRoundTrips; x++)
        for (int y = 0; y < 40; y++)
        {
            // A hair inside the cell: its own corner belongs to a neighbour just as legitimately.
            Vector2 screen = IsoTileToScreen((IsoTile){x, y});
            screen.x += 1;
            screen.y += 1;
            IsoTile back = IsoScreenToTile(screen);
            if (back.x != x || back.y != y)
            {
                tileRoundTrips = false;
                break;
            }
        }
    Check(hexRoundTrips, "a hex centre picks its own hex back out of the screen");
    Check(tileRoundTrips, "a point inside a tile picks its own tile back out of the screen");
    // Neighbours must be mutual, distinct, and one step away in both directions.
    bool reciprocal = true, distinct = true, adjacent = true;
    for (int x = 0; x < 20; x++)
        for (int y = 0; y < 20; y++)
        {
            IsoHex hex = {x, y};
            for (int dir = 0; dir < ISO_DIR_COUNT; dir++)
            {
                IsoHex step = IsoHexNeighbour(hex, (IsoDir)dir);
                IsoHex home = IsoHexNeighbour(step, (IsoDir)((dir + 3) % ISO_DIR_COUNT));
                reciprocal &= home.x == hex.x && home.y == hex.y;
                adjacent &= IsoHexDistance(hex, step) == 1;
                for (int other = 0; other < dir; other++)
                {
                    IsoHex was = IsoHexNeighbour(hex, (IsoDir)other);
                    distinct &= was.x != step.x || was.y != step.y;
                }
            }
        }
    Check(reciprocal, "stepping to a neighbour and back again returns to the same hex");
    Check(distinct, "the six neighbours of a hex are six different hexes");
    Check(adjacent, "every neighbour is exactly one step away");
    // The real proof that the distance is a hex distance: a breadth-first search agrees with it.
    enum
    {
        SPAN = 20
    };
    int bfs[SPAN][SPAN];
    for (int x = 0; x < SPAN; x++)
        for (int y = 0; y < SPAN; y++)
            bfs[x][y] = -1;
    IsoHex queue[SPAN * SPAN];
    int head = 0, tail = 0;
    IsoHex origin = {10, 10};
    bfs[origin.x][origin.y] = 0;
    queue[tail++] = origin;
    while (head < tail)
    {
        IsoHex hex = queue[head++];
        for (int dir = 0; dir < ISO_DIR_COUNT; dir++)
        {
            IsoHex step = IsoHexNeighbour(hex, (IsoDir)dir);
            if (step.x < 0 || step.y < 0 || step.x >= SPAN || step.y >= SPAN)
                continue;
            if (bfs[step.x][step.y] >= 0)
                continue;
            bfs[step.x][step.y] = bfs[hex.x][hex.y] + 1;
            queue[tail++] = step;
        }
    }
    bool distanceAgrees = true;
    for (int x = 0; x < SPAN; x++)
        for (int y = 0; y < SPAN; y++)
        {
            int measured = IsoHexDistance(origin, (IsoHex){x, y});
            if (measured <= 5 && bfs[x][y] != measured)
                distanceAgrees = false;
        }
    Check(distanceAgrees, "hex distance agrees with a breadth-first walk of the same grid");
    // Facing: stepping in a direction should read back as that direction.
    bool facingReads = true;
    for (int x = 0; x < 20; x++)
        for (int y = 0; y < 20; y++)
            for (int dir = 0; dir < ISO_DIR_COUNT; dir++)
            {
                IsoHex hex = {x, y};
                IsoHex step = IsoHexNeighbour(hex, (IsoDir)dir);
                facingReads &= IsoHexDirection(hex, step) == (IsoDir)dir;
            }
    Check(facingReads, "a step in one of the six directions reads back as that direction");
    // Cell outlines. A cell that tiles the plane has exactly the area of the lattice it belongs to,
    // so an outline that overlaps its neighbours or leaves a gap between them fails this.
    Vector2 tileCell[4];
    IsoTileCorners((IsoTile){3, 5}, tileCell);
    float tileArea = 0;
    for (int i = 0; i < 4; i++)
    {
        Vector2 a = tileCell[i], b = tileCell[(i + 1) % 4];
        tileArea += a.x * b.y - b.x * a.y;
    }
    tileArea = fabsf(tileArea) / 2;
    Check(tileArea == 1536.0f, "a tile's outline covers exactly one tile of ground");
    Vector2 hexCell[6];
    IsoHexCellCorners((IsoHex){7, 9}, hexCell);
    float hexArea = 0;
    for (int i = 0; i < 6; i++)
    {
        Vector2 a = hexCell[i], b = hexCell[(i + 1) % 6];
        hexArea += a.x * b.y - b.x * a.y;
    }
    hexArea = fabsf(hexArea) / 2;
    Check(hexArea == 384.0f, "a hex's outline covers exactly one hex of ground");
    // And the outline agrees with which hex a point actually belongs to: just inside every corner
    // is still this hex's ground.
    IsoHex owner = {7, 9};
    Vector2 centre = IsoHexToScreen(owner);
    bool cornersBelong = true;
    for (int i = 0; i < 6; i++)
    {
        Vector2 inside = {centre.x + (hexCell[i].x - centre.x) * 0.9f,
                          centre.y + (hexCell[i].y - centre.y) * 0.9f};
        IsoHex at = IsoScreenToHex(inside);
        cornersBelong &= at.x == owner.x && at.y == owner.y;
    }
    Check(cornersBelong, "ground just inside a hex's outline belongs to that hex");
    // Painting order. Storage order is not draw order, and getting it wrong is a character standing
    // in front of the wall that should hide it.
    IsoDrawItem items[64];
    int count = 0;
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            items[count++] = (IsoDrawItem){{x, y}, NULL};
    // Shuffle deterministically, so the sort is doing the work and not the order they went in.
    for (int i = count - 1; i > 0; i--)
    {
        int j = (i * 7919 + 13) % (i + 1);
        IsoDrawItem swap = items[i];
        items[i] = items[j];
        items[j] = swap;
    }
    IsoDepthSort(items, count);
    bool farToNear = true;
    for (int i = 1; i < count; i++)
        farToNear &= IsoHexToScreen(items[i - 1].hex).y <= IsoHexToScreen(items[i].hex).y;
    Check(farToNear, "painting order runs from furthest to nearest");
    // A hex one step toward the viewer must be painted after the one it stands in front of, in
    // whichever order the two were handed over.
    bool coversBoth = true;
    for (int dir = 0; dir < ISO_DIR_COUNT; dir++)
    {
        IsoHex behind = {4, 4}, front = IsoHexNeighbour(behind, (IsoDir)dir);
        if (IsoHexToScreen(front).y <= IsoHexToScreen(behind).y)
            continue; // a level step: neither is in front, nothing to prove
        IsoDrawItem pair[2] = {{front, NULL}, {behind, NULL}};
        IsoDepthSort(pair, 2);
        coversBoth &= pair[0].hex.x == behind.x && pair[0].hex.y == behind.y;
        IsoDrawItem swapped[2] = {{behind, NULL}, {front, NULL}};
        IsoDepthSort(swapped, 2);
        coversBoth &= swapped[0].hex.x == behind.x && swapped[0].hex.y == behind.y;
    }
    Check(coversBoth, "a nearer hex is painted over the hex it stands in front of, given either way round");
    // Two hexes at the same depth are ordered the same way every time they are sorted.
    IsoHex level = IsoHexNeighbour((IsoHex){4, 4}, ISO_E);
    Check(IsoHexToScreen(level).y == IsoHexToScreen((IsoHex){4, 4}).y,
          "a level step really is level, so the tie is a real tie");
    IsoDrawItem tie[2] = {{level, NULL}, {{4, 4}, NULL}}, retie[2] = {{{4, 4}, NULL}, {level, NULL}};
    IsoDepthSort(tie, 2);
    IsoDepthSort(retie, 2);
    Check(tie[0].hex.x == retie[0].hex.x && tie[0].hex.y == retie[0].hex.y,
          "items at the same depth are ordered the same way whichever order they arrive in");
}
static void PlaybackChecks(void)
{
    // One rule for where a clip has got to, so skeletal animation and sprite sheets cannot drift
    // apart on it. Four frames at four a second: a second is exactly one time round.
    CoreClip looping = {4, 4.0f, true}, once = {4, 4.0f, false};
    Check(CoreClipFrame(looping, 0.0) == 0 && CoreClipFrame(looping, 0.30) == 1 &&
              CoreClipFrame(looping, 0.80) == 3,
          "a clip runs through its frames at the rate it declares");
    Check(CoreClipFrame(looping, 1.0) == 0 && CoreClipFrame(looping, 1.30) == 1,
          "a looping clip starts over rather than running off the end");
    Check(CoreClipFrame(once, 1.0) == 3 && CoreClipFrame(once, 50.0) == 3,
          "a clip that does not loop holds its last frame");
    Check(CoreClipFrame(looping, -5.0) == 0, "time before the start reads as the first frame");
    int from = -1, to = -1;
    float between = -1;
    CoreClipBlend(looping, 0.625, &from, &to, &between);
    Check(from == 2 && to == 3 && between > 0.49f && between < 0.51f,
          "blending reports the frames either side and how far between them");
    CoreClipBlend(looping, 0.875, &from, &to, &between);
    Check(from == 3 && to == 0, "a looping clip blends its last frame back into its first");
    CoreClipBlend(once, 0.875, &from, &to, &between);
    Check(from == 3 && to == 3, "a clip that does not loop has nothing ahead to blend into");
}
static void SpriteSheetChecks(void)
{
    // A cell is taller than what it holds, so drawing it by its bottom edge leaves the subject
    // floating above whatever it is standing on. The anchor is what stops that being guesswork.
    SpriteSheet sheet = {0};
    sheet.meta = (SpriteSheetMeta){96, 112, 4, 3, 10, 10.0f, true, 48, 65};
    Rectangle at = SpriteSheetGroundRect(&sheet, (Vector2){500, 300});
    Check(at.x == 500 - 48 && at.y == 300 - 65,
          "a frame is placed so its ground anchor lands on the point it stands on");
    Check(at.width == 96 && at.height == 112, "a placed frame keeps its own size");
    Check(at.y + sheet.meta.anchorY == 300, "the anchor, not the cell edge, meets the ground");
    // The format carries the anchor through a write and a read.
    const char *path = Scratch("regression_sheet.txt");
    Check(SpriteSheetWriteMeta(path, &sheet.meta), "a sheet writes its metadata");
    char *text = CoreReadFile(path);
    Check(text && strstr(text, "anchorx 48") && strstr(text, "anchory 65"),
          "a written sheet records where its subject meets the ground");
    if (text)
        CoreFreeFile(text);
    // The sheet must answer with the shared rule, not one of its own.
    bool agrees = true;
    for (int i = 0; i < 40; i++)
    {
        double at = i * 0.037;
        agrees &= SpriteSheetFrameAt(&sheet, at) == CoreClipFrame(SpriteSheetClip(&sheet), at);
    }
    Check(agrees, "a sheet reports the same frame the shared clip rule does");
    // A sheet carries its own clock, the way Actor does, so a caller never holds one itself.
    SpriteAnim anim = {0};
    SpriteAnimPlay(&anim, &sheet);
    SpriteAnimUpdate(&anim, 0.25);
    int mid = SpriteAnimFrame(&anim);
    SpriteSheet other = sheet;
    SpriteAnimView(&anim, &other);
    Check(SpriteAnimFrame(&anim) == mid && anim.sheet == &other,
          "changing which view is showing does not restart the motion");
    SpriteAnimPlay(&anim, &sheet);
    Check(SpriteAnimFrame(&anim) == 0, "playing a sheet starts it at its first frame");
    SpriteSheetMeta bad = sheet.meta;
    bad.anchorY = bad.cellHeight + 1;
    Check(!SpriteSheetWriteMeta(path, &bad), "an anchor outside its own cell is refused");
}
static void SpritePresentationChecks(void)
{
    SpritePresentation present = SpritePresentationDefault();
    Check(present.scale == 1 && present.tint.r == 255 && !present.flipX && !present.flipY,
          "a sprite presentation defaults to an unflipped white unit-scale sprite");
    SpriteDrawItem items[] = {
        {.presentation = {.layer = 1, .order = 5}, .sequence = 4},
        {.presentation = {.layer = 0, .order = 99}, .sequence = 8},
        {.presentation = {.layer = 1, .order = 5}, .sequence = 2},
        {.presentation = {.layer = 1, .order = 2}, .sequence = 9},
    };
    qsort(items, sizeof items / sizeof *items, sizeof *items, SpriteDrawItemCompare);
    Check(items[0].presentation.layer == 0 && items[1].presentation.order == 2 &&
              items[2].sequence == 2 && items[3].sequence == 4,
          "sprite draw order is layer, then order, then explicit stable sequence");
}
/* A row must not be named for a language's syntax. Scheme will let a row shadow one of its
   procedures -- `log` does, and works -- but not one of its special forms: `(set! "f" 1)` is read as
   assignment however the row is registered, so the engine's function is simply never called and the
   rest of the script's expression goes with it. That cost a scripted entity its rescheduling.

   Asking s7 at runtime does not catch it: the name resolves to our value, so `(procedure? set!)`
   answers #t while `(set! ...)` in operator position is still syntax. The rule has to be stated. */
static void RowNameChecks(void)
{
    static const char *syntax[] = {"set!",   "if",     "define", "lambda", "let",  "let*",
                                   "letrec", "begin",  "quote",  "do",     "cond", "case",
                                   "and",    "or",     "when",   "unless", "else", "define-macro"};
    int count = 0;
    const ScriptBinding *table = ScriptBindings(&count);
    const char *clash = NULL;
    for (int i = 0; i < count; i++)
        for (size_t k = 0; k < sizeof syntax / sizeof *syntax; k++)
            if (!strcmp(table[i].name, syntax[k]))
                clash = table[i].name;
    if (clash)
        printf("  row named for Scheme syntax: %s\n", clash);
    Check(count > 0 && !clash, "no row is named for a language's own syntax");
}
static void SheetCacheChecks(void)
{
    // A script names a sheet the way it names a sound. The point of the cache is that naming the
    // same one twice costs one load and hands back the same copy, not two textures of the same art.
    const char *name = Scratch("regression_cached");
    char atlas[300];
    snprintf(atlas, sizeof atlas, "%s.png", name);
    char meta[300];
    snprintf(meta, sizeof meta, "%s.sheet", name);
    Image art = GenImageColor(64, 32, BLUE);
    bool wrote = ExportImage(art, atlas);
    UnloadImage(art);
    SpriteSheetMeta described = {32, 32, 2, 1, 2, 8.0f, true, 16, 30};
    wrote = wrote && SpriteSheetWriteMeta(meta, &described);
    Check(wrote, "a sheet can be written for the cache to find");

    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){8, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    const SpriteSheet *once = ScriptHostSheet(&host, name);
    const SpriteSheet *twice = ScriptHostSheet(&host, name);
    Check(once && once == twice, "naming the same sheet twice loads it once and hands back that one");
    Check(once && once->atlas.id && once->meta.anchorY == 30,
          "a cached sheet arrives loaded, anchor and all");
    Check(!ScriptHostSheet(&host, Scratch("no-such-sheet")), "a sheet that is not there is refused");
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}
static void IsoMoveChecks(void)
{
    IsoPathfinder finder;
    Check(IsoPathfinderInit(&finder, 20, 20), "pathfinder sizes itself to the grid");
    IsoPath path;
    Check(IsoPathInit(&path, 512), "path allocates");
    // Open ground: the route is as long as the distance, and every step is a real neighbour.
    IsoHex from = {2, 2}, to = {14, 11};
    Check(IsoPathfinderSolve(&finder, &path, from, to, NULL, NULL), "a route across open ground exists");
    Check(path.count == IsoHexDistance(from, to) + 1,
          "an open-ground route is exactly as long as the hex distance");
    bool contiguous = path.count > 1;
    for (int i = 1; i < path.count; i++)
        contiguous &= IsoHexDistance(path.hexes[i - 1], path.hexes[i]) == 1;
    Check(contiguous, "every step of a route enters an adjacent hex");
    Check(path.hexes[0].x == from.x && path.hexes[0].y == from.y &&
              path.hexes[path.count - 1].x == to.x && path.hexes[path.count - 1].y == to.y,
          "a route starts where asked and ends where asked");
    // A wall with one gap: the route must exist, and must be longer than the open-ground one.
    IsoHex left = {2, 2}, right = {9, 2};
    int direct = IsoHexDistance(left, right);
    Check(IsoPathfinderSolve(&finder, &path, left, right, GapInWall, NULL), "a route finds the gap in a wall");
    Check(path.count - 1 > direct, "going round a wall costs more than going straight");
    bool avoidsWall = true;
    for (int i = 0; i < path.count; i++)
        avoidsWall &= !GapInWall(NULL, path.hexes[i]);
    Check(avoidsWall, "a route never enters a blocked hex");
    Check(!IsoPathfinderSolve(&finder, &path, left, (IsoHex){5, 3}, GapInWall, NULL),
          "there is no route onto a blocked hex");
    Check(!IsoPathfinderSolve(&finder, &path, left, (IsoHex){99, 99}, NULL, NULL),
          "there is no route off the edge of the grid");
    IsoPathFree(&path);
    // A walk runs its whole route unless the caller cuts it; what rations movement is the game's.
    IsoMover mover;
    Check(IsoMoverInit(&mover, (IsoHex){2, 2}, 512), "a mover starts on its hex");
    IsoHex goal = {14, 11};
    Check(IsoMoverGoTo(&mover, &finder, goal, NULL, NULL), "a walk sets off");
    Check(IsoMoverRemainingSteps(&mover) == IsoHexDistance((IsoHex){2, 2}, goal),
          "a fresh walk has one step per hex of the route");
    mover.hexesPerSecond = 1000;
    IsoMoverUpdate(&mover, 1.0f);
    Check(mover.hex.x == goal.x && mover.hex.y == goal.y, "an uncut walk arrives");
    Check(!IsoMoverMoving(&mover), "a walk that arrives has stopped");
    // Cut short: the walk stops exactly where the caller allowed, and no further.
    mover.hex = (IsoHex){2, 2};
    Check(IsoMoverGoTo(&mover, &finder, goal, NULL, NULL), "a walk to be cut sets off");
    IsoMoverTruncate(&mover, 3);
    Check(IsoMoverRemainingSteps(&mover) == 3, "a cut walk keeps only the steps it was allowed");
    IsoMoverUpdate(&mover, 1.0f);
    Check(IsoHexDistance((IsoHex){2, 2}, mover.hex) == 3, "a cut walk stops where it was cut");
    Check(!IsoMoverMoving(&mover), "a cut walk ends rather than carrying on");
    // Cutting to more than the route has leaves the route alone.
    mover.hex = (IsoHex){2, 2};
    IsoMoverGoTo(&mover, &finder, goal, NULL, NULL);
    int full = IsoMoverRemainingSteps(&mover);
    IsoMoverTruncate(&mover, full + 50);
    Check(IsoMoverRemainingSteps(&mover) == full, "cutting past the end of a route changes nothing");
    IsoMoverFree(&mover);
    IsoPathfinderFree(&finder);
}

// ---- transport connects two clients and preserves message boundaries ---------------------------
static void NetworkChecks(void)
{
    unsigned char encoded[9];
    CoreNetWriter writer = CoreNetWriterBegin(encoded, sizeof encoded);
    Check(CoreNetWriteU8(&writer, 0x7a) && CoreNetWriteU32(&writer, 0x12345678u) &&
              CoreNetWriteF32(&writer, -3.25f) && writer.size == sizeof encoded,
          "network writer encodes fixed-width values within capacity");
    Check(!CoreNetWriteU8(&writer, 1) && writer.failed,
          "network writer fails without overflowing its destination");
    CoreNetReader reader = CoreNetReaderBegin(encoded, sizeof encoded);
    uint8_t byte = 0;
    uint32_t integer = 0;
    float number = 0.0f;
    Check(CoreNetReadU8(&reader, &byte) && CoreNetReadU32(&reader, &integer) &&
              CoreNetReadF32(&reader, &number) && byte == 0x7a && integer == 0x12345678u &&
              number == -3.25f,
          "network reader reverses the fixed-width wire encoding");
    Check(!CoreNetReadU8(&reader, &byte) && reader.failed,
          "network reader fails safely at the end of a message");

    CoreNetEndpoint server = {0}, clients[2] = {{0}};
    Check(!CoreNetOpenServer(NULL, 0, 2, 2), "network rejects missing endpoint storage");
    Check(!CoreNetOpenServer(&server, 0, 0, 2), "network rejects a server with no peer capacity");
    if (!CoreNetOpenServer(&server, 0, 2, 2))
    {
        Check(false, "network opens a server on an available port");
        return;
    }
    Check(CoreNetPort(&server) != 0, "network reports the operating system assigned port");
    Check(!CoreNetOpenServer(&server, 0, 2, 2), "network cannot overwrite an open endpoint");
    bool opened = CoreNetOpenClient(&clients[0], 2) && CoreNetOpenClient(&clients[1], 2);
    Check(opened, "network opens two clients in one process");
    if (!opened)
    {
        CoreNetClose(&clients[0]);
        CoreNetClose(&clients[1]);
        CoreNetClose(&server);
        return;
    }
    CoreNetPeer clientPeers[2] = {
        CoreNetConnect(&clients[0], "127.0.0.1", CoreNetPort(&server)),
        CoreNetConnect(&clients[1], "127.0.0.1", CoreNetPort(&server)),
    };
    Check(clientPeers[0] != CORE_NET_PEER_NONE && clientPeers[1] != CORE_NET_PEER_NONE,
          "network begins both loopback connections");
    Check(CoreNetConnect(&clients[0], "127.0.0.1", 0) == CORE_NET_PEER_NONE,
          "network rejects a zero destination port");

    bool clientConnected[2] = {false, false};
    int serverConnections = 0;
    for (int attempt = 0; attempt < 1000 &&
                          (serverConnections < 2 || !clientConnected[0] || !clientConnected[1]);
         attempt++)
    {
        CoreNetEvent event;
        if (CoreNetPoll(&server, 1, &event))
        {
            serverConnections += event.type == CORE_NET_EVENT_CONNECTED;
            CoreNetEventFree(&event);
        }
        for (int i = 0; i < 2; i++)
            if (CoreNetPoll(&clients[i], 0, &event))
            {
                clientConnected[i] |= event.type == CORE_NET_EVENT_CONNECTED;
                CoreNetEventFree(&event);
            }
    }
    Check(serverConnections == 2 && clientConnected[0] && clientConnected[1],
          "server and both clients receive connection events");

    const unsigned char identities[2] = {11, 22};
    for (int i = 0; i < 2; i++)
        Check(CoreNetSend(&clients[i], clientPeers[i], 0, &identities[i], 1, true),
              "client queues a reliable identified message");
    Check(!CoreNetSend(&clients[0], clientPeers[0], 2, identities, 1, true),
          "network rejects a channel outside the endpoint contract");
    CoreNetFlush(&clients[0]);
    CoreNetFlush(&clients[1]);

    bool serverSaw[2] = {false, false};
    for (int attempt = 0; attempt < 1000 && (!serverSaw[0] || !serverSaw[1]); attempt++)
    {
        CoreNetEvent event;
        if (!CoreNetPoll(&server, 1, &event))
            continue;
        if (event.type == CORE_NET_EVENT_RECEIVED && event.channel == 0 && event.size == 1)
        {
            serverSaw[0] |= event.data[0] == identities[0];
            serverSaw[1] |= event.data[0] == identities[1];
        }
        CoreNetEventFree(&event);
    }
    Check(serverSaw[0] && serverSaw[1], "reliable messages arrive intact from both clients");

    const unsigned char snapshot[] = {0x54, 0x4e, 0x01, 0x02};
    Check(CoreNetBroadcast(&server, 1, snapshot, sizeof snapshot, false),
          "server queues an unreliable sequenced snapshot");
    CoreNetFlush(&server);
    bool gotSnapshot[2] = {false, false};
    for (int attempt = 0; attempt < 1000 && (!gotSnapshot[0] || !gotSnapshot[1]); attempt++)
        for (int i = 0; i < 2; i++)
        {
            CoreNetEvent event;
            if (!CoreNetPoll(&clients[i], i == 0 ? 1 : 0, &event))
                continue;
            gotSnapshot[i] |= event.type == CORE_NET_EVENT_RECEIVED && event.channel == 1 &&
                              event.size == sizeof snapshot &&
                              !memcmp(event.data, snapshot, sizeof snapshot);
            CoreNetEventFree(&event);
        }
    Check(gotSnapshot[0] && gotSnapshot[1], "one snapshot broadcast reaches both clients");

    CoreNetClose(&clients[0]);
    CoreNetClose(&clients[1]);
    CoreNetClose(&server);
    CoreNetClose(&server);
}

typedef struct NetTestState
{
    Vector3 position;
    float yaw;
    int32_t health;
    bool carrying;
} NetTestState;

static void NetSyncChecks(void)
{
    const CoreNetField fields[] = {
        CORE_NET_FIELD_LERP(NetTestState, position, CORE_NET_VECTOR3),
        CORE_NET_FIELD_LERP(NetTestState, yaw, CORE_NET_F32),
        CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
        CORE_NET_FIELD(NetTestState, carrying, CORE_NET_BOOL),
    };
    const CoreNetSchema schema = {7, "player", sizeof(NetTestState), fields,
                                  sizeof fields / sizeof fields[0], CORE_NET_AUTHORITY_OWNER};
    CoreNetSync server = {0}, client = {0};
    Check(CoreNetSyncInit(&server, 4, 2) && CoreNetSyncInit(&client, 4, 2),
          "replication allocates server and client registries");
    Check(CoreNetSyncRegister(&server, &schema) && CoreNetSyncRegister(&client, &schema),
          "replication registers the same object schema at both ends");
    Check(!CoreNetSyncRegister(&server, &schema), "replication rejects duplicate schema IDs");

    CoreNetObject *source = CoreNetSyncSpawn(&server, 42, schema.type, 2);
    Check(source != NULL, "replication spawns a stable network object identity");
    NetTestState *state = source ? source->state : NULL;
    if (!state)
    {
        CoreNetSyncFree(&client);
        CoreNetSyncFree(&server);
        return;
    }
    *state = (NetTestState){{2, 4, 6}, 0.5f, 80, true};
    Check(CoreNetObjectCanWrite(source, 2, false) && !CoreNetObjectCanWrite(source, 3, false) &&
              CoreNetObjectCanWrite(source, 0, true),
          "replication distinguishes owner input authority from server state authority");

    unsigned char packet[512];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    Check(CoreNetSyncWrite(&server, 11, &writer), "replication writes a complete snapshot");
    CoreNetReader reader = CoreNetReaderBegin(packet, writer.size);
    uint32_t tick = 0;
    Check(CoreNetSyncRead(&client, &reader, &tick) && tick == 11,
          "replication reads a complete snapshot and its simulation tick");
    CoreNetObject *copy = CoreNetSyncFind(&client, 42);
    NetTestState *received = copy ? copy->state : NULL;
    Check(received && received->position.x == 2 && received->position.y == 4 &&
              received->position.z == 6 && received->yaw == 0.5f && received->health == 80 &&
              received->carrying,
          "replication creates remote objects and applies every described field");

    *received = (NetTestState){{3, 5, 7}, 0.75f, 70, false};
    writer = CoreNetWriterBegin(packet, sizeof packet);
    Check(CoreNetObjectWrite(copy, &writer), "an owner can encode one object update");
    reader = CoreNetReaderBegin(packet, writer.size);
    Check(CoreNetSyncReadObject(&server, &reader, 2, false) && state->position.x == 3 &&
              state->health == 70 && !state->carrying,
          "the server accepts a complete update from the object's owner");
    NetTestState accepted = *state;
    reader = CoreNetReaderBegin(packet, writer.size);
    Check(!CoreNetSyncReadObject(&server, &reader, 3, false) &&
              !memcmp(&accepted, state, sizeof accepted),
          "the server rejects another actor's object update without changing state");
    reader = CoreNetReaderBegin(packet, writer.size - 1);
    Check(!CoreNetSyncReadObject(&server, &reader, 2, false) &&
              !memcmp(&accepted, state, sizeof accepted),
          "a truncated object update cannot partially change state");

    *state = (NetTestState){{6, 8, 10}, 1.5f, 35, false};
    writer = CoreNetWriterBegin(packet, sizeof packet);
    CoreNetSyncWrite(&server, 12, &writer);
    reader = CoreNetReaderBegin(packet, writer.size);
    Check(CoreNetSyncRead(&client, &reader, &tick), "replication accepts a later snapshot");
    copy = CoreNetSyncFind(&client, 42);
    NetTestState sampled = {0};
    Check(CoreNetObjectSample(copy, 0.5f, &sampled) && sampled.position.x == 4.5f &&
              sampled.position.y == 6.5f && sampled.position.z == 8.5f && sampled.yaw == 1.125f &&
              sampled.health == 35 && !sampled.carrying,
          "replication interpolates marked fields and applies discrete fields immediately");

    NetTestState before = *(NetTestState *)copy->state;
    reader = CoreNetReaderBegin(packet, writer.size - 1);
    Check(!CoreNetSyncRead(&client, &reader, &tick) &&
              !memcmp(&before, copy->state, sizeof before),
          "a truncated snapshot is rejected without partially changing live state");

    CoreNetSyncDespawn(&server, 42);
    writer = CoreNetWriterBegin(packet, sizeof packet);
    CoreNetSyncWrite(&server, 13, &writer);
    reader = CoreNetReaderBegin(packet, writer.size);
    Check(CoreNetSyncRead(&client, &reader, &tick) && !CoreNetSyncFind(&client, 42),
          "a complete snapshot removes objects that no longer exist");
    CoreNetSyncFree(&client);
    CoreNetSyncFree(&server);
}

int main(int argc, char **argv)
{
    SetTraceLogLevel(LOG_WARNING);
    if (argc > 1 && !strcmp(argv[1], "--runner"))
        return RunnerChecks();
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(320, 240, "regression");
    if (!UiInit(&ui, UiThemeDefault()))
        return 1;
    scratch = LoadRenderTexture(320, 240);
    FrameTextureChecks();
    InputChecks();
    DrawingChecks();
    LayoutChecks();
    ActivationChecks();
    FileChecks();
    TextureChecks();
    ConventionChecks();
    DeferredChecks();
    ScriptChecks();
    StorageChecks();
    GameCallChecks();
    PlaybackChecks();
    SpriteSheetChecks();
    SpritePresentationChecks();
    RowNameChecks();
    SheetCacheChecks();
    IsoGridChecks();
    IsoMoveChecks();
    NetworkChecks();
    NetSyncChecks();
    UnloadRenderTexture(scratch);
    UiFree(&ui);
    CloseWindow();
    printf("REGRESSION TEST failures=%d checks=%d\n", failures, checks);
    return failures ? 1 : 0;
}
