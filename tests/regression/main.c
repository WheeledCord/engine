/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One check per bug that has been fixed here, so none of them can come back unnoticed. Each one
// asserts on a value read back: a pixel, a rectangle, a return code. Run with no arguments for the
// checks that share one window, and with --runner for the ones that need the engine's own loop.
#define _DEFAULT_SOURCE
#include "checks.h"
#include "core/collision3d.h"
#include "core/engine.h"
#include "core/fps_camera.h"
#include "core/frame_uniforms.h"
#include "core/iso_grid.h"
#include "core/network.h"
#include "core/net_clock.h"
#include "core/net_sync.h"
#include "core/node.h"
#include "core/net_session.h"
#include "core/object.h"
#include "core/audio.h"
#include "core/particles.h"
#include "core/sprite_sheet.h"
#include "core/texture.h"
#include "core/playback.h"
#include "core/skeleton.h"
#include "core/animation.h"
#include "core/rig_file.h"
#include "core/file.h"
#include "core/save.h"
#include "core/input_map.h"
#include "core/waypoints.h"
#include "gameplay/iso_move.h"
#include "core/transform.h"
#include "core/ui.h"
#include "core/ui_document.h"
#include "gameplay/runtime.h"
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
#include <unistd.h>

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

    CoreMouseCapture mouseCapture = {0};
    Vector2 firstLook = CoreMouseCaptureUpdate(&mouseCapture, true);
    Check(firstLook.x == 0 && firstLook.y == 0,
          "entering relative pointer mode discards its cursor rebase");
    CoreMouseCaptureRelease(&mouseCapture);
    Check(!mouseCapture.active && mouseCapture.width == 0 && mouseCapture.height == 0 &&
              !IsCursorHidden(),
          "releasing relative pointer mode restores the cursor and clears transition state");
    Vector2 absentLook = CoreMouseCaptureUpdate(NULL, true);
    Check(absentLook.x == 0 && absentLook.y == 0,
          "a missing relative pointer owner fails without camera motion");
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
// Evaluates Scheme and compares the printed answer, so a script check reads as what it expects.
static bool EvalIs(const char *expression, const char *expected)
{
    char *answer = NULL;
    bool ok = ScriptS7Eval(expression, &answer);
    bool same = ok && answer && !strcmp(answer, expected);
    if (!same)
        printf("  %s => %s (wanted %s)\n", expression, answer ? answer : "(error)", expected);
    free(answer);
    return same;
}

static void ScriptChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptS7Open(&host), "the Scheme frontend registers the binding table");
    bool declared = ScriptS7Eval(
        "(begin"
        " (define (t-spawn self) (self 'think-next!))"
        " (define (t-think self) (self 'move-world! (vec (* (self 'speed) (dt)) 0)) (self 'think-next!))"
        " (define-entity \"tester\" '((\"speed\" \"float\") (\"target\" \"vector2\"))"
        "   '((\"spawn\" \"t-spawn\") (\"think\" \"t-think\"))))",
        NULL);
    EntityProperty properties[] = {{"position", "10 20"}, {"speed", "100"}};
    EntityHandle entity = EntitySpawnWith(&world, "tester", properties, 2);
    ScriptEntity *body = EntityData(&world, entity);
    bool placed = body && body->transform.translation.x == 10 && body->slots[0].as.number == 100;
    GameplayWorldStep(&world); // one tick of 0.1s at 100 a second
    Check(declared && placed && body && fabsf(body->transform.translation.x - 20) < 0.001f,
          "a scripted class takes its fields from a scene, and its callbacks are handed their entity");

    Check(ScriptS7DefineObject("tester-1", ScriptHostObjectOf(&host, entity)) &&
              !ScriptS7Eval("(tester-1 'rotate! \"sideways\")", NULL) &&
              !ScriptS7Eval("(tester-1 'move-world!)", NULL) && !ScriptS7Eval("(vec-length \"x\")", NULL),
          "arguments are checked against what the type or the table declared, instead of trusted");

    EntityHandle second = EntitySpawnWith(&world, "tester", properties, 2);
    ScriptEntity *secondBody = EntityData(&world, second);
    bool coordinated = ScriptS7Eval("(define a (find-first \"tester\"))", NULL) &&
                       ScriptS7Eval("(define b (find-next a \"tester\"))", NULL) &&
                       ScriptS7Eval("(set! (a 'speed) 55)", NULL) &&
                       ScriptS7Eval("(set! (b 'target) (vec 7 8))", NULL) && EvalIs("(a 'speed)", "55.0") &&
                       EvalIs("(b 'target)", "(7.0 8.0)") && EvalIs("(eq? (a 'classname) (b 'classname))", "#f") &&
                       EvalIs("(equal? (a 'classname) \"tester\")", "#t") && body->slots[0].as.number == 55 &&
                       secondBody && secondBody->slots[1].as.vector2.x == 7;
    Check(coordinated, "scripts find scripted entities and read or write their declared fields as properties");
    Check(!ScriptS7Eval("(a 'colour)", NULL) && !ScriptS7Eval("(set! (a 'classname) \"x\")", NULL) &&
              !ScriptS7Eval("(set! (a 'speed) \"fast\")", NULL) && EvalIs("(a 'speed)", "55.0"),
          "an unknown name, a read-only property or a value of the wrong type is an error, not a zero");
    Check(ScriptS7Eval("(class-new \"clash\")", NULL) && EvalIs("(class-field \"clash\" \"position\" \"float\")", "#f") &&
              EvalIs("(class-field \"clash\" \"hp\" \"float\")", "#t") &&
              EvalIs("(class-field \"clash\" \"hp\" \"int\")", "#f"),
          "a class cannot declare a field that hides what every entity has, or the same field twice");

    EntityDestroy(&world, second);
    Check(!ScriptS7Eval("(b 'speed)", NULL) && EvalIs("(alive? b)", "#f") && EvalIs("(alive? a)", "#t"),
          "an entity that has gone is refused by name instead of reaching whatever replaced it");

    // The small math and query rows: answers checked against raymath, and types still checked.
    const char *message = NULL;
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

    // Engine types, made by name: one way to create, read, write, call and end any of them.
    Check(EvalIs("(let ((c (make 'camera2d))) (set! (c 'zoom) 2) (c 'zoom))", "2.0") &&
              EvalIs("(let ((c (make 'camera2d))) (set! (c 'viewport) (vec 800 600))"
                     " (set! (c 'position) (vec 100 50)) (c 'world->screen (vec 100 50)))",
                     "(400.0 300.0)") &&
              EvalIs("(object-type (make 'camera2d))", "camera2d"),
          "an engine type is made by name and reached through its properties and methods");
    Check(!ScriptS7Eval("(make 'spaceship)", NULL) &&
              !ScriptS7Eval("(let ((c (make 'camera2d))) (c 'follow! 3 4))", NULL) &&
              !ScriptS7Eval("(let ((c (make 'camera2d))) (c 'fly!))", NULL),
          "an unknown type, a wrong argument or an unknown method is an error");
    Check(ScriptS7Eval("(define cam (make 'camera2d))", NULL) && EvalIs("(free! cam)", "#t") &&
              EvalIs("(alive? cam)", "#f") && !ScriptS7Eval("(cam 'zoom)", NULL) &&
              EvalIs("(free! cam)", "#f"),
          "an ended object is refused, and ending it twice answers #f");
    Check(EvalIs("(properties (make 'timer))", "(wait one-shot time-left running)") &&
              EvalIs("(if (memq 'destroy! (methods a)) #t #f)", "#t"),
          "a script can ask what an object has, its parents' members included");

    /* Networking is script-facing like everything else: a script must be able to run a server's
       fixed tick and a client's view of it without writing C. */
    Check(ScriptS7Eval("(define nc (make 'net-clock 60 20))", NULL) &&
              EvalIs("(nc 'advance! (/ 2.0 60) 8)", "2") && EvalIs("(nc 'ticked!)", "1") &&
              EvalIs("(nc 'tick)", "1") && !ScriptS7Eval("(set! (nc 'tick) 5)", NULL) &&
              !ScriptS7Eval("(make 'net-clock 60 0)", NULL),
          "a net clock runs every step a long frame owes, keeps its tick, and refuses a zero rate");
    Check(ScriptS7Eval("(define ni (make 'net-interpolator 60 20))", NULL) &&
              ScriptS7Eval("(ni 'snapshot! 60)", NULL) && ScriptS7Eval("(ni 'advance! 0.1)", NULL) &&
              EvalIs("(real? (ni 'render-tick))", "#t") && !ScriptS7Eval("(ni 'snapshot! -1)", NULL),
          "a net interpolator records snapshots and says which tick to draw, refusing a negative one");
    Check(!ScriptS7Eval("(make 'texture \"no-such-texture.png\")", NULL),
          "a texture that cannot be loaded is an error, not a zero handle");

    Check(ScriptS7Eval("(define w (make 'collision-world 64 32.0))", NULL) &&
              ScriptS7Eval("(define s (w 'add-circle! (vec 10 10) 5.0 1 1))", NULL) &&
              EvalIs("(> s 0)", "#t") && EvalIs("(w 'query-circle (vec 12 10) 3.0 1)", "1") &&
              EvalIs("(w 'query-circle (vec 100 100) 3.0 1)", "0") &&
              ScriptS7Eval("(define box (w 'add-aabb! (vec 50 50) (vec 4 4) 2 2))", NULL) &&
              EvalIs("(w 'query-aabb (vec 52 50) (vec 1 1) 2)", "1") &&
              EvalIs("(< (w 'sweep-circle (vec 30 10) 1.0 (vec -40 0) 1) 1.0)", "#t") &&
              EvalIs("(w 'remove! s)", "#t") && EvalIs("(w 'query-circle (vec 12 10) 3.0 1)", "0"),
          "a collision world adds, queries, sweeps and removes shapes");

    Check(ScriptS7Eval("(define wp (make 'waypoints))", NULL) && EvalIs("(wp 'count)", "0") &&
              ScriptS7Eval("(define p0 (wp 'add! (list 0 0 0)))", NULL) &&
              ScriptS7Eval("(define p1 (wp 'add! (list 0 0 3)))", NULL) && EvalIs("(wp 'count)", "2") &&
              EvalIs("(wp 'link! p0 p1)", "#t") && EvalIs("(wp 'next-hop p0 p1)", "1") &&
              EvalIs("(wp 'nearest (list 0 0 4))", "1") &&
              EvalIs("(wp 'position p1)", "(0.0 0.0 3.0)") &&
              !ScriptS7Eval("(wp 'position 99)", NULL) && EvalIs("(wp 'link! p0 99)", "#f"),
          "a waypoint graph is made by name, linked and queried through its methods");

    Check(ScriptS7Eval("(define pf (make 'pathfinder 8 8))", NULL) &&
              ScriptS7Eval("(for-each (lambda (y) (pf 'block! (vec 3 y))) '(0 1 2 3 4 5 6))", NULL) &&
              EvalIs("(pf 'blocked? (vec 3 2))", "#t") && EvalIs("(pf 'blocked? (vec 20 2))", "#t") &&
              EvalIs("(> (pf 'solve! (vec 0 0) (vec 6 0)) 0)", "#t") &&
              EvalIs("(pf 'route-hex 0)", "(0.0 0.0)") &&
              EvalIs("(pf 'route-hex (- (pf 'route-length) 1))", "(6.0 0.0)") &&
              EvalIs("(pf 'solve! (vec 0 0) (vec 3 3))", "0") && !ScriptS7Eval("(pf 'route-hex 0)", NULL),
          "a pathfinder routes around its own blocked hexes, and refuses a route onto one");

    Check(ScriptS7Eval("(define mv (make 'mover (vec 0 0)))", NULL) &&
              ScriptS7Eval("(define arrived-at #f)", NULL) &&
              ScriptS7Eval("(connect! mv 'arrived (lambda (hex) (set! arrived-at hex)))", NULL) &&
              EvalIs("(mv 'go-to! pf (vec 2 0))", "#t") && EvalIs("(mv 'moving?)", "#t") &&
              !ScriptS7Eval("(mv 'go-to! (make 'timer) (vec 1 1))", NULL),
          "a mover sets off through a pathfinder, and refuses anything else as one");
    for (int i = 0; i < 100; i++)
        ScriptHostStep(&host, 0.1f);
    Check(EvalIs("arrived-at", "(2.0 0.0)") && EvalIs("(mv 'hex)", "(2.0 0.0)") &&
              EvalIs("(mv 'moving?)", "#f"),
          "a mover walks as the host steps, and says so with arrived when it gets there");

    Check(ScriptS7Eval("(define fired 0)", NULL) && ScriptS7Eval("(define tm (make 'timer 0.5))", NULL) &&
              ScriptS7Eval("(define heard (connect! tm 'timeout (lambda () (set! fired (+ fired 1)))))", NULL) &&
              ScriptS7Eval("(connect! tm 'timeout (lambda () (error 'oops \"a broken handler\")))", NULL) &&
              ScriptS7Eval("(define once 0)", NULL) &&
              ScriptS7Eval("(connect! (make 'timer 0.1 #t) 'timeout (lambda () (set! once (+ once 1))))", NULL) &&
              !ScriptS7Eval("(connect! tm 'exploded (lambda () #f))", NULL),
          "handlers connect to signals a type declares, and not to others");
    for (int i = 0; i < 4; i++)
        ScriptHostStep(&host, 0.3f);
    Check(EvalIs("fired", "2") && EvalIs("once", "1") && EvalIs("(tm 'running)", "#t"),
          "a timer times out on time and again, a one-shot once, and a failing handler stops no one");
    Check(EvalIs("(disconnect! heard)", "#t") && EvalIs("(disconnect! heard)", "#f"),
          "a connection ends once");
    ScriptHostStep(&host, 0.5f);
    Check(EvalIs("fired", "2"), "a disconnected handler is not called again");

    Check(ScriptS7Eval("(define au (make 'audio))", NULL) && EvalIs("(au 'add-bus! \"voice\" 0.5)", "#t") &&
              EvalIs("(au 'add-bus! \"voice\" 0.5)", "#f") && EvalIs("(au 'set-bus-muted! \"voice\" #t)", "#t"),
          "an audio service adds and mutes its own volume groups");
    Check(ScriptS7Eval("(au 'set-listener! (list 0 0 0) (list 0 0 1) (list 0 1 0))", NULL) &&
              EvalIs("(au 'play-sound-at! \"no-such.wav\" \"sfx\" (list 0 0 900) 10.0 1.0)", "#f") &&
              !ScriptS7Eval("(make 'voice au \"no-such.wav\" \"sfx\")", NULL) &&
              !ScriptS7Eval("(make 'voice (make 'timer) \"x.wav\" \"sfx\")", NULL),
          "positional sound and voices are reachable from scripts, and refuse what cannot work");

    Check(EvalIs("(free! a)", "#t") && !EntityAlive(&world, entity),
          "freeing an entity takes it out of the world");

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

    // node3d: a script attaches one node to another, and its world position follows its parent.
    Check(ScriptS7Eval("(define hand (make 'node3d))", NULL) &&
              ScriptS7Eval("(define gun (make 'node3d))", NULL) &&
              ScriptS7Eval("(set! (gun 'parent) hand)", NULL) &&
              ScriptS7Eval("(set! (hand 'position) (list 1 2 3))", NULL) &&
              EvalIs("(gun 'world-position)", "(1.0 2.0 3.0)") &&
              !ScriptS7Eval("(set! (hand 'parent) gun)", NULL),
          "a node made and attached from Scheme follows its parent, and cannot become its own ancestor");

    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

// ---- scripting: sharing an engine object over the network ---------------------------------------
typedef struct NetObjTestState
{
    float value;
    Vector3 place;
} NetObjTestState;
static const EngineProperty netObjTestProperties[] = {
    ENGINE_FIELD("value", NetObjTestState, value, ENGINE_FLOAT, ENGINE_PROPERTY_SHARED,
                 "a shared float"),
    ENGINE_FIELD("place", NetObjTestState, place, ENGINE_VECTOR3, ENGINE_PROPERTY_SHARED,
                 "a shared vector3"),
};
static const EngineType netObjTestType = {
    .name = "net-obj-test",
    .size = sizeof(NetObjTestState),
    .properties = netObjTestProperties,
    .propertyCount = sizeof netObjTestProperties / sizeof netObjTestProperties[0],
    .help = "a small type with shared fields, for testing (make 'network ...) object sharing",
};

static bool SchemeTrue(const char *expression)
{
    char *answer = NULL;
    bool ok = ScriptS7Eval(expression, &answer);
    bool yes = ok && answer && !strcmp(answer, "#t");
    free(answer);
    return yes;
}

// Steps the host until expression reads #t or the budget runs out, giving loopback packets a
// little real time to arrive between polls: the network object's own step never blocks (it calls
// CoreNetSessionStep with waitMs 0, so ScriptHostStep alone drives it), so the wait belongs here.
static bool PumpScriptUntil(ScriptHost *host, const char *expression, int maxSteps)
{
    for (int i = 0; i < maxSteps; i++)
    {
        if (SchemeTrue(expression))
            return true;
        ScriptHostStep(host, 1.0f / 60.0f);
        usleep(1000);
    }
    return SchemeTrue(expression);
}

// A property marked shared that cannot go on the wire must fail the whole make, not leave that
// type quietly unable to be shared -- CoreNetFieldsFromType's own "answer nothing usable, not a
// partial list" rule, carried through to how the network type uses it.
typedef struct NetBadSharedBody
{
    const char *label;
} NetBadSharedBody;
static const EngineProperty netBadSharedProperties[] = {
    ENGINE_FIELD("label", NetBadSharedBody, label, ENGINE_STRING, ENGINE_PROPERTY_SHARED,
                 "bad: a shared string"),
};
static const EngineType netBadSharedType = {
    .name = "net-bad-shared-test",
    .size = sizeof(NetBadSharedBody),
    .properties = netBadSharedProperties,
    .propertyCount = 1,
};

static void NetworkBadSharedTypeChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptHostRegisterType(&host, &netBadSharedType),
          "a type with an unusable shared property registers with the host like any other");
    Check(ScriptS7Open(&host), "the Scheme frontend opens");
    Check(!ScriptS7Eval("(make 'network \"bad-shared-test\" 1)", NULL),
          "a registered type marking a property shared that cannot go on the wire fails the whole make");
    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

static void NetObjectsChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptHostRegisterType(&host, &netObjTestType),
          "a game's own shared type registers with the host before any network object is made");
    Check(ScriptS7Open(&host), "the Scheme frontend opens");

    Check(!ScriptS7Eval("(make 'network \"netobj-test\")", NULL),
          "network needs both a game name and a version");
    Check(ScriptS7Eval("(define srv (make 'network \"netobj-test\" 1))", NULL) &&
              ScriptS7Eval("(define cli (make 'network \"netobj-test\" 1))", NULL),
          "two network objects are made in the same script host");
    Check(EvalIs("(srv 'host! 99999)", "#f") && EvalIs("(cli 'join! \"127.0.0.1\" -1)", "#f") &&
              EvalIs("(equal? (srv 'status) \"off\")", "#t"),
          "a port outside 0..65535 is refused rather than truncated, and nothing was started");
    Check(EvalIs("(srv 'host! 0)", "#t") && EvalIs("(srv 'server)", "#t") &&
              EvalIs("(equal? (srv 'status) \"active\")", "#t"),
          "hosting on port 0 lets the system pick a port, and the server is active at once");

    char *portAnswer = NULL;
    ScriptS7Eval("(srv 'port)", &portAnswer);
    Check(portAnswer && strcmp(portAnswer, "0") != 0, "the bound port is read back from the server object");
    char joinExpr[128];
    snprintf(joinExpr, sizeof joinExpr, "(cli 'join! \"127.0.0.1\" %s)", portAnswer ? portAnswer : "0");
    free(portAnswer);
    Check(EvalIs(joinExpr, "#t"), "joining the host's own actual port succeeds");

    Check(PumpScriptUntil(&host, "(equal? (cli 'status) \"welcomed\")", 300),
          "pumping ScriptHostStep drives both sessions until the joiner is welcomed");
    Check(EvalIs("(cli 'ready!)", "#t"), "the joiner says it is ready");
    Check(EvalIs("(equal? (cli 'status) \"active\")", "#t"),
          "readying a welcomed joiner makes it active at once");

    Check(ScriptS7Eval("(define client-obj #f)", NULL) &&
              ScriptS7Eval("(connect! cli 'appeared (lambda (obj) (set! client-obj obj)))", NULL),
          "the joiner listens for appeared");

    Check(ScriptS7Eval("(define host-obj (make 'net-obj-test))", NULL) &&
              ScriptS7Eval("(set! (host-obj 'value) 4.5)", NULL) &&
              ScriptS7Eval("(set! (host-obj 'place) (list 1 2 3))", NULL),
          "a shared object is made and given values before it is shared");
    Check(EvalIs("(cli 'share! host-obj)", "#f"), "share! refuses on a machine that is not the server");
    Check(EvalIs("(srv 'share! host-obj)", "#t"), "the server shares it");
    Check(EvalIs("(srv 'mine? host-obj)", "#t"), "the server decides the object it just shared");

    Check(PumpScriptUntil(&host, "(and client-obj (equal? (client-obj 'value) 4.5))", 300),
          "the joiner gets an object of the shared type carrying the host's own values");
    Check(EvalIs("(client-obj 'place)", "(1.0 2.0 3.0)"), "...and its vector3 field too");
    Check(EvalIs("(object-type client-obj)", "net-obj-test"),
          "the joiner's object is of the same type the host shared");
    Check(EvalIs("(cli 'mine? client-obj)", "#f"), "the joiner does not decide an object the server owns");

    Check(ScriptS7Eval("(set! (host-obj 'value) 9.5)", NULL), "the host changes a shared value");
    Check(PumpScriptUntil(&host, "(equal? (client-obj 'value) 9.5)", 300),
          "the changed value reaches the joiner without a fresh share!");

    Check(ScriptS7Eval("(cli 'leave!)", NULL), "leave! runs without error");
    Check(EvalIs("(alive? client-obj)", "#f"),
          "leaving destroys the object the network object created here for the joiner");

    Check(ScriptS7Eval("(srv 'leave!)", NULL), "the server can leave! too");
    Check(EvalIs("(alive? host-obj)", "#t"),
          "leave! never destroys an object share! was given: it is the caller's, not the network object's");

    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

// A class that thinks less often than every step times its work by what really passed.
static void ElapsedChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    ScriptS7Open(&host);
    Check(ScriptS7Eval("(begin (define waited #f)"
                       " (define (slow-spawn self) (self 'think-after! 0.3))"
                       " (define (slow-think self) (set! waited (elapsed)))"
                       " (define-entity \"slow\" '() '((\"spawn\" \"slow-spawn\") (\"think\" \"slow-think\"))))",
                       NULL) &&
              !EngineObjectIdIsNull(ScriptHostObjectOf(&host, EntitySpawn(&world, "slow"))),
          "a class that thinks less than every step can be declared");
    for (int i = 0; i < 4; i++)
        GameplayWorldStep(&world);
    Check(EvalIs("(< (abs (- waited 0.3)) 0.001)", "#t"),
          "elapsed tells a slow thinker how long it has really been, not one step");
    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
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
    bool named = ScriptS7DefineObject("big-one", ScriptHostObjectOf(&host, big));
    bool sees = named && EvalIs("(alive? big-one)", "#t");
    EntityDestroy(&world, big);
    bool gone = EvalIs("(alive? big-one)", "#f");
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

// ---- save files and input maps: an empty string, and a load that must not half-apply -------------
typedef struct SaveThing
{
    char name[32];
    int amount;
} SaveThing;

static void SaveAndInputMapChecks(void)
{
    CoreSaveField fields[] = {
        {"name", CORE_SAVE_STRING, offsetof(SaveThing, name), sizeof(((SaveThing *)0)->name)},
        {"amount", CORE_SAVE_INT, offsetof(SaveThing, amount), sizeof(int)},
    };

    // Bug: a written empty string could not be read back at all.
    SaveThing emptyThing = {.name = "", .amount = 7};
    const char *emptyPath = Scratch("regression_save_empty.txt");
    SaveThing emptyLoaded;
    memset(&emptyLoaded, 0xaa, sizeof emptyLoaded);
    bool emptyRoundTrip = CoreSaveWrite(emptyPath, 1, &emptyThing, fields, 2) &&
                          CoreSaveRead(emptyPath, 1, &emptyLoaded, fields, 2, NULL, NULL);
    Check(emptyRoundTrip && emptyLoaded.name[0] == '\0' && emptyLoaded.amount == 7,
          "a saved empty string loads back empty, with the rest of the record intact");

    // A non-empty string must still round-trip the same way.
    SaveThing fullThing = {.name = "Fionn", .amount = 99};
    const char *fullPath = Scratch("regression_save_string.txt");
    SaveThing fullLoaded;
    memset(&fullLoaded, 0xaa, sizeof fullLoaded);
    bool fullRoundTrip = CoreSaveWrite(fullPath, 1, &fullThing, fields, 2) &&
                         CoreSaveRead(fullPath, 1, &fullLoaded, fields, 2, NULL, NULL);
    Check(fullRoundTrip && !strcmp(fullLoaded.name, "Fionn") && fullLoaded.amount == 99,
          "a non-empty saved string still round-trips");

    // Two actions, currently jump=Enter and fire=Space.
    InputBinding jumpDefault[] = {{INPUT_KEY, KEY_ENTER}};
    InputBinding fireDefault[] = {{INPUT_KEY, KEY_SPACE}};
    InputMapDefinition defs[] = {
        {"jump", jumpDefault, 1},
        {"fire", fireDefault, 1},
    };
    InputMap map = {0};
    bool mapInit = InputMapInit(&map, defs, 2);

    // A file with the same two actions but their keys swapped: jump=Space, fire=Enter.
    InputBinding jumpSwapped[] = {{INPUT_KEY, KEY_SPACE}};
    InputBinding fireSwapped[] = {{INPUT_KEY, KEY_ENTER}};
    InputMapDefinition swappedDefs[] = {
        {"jump", jumpSwapped, 1},
        {"fire", fireSwapped, 1},
    };
    InputMap swapped = {0};
    bool swappedInit = InputMapInit(&swapped, swappedDefs, 2);
    const char *swappedPath = Scratch("regression_input_swapped.txt");
    bool swappedWritten = swappedInit && InputMapWrite(&swapped, swappedPath);
    InputMapFree(&swapped);

    // Bug: applying line by line always conflicted on a plain swap.
    bool loadedSwap = mapInit && swappedWritten && InputMapRead(&map, swappedPath);
    InputBinding jumpAfterSwap = InputMapActionGet(&map, "jump").bindings[0];
    InputBinding fireAfterSwap = InputMapActionGet(&map, "fire").bindings[0];
    Check(loadedSwap && jumpAfterSwap.type == INPUT_KEY && jumpAfterSwap.code == KEY_SPACE &&
              fireAfterSwap.type == INPUT_KEY && fireAfterSwap.code == KEY_ENTER,
          "a file with two bindings swapped relative to the map loads in one pass");

    // A file whose own two lines collide with each other must fail, and change nothing.
    InputBinding jumpBefore = InputMapActionGet(&map, "jump").bindings[0];
    InputBinding fireBefore = InputMapActionGet(&map, "fire").bindings[0];
    const char *conflictPath = Scratch("regression_input_conflict.txt");
    FILE *conflictFile = fopen(conflictPath, "w");
    bool conflictWritten = conflictFile != NULL;
    if (conflictFile)
    {
        fprintf(conflictFile, "input-map 1\njump 0 0 %d\nfire 0 0 %d\n", KEY_A, KEY_A);
        fclose(conflictFile);
    }
    bool conflictFailed = conflictWritten && !InputMapRead(&map, conflictPath);
    InputBinding jumpAfterConflict = InputMapActionGet(&map, "jump").bindings[0];
    InputBinding fireAfterConflict = InputMapActionGet(&map, "fire").bindings[0];
    Check(conflictFailed && jumpAfterConflict.type == jumpBefore.type &&
              jumpAfterConflict.code == jumpBefore.code && fireAfterConflict.type == fireBefore.type &&
              fireAfterConflict.code == fireBefore.code,
          "a file whose own bindings collide fails to load, and leaves the map untouched");

    // A file naming an action the map does not have must fail the same way.
    const char *unknownPath = Scratch("regression_input_unknown.txt");
    FILE *unknownFile = fopen(unknownPath, "w");
    bool unknownWritten = unknownFile != NULL;
    if (unknownFile)
    {
        fprintf(unknownFile, "input-map 1\ngrapple 0 0 %d\n", KEY_A);
        fclose(unknownFile);
    }
    bool unknownFailed = unknownWritten && !InputMapRead(&map, unknownPath);
    InputBinding jumpAfterUnknown = InputMapActionGet(&map, "jump").bindings[0];
    InputBinding fireAfterUnknown = InputMapActionGet(&map, "fire").bindings[0];
    Check(unknownFailed && jumpAfterUnknown.type == jumpBefore.type &&
              jumpAfterUnknown.code == jumpBefore.code && fireAfterUnknown.type == fireBefore.type &&
              fireAfterUnknown.code == fireBefore.code,
          "a file naming an unknown action fails to load, and leaves the map untouched");

    InputMapFree(&map);
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

static void NetDeltaChecks(void)
{
    /* A world that is mostly still should cost almost nothing to send. id Tech 3 deltas each
       snapshot against the newest one the receiver has acknowledged, and falls back to a full
       snapshot when that acknowledgement is missing or too old. */
    const CoreNetField fields[] = {
        CORE_NET_FIELD_LERP(NetTestState, position, CORE_NET_VECTOR3),
        CORE_NET_FIELD_LERP(NetTestState, yaw, CORE_NET_F32),
        CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
        CORE_NET_FIELD(NetTestState, carrying, CORE_NET_BOOL),
    };
    const CoreNetSchema schema = {9, "delta", sizeof(NetTestState), fields,
                                  sizeof fields / sizeof fields[0], CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync server = {0}, client = {0};
    Check(CoreNetSyncInit(&server, 4, 2) && CoreNetSyncRegister(&server, &schema) &&
          CoreNetSyncInit(&client, 4, 2) && CoreNetSyncRegister(&client, &schema),
          "delta registries start");

    CoreNetObject *object = CoreNetSyncSpawn(&server, 1, 9, 0);
    Check(object != NULL, "delta object spawns");
    NetTestState *live = (NetTestState *)object->state;
    live->position = (Vector3){1.0f, 2.0f, 3.0f};
    live->health = 100;

    unsigned char full[512];
    CoreNetWriter writer = CoreNetWriterBegin(full, sizeof full);
    Check(CoreNetSyncWrite(&server, 10, &writer), "a full snapshot writes");
    size_t fullSize = writer.size;
    Check(CoreNetSyncRemember(&server, 10), "the sender remembers what it sent");

    uint32_t readTick = 0;
    CoreNetReader reader = CoreNetReaderBegin(full, fullSize);
    Check(CoreNetSyncRead(&client, &reader, &readTick) && readTick == 10,
          "the receiver applies the full snapshot");
    Check(CoreNetSyncRemember(&client, 10), "the receiver remembers it too");
    CoreNetObject *mirror = CoreNetSyncFind(&client, 1);
    Check(mirror && ((NetTestState *)mirror->state)->health == 100, "the full snapshot arrived");

    /* Nothing has changed: the delta should be markedly smaller than the full snapshot. */
    unsigned char quiet[512];
    writer = CoreNetWriterBegin(quiet, sizeof quiet);
    Check(CoreNetSyncWriteDelta(&server, 11, 10, &writer), "an unchanged delta writes");
    Check(writer.size < fullSize, "an unchanged world costs less than a full snapshot");

    reader = CoreNetReaderBegin(quiet, writer.size);
    Check(CoreNetSyncRead(&client, &reader, &readTick) && readTick == 11, "the delta applies");
    mirror = CoreNetSyncFind(&client, 1);
    Check(mirror && ((NetTestState *)mirror->state)->health == 100 &&
              ((NetTestState *)mirror->state)->position.x == 1.0f,
          "fields nobody sent keep the value they had");
    Check(CoreNetSyncRemember(&client, 11), "the receiver remembers the delta result");
    Check(CoreNetSyncRemember(&server, 11), "so does the sender");

    /* One field moves: it arrives, and the others are still not on the wire. */
    live->health = 55;
    unsigned char moved[512];
    writer = CoreNetWriterBegin(moved, sizeof moved);
    Check(CoreNetSyncWriteDelta(&server, 12, 11, &writer), "a changed delta writes");
    reader = CoreNetReaderBegin(moved, writer.size);
    Check(CoreNetSyncRead(&client, &reader, &readTick), "the changed delta applies");
    mirror = CoreNetSyncFind(&client, 1);
    Check(mirror && ((NetTestState *)mirror->state)->health == 55, "the changed field arrived");
    Check(mirror && ((NetTestState *)mirror->state)->position.x == 1.0f,
          "an unchanged field beside it was not disturbed");

    /* A baseline the receiver never had cannot be reconstructed, and must not be half applied. */
    CoreNetSync fresh = {0};
    Check(CoreNetSyncInit(&fresh, 4, 2) && CoreNetSyncRegister(&fresh, &schema),
          "a late joiner starts");
    reader = CoreNetReaderBegin(moved, writer.size);
    Check(CoreNetSyncRead(&fresh, &reader, &readTick),
          "a delta against an unknown baseline is understood");
    Check(CoreNetSyncFind(&fresh, 1) == NULL,
          "a delta against an unknown baseline changes nothing");

    /* The sender is in the same position when the receiver's acknowledgement is too old: it must
       fall back to a full snapshot rather than emit something unreadable. */
    writer = CoreNetWriterBegin(moved, sizeof moved);
    Check(CoreNetSyncWriteDelta(&server, 13, 999999, &writer),
          "a baseline the sender has forgotten still writes");
    reader = CoreNetReaderBegin(moved, writer.size);
    Check(CoreNetSyncRead(&fresh, &reader, &readTick) && CoreNetSyncFind(&fresh, 1) != NULL,
          "it fell back to a full snapshot the late joiner could read");

    /* A schema that promises to mark its own changes buys the right to be skipped entirely: an
       object nobody touched costs nothing, not even the comparison. Forgetting the mark is then a
       field that stops replicating, which is why it has to be asked for rather than assumed. */
    const CoreNetSchema explicitSchema = {12, "explicit", sizeof(NetTestState), fields,
                                          sizeof fields / sizeof fields[0],
                                          CORE_NET_AUTHORITY_SERVER,
                                          CORE_NET_SCHEMA_EXPLICIT_DIRTY};
    CoreNetSync marked = {0};
    Check(CoreNetSyncInit(&marked, 4, 2) && CoreNetSyncRegister(&marked, &explicitSchema),
          "an explicitly-dirtied registry starts");
    CoreNetObject *watched = CoreNetSyncSpawn(&marked, 1, 12, 0);
    Check(watched != NULL, "its object spawns");
    unsigned char first[512];
    writer = CoreNetWriterBegin(first, sizeof first);
    Check(CoreNetSyncWrite(&marked, 20, &writer), "its first snapshot writes in full");
    Check(CoreNetSyncRemember(&marked, 20), "and is remembered");
    size_t firstSize = writer.size;

    writer = CoreNetWriterBegin(first, sizeof first);
    Check(CoreNetSyncWriteDelta(&marked, 21, 20, &writer), "an unmarked delta writes");
    size_t quietSize = writer.size;
    Check(quietSize < firstSize, "an object nobody marked is left out of it entirely");

    ((NetTestState *)watched->state)->health = 9;
    Check(CoreNetSyncDirty(&marked, 1), "marking it changed succeeds");
    writer = CoreNetWriterBegin(first, sizeof first);
    Check(CoreNetSyncWriteDelta(&marked, 22, 20, &writer), "a marked delta writes");
    Check(writer.size > quietSize, "and carries the object again");
    Check(!CoreNetSyncDirty(&marked, 999), "marking an object that is not there fails");
    CoreNetSyncFree(&marked);

    /* STREAM and ONCE are the documented macros for a steady interpolation feed and a
       decided-once value; both must register, and a field cannot claim both jobs at once. */
    typedef struct NetFlagState
    {
        float tracked;
        int32_t born;
    } NetFlagState;
    const CoreNetField streamFields[] = {
        CORE_NET_FIELD_STREAM(NetFlagState, tracked, CORE_NET_F32),
    };
    const CoreNetSchema streamSchema = {20, "stream", sizeof(NetFlagState), streamFields, 1,
                                        CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync streamSync = {0};
    Check(CoreNetSyncInit(&streamSync, 4, 2) && CoreNetSyncRegister(&streamSync, &streamSchema),
          "a schema using CORE_NET_FIELD_STREAM registers");
    CoreNetSyncFree(&streamSync);

    const CoreNetField onceFields[] = {
        CORE_NET_FIELD_ONCE(NetFlagState, born, CORE_NET_I32),
    };
    const CoreNetSchema onceSchema = {21, "once", sizeof(NetFlagState), onceFields, 1,
                                      CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync onceSync = {0};
    Check(CoreNetSyncInit(&onceSync, 4, 2) && CoreNetSyncRegister(&onceSync, &onceSchema),
          "a schema using CORE_NET_FIELD_ONCE registers");
    CoreNetSyncFree(&onceSync);

    CoreNetField badField = CORE_NET_FIELD(NetFlagState, born, CORE_NET_I32);
    badField.flags = CORE_NET_FIELD_SYNC | CORE_NET_FIELD_SPAWN;
    const CoreNetField badFields[] = {badField};
    const CoreNetSchema badSchema = {22, "bad", sizeof(NetFlagState), badFields, 1,
                                     CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync badSync = {0};
    Check(CoreNetSyncInit(&badSync, 4, 2) && !CoreNetSyncRegister(&badSync, &badSchema),
          "a field cannot be marked both SYNC and SPAWN at once");
    CoreNetSyncFree(&badSync);

    /* Behaviour: an unchanged SYNC field still rides every delta; an unchanged SPAWN field rides
       only the first snapshot that ever mentions the object. */
    const CoreNetField mixedFields[] = {
        CORE_NET_FIELD_STREAM(NetFlagState, tracked, CORE_NET_F32),
        CORE_NET_FIELD_ONCE(NetFlagState, born, CORE_NET_I32),
    };
    const CoreNetSchema mixedSchema = {23, "mixed", sizeof(NetFlagState), mixedFields, 2,
                                       CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync mixedServer = {0}, mixedClient = {0};
    Check(CoreNetSyncInit(&mixedServer, 4, 2) && CoreNetSyncRegister(&mixedServer, &mixedSchema) &&
              CoreNetSyncInit(&mixedClient, 4, 2) && CoreNetSyncRegister(&mixedClient, &mixedSchema),
          "STREAM/ONCE registries start");
    CoreNetObject *mixedObject = CoreNetSyncSpawn(&mixedServer, 1, 23, 0);
    Check(mixedObject != NULL, "the mixed-flags object spawns");
    ((NetFlagState *)mixedObject->state)->tracked = 1.0f;
    ((NetFlagState *)mixedObject->state)->born = 5;

    unsigned char mixedFull[512];
    CoreNetWriter mixedWriter = CoreNetWriterBegin(mixedFull, sizeof mixedFull);
    Check(CoreNetSyncWrite(&mixedServer, 30, &mixedWriter), "the first mixed-flags snapshot writes");
    Check(CoreNetSyncRemember(&mixedServer, 30), "the sender remembers it");
    CoreNetReader mixedReader = CoreNetReaderBegin(mixedFull, mixedWriter.size);
    uint32_t mixedTick = 0;
    Check(CoreNetSyncRead(&mixedClient, &mixedReader, &mixedTick), "the client applies it");

    unsigned char mixedDelta[512];
    mixedWriter = CoreNetWriterBegin(mixedDelta, sizeof mixedDelta);
    Check(CoreNetSyncWriteDelta(&mixedServer, 31, 30, &mixedWriter),
          "an unchanged mixed-flags delta writes");
    CoreNetReader raw = CoreNetReaderBegin(mixedDelta, mixedWriter.size);
    uint32_t rawMagic, rawTick, rawBaseline, rawId;
    uint16_t rawCount, rawRemoved, rawType, rawOwner;
    uint8_t mask = 0;
    Check(CoreNetReadU32(&raw, &rawMagic) && CoreNetReadU32(&raw, &rawTick) &&
              CoreNetReadU32(&raw, &rawBaseline) && CoreNetReadU16(&raw, &rawCount) &&
              CoreNetReadU16(&raw, &rawRemoved) && rawCount == 1 && rawRemoved == 0 &&
              CoreNetReadU32(&raw, &rawId) && CoreNetReadU16(&raw, &rawType) &&
              CoreNetReadU16(&raw, &rawOwner) && CoreNetReadU8(&raw, &mask),
          "the unchanged delta's header and mask byte are read back");
    Check((mask & 1u) != 0, "an unchanged SYNC field still rides every delta");
    Check((mask & 2u) == 0, "an unchanged SPAWN field is left out after the first snapshot");
    CoreNetSyncFree(&mixedServer);
    CoreNetSyncFree(&mixedClient);

    /* A state larger than the serializer's scratch buffer must be refused at registration, not
       silently dropped every frame at runtime. */
    const CoreNetField sizeField[] = {
        {.name = "byte", .type = CORE_NET_U8, .offset = 0, .size = 1, .flags = 0},
    };
    const CoreNetSchema tooBigSchema = {24, "toobig", CORE_NET_STATE_MAX + 1, sizeField, 1,
                                        CORE_NET_AUTHORITY_SERVER, 0};
    const CoreNetSchema atMaxSchema = {25, "atmax", CORE_NET_STATE_MAX, sizeField, 1,
                                       CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync sizeSync = {0};
    Check(CoreNetSyncInit(&sizeSync, 4, 2), "a size-limit registry starts");
    Check(!CoreNetSyncRegister(&sizeSync, &tooBigSchema),
          "a schema larger than CORE_NET_STATE_MAX is refused at registration");
    Check(CoreNetSyncRegister(&sizeSync, &atMaxSchema),
          "a schema exactly at CORE_NET_STATE_MAX registers");
    CoreNetSyncFree(&sizeSync);

    CoreNetSyncFree(&server);
    CoreNetSyncFree(&client);
    CoreNetSyncFree(&fresh);
}

static void NetCommandChecks(void)
{
    /* A client cannot write the world, so a pickup or a drop is a request it sends and the server
       executes. Sent reliably and numbered, because doing a pickup twice is not doing it once. */
    unsigned char packet[64];
    CoreNetWriter w = CoreNetWriterBegin(packet, sizeof packet);
    CoreNetCommand out = {7, 4002, 2};
    Check(CoreNetCommandWrite(&w, out), "a command writes");
    Check(CoreNetWriteF32(&w, 1.5f), "the game's own payload follows it");

    CoreNetReader r = CoreNetReaderBegin(packet, w.size);
    CoreNetCommand in;
    float payload = 0.0f;
    Check(CoreNetCommandRead(&r, &in) && in.sequence == 7 && in.object == 4002 && in.op == 2,
          "the command reads back as it was written");
    Check(CoreNetReadF32(&r, &payload) && payload == 1.5f,
          "the reader is left on the payload, not past it");
    Check(r.at == r.size, "between them they consumed the packet exactly");

    Check(!CoreNetCommandRead(&r, NULL), "reading into nothing is refused");
    CoreNetReader truncated = CoreNetReaderBegin(packet, 3);
    Check(!CoreNetCommandRead(&truncated, &in), "a truncated command is refused");

    /* Executing the same request twice is the bug this guards: the thing gets picked up twice. */
    uint32_t last = 0;
    Check(CoreNetCommandAccept(&last, 1), "the first command is new");
    Check(!CoreNetCommandAccept(&last, 1), "the same command again is not");
    Check(CoreNetCommandAccept(&last, 2), "a later command is new");
    Check(!CoreNetCommandAccept(&last, 2), "and only once");
    Check(!CoreNetCommandAccept(&last, 1), "one that arrived late is still not new");
    last = 0;   /* what the server does when a client connects */
    Check(CoreNetCommandAccept(&last, 1),
          "a reconnected client is admitted by resetting the counter, not by guessing");
    Check(!CoreNetCommandAccept(NULL, 1), "accepting against nothing is refused");
}

static void NetPackedObjectChecks(void)
{
    /* A packet may carry several objects. Reading one must consume only its own bytes and leave the
       rest, or a sender that packs more than one has every object but the last rejected. */
    const CoreNetField fields[] = {
        CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
    };
    const CoreNetSchema schema = {11, "packed", sizeof(NetTestState), fields, 1,
                                  CORE_NET_AUTHORITY_OWNER, 0};
    CoreNetSync sync = {0};
    Check(CoreNetSyncInit(&sync, 4, 2) && CoreNetSyncRegister(&sync, &schema),
          "packed registry starts");
    CoreNetObject *a = CoreNetSyncSpawn(&sync, 1, 11, 5);
    CoreNetObject *b = CoreNetSyncSpawn(&sync, 2, 11, 5);
    Check(a && b, "two objects for one owner spawn");
    ((NetTestState *)a->state)->health = 11;
    ((NetTestState *)b->state)->health = 22;

    unsigned char packet[256];
    CoreNetWriter writer = CoreNetWriterBegin(packet, sizeof packet);
    Check(CoreNetObjectWrite(a, &writer) && CoreNetObjectWrite(b, &writer),
          "two objects pack into one packet");

    ((NetTestState *)a->state)->health = 0;
    ((NetTestState *)b->state)->health = 0;
    CoreNetReader reader = CoreNetReaderBegin(packet, writer.size);
    Check(CoreNetSyncReadObject(&sync, &reader, 5, false),
          "the first of two packed objects reads");
    Check(reader.at < reader.size, "it left the second one in the buffer");
    Check(CoreNetSyncReadObject(&sync, &reader, 5, false),
          "the second packed object reads too");
    Check(reader.at == reader.size, "between them they consumed the packet exactly");
    Check(((NetTestState *)a->state)->health == 11 && ((NetTestState *)b->state)->health == 22,
          "both objects arrived with their own values");
    CoreNetSyncFree(&sync);
}

static void NetClockChecks(void)
{
    /* A server simulates at a fixed rate and sends snapshots at its own, lower one -- Source runs
       66 ticks and about 20 snapshots a second and never sends more snapshots than ticks. */
    CoreNetClock clock;
    Check(!CoreNetClockInit(&clock, 0, 20), "clock rejects a zero tick rate");
    Check(!CoreNetClockInit(&clock, 60, 0), "clock rejects a zero send rate");
    Check(CoreNetClockInit(&clock, 60, 20), "clock starts");
    Check(clock.sendInterval > clock.tickInterval, "snapshots are rarer than ticks");

    CoreNetClock capped;
    Check(CoreNetClockInit(&capped, 30, 60), "clock accepts a send rate above the tick rate");
    Check(capped.sendInterval >= capped.tickInterval,
          "a send rate above the tick rate is clamped to it");

    /* Time is banked, not rounded: a frame worth three steps runs three, a frame worth none runs
       none, and the simulation keeps its rate whatever the frame rate does. */
    Check(CoreNetClockAdvance(&clock, 1.0 / 240.0, 8) == 0, "a short frame runs no tick");
    int ran = CoreNetClockAdvance(&clock, 3.0 / 60.0, 8);
    Check(ran == 3, "a long frame runs every step it owes");
    Check(CoreNetClockTicked(&clock) == 1, "ticks are counted");

    /* A stall must not demand an unbounded catch up on the next frame. */
    CoreNetClock stalled;
    CoreNetClockInit(&stalled, 60, 20);
    Check(CoreNetClockAdvance(&stalled, 10.0, 5) == 5, "a stall is capped at the ceiling");
    Check(CoreNetClockAdvance(&stalled, 1.0 / 60.0, 5) <= 1,
          "the unrun backlog is discarded, not banked into a spiral");

    CoreNetClock sender;
    CoreNetClockInit(&sender, 60, 20);
    Check(!CoreNetClockShouldSend(&sender), "nothing is due before any time passes");
    CoreNetClockAdvance(&sender, 1.0 / 60.0, 8);
    Check(!CoreNetClockShouldSend(&sender), "one tick is not yet a snapshot");
    CoreNetClockAdvance(&sender, 1.0 / 20.0, 8);
    Check(CoreNetClockShouldSend(&sender), "a snapshot falls due at the send rate");
    Check(!CoreNetClockShouldSend(&sender), "the due snapshot is consumed, not repeated");

    /* The client draws in the past, far enough back that both snapshots bracketing the moment it
       is drawing have arrived. */
    CoreNetInterpolator interp;
    Check(!CoreNetInterpolatorInit(&interp, 60, 0), "interpolator rejects a zero send rate");
    Check(CoreNetInterpolatorInit(&interp, 60, 20), "interpolator starts");
    Check(CoreNetInterpolatorRenderTick(&interp) == 0.0, "nothing is drawn before a snapshot");
    CoreNetInterpolatorSnapshot(&interp, 600);
    double render = CoreNetInterpolatorRenderTick(&interp);
    Check(render < 600.0, "the client draws behind the newest snapshot");
    Check(fabs((600.0 - render) - 6.0) < 0.001,
          "the delay is two snapshot intervals of ticks");
    CoreNetInterpolatorSnapshot(&interp, 500);
    Check(CoreNetInterpolatorRenderTick(&interp) == render,
          "a snapshot that overtook an older one cannot drag the clock backwards");
    CoreNetInterpolatorAdvance(&interp, 1.0 / 60.0);
    Check(CoreNetInterpolatorRenderTick(&interp) > render, "the drawn moment runs forward");

    /* Drift is closed by retiming playback, not by moving the drawn moment: a correction applied to
       the position makes the whole world jump by exactly that much. So a step must never advance by
       more than a little over one tick, however far behind it is. */
    CoreNetInterpolator drifting;
    CoreNetInterpolatorInit(&drifting, 60, 20);
    CoreNetInterpolatorSnapshot(&drifting, 1000);
    CoreNetInterpolatorSnapshot(&drifting, 1012);      /* suddenly twelve ticks further on */
    double before = CoreNetInterpolatorRenderTick(&drifting);
    CoreNetInterpolatorAdvance(&drifting, 1.0 / 60.0);
    double step = CoreNetInterpolatorRenderTick(&drifting) - before;
    Check(step > 0.0 && step < 1.0 + CORE_NET_RETIME_LIMIT + 0.001,
          "catching up never advances the world by more than a fraction over one tick");

    CoreNetInterpolator jumped;
    CoreNetInterpolatorInit(&jumped, 60, 20);
    CoreNetInterpolatorSnapshot(&jumped, 1000);
    CoreNetInterpolatorSnapshot(&jumped, 9000);        /* a join, a stall, or a restart */
    CoreNetInterpolatorAdvance(&jumped, 1.0 / 60.0);
    Check(CoreNetInterpolatorRenderTick(&jumped) > 8000.0,
          "a gap too large to retime across is snapped instead");
}

static void NetSyncChecks(void)
{
    const CoreNetField fields[] = {
        CORE_NET_FIELD_LERP(NetTestState, position, CORE_NET_VECTOR3),
        CORE_NET_FIELD_LERP(NetTestState, yaw, CORE_NET_F32),
        CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
        CORE_NET_FIELD(NetTestState, carrying, CORE_NET_BOOL),
    };
    const CoreNetSchema schema = {7, "player", sizeof(NetTestState), fields,
                                  sizeof fields / sizeof fields[0], CORE_NET_AUTHORITY_OWNER, 0};
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
    Check(copy->currentTick == 11 && copy->previousTick == 11,
          "a freshly seen object is stamped with the snapshot's own tick, not the registry's previous one");

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
    Check(copy->previousTick == 11 && copy->currentTick == 12,
          "a later snapshot moves previousTick to the last one and currentTick to its own, not both to the old one");
    NetTestState atLatest = {0};
    Check(CoreNetObjectSampleAt(copy, 12.0, &atLatest) && atLatest.position.x == 6.0f &&
              atLatest.position.y == 8.0f && atLatest.position.z == 10.0f && atLatest.yaw == 1.5f &&
              atLatest.health == 35 && !atLatest.carrying,
          "sampling exactly at the newest tick returns its state, not a blend with the old one");

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

// ---- CoreNetFieldsFromType: a schema's fields read back from an engine type's shared properties --
typedef struct NetFieldTestBody
{
    bool flag;
    int count;
    float amount;
    Vector3 place;
} NetFieldTestBody;
static const EngineProperty netFieldTestProperties[] = {
    ENGINE_FIELD("shared-int", NetFieldTestBody, count, ENGINE_INT, ENGINE_PROPERTY_SHARED,
                 "a shared int"),
    ENGINE_FIELD("shared-float", NetFieldTestBody, amount, ENGINE_FLOAT, ENGINE_PROPERTY_SHARED,
                 "a shared float"),
    ENGINE_FIELD("shared-vector3", NetFieldTestBody, place, ENGINE_VECTOR3, ENGINE_PROPERTY_SHARED,
                 "a shared vector3"),
    ENGINE_FIELD("plain-bool", NetFieldTestBody, flag, ENGINE_BOOL, 0, "not shared"),
};
static const EngineType netFieldTestType = {
    .name = "net-field-test",
    .size = sizeof(NetFieldTestBody),
    .properties = netFieldTestProperties,
    .propertyCount = sizeof netFieldTestProperties / sizeof netFieldTestProperties[0],
};

typedef struct NetFieldStringBody
{
    const char *label;
    int count;
} NetFieldStringBody;
static const EngineProperty netFieldStringProperties[] = {
    ENGINE_FIELD("shared-string", NetFieldStringBody, label, ENGINE_STRING, ENGINE_PROPERTY_SHARED,
                 "bad: a shared string, which has no fixed size to put on the wire"),
    ENGINE_FIELD("shared-int", NetFieldStringBody, count, ENGINE_INT, ENGINE_PROPERTY_SHARED,
                 "otherwise a perfectly fine shared int"),
};
static const EngineType netFieldStringType = {
    .name = "net-field-string-test",
    .size = sizeof(NetFieldStringBody),
    .properties = netFieldStringProperties,
    .propertyCount = sizeof netFieldStringProperties / sizeof netFieldStringProperties[0],
};

static bool NetFieldComputedGet(const void *object, EngineValue *out)
{
    (void)object;
    *out = EngineInt(1);
    return true;
}
static const EngineProperty netFieldComputedProperties[] = {
    ENGINE_COMPUTED("shared-computed", ENGINE_INT, ENGINE_PROPERTY_SHARED, NetFieldComputedGet, NULL,
                    "bad: computed, so it has no offset into storage to copy"),
};
static const EngineType netFieldComputedType = {
    .name = "net-field-computed-test",
    .size = sizeof(int),
    .properties = netFieldComputedProperties,
    .propertyCount = 1,
};

static void NetFieldsFromTypeChecks(void)
{
    CoreNetField fields[8] = {0};
    size_t count = CoreNetFieldsFromType(&netFieldTestType, fields, 8);
    Check(count == 3, "one field per shared property, skipping the one that is not shared");
    Check(!strcmp(fields[0].name, "shared-int") && fields[0].type == CORE_NET_I32 &&
              fields[0].offset == offsetof(NetFieldTestBody, count) &&
              fields[0].size == sizeof(int32_t) && fields[0].flags == 0,
          "a shared int becomes a plain CORE_NET_I32 field at the property's own offset and size");
    Check(!strcmp(fields[1].name, "shared-float") && fields[1].type == CORE_NET_F32 &&
              fields[1].offset == offsetof(NetFieldTestBody, amount) &&
              fields[1].size == sizeof(float) && fields[1].flags == CORE_NET_FIELD_INTERPOLATED,
          "a shared float becomes an interpolated CORE_NET_F32 field");
    Check(!strcmp(fields[2].name, "shared-vector3") && fields[2].type == CORE_NET_VECTOR3 &&
              fields[2].offset == offsetof(NetFieldTestBody, place) &&
              fields[2].size == sizeof(Vector3) && fields[2].flags == CORE_NET_FIELD_INTERPOLATED,
          "a shared vector3 becomes an interpolated CORE_NET_VECTOR3 field");

    Check(CoreNetFieldsFromType(&netFieldTestType, fields, 2) == 0,
          "too little capacity for every shared field answers nothing usable, not a partial list");
    Check(CoreNetFieldsFromType(&netFieldStringType, fields, 8) == 0,
          "a shared property of a value type with no wire form (a string) answers nothing usable, "
          "even though the type has another shared property that would have been fine alone");
    Check(CoreNetFieldsFromType(&netFieldComputedType, fields, 8) == 0,
          "a shared computed property, which has no offset to copy, answers nothing usable");
    Check(CoreNetFieldsFromType(NULL, fields, 8) == 0, "no type answers nothing usable");

    // The sizes and offsets CoreNetFieldsFromType hands out are exactly what CoreNetSyncRegister
    // itself checks a field against, so a schema built from them registers cleanly.
    count = CoreNetFieldsFromType(&netFieldTestType, fields, 8);
    const CoreNetSchema schema = {50, "net-field-test", sizeof(NetFieldTestBody), fields, count,
                                  CORE_NET_AUTHORITY_SERVER, 0};
    CoreNetSync sync = {0};
    Check(CoreNetSyncInit(&sync, 4, 2) && CoreNetSyncRegister(&sync, &schema),
          "a schema built from a type's shared properties registers with CoreNetSyncRegister");
    CoreNetSyncFree(&sync);
}

static void NetOwnerRecordCall(void *user, void *state, bool writing)
{
    bool *record = (bool *)user;   /* record[0] = called, record[1] = writing */
    record[0] = true;
    record[1] = writing;
    (void)state;
}

// ---- the type registry and object pool --------------------------------------------------------
typedef struct TestBody
{
    Vector2 position;
    float speed;
    int hits;
    bool ready;
} TestBody;

static int testCreated, testDestroyed, testStepped;
static bool TestBodyCreate(EngineCall *call)
{
    TestBody *body = call->data;
    body->speed = call->count > 0 ? call->arguments[0].as.number : 1.0f;
    body->ready = true;
    testCreated++;
    return true;
}
static void TestBodyDestroy(void *data) { (void)data; testDestroyed++; }
static void TestBodyStep(EngineObjects *objects, EngineObjectId self, void *data, float dt)
{
    TestBody *body = data;
    body->position.x += body->speed * dt;
    testStepped++;
    if (body->position.x >= 10.0f)
        EngineObjectEmit(objects, self, "arrived", (EngineValue[]){EngineFloat(body->position.x)}, 1);
}
static bool TestBodyHit(EngineCall *call)
{
    TestBody *body = call->data;
    body->hits += call->arguments[0].as.integer;
    call->result = EngineInt(body->hits);
    return true;
}
static bool TestBodyDistance(EngineCall *call)
{
    TestBody *self = call->data;
    TestBody *other = EngineCallObject(call, 0, NULL);
    if (!other)
    {
        call->error = "needs another body";
        return false;
    }
    call->result = EngineFloat(Vector2Distance(self->position, other->position));
    return true;
}
static bool TestDoubleSpeed(const void *object, EngineValue *out)
{
    *out = EngineFloat(((const TestBody *)object)->speed * 2.0f);
    return true;
}
static const EngineProperty testThingProperties[] = {
    ENGINE_FIELD("ready", TestBody, ready, ENGINE_BOOL, ENGINE_PROPERTY_READ_ONLY, "set up"),
};
static const EngineType testThing = {.name = "thing", .size = sizeof(TestBody),
                                     .properties = testThingProperties, .propertyCount = 1};
static const EngineProperty testBodyProperties[] = {
    ENGINE_FIELD("position", TestBody, position, ENGINE_VECTOR2, 0, "where"),
    ENGINE_FIELD("speed", TestBody, speed, ENGINE_FLOAT, ENGINE_PROPERTY_SAVE, "how fast"),
    ENGINE_COMPUTED("double-speed", ENGINE_FLOAT, ENGINE_PROPERTY_READ_ONLY, TestDoubleSpeed, NULL,
                    "twice as fast"),
};
static const EngineMethod testBodyMethods[] = {
    {"hit!", ENGINE_INT, {ENGINE_INT}, 1, TestBodyHit, "take hits, answer the total"},
    {"distance-to", ENGINE_FLOAT, {ENGINE_OBJECT}, 1, TestBodyDistance, "how far to another"},
};
static const char *const testBodySignals[] = {"arrived"};
static const EngineType testBody = {
    .name = "body", .parent = &testThing, .size = sizeof(TestBody),
    .properties = testBodyProperties, .propertyCount = 3,
    .methods = testBodyMethods, .methodCount = 2,
    .signals = testBodySignals, .signalCount = 1,
    .createArguments = {ENGINE_FLOAT}, .createArgumentCount = 1, .createRequired = 0,
    .create = TestBodyCreate, .destroy = TestBodyDestroy, .step = TestBodyStep};

typedef struct TestHeard { int calls; float last; int released; EngineObjectId destroyOnCall; } TestHeard;
static void TestListen(void *user, EngineObjects *objects, EngineObjectId sender,
                       const EngineValue *arguments, int count)
{
    (void)sender;
    TestHeard *heard = user;
    heard->calls++;
    heard->last = count > 0 ? arguments[0].as.number : -1.0f;
    if (!EngineObjectIdIsNull(heard->destroyOnCall))
        EngineObjectDestroy(objects, heard->destroyOnCall);
}
static void TestRelease(void *user) { ((TestHeard *)user)->released++; }

static void ObjectChecks(void)
{
    Check(EngineTypeProperty(&testBody, "ready") == &testThingProperties[0] &&
              EngineTypeProperty(&testBody, "speed") == &testBodyProperties[1] &&
              EngineTypeProperty(&testThing, "speed") == NULL &&
              EngineTypeMethod(&testBody, "hit!") == &testBodyMethods[0] &&
              EngineTypeSignal(&testBody, "arrived") == testBodySignals[0] &&
              EngineTypeSignal(&testBody, "left") == NULL,
          "a type finds its own and its parent's properties, methods and signals by name");
    Check(EngineTypeIs(&testBody, &testThing) && !EngineTypeIs(&testThing, &testBody),
          "a type is its parent, and a parent is not its child");

    EngineObjects objects;
    Check(EngineObjectsInit(&objects), "an object pool starts empty");
    Check(EngineObjectsRegisterType(&objects, &testBody) &&
              EngineObjectsRegisterType(&objects, &testBody) &&
              EngineObjectsTypeNamed(&objects, "body") == &testBody &&
              EngineObjectsTypeNamed(&objects, "nothing") == NULL,
          "types are found by name once registered, and registering twice is harmless");

    const char *error = NULL;
    testCreated = testDestroyed = 0;
    EngineObjectId a = EngineObjectCreate(&objects, &testBody, (EngineValue[]){EngineInt(3)}, 1, &error);
    EngineObjectId b = EngineObjectCreate(&objects, &testBody, NULL, 0, &error);
    TestBody *bodyA = EngineObjectData(&objects, a, &testBody);
    TestBody *bodyB = EngineObjectData(&objects, b, &testThing);
    Check(bodyA && bodyB && bodyA != bodyB && bodyA->speed == 3.0f && bodyB->speed == 1.0f &&
              bodyA->ready && testCreated == 2,
          "objects are created with their arguments, an integer accepted where a float is wanted");
    Check(EngineObjectIdIsNull(EngineObjectCreate(&objects, &testBody,
                                                  (EngineValue[]){EngineString("fast")}, 1, &error)) &&
              error != NULL,
          "a creation argument of the wrong type is refused with a reason");

    EngineValue value = EngineNone();
    bool read = EngineObjectGet(&objects, a, "speed", &value, &error) && value.type == ENGINE_FLOAT &&
                value.as.number == 3.0f;
    bool computed = EngineObjectGet(&objects, a, "double-speed", &value, &error) &&
                    value.as.number == 6.0f;
    bool written = EngineObjectSet(&objects, a, "position", &(EngineValue){ENGINE_VECTOR2, {.vector2 = {4, 5}}},
                                   &error) &&
                   bodyA->position.x == 4 && bodyA->position.y == 5;
    Check(read && computed && written, "properties read and write by name, stored or computed");
    error = NULL;
    Check(!EngineObjectSet(&objects, a, "ready", &(EngineValue){ENGINE_BOOL, {.boolean = false}}, &error) &&
              error && bodyA->ready,
          "a read-only property refuses a write and says so");
    error = NULL;
    Check(!EngineObjectSet(&objects, a, "speed", &(EngineValue){ENGINE_STRING, {.string = "x"}}, &error) &&
              error && bodyA->speed == 3.0f,
          "a property refuses a value of the wrong type and keeps its old one");
    error = NULL;
    Check(!EngineObjectGet(&objects, a, "colour", &value, &error) && error,
          "an unknown property is an error, not a zero");

    EngineValue total = EngineNone();
    Check(EngineObjectCall(&objects, a, "hit!", (EngineValue[]){EngineInt(2)}, 1, &total, &error) &&
              EngineObjectCall(&objects, a, "hit!", (EngineValue[]){EngineFloat(3)}, 1, &total, &error) &&
              total.as.integer == 5,
          "methods run with converted arguments and answer");
    error = NULL;
    Check(!EngineObjectCall(&objects, a, "hit!", NULL, 0, &total, &error) && error &&
              !EngineObjectCall(&objects, a, "fly!", NULL, 0, &total, &error),
          "a method called with the wrong arity, or one that does not exist, is refused");
    bodyB->position = (Vector2){7, 9};
    EngineValue distance = EngineNone();
    Check(EngineObjectCall(&objects, a, "distance-to", (EngineValue[]){EngineObject(b)}, 1, &distance,
                           &error) &&
              fabsf(distance.as.number - 5.0f) < 0.001f,
          "an object passed to a method resolves to its storage");

    TestHeard heard = {0, 0, 0, ENGINE_OBJECT_NULL};
    int connection = EngineObjectConnect(&objects, a, "arrived", TestListen, &heard, TestRelease);
    Check(connection > 0 && EngineObjectConnect(&objects, a, "left", TestListen, &heard, TestRelease) == 0 &&
              heard.released == 1,
          "connecting to a signal the type does not have fails and releases what it was given");
    heard.released = 0;
    testStepped = 0;
    bodyA->position.x = 0;
    bodyA->speed = 5.0f;
    EngineObjectsStep(&objects, 1.0f);
    EngineObjectsStep(&objects, 1.0f);
    Check(testStepped == 4 && heard.calls == 1 && heard.last == 10.0f,
          "stepping advances every object, and a step can emit a signal with values");
    Check(EngineObjectDisconnect(&objects, connection) && heard.released == 1 &&
              EngineObjectEmit(&objects, a, "arrived", NULL, 0) == 0 && heard.calls == 1,
          "a disconnected handler is released and no longer called");

    TestHeard fatal = {0, 0, 0, a};
    EngineObjectConnect(&objects, a, "arrived", TestListen, &fatal, TestRelease);
    EngineObjectConnect(&objects, a, "arrived", TestListen, &fatal, TestRelease);
    int delivered = EngineObjectEmit(&objects, a, "arrived", NULL, 0);
    Check(delivered == 1 && fatal.calls == 1 && fatal.released == 2 && !EngineObjectAlive(&objects, a) &&
              testDestroyed == 1,
          "a handler that destroys the sender stops the emission and releases its connections");
    error = NULL;
    Check(!EngineObjectGet(&objects, a, "speed", &value, &error) && error &&
              EngineObjectData(&objects, a, NULL) == NULL && EngineObjectTypeOf(&objects, a) == NULL,
          "a destroyed object's handle is refused");
    EngineObjectId reused = EngineObjectCreate(&objects, &testBody, NULL, 0, &error);
    Check(reused.index == a.index && reused.generation != a.generation && !EngineObjectAlive(&objects, a),
          "a reused slot gets a new generation, so the old handle stays refused");

    TestBody mine = {.speed = 8};
    int destroyedBefore = testDestroyed;
    EngineObjectId adopted = EngineObjectAdopt(&objects, &testBody, &mine);
    EngineValue adoptedSpeed = EngineNone();
    Check(EngineObjectGet(&objects, adopted, "speed", &adoptedSpeed, &error) && adoptedSpeed.as.number == 8 &&
              EngineObjectDestroy(&objects, adopted) && testDestroyed == destroyedBefore && mine.speed == 8,
          "adopted storage is reachable by handle, and ending the handle leaves the storage alone");

    int destroyedBeforeFree = testDestroyed;
    EngineObjectsFree(&objects);
    Check(testDestroyed == destroyedBeforeFree + 2, "freeing the pool destroys every object it owns");
}

// A game's own type reaches scripts the same way the engine's do.
static void GameObjectChecks(void)
{
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    ScriptS7Open(&host);
    TestBody hero = {.position = {1, 2}, .speed = 4};
    EngineObjectId id = EngineObjectAdopt(&host.objects, &testBody, &hero);
    Check(ScriptHostRegisterType(&host, &testBody) && ScriptS7DefineObject("hero", id) &&
              EvalIs("(hero 'speed)", "4.0") && EvalIs("(hero 'ready)", "#f") &&
              ScriptS7Eval("(set! (hero 'speed) 9)", NULL) && hero.speed == 9 &&
              EvalIs("(hero 'hit! 3)", "3") && hero.hits == 3,
          "a game adopts its own storage and scripts read, write and call it by name");
    Check(EvalIs("(let ((b (make 'body 2.5))) (b 'double-speed))", "5.0") &&
              EvalIs("(< (abs (- (hero 'distance-to (make 'body)) 2.236068)) 0.0001)", "#t") &&
              !ScriptS7Eval("(make 'body \"quickly\")", NULL),
          "a game's registered type is made from Scheme like the engine's, with checked arguments");
    ScriptS7Close();
    ScriptHostFree(&host);
    Check(hero.speed == 9, "freeing the host leaves adopted storage alone");
    GameplayWorldFree(&world);
}

// ---- positional audio: distance, pan, voices -------------------------------------------------
static void AudioChecks(void)
{
    // Distance: full at the source, gone at the edge, linear between -- Godot's max_distance falloff.
    Check(CoreAudioFalloff(0, 10) == 1.0f && fabsf(CoreAudioFalloff(5, 10) - 0.5f) < 1e-6f &&
              CoreAudioFalloff(10, 10) == 0.0f && CoreAudioFalloff(25, 10) == 0.0f &&
              CoreAudioFalloff(3, 0) == 1.0f,
          "a sound falls off linearly to silence at its range, and a zero range never falls off");

    CoreAudio audio;
    CoreAudioInit(&audio);
    CoreAudioSetListener(&audio, (Vector3){0, 0, 0}, (Vector3){0, 0, 1}, (Vector3){0, 1, 0});
    // The engine's convention: facing +Z with +Y up, the listener's right is -X (core/README.md).
    float right = CoreAudioPanAt(&audio, (Vector3){-5, 0, 0});
    float left = CoreAudioPanAt(&audio, (Vector3){5, 0, 0});
    float ahead = CoreAudioPanAt(&audio, (Vector3){0, 0, 5});
    float above = CoreAudioPanAt(&audio, (Vector3){0, 5, 0});
    Check(right < 0.5f && left > 0.5f && fabsf(ahead - 0.5f) < 1e-6f && fabsf(above - 0.5f) < 1e-6f &&
              fabsf((right - 0.5f) + (left - 0.5f)) < 1e-6f,
          "a sound to the listener's right pans right, left pans left, and ahead or overhead stays centred");
    CoreAudioSetListener(&audio, (Vector3){0, 0, 0}, (Vector3){1, 0, 0}, (Vector3){0, 1, 0});
    Check(CoreAudioPanAt(&audio, (Vector3){0, 0, 5}) < 0.5f && CoreAudioPanAt(&audio, (Vector3){0, 0, -5}) > 0.5f,
          "panning follows the way the listener faces, not the world axes");
    Check(fabsf(CoreAudioGainAt(&audio, (Vector3){3, 0, 4}, 10, 0.8f) - 0.4f) < 1e-6f,
          "a placed sound's gain is its own gain times the falloff at its distance from the listener");

    // Everything past here needs a real device and a real sound.
    Wave wave = {.frameCount = 4410, .sampleRate = 44100, .sampleSize = 16, .channels = 1};
    short *samples = calloc(wave.frameCount, sizeof *samples);
    for (unsigned int i = 0; samples && i < wave.frameCount; i++)
        samples[i] = (short)(8000 * sinf((float)i * 0.05f));
    wave.data = samples;
    char path[512];
    snprintf(path, sizeof path, "%s", Scratch("regression_tone.wav"));
    bool exported = samples && ExportWave(wave, path);
    free(samples);
    if (!exported || !CoreAudioLoadSound(&audio, path, "sfx"))
    {
        printf("  audio device unavailable: voice checks skipped\n");
        CoreAudioFree(&audio);
        return;
    }
    Check(CoreAudioPlaySoundGain(&audio, path, "sfx", 0.5f) &&
              CoreAudioPlaySoundGain(&audio, path, "sfx", 0.5f) && CoreAudioSoundPlaying(&audio, path) &&
              !CoreAudioPlaySoundGain(&audio, path, "no-such-bus", 1.0f),
          "one sound plays over itself on separate voices, and an unknown bus is refused");
    Check(!CoreAudioPlaySoundAt(&audio, path, "sfx", (Vector3){100, 0, 0}, 10, 1.0f) &&
              CoreAudioPlaySoundAt(&audio, path, "sfx", (Vector3){2, 0, 0}, 10, 1.0f),
          "a placed sound out of range is not started, and one in range is");
    CoreAudioStopSound(&audio, path);
    Check(!CoreAudioSoundPlaying(&audio, path), "stopping a sound stops every voice of it");

    CoreAudioVoice voice = CoreAudioVoiceCreate(&audio, path, "sfx");
    Check(CoreAudioVoiceValid(&audio, voice) && CoreAudioVoicePlayAt(&audio, voice, (Vector3){0, 0, 2}, 10, 1.0f) &&
              CoreAudioVoicePlaying(&audio, voice),
          "a held voice starts at a place");
    Check(CoreAudioVoiceMove(&audio, voice, (Vector3){0, 0, 50}) && CoreAudioVoiceGain(&audio, voice) == 0.0f,
          "moving a held voice out of range silences it");
    CoreAudioVoiceSetLoop(&audio, voice, true);
    CoreAudioVoiceStop(&audio, voice);
    CoreAudioUpdate(&audio);
    Check(!CoreAudioVoicePlaying(&audio, voice), "a stopped looping voice is not restarted by the update");
    CoreAudioVoiceFree(&audio, voice);
    Check(!CoreAudioVoiceValid(&audio, voice) && !CoreAudioVoicePlay(&audio, voice, 1.0f),
          "a freed voice's handle is refused");
    CoreAudioFree(&audio);
}

// ---- the first-person controller, against the hand-written one Trenchfoot carried ----------
/* A line-for-line transcription of the movement Trenchfoot wrote inline in its main loop
   (main_trench.c, look + sway + walk + slide + eye follow + gait + bob), with the game's own
   modifiers -- wading, wounds, the limp, the ladder, the mortar shake -- left out, because those
   stay the game's. The engine controller must reproduce it step for step. */
typedef struct FpsReference
{
    float yaw, pitch, prevYaw, prevPitch, lagYaw, lagPitch, camY, phase, blend;
    Vector3 body, vel;
    double time;
} FpsReference;

static bool FpsTestCanWalk(float x, float z) { (void)z; return x < 5.0f; } // a wall at x = 5
static float FpsTestGround(void *context, float x, float z)
{
    (void)context;
    return 0.2f * sinf(x) + 0.1f * z;
}
static Vector3 FpsTestSlide(void *context, Vector3 p, Vector3 desired)
{
    (void)context;
    if (FpsTestCanWalk(desired.x, desired.z))
        return desired;
    if (FpsTestCanWalk(desired.x, p.z))
        return (Vector3){desired.x, desired.y, p.z};
    if (FpsTestCanWalk(p.x, desired.z))
        return (Vector3){p.x, desired.y, desired.z};
    return p;
}

static int FpsReferenceStep(FpsReference *r, const FpsCameraConfig *g, Vector2 md, float ix, float iz,
                            float speed, bool crouch, float dt, Camera *cam)
{
    r->yaw -= md.x * g->sensitivity;
    r->pitch -= md.y * g->sensitivity;
    r->pitch = Clamp(r->pitch, -g->pitchLimit, g->pitchLimit);
    const ViewmodelMotion *vm = &g->viewmodel;
    float yawDelta = r->yaw - r->prevYaw;
    if (yawDelta > PI) yawDelta -= 2.0f * PI;
    if (yawDelta < -PI) yawDelta += 2.0f * PI;
    float swayResponse = fmaxf(vm->swayResponse, 0.01f), swayMax = fmaxf(vm->swayMax, 0.0001f);
    float invFrame = 1.0f / fmaxf(dt, 0.001f);
    float yawTarget = -yawDelta * invFrame / swayResponse;
    float pitchTarget = -(r->pitch - r->prevPitch) * invFrame / swayResponse;
    yawTarget = swayMax * tanhf(yawTarget / swayMax);
    pitchTarget = swayMax * tanhf(pitchTarget / swayMax);
    float swayFollow = 1.0f - expf(-swayResponse * dt);
    r->lagYaw += (yawTarget - r->lagYaw) * swayFollow;
    r->lagPitch += (pitchTarget - r->lagPitch) * swayFollow;
    r->prevYaw = r->yaw;
    r->prevPitch = r->pitch;
    Vector3 fwd = {cosf(r->pitch) * sinf(r->yaw), sinf(r->pitch), cosf(r->pitch) * cosf(r->yaw)};
    Vector3 flat = Vector3Normalize((Vector3){fwd.x, 0, fwd.z});
    Vector3 right = {-flat.z, 0, flat.x};
    Vector3 old = r->body;
    float il = sqrtf(ix * ix + iz * iz);
    if (il > 1.0f) { ix /= il; iz /= il; }
    Vector3 wish = Vector3Scale(Vector3Add(Vector3Scale(right, ix), Vector3Scale(flat, iz)), speed);
    float va = 1.0f - expf(-((il > 0) ? g->acceleration : g->deceleration) * dt);
    r->vel.x += (wish.x - r->vel.x) * va;
    r->vel.z += (wish.z - r->vel.z) * va;
    Vector3 p = r->body, mv = Vector3Scale(r->vel, dt);
    p = FpsTestSlide(NULL, p, (Vector3){p.x + mv.x, p.y, p.z + mv.z});
    if (fabsf(p.x - old.x) < fabsf(mv.x) * 0.5f) r->vel.x = 0;
    if (fabsf(p.z - old.z) < fabsf(mv.z) * 0.5f) r->vel.z = 0;
    float target = FpsTestGround(NULL, p.x, p.z) + (crouch ? g->crouchEye : g->eyeHeight);
    r->camY += (target - r->camY) * (1.0f - expf(-g->eyeFollow * dt));
    p.y = r->camY;
    r->body = p;
    float travelled = sqrtf((p.x - old.x) * (p.x - old.x) + (p.z - old.z) * (p.z - old.z));
    float actualSpeed = travelled / fmaxf(dt, 0.0001f);
    float runMix = Clamp((actualSpeed - g->walkSpeed) / fmaxf(g->runSpeed - g->walkSpeed, 0.01f), 0, 1);
    r->blend += (Clamp(actualSpeed / g->walkSpeed, 0, 1) - r->blend) * (1.0f - expf(-g->bobResponse * dt));
    float stepLen = g->stepWalk + (g->stepRun - g->stepWalk) * runMix;
    float next = r->phase + travelled * PI / stepLen;
    int footfall = (int)(next / PI) > (int)(r->phase / PI);
    r->phase = fmodf(next, 2.0f * PI);
    r->time += dt;
    float amp = r->blend * (1.0f + (g->bobRunScale - 1.0f) * runMix);
    float doubleWave = sinf(2.0f * r->phase);
    float bobY = g->bobVertical * amp * (-0.5f * cosf(2.0f * r->phase) + 0.1f * sinf(4.0f * r->phase));
    float roll = g->bobRoll * amp * cosf(r->phase), nod = g->bobPitch * amp * doubleWave;
    float breath = sinf((float)r->time * 2.0f * PI * g->breathRate) * g->breathAmount * (1.0f - r->blend);
    Vector3 viewPos = Vector3Add(p, Vector3Add(Vector3Scale(right, g->bobSide * amp * cosf(r->phase)),
                                               Vector3Scale(flat, g->bobForward * amp * doubleWave)));
    viewPos.y += bobY + breath;
    fwd = Vector3Normalize(Vector3RotateByAxisAngle(fwd, right, nod));
    Vector3 viewUp = Vector3Normalize(Vector3CrossProduct(right, fwd));
    *cam = (Camera){viewPos, Vector3Add(viewPos, fwd), Vector3Normalize(Vector3RotateByAxisAngle(viewUp, fwd, roll)),
                    g->fov, CAMERA_PERSPECTIVE};
    return footfall;
}

static float FpsTestGap(Vector3 a, Vector3 b) { return Vector3Distance(a, b); }

static void FpsControllerChecks(void)
{
    FpsCameraConfig config = FpsCameraDefaults();
    FpsCamera engine;
    Vector3 start = {0, FpsTestGround(NULL, 0, 0) + config.eyeHeight, 0};
    FpsCameraInit(&engine, start, 0.3f, 0.0f, config.fov);
    FpsReference ref = {.yaw = 0.3f, .prevYaw = 0.3f, .body = start, .camY = start.y};
    FpsWorld world = {NULL, FpsTestSlide, FpsTestGround};
    float worst = 0, worstLag = 0, worstPhase = 0;
    int engineSteps = 0, referenceSteps = 0;
    for (int i = 0; i < 600; i++)
    {
        float dt = (i % 7 == 0) ? 1.0f / 30.0f : 1.0f / 60.0f;      // an uneven frame rate
        Vector2 look = {(i % 90 < 30) ? 6.0f : ((i % 90 < 45) ? -14.0f : 0.0f), (i % 50 < 10) ? 3.0f : -1.0f};
        float ix = (i % 200 < 100) ? 1.0f : 0.0f, iz = (i % 240 < 200) ? 1.0f : 0.0f;
        bool crouch = i % 300 > 260;
        float speed = (i % 150 < 75 ? config.runSpeed : config.walkSpeed) * (i % 100 < 20 ? 0.45f : 1.0f);
        FpsInput input = {.move = {ix, 0, iz}, .lookDelta = look, .crouch = crouch, .useSpeed = true, .speed = speed};
        engineSteps += FpsCameraUpdate(&engine, &config, input, world, dt);
        Camera reference;
        referenceSteps += FpsReferenceStep(&ref, &config, look, ix, iz, speed, crouch, dt, &reference);
        worst = fmaxf(worst, FpsTestGap(engine.current.position, reference.position));
        worst = fmaxf(worst, FpsTestGap(Vector3Subtract(engine.current.target, engine.current.position),
                                        Vector3Subtract(reference.target, reference.position)));
        worst = fmaxf(worst, FpsTestGap(engine.current.up, reference.up));
        worstLag = fmaxf(worstLag, fmaxf(fabsf(engine.lagYaw - ref.lagYaw), fabsf(engine.lagPitch - ref.lagPitch)));
        worstPhase = fmaxf(worstPhase, fabsf(engine.phase - ref.phase));
    }
    // Float rounding only: measured at 8.6e-6 m and 1.4e-5 rad over these 600 steps.
    Check(worst < 1e-4f && worstLag < 1e-6f && worstPhase < 1e-4f && engineSteps == referenceSteps &&
              engineSteps > 10,
          "the engine's FPS controller reproduces the hand-written game controller step for step");
    Check(engine.position.x < 5.0f, "the controller slides along what the world says blocks it");
    FpsInput rooted = {.move = {0, 0, 1}, .useSpeed = true, .speed = 0};
    Vector3 before = engine.position;
    for (int i = 0; i < 30; i++)
        FpsCameraUpdate(&engine, &config, rooted, world, 1.0f / 60.0f);
    Check(fabsf(engine.position.x - before.x) < 0.2f && fabsf(engine.position.z - before.z) < 0.2f &&
              engine.bobAmp >= 0.0f,
          "a speed the game sets holds even at zero, rather than falling back to walking");
    Vector3 standing = engine.position, facing = Vector3Subtract(engine.current.target, engine.current.position);
    float lagBefore = engine.lagYaw;
    FpsCameraLook(&engine, &config, (Vector2){200, 0});
    Vector3 turned = Vector3Subtract(engine.current.target, engine.current.position);
    Check(Vector3Distance(engine.position, standing) < 1e-6f && Vector3DotProduct(facing, turned) < 0.95f &&
              engine.lagYaw == lagBefore,
          "the view turns while time stands still, and nothing else moves");
}

// ---- .rig binary loader ----------------------------------------------------------------------
static unsigned short RigTestFloatToHalf(float f)
{
    unsigned int x;
    memcpy(&x, &f, sizeof x);
    unsigned int sign = (x >> 16) & 0x8000u;
    int exp = (int)((x >> 23) & 0xFFu) - 127 + 15;
    unsigned int man = x & 0x7FFFFFu;
    if (exp <= 0)
        return (unsigned short)sign; // flush tiny magnitudes to zero; unused by this test's values
    if (exp >= 0x1F)
        return (unsigned short)(sign | 0x7C00u); // inf; unused by this test's values
    return (unsigned short)(sign | ((unsigned int)exp << 10) | (man >> 13));
}
// Matrix and a row-major float[16] share memory layout (see core/rig_file.c), so a plain memcpy
// gets at the top three rows -- the 12 values the binary format stores per matrix.
static void RigTestWriteMatrix(FILE *f, Matrix m)
{
    float a[16];
    memcpy(a, &m, sizeof a);
    for (int i = 0; i < 12; i++)
    {
        unsigned short h = RigTestFloatToHalf(a[i]);
        fwrite(&h, sizeof h, 1, f);
    }
}
static bool RigTestClose(float a, float b, float eps) { return fabsf(a - b) < eps; }
static bool RigTestMatrixClose(Matrix a, Matrix b, float eps)
{
    const float *pa = (const float *)&a, *pb = (const float *)&b;
    for (int i = 0; i < 16; i++)
        if (!RigTestClose(pa[i], pb[i], eps))
            return false;
    return true;
}

static void RigFileChecks(void)
{
    // Three bones (root, child, grandchild), non-trivial rotation and translation, scale 1.
    Matrix rest[3] = {
        MatrixMultiply(MatrixRotateXYZ((Vector3){0.30f, 0.15f, -0.20f}), MatrixTranslate(0.50f, 1.00f, -0.30f)),
        MatrixMultiply(MatrixRotateXYZ((Vector3){-0.40f, 0.25f, 0.10f}), MatrixTranslate(0.05f, 0.60f, 0.02f)),
        MatrixMultiply(MatrixRotateXYZ((Vector3){0.20f, -0.30f, 0.35f}), MatrixTranslate(-0.10f, 0.50f, 0.08f)),
    };
    Matrix frame0[3] = {rest[0], rest[1], rest[2]};
    Matrix frame1[3] = {
        MatrixMultiply(MatrixRotateXYZ((Vector3){0.50f, -0.10f, 0.40f}), MatrixTranslate(0.60f, 1.10f, -0.25f)),
        MatrixMultiply(MatrixRotateXYZ((Vector3){-0.20f, 0.40f, -0.15f}), MatrixTranslate(0.10f, 0.65f, 0.00f)),
        MatrixMultiply(MatrixRotateXYZ((Vector3){0.30f, -0.20f, 0.50f}), MatrixTranslate(-0.05f, 0.55f, 0.12f)),
    };
    const char *names[3] = {"root", "child", "grandchild"};
    short parents[3] = {-1, 0, 1};

    const char *path = Scratch("regression_test.rig");
    FILE *f = fopen(path, "wb");
    fwrite("RIGB", 1, 4, f);
    float fps = 30.0f;
    fwrite(&fps, sizeof fps, 1, f);
    unsigned short nb = 3, nv = 5, na = 1;
    fwrite(&nb, sizeof nb, 1, f);
    fwrite(&nv, sizeof nv, 1, f);
    fwrite(&na, sizeof na, 1, f);
    for (int i = 0; i < 3; i++)
    {
        char name[32] = {0};
        strncpy(name, names[i], sizeof name - 1);
        fwrite(name, 1, sizeof name, f);
        fwrite(&parents[i], sizeof parents[i], 1, f);
        RigTestWriteMatrix(f, rest[i]);
    }
    // Vertices: no weights, one, two, three (ordinary cases), then five (must truncate to four).
    int vertN[5] = {0, 1, 2, 3, 5};
    unsigned char vertBones[5][5] = {{0}, {1}, {0, 2}, {0, 1, 2}, {0, 1, 2, 0, 1}};
    float vertWeights[5][5] = {
        {0}, {1.0f}, {0.25f, 0.75f}, {0.20f, 0.30f, 0.50f}, {0.10f, 0.10f, 0.10f, 0.35f, 0.35f}};
    for (int v = 0; v < 5; v++)
    {
        unsigned char n = (unsigned char)vertN[v];
        fwrite(&n, 1, 1, f);
        for (int k = 0; k < vertN[v]; k++)
        {
            unsigned char bone = vertBones[v][k];
            unsigned short w = RigTestFloatToHalf(vertWeights[v][k]);
            fwrite(&bone, 1, 1, f);
            fwrite(&w, sizeof w, 1, f);
        }
    }
    char clipName[64] = {0};
    strncpy(clipName, "clip0", sizeof clipName - 1);
    fwrite(clipName, 1, sizeof clipName, f);
    short clipF0 = 10, clipF1 = 11;
    fwrite(&clipF0, sizeof clipF0, 1, f);
    fwrite(&clipF1, sizeof clipF1, 1, f);
    Matrix *frames[2] = {frame0, frame1};
    for (int fr = 0; fr < 2; fr++)
        for (int b = 0; b < 3; b++)
            RigTestWriteMatrix(f, frames[fr][b]);
    fclose(f);

    CoreRigFile rig = {0};
    Check(CoreRigFileLoad(&rig, path), "a well-formed .rig file loads");
    Check(rig.boneCount == 3 && !strcmp(rig.bones[0].name, "root") && !strcmp(rig.bones[1].name, "child") &&
              !strcmp(rig.bones[2].name, "grandchild") && rig.bones[0].parent == -1 &&
              rig.bones[1].parent == 0 && rig.bones[2].parent == 1 && RigTestClose(rig.fps, 30.0f, 1e-4f),
          "bone names, parents and fps read back");

    Check(rig.boneIds[0 * 4 + 0] == 0 && RigTestClose(rig.boneWeights[0 * 4 + 0], 1.0f, 1e-3f),
          "a vertex with no weights gets bone 0 weight 1");
    Check(rig.boneIds[1 * 4 + 0] == 1 && RigTestClose(rig.boneWeights[1 * 4 + 0], 1.0f, 1e-3f),
          "a single-weight vertex reads back");
    Check(rig.boneIds[2 * 4 + 0] == 0 && RigTestClose(rig.boneWeights[2 * 4 + 0], 0.25f, 1e-3f) &&
              rig.boneIds[2 * 4 + 1] == 2 && RigTestClose(rig.boneWeights[2 * 4 + 1], 0.75f, 1e-3f),
          "a two-weight vertex reads back at half-float precision");
    Check(rig.boneIds[3 * 4 + 0] == 0 && rig.boneIds[3 * 4 + 1] == 1 && rig.boneIds[3 * 4 + 2] == 2 &&
              RigTestClose(rig.boneWeights[3 * 4 + 0], 0.20f, 1e-3f) &&
              RigTestClose(rig.boneWeights[3 * 4 + 1], 0.30f, 1e-3f) &&
              RigTestClose(rig.boneWeights[3 * 4 + 2], 0.50f, 1e-3f),
          "a three-weight vertex reads back");
    Check(rig.boneIds[4 * 4 + 0] == 0 && rig.boneIds[4 * 4 + 1] == 1 && rig.boneIds[4 * 4 + 2] == 2 &&
              rig.boneIds[4 * 4 + 3] == 0 && RigTestClose(rig.boneWeights[4 * 4 + 3], 0.35f, 1e-3f),
          "a vertex with more than four weights keeps only the first four");

    bool bindOk = true;
    for (int i = 0; i < 3; i++)
        if (!RigTestMatrixClose(TransformMatrix(rig.bindPose[i]), rest[i], 5e-3f))
            bindOk = false;
    Check(bindOk, "TransformMatrix(bindPose[i]) recomposes to the rest matrix it was decomposed from");

    Check(rig.clipCount == 1 && !strcmp(rig.clips[0].name, "clip0") &&
              rig.clips[0].animation.boneCount == 3 && rig.clips[0].animation.frameCount == 2 &&
              rig.clips[0].animation.bones != NULL && rig.clips[0].animation.framePoses != NULL,
          "one clip with two frames reads back, with animation.bones/boneCount set");

    CoreSkeleton skel;
    Check(RigInit(&skel, rig.bones, rig.bindPose, rig.boneCount),
          "a CoreSkeleton initializes from the rig's bones and bind pose");
    Matrix skin[3];
    RigSkinMatrices(&skel, rig.clips[0].animation.framePoses[1], skin);
    // Which order matches raylib's convention is exactly what this checks, two ways: matrix
    // equality against both candidate products, and a rest-space vertex moved by the skin matrix
    // versus moved by the two matrices applied in sequence, each in both orders.
    bool matchesInvBindThenPose = true, matchesPoseThenInvBind = true;
    bool vertexMatchesInvBindThenPose = true, vertexMatchesPoseThenInvBind = true;
    Vector3 sample = {0.2f, 0.4f, -0.1f};
    for (int i = 0; i < 3; i++)
    {
        Matrix invBind = MatrixInvert(rest[i]);
        Matrix invBindThenPose = MatrixMultiply(invBind, frame1[i]);
        Matrix poseThenInvBind = MatrixMultiply(frame1[i], invBind);
        if (!RigTestMatrixClose(skin[i], invBindThenPose, 5e-3f))
            matchesInvBindThenPose = false;
        if (!RigTestMatrixClose(skin[i], poseThenInvBind, 5e-3f))
            matchesPoseThenInvBind = false;
        Vector3 viaSkin = Vector3Transform(sample, skin[i]);
        Vector3 viaInvBindThenPose = Vector3Transform(Vector3Transform(sample, invBind), frame1[i]);
        Vector3 viaPoseThenInvBind = Vector3Transform(Vector3Transform(sample, frame1[i]), invBind);
        if (Vector3Distance(viaSkin, viaInvBindThenPose) > 5e-3f)
            vertexMatchesInvBindThenPose = false;
        if (Vector3Distance(viaSkin, viaPoseThenInvBind) > 5e-3f)
            vertexMatchesPoseThenInvBind = false;
    }
    // Report: MatrixMultiply(MatrixInvert(rest), pose) -- invBind applied first, pose applied
    // second -- is the order that matches RigSkinMatrices, both as a matrix and as a vertex moved
    // through the two steps in that sequence.
    Check(matchesInvBindThenPose && vertexMatchesInvBindThenPose && !matchesPoseThenInvBind &&
              !vertexMatchesPoseThenInvBind,
          "RigSkinMatrices matches MatrixMultiply(MatrixInvert(rest), pose): invBind applied first, "
          "pose applied second, not the reverse order");

    CoreRigFile truncated = {0};
    const char *truncatedPath = Scratch("regression_test_truncated.rig");
    FILE *tf = fopen(truncatedPath, "wb");
    fwrite("RIGB", 1, 4, tf);
    fwrite(&fps, sizeof fps, 1, tf);
    fwrite(&nb, sizeof nb, 1, tf);
    fwrite(&nv, sizeof nv, 1, tf);
    fwrite(&na, sizeof na, 1, tf);
    char rootName[32] = {0};
    strncpy(rootName, names[0], sizeof rootName - 1);
    fwrite(rootName, 1, sizeof rootName, tf); // only the first bone's name -- the file ends here
    fclose(tf);
    Check(!CoreRigFileLoad(&truncated, truncatedPath) && truncated.boneCount == 0 && truncated.bones == NULL &&
              truncated.clips == NULL,
          "a truncated .rig file fails to load and leaves the struct zeroed");

    CoreRigFileFree(&rig);

    // ActorLookAt only reads/writes the aim/look fields, so the snap can be checked on a bare
    // Actor{0} without ActorInit, a Model, or a GPU -- no heavy Actor construction needed.
    Actor snap = {0};
    snap.aim.maxYaw = 75 * DEG2RAD;
    snap.aim.maxPitch = 45 * DEG2RAD;
    snap.aim.response = 0; // no response: ActorUpdate would never move lookYaw toward targetYaw
    snap.lookYaw = 0.1f;
    snap.lookPitch = -0.2f;
    snap.previousYaw = -0.3f;
    snap.previousPitch = 0.4f;
    ActorLookAt(&snap, (Vector3){0, 0, 0}, (Vector3){1, 0, 1}, 0.0f);
    Check((snap.targetYaw != 0.1f) && RigTestClose(snap.lookYaw, snap.targetYaw, 1e-6f) &&
              RigTestClose(snap.previousYaw, snap.targetYaw, 1e-6f) &&
              RigTestClose(snap.lookPitch, snap.targetPitch, 1e-6f) &&
              RigTestClose(snap.previousPitch, snap.targetPitch, 1e-6f),
          "ActorLookAt snaps look/previous yaw and pitch to target immediately when aim.response is zero");
}

// ---- node3d: parent/child transforms -----------------------------------------------------------
static void NodeChecks(void)
{
    EngineObjects objects;
    Check(EngineObjectsInit(&objects), "a node pool starts empty");

    const char *error = NULL;
    EngineObjectId parent = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId child = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    CoreNode *parentData = EngineObjectData(&objects, parent, &CoreNodeType);
    CoreNode *childData = EngineObjectData(&objects, child, &CoreNodeType);
    Check(parentData && childData && Vector3Equals(parentData->scale, (Vector3){1, 1, 1}) &&
              QuaternionEquals(parentData->rotation, QuaternionIdentity()) &&
              EngineObjectIdIsNull(parentData->parent),
          "a new node starts at the local identity, scale one, and no parent");

    // A child at local (1,0,0) under a parent at (10,0,0) rotated 90 degrees about Y ends at the
    // point that rotation carries local +X to, offset by the parent's position.
    parentData->position = (Vector3){10, 0, 0};
    parentData->rotation = QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2.0f);
    childData->position = (Vector3){1, 0, 0};
    Check(CoreNodeSetParent(&objects, child, parent, false), "a node is parented to another live node");

    Vector3 childWorld = {0};
    Matrix childMatrix = {0};
    Check(CoreNodeWorldPosition(&objects, child, &childWorld) &&
              Vector3Distance(childWorld, (Vector3){10, 0, -1}) < 1e-5f &&
              CoreNodeWorld(&objects, child, &childMatrix) &&
              Vector3Distance(Vector3Transform((Vector3){0, 0, 0}, childMatrix), (Vector3){10, 0, -1}) < 1e-5f,
          "a child's world transform is its parent's world transform times its own local one");

    // A grandchild chain: root at (1,2,3) rotated 90 about Y, a mid node one local unit along its
    // rotated +Z, a leaf five local units along the mid node's own (equally rotated) +X.
    EngineObjectId root = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId mid = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId leaf = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    CoreNode *rootData = EngineObjectData(&objects, root, &CoreNodeType);
    rootData->position = (Vector3){1, 2, 3};
    rootData->rotation = QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2.0f);
    ((CoreNode *)EngineObjectData(&objects, mid, &CoreNodeType))->position = (Vector3){0, 0, 1};
    ((CoreNode *)EngineObjectData(&objects, leaf, &CoreNodeType))->position = (Vector3){5, 0, 0};
    CoreNodeSetParent(&objects, mid, root, false);
    CoreNodeSetParent(&objects, leaf, mid, false);
    Vector3 midWorld = {0}, leafWorld = {0};
    Check(CoreNodeWorldPosition(&objects, mid, &midWorld) &&
              Vector3Distance(midWorld, (Vector3){2, 2, 3}) < 1e-5f &&
              CoreNodeWorldPosition(&objects, leaf, &leafWorld) &&
              Vector3Distance(leafWorld, (Vector3){2, 2, -2}) < 1e-5f,
          "a grandchild's world transform carries its whole ancestor chain, not just its parent");

    // Moving the parent moves the child's world position by the same translation.
    EngineObjectId still = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId moved = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    ((CoreNode *)EngineObjectData(&objects, moved, &CoreNodeType))->position = (Vector3){3, 4, 5};
    CoreNodeSetParent(&objects, moved, still, false);
    ((CoreNode *)EngineObjectData(&objects, still, &CoreNodeType))->position = (Vector3){1, 1, 1};
    Vector3 afterMove = {0};
    Check(CoreNodeWorldPosition(&objects, moved, &afterMove) &&
              Vector3Distance(afterMove, (Vector3){4, 5, 6}) < 1e-5f,
          "moving the parent moves the child's world position with it");

    // SetParent with keepWorld: reparenting under a scaled, rotated node keeps the world position,
    // and halves the local scale needed to reproduce the same world size under a doubled parent.
    EngineObjectId flat = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId big = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId reparented = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    ((CoreNode *)EngineObjectData(&objects, flat, &CoreNodeType))->position = (Vector3){5, 0, 0};
    CoreNode *bigData = EngineObjectData(&objects, big, &CoreNodeType);
    bigData->rotation = QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2.0f);
    bigData->scale = (Vector3){2, 2, 2};
    CoreNode *reparentedData = EngineObjectData(&objects, reparented, &CoreNodeType);
    reparentedData->position = (Vector3){1, 0, 0};
    CoreNodeSetParent(&objects, reparented, flat, false);
    Vector3 worldBeforeReparent = {0};
    CoreNodeWorldPosition(&objects, reparented, &worldBeforeReparent);
    Check(CoreNodeSetParent(&objects, reparented, big, true), "keepWorld reparents to another live node");
    Vector3 worldAfterReparent = {0};
    Check(CoreNodeWorldPosition(&objects, reparented, &worldAfterReparent) &&
              Vector3Distance(worldBeforeReparent, worldAfterReparent) < 1e-5f &&
              Vector3Distance(reparentedData->scale, (Vector3){0.5f, 0.5f, 0.5f}) < 1e-5f,
          "SetParent with keepWorld keeps the world position, adjusting the local transform under it");

    // A cycle, self-parenting included, is refused and leaves the node's parent unchanged.
    EngineObjectId x = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId y = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId z = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    CoreNodeSetParent(&objects, y, x, false);
    CoreNodeSetParent(&objects, z, y, false);
    CoreNode *xData = EngineObjectData(&objects, x, &CoreNodeType);
    Check(!CoreNodeSetParent(&objects, x, z, false) && EngineObjectIdIsNull(xData->parent) &&
              !CoreNodeSetParent(&objects, x, x, false) && EngineObjectIdIsNull(xData->parent),
          "a node cannot become its own ancestor, directly or through a chain, or its own parent");

    // Destroying a parent reparents its children to none, keeping their world position.
    EngineObjectId gone = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId survivor = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    CoreNode *goneData = EngineObjectData(&objects, gone, &CoreNodeType);
    goneData->position = (Vector3){2, 3, 4};
    goneData->rotation = QuaternionFromAxisAngle((Vector3){0, 1, 0}, PI / 2.0f);
    ((CoreNode *)EngineObjectData(&objects, survivor, &CoreNodeType))->position = (Vector3){1, 0, 0};
    CoreNodeSetParent(&objects, survivor, gone, false);
    Vector3 survivorWorldBefore = {0};
    CoreNodeWorldPosition(&objects, survivor, &survivorWorldBefore);
    Check(EngineObjectDestroy(&objects, gone) && !EngineObjectAlive(&objects, gone),
          "the parent is destroyed");
    Vector3 survivorWorldAfter = {0};
    CoreNode *survivorData = EngineObjectData(&objects, survivor, &CoreNodeType);
    Check(EngineObjectAlive(&objects, survivor) && survivorData &&
              EngineObjectIdIsNull(survivorData->parent) &&
              CoreNodeWorldPosition(&objects, survivor, &survivorWorldAfter) &&
              Vector3Distance(survivorWorldBefore, survivorWorldAfter) < 1e-5f,
          "the child outlives its destroyed parent, at the same world position, with no parent");

    // Stale handles answer false rather than reaching whatever took their place.
    EngineObjectId staleParent = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectId aliveNode = EngineObjectCreate(&objects, &CoreNodeType, NULL, 0, &error);
    EngineObjectDestroy(&objects, staleParent);
    Vector3 staleVector = {0};
    Matrix staleMatrix = {0};
    Check(!CoreNodeWorld(&objects, staleParent, &staleMatrix) &&
              !CoreNodeWorldPosition(&objects, staleParent, &staleVector) &&
              !CoreNodeSetWorldPosition(&objects, staleParent, (Vector3){1, 1, 1}) &&
              !CoreNodeSetParent(&objects, staleParent, aliveNode, false),
          "a stale node handle is refused rather than reaching whatever replaced it");
    CoreNode *aliveData = EngineObjectData(&objects, aliveNode, &CoreNodeType);
    Check(CoreNodeSetParent(&objects, aliveNode, staleParent, false) &&
              EngineObjectIdIsNull(aliveData->parent),
          "a parent argument that no longer names a live node counts as no parent");

    EngineObjectsFree(&objects);
}

static void NetOwnershipChecks(void)
{
    /* A listen server's client registry has localIsServer true and localActor its own player id
       (not zero), so the read path's "is this mine" and CoreNetSyncSerialize's must agree, or the
       host reads its own old snapshots back over the live world it is simulating. */
    const CoreNetField fields[] = {
        CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
    };
    const CoreNetSchema serverSchema = {30, "hostmine", sizeof(NetTestState), fields, 1,
                                        CORE_NET_AUTHORITY_SERVER, 0};
    const CoreNetSchema ownerSchema = {31, "ownermine", sizeof(NetTestState), fields, 1,
                                       CORE_NET_AUTHORITY_OWNER, 0};
    CoreNetSync sync = {0};
    Check(CoreNetSyncInit(&sync, 4, 2) && CoreNetSyncRegister(&sync, &serverSchema) &&
              CoreNetSyncRegister(&sync, &ownerSchema),
          "ownership registry starts with a server-authority and an owner-authority schema");

    CoreNetObject *serverObject = CoreNetSyncSpawn(&sync, 1, 30, 0);
    Check(serverObject != NULL, "the server-authority object spawns");

    bool record[2] = {false, false};
    Check(CoreNetSyncObserve(&sync, 1, NetOwnerRecordCall, record),
          "a callback attaches to the server-authority object");

    CoreNetSyncSetLocalActor(&sync, 3, true);   /* a listen-server host whose player is actor 3 */
    Check(CoreNetSyncSerialize(&sync, 3, 0.0) == 1 && record[0] && record[1],
          "a listen-server host writes its own server-authority object instead of reading it back");

    Check(CoreNetSyncIsMine(&sync, serverObject),
          "CoreNetSyncIsMine agrees: the host owns the server object it also runs");
    CoreNetSyncSetLocalActor(&sync, 3, false);
    Check(!CoreNetSyncIsMine(&sync, serverObject),
          "a plain client with the same actor id does not own that server object");

    CoreNetObject *mineObject = CoreNetSyncSpawn(&sync, 2, 31, 3);
    CoreNetObject *otherObject = CoreNetSyncSpawn(&sync, 3, 31, 4);
    Check(mineObject && otherObject, "two owner-authority objects spawn for two different owners");
    CoreNetSyncSetLocalActor(&sync, 3, false);
    Check(CoreNetSyncIsMine(&sync, mineObject), "client 3 owns the owner-authority object it owns");
    Check(!CoreNetSyncIsMine(&sync, otherObject),
          "client 3 does not own another actor's owner-authority object");
    Check(!CoreNetSyncIsMine(&sync, NULL), "a null object is never mine");

    CoreNetSyncFree(&sync);
}

// ---- a ray query over a fixed set of Models ------------------------------------------------------
static void Collision3DChecks(void)
{
    // A unit cube at the origin (faces at +-1) and a second one translated to +-1 around z=3, both
    // in front of a ray fired from (0,0,5) toward -Z -- the same setup Trenchfoot's PropWorldRay
    // family raycasts against, just with two boxes standing in for its four models.
    Model nearModel = LoadModelFromMesh(GenMeshCube(2, 2, 2));
    Model farModel = LoadModelFromMesh(GenMeshCube(2, 2, 2));
    farModel.transform = MatrixTranslate(0, 0, 3);
    Ray ray = {{0, 0, 5}, {0, 0, -1}};

    CoreCollision3D world = {0};
    Check(CoreCollision3DAdd(&world, &nearModel, 1, 11) && CoreCollision3DAdd(&world, &farModel, 2, 22),
          "two models register with distinct layers and tags");
    Check(!CoreCollision3DAdd(&world, NULL, 1, 0), "adding a NULL model is rejected");

    CoreRayHit hit = CoreCollision3DRay(&world, ray, 100.0f, 1 | 2);
    Check(hit.hit && fabsf(hit.distance - 1.0f) < 1e-4f && Vector3Distance(hit.point, (Vector3){0, 0, 4}) < 1e-4f,
          "the closer of two entries wins, at the near face of the translated cube");
    Check(Vector3Distance(hit.normal, (Vector3){0, 0, 1}) < 1e-4f && Vector3DotProduct(hit.normal, ray.direction) < 0,
          "the hit normal is unit length and faces back against the ray");
    Check(hit.tag == 22, "the hit reports the winning entry's tag");

    Check(!CoreCollision3DRay(&world, ray, 100.0f, 4).hit, "a mask that meets no entry's layers hits nothing");
    CoreRayHit onlyNear = CoreCollision3DRay(&world, ray, 100.0f, 1);
    Check(onlyNear.hit && onlyNear.tag == 11 && fabsf(onlyNear.distance - 4.0f) < 1e-4f,
          "excluding the closer entry's layer from the mask lets the farther one be found instead");
    Check(!CoreCollision3DRay(&world, ray, 0.5f, 1 | 2).hit, "maxDistance shorter than every hit finds nothing");
    Check(!CoreCollision3DRay(&world, ray, 2.0f, 1).hit,
          "maxDistance between the two distances cuts off an entry that would otherwise be hit");

    farModel.transform = MatrixTranslate(0, 0, -3);
    CoreRayHit moved = CoreCollision3DRay(&world, ray, 100.0f, 2);
    Check(moved.hit && fabsf(moved.distance - 7.0f) < 1e-4f && Vector3Distance(moved.point, (Vector3){0, 0, -2}) < 1e-4f,
          "changing a model's transform in place moves where its entry hits");

    CoreCollision3D full = {0};
    Model fill = LoadModelFromMesh(GenMeshCube(1, 1, 1));
    bool filled = true;
    for (int i = 0; i < CORE_COLLISION3D_MAX; i++)
        filled = filled && CoreCollision3DAdd(&full, &fill, 1, i);
    Check(filled, "the set accepts exactly its capacity");
    Check(!CoreCollision3DAdd(&full, &fill, 1, 99), "adding past capacity is rejected");

    CoreCollision3DClear(&world);
    Check(!CoreCollision3DRay(&world, ray, 100.0f, 1 | 2).hit, "a cleared set has nothing left to hit");

    UnloadModel(nearModel);
    UnloadModel(farModel);
    UnloadModel(fill);
}

/* What a test game records from its session callbacks. */
typedef struct SessionTestGame
{
    uint32_t seed, seedRead;
    int joined[CORE_NET_SESSION_ACTORS], left[CORE_NET_SESSION_ACTORS];
    int commands, lastCommandActor, lastCommandOp;
    int32_t lastCommandValue;
    int events, lastEventOp;
    int32_t lastEventValue;
    bool rejectUploads;
    NetTestState world, player, seen;
    int appeared, vanished; // joiner only: how many times, and the identity of the last one
    uint32_t lastAppearedId, lastVanishedId;
} SessionTestGame;

static const CoreNetField sessionTestFields[] = {
    CORE_NET_FIELD(NetTestState, position, CORE_NET_VECTOR3),
    CORE_NET_FIELD(NetTestState, health, CORE_NET_I32),
};
static const CoreNetSchema sessionWorldSchema = {40, "session-world", sizeof(NetTestState),
                                                 sessionTestFields, 2, CORE_NET_AUTHORITY_SERVER, 0};
static const CoreNetSchema sessionPlayerSchema = {41, "session-player", sizeof(NetTestState),
                                                  sessionTestFields, 2, CORE_NET_AUTHORITY_OWNER, 0};

static bool SessionTestRegister(void *user, CoreNetSync *sync)
{
    (void)user;
    return CoreNetSyncRegister(sync, &sessionWorldSchema) &&
           CoreNetSyncRegister(sync, &sessionPlayerSchema);
}

static void SessionTestStarted(void *user, CoreNetSync *sync)
{
    SessionTestGame *game = user;
    CoreNetSyncSpawn(sync, 100, 40, CORE_NET_SERVER_ACTOR);
    CoreNetSyncBindState(sync, 100, &game->world);
}

static void SessionTestWriteWelcome(void *user, CoreNetWriter *writer)
{
    CoreNetWriteU32(writer, ((SessionTestGame *)user)->seed);
}

static bool SessionTestReadWelcome(void *user, CoreNetReader *reader)
{
    return CoreNetReadU32(reader, &((SessionTestGame *)user)->seedRead);
}

static void SessionTestJoined(void *user, CoreNetSync *sync, uint16_t actor)
{
    SessionTestGame *game = user;
    game->joined[actor]++;
    CoreNetSyncSpawn(sync, actor, 41, actor);
}

static void SessionTestLeft(void *user, uint16_t actor)
{
    ((SessionTestGame *)user)->left[actor]++;
}

static bool SessionTestAccept(void *user, const CoreNetObject *object)
{
    (void)object;
    return !((SessionTestGame *)user)->rejectUploads;
}

static void SessionTestCommand(void *user, uint16_t actor, const CoreNetCommand *command,
                               CoreNetReader *reader)
{
    SessionTestGame *game = user;
    uint32_t value = 0;
    CoreNetReadU32(reader, &value);
    game->commands++;
    game->lastCommandActor = actor;
    game->lastCommandOp = command->op;
    game->lastCommandValue = (int32_t)value;
}

static void SessionTestEvent(void *user, uint8_t op, uint32_t object, CoreNetReader *reader)
{
    SessionTestGame *game = user;
    uint32_t value = 0;
    (void)object;
    CoreNetReadU32(reader, &value);
    game->events++;
    game->lastEventOp = op;
    game->lastEventValue = (int32_t)value;
}

static void SessionTestAppeared(void *user, CoreNetSync *sync, CoreNetObject *object)
{
    (void)sync;
    SessionTestGame *game = user;
    game->appeared++;
    game->lastAppearedId = object->id;
}

static void SessionTestVanished(void *user, uint32_t id)
{
    SessionTestGame *game = user;
    game->vanished++;
    game->lastVanishedId = id;
}

static CoreNetSessionConfig SessionTestConfig(SessionTestGame *game, const char *name, uint32_t version)
{
    return (CoreNetSessionConfig){
        .game = name, .version = version, .objectCapacity = 32, .schemaCapacity = 4,
        .user = game, .registerSchemas = SessionTestRegister, .started = SessionTestStarted,
        .writeWelcome = SessionTestWriteWelcome, .readWelcome = SessionTestReadWelcome,
        .joined = SessionTestJoined, .left = SessionTestLeft, .accept = SessionTestAccept,
        .command = SessionTestCommand, .event = SessionTestEvent,
        .appeared = SessionTestAppeared, .vanished = SessionTestVanished,
    };
}

/* Steps both ends until done() says so or about two seconds pass. */
static bool SessionPump(CoreNetSession *host, CoreNetSession *client,
                        bool (*done)(const CoreNetSession *, const CoreNetSession *))
{
    for (int i = 0; i < 400; i++)
    {
        CoreNetSessionStep(host, 1.0 / 60.0, 2);
        CoreNetSessionStep(client, 1.0 / 60.0, 2);
        if (done(host, client))
            return true;
    }
    return false;
}

static bool SessionClientSettled(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    return client->status == CORE_NET_SESSION_WELCOMED || client->status == CORE_NET_SESSION_FAILED;
}

static bool SessionClientFailed(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    return client->status == CORE_NET_SESSION_FAILED;
}

static SessionTestGame *sessionHostGame, *sessionClientGame;

static bool SessionWorldArrived(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    const CoreNetObject *world = CoreNetSyncFind((CoreNetSync *)&client->sync, 100);
    return world && ((const NetTestState *)world->state)->health == 77;
}

static bool SessionUploadArrived(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)client;
    const CoreNetObject *player = CoreNetSyncFind((CoreNetSync *)&host->sync, 2);
    return player && ((const NetTestState *)player->state)->health == 55;
}

static bool SessionCommandHandled(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    return CoreNetSessionCommandDone(client, client->commandSequence);
}

static bool SessionEventHeard(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    (void)client;
    return sessionClientGame->events > 0;
}

static bool SessionClientGone(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)client;
    return sessionHostGame->left[2] > 0 && !CoreNetSyncFind((CoreNetSync *)&host->sync, 2);
}

static bool SessionAllAppeared(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    (void)client;
    return sessionClientGame->appeared >= 3;
}

static bool SessionWorldVanished(const CoreNetSession *host, const CoreNetSession *client)
{
    (void)host;
    (void)client;
    return sessionClientGame->vanished > 0;
}

static void NetSessionChecks(void)
{
    SessionTestGame hostGame = {.seed = 12345}, clientGame = {0};
    sessionHostGame = &hostGame;
    sessionClientGame = &clientGame;
    CoreNetSessionConfig hostConfig = SessionTestConfig(&hostGame, "session-test", 3);
    CoreNetSessionConfig clientConfig = SessionTestConfig(&clientGame, "session-test", 3);
    CoreNetSession host = {0}, client = {0};

    Check(CoreNetSessionHost(&host, &hostConfig, 0, true) && host.status == CORE_NET_SESSION_ACTIVE &&
              host.isServer && host.localActor == 1,
          "hosting opens a server whose own player is actor 1");
    Check(hostGame.joined[1] == 1 && CoreNetSyncFind(&host.sync, 1) && CoreNetSyncFind(&host.sync, 100),
          "the host's own player joins at once, and the server's objects are spawned");
    Check(CoreNetSessionActorActive(&host, 1) && !CoreNetSessionActorActive(&host, 2),
          "the host knows its own player is in the game and nobody else yet");

    /* The host's command is handled in the call, by the same handler, with no socket involved. */
    unsigned char payload[4];
    CoreNetWriter w = CoreNetWriterBegin(payload, sizeof payload);
    CoreNetWriteU32(&w, 9);
    uint32_t hostCommand = CoreNetSessionCommand(&host, 7, 100, payload, w.size);
    Check(hostCommand && hostGame.commands == 1 && hostGame.lastCommandActor == 1 &&
              hostGame.lastCommandOp == 7 && hostGame.lastCommandValue == 9 &&
              CoreNetSessionCommandDone(&host, hostCommand),
          "a host's command runs immediately through the server's handler as actor 1");

    uint16_t port = CoreNetPort(&host.endpoint);
    Check(CoreNetSessionJoin(&client, &clientConfig, "127.0.0.1", port) &&
              client.status == CORE_NET_SESSION_CONNECTING,
          "joining puts an attempt out");
    Check(SessionPump(&host, &client, SessionClientSettled) &&
              client.status == CORE_NET_SESSION_WELCOMED && client.localActor == 2 &&
              clientGame.seedRead == 12345,
          "the server welcomes a matching joiner as actor 2 with the world's seed");
    Check(hostGame.joined[2] == 0, "a welcomed joiner is not in the game until it says it is ready");
    Check(CoreNetSessionReady(&client) && client.status == CORE_NET_SESSION_ACTIVE,
          "a welcomed joiner becomes active once ready");

    hostGame.world.health = 77;
    Check(SessionPump(&host, &client, SessionWorldArrived), "a server object reaches the joiner");
    Check(hostGame.joined[2] == 1 && CoreNetSyncFind(&client.sync, 2),
          "the server's joined callback spawned the joiner's player, and the joiner has it");

    CoreNetObject *mine = CoreNetSyncFind(&client.sync, 2);
    Check(mine && CoreNetSessionIsMine(&client, mine) &&
              !CoreNetSessionIsMine(&client, CoreNetSyncFind(&client.sync, 100)),
          "the joiner owns its player and not the server's world");
    if (mine)
        CoreNetSyncBindState(&client.sync, 2, &clientGame.player);
    clientGame.player.health = 55;
    Check(SessionUploadArrived(&host, &client) || SessionPump(&host, &client, SessionUploadArrived),
          "the joiner's own player state reaches the server");
    hostGame.rejectUploads = true;
    clientGame.player.health = 66;
    for (int i = 0; i < 30; i++)
    {
        CoreNetSessionStep(&host, 1.0 / 60.0, 2);
        CoreNetSessionStep(&client, 1.0 / 60.0, 2);
    }
    const CoreNetObject *hostCopy = CoreNetSyncFind(&host.sync, 2);
    Check(hostCopy && ((const NetTestState *)hostCopy->state)->health == 55,
          "an upload the game does not accept leaves the server's copy as it was");
    hostGame.rejectUploads = false;

    w = CoreNetWriterBegin(payload, sizeof payload);
    CoreNetWriteU32(&w, 21);
    uint32_t sent = CoreNetSessionCommand(&client, 8, 100, payload, w.size);
    Check(sent && !CoreNetSessionCommandDone(&client, sent),
          "a joiner's command is not done before the server has answered");
    Check(SessionPump(&host, &client, SessionCommandHandled) && hostGame.commands == 2 &&
              hostGame.lastCommandActor == 2 && hostGame.lastCommandOp == 8 &&
              hostGame.lastCommandValue == 21,
          "a joiner's command runs on the server as actor 2 and the next snapshot says so");

    w = CoreNetWriterBegin(payload, sizeof payload);
    CoreNetWriteU32(&w, 5);
    Check(CoreNetSessionEvent(&host, 2, 3, 0, payload, w.size) && hostGame.events == 0,
          "an event for actor 2 alone does not reach the host's own player");
    Check(SessionPump(&host, &client, SessionEventHeard) && clientGame.lastEventOp == 3 &&
              clientGame.lastEventValue == 5,
          "an event for actor 2 reaches that joiner");
    Check(CoreNetSessionEvent(&host, CORE_NET_EVERYONE, 4, 0, payload, w.size) &&
              hostGame.events == 1 && hostGame.lastEventOp == 4,
          "an event for everyone reaches the host's own player at once");
    Check(!CoreNetSessionEvent(&client, CORE_NET_EVERYONE, 4, 0, NULL, 0),
          "only the server sends events");

    CoreNetSessionLeave(&client);
    Check(client.status == CORE_NET_SESSION_OFF, "leaving turns the session off");
    CoreNetSession idle = {0};
    Check(SessionPump(&host, &idle, SessionClientGone),
          "the server hears a leaver go, calls left, and removes what it owned");

    /* A different version or game is refused at the door, with the reason. */
    SessionTestGame otherGame = {0};
    CoreNetSessionConfig oldVersion = SessionTestConfig(&otherGame, "session-test", 2);
    Check(CoreNetSessionJoin(&client, &oldVersion, "127.0.0.1", port) &&
              SessionPump(&host, &client, SessionClientFailed) &&
              client.refusal == CORE_NET_REFUSED_VERSION,
          "a joiner at another version is refused as such");
    CoreNetSessionLeave(&client);
    CoreNetSessionConfig otherName = SessionTestConfig(&otherGame, "another-game", 3);
    Check(CoreNetSessionJoin(&client, &otherName, "127.0.0.1", port) &&
              SessionPump(&host, &client, SessionClientFailed) &&
              client.refusal == CORE_NET_REFUSED_GAME,
          "a joiner running another game is refused as such");
    Check(otherGame.seedRead == 0 && hostGame.joined[2] == 1 && hostGame.joined[3] == 0,
          "a refused joiner is never welcomed or joined");
    CoreNetSessionLeave(&client);

    Check(CoreNetSessionCommand(&client, 1, 0, NULL, 0) == 0, "an idle session sends no command");
    Check(!CoreNetSessionJoin(&client, &clientConfig, "127.0.0.1", 0) &&
              client.status == CORE_NET_SESSION_FAILED,
          "joining port zero fails at once");
    CoreNetSessionLeave(&client);
    CoreNetSessionLeave(&host);
    Check(host.status == CORE_NET_SESSION_OFF, "the host leaves cleanly");
}

// Keeps every particle whose kind is not 99, so the removed one exercises the same swap-with-last
// path as expiry.
static bool ParticlesStepKeepExceptKind99(CoreParticle *p, Vector3 previous, float dt, void *user)
{
    (void)previous;
    (void)dt;
    (*(int *)user)++;
    return p->kind != 99;
}

static void ParticlesChecks(void)
{
    CoreParticles pool;
    Check(CoreParticlesInit(&pool, 3), "a particle pool allocates its storage");
    CoreParticle base = {0};
    base.life = 10.0f;
    bool filled = true;
    for (int i = 0; i < 3; i++)
        filled = filled && CoreParticleEmit(&pool, &base) != NULL;
    Check(filled && CoreParticleEmit(&pool, &base) == NULL && pool.count == 3,
          "emitting up to capacity succeeds, and one more returns NULL and leaves the pool as it was");
    CoreParticlesFree(&pool);

    Check(CoreParticlesInit(&pool, 4), "a second particle pool allocates for the expiry check");
    CoreParticle soon = {0}, later = {0};
    soon.life = 0.5f;
    later.life = 5.0f;
    CoreParticleEmit(&pool, &soon);
    CoreParticleEmit(&pool, &later);
    CoreParticlesUpdate(&pool, 0.6f, NULL, NULL);
    Check(pool.count == 1 && pool.items[0].life == 5.0f,
          "an expired particle is removed, swapping the last live particle into its place");
    CoreParticlesFree(&pool);

    Check(CoreParticlesInit(&pool, 1), "a third particle pool allocates for the gravity/drag check");
    CoreParticle falling = {0};
    falling.life = 100.0f;
    falling.gravity = 10.0f;
    falling.drag = 0.5f;
    CoreParticleEmit(&pool, &falling);
    for (int i = 0; i < 3; i++)
        CoreParticlesUpdate(&pool, 0.1f, NULL, NULL);
    // vel.y -= grav*dt; vel *= 1/(1+drag*dt); pos.y += vel.y*dt, three times over: worked out in
    // float32 ahead of time so this check catches a changed formula, not just a changed rounding.
    Check(fabsf(pool.items[0].velocity.y - (-2.7232485f)) < 0.001f &&
              fabsf(pool.items[0].position.y - (-0.55350405f)) < 0.001f,
          "gravity and drag integrate exactly as specified over a few fixed steps");
    CoreParticlesFree(&pool);

    Check(CoreParticlesInit(&pool, 3), "a fourth particle pool allocates for the step-hook check");
    CoreParticle keep = {0}, drop = {0};
    keep.life = 10.0f;
    keep.kind = 1;
    drop.life = 10.0f;
    drop.kind = 99;
    CoreParticleEmit(&pool, &keep);
    CoreParticleEmit(&pool, &drop);
    CoreParticleEmit(&pool, &keep);
    int stepCalls = 0;
    CoreParticlesUpdate(&pool, 0.01f, ParticlesStepKeepExceptKind99, &stepCalls);
    bool anyDropped = false;
    for (int i = 0; i < pool.count; i++)
        anyDropped = anyDropped || pool.items[i].kind == 99;
    Check(stepCalls == 3 && pool.count == 2 && !anyDropped,
          "the step hook's false removes a particle the same way expiry does");
    CoreParticlesFree(&pool);

    // The Scheme type: made by name, its template set, emitted from, counted and cleared.
    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){16, 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptS7Open(&host), "the Scheme frontend opens for the particles check");
    Check(ScriptS7Eval("(define pp (make 'particles))", NULL) &&
              ScriptS7Eval("(set! (pp 'life) 2.0)", NULL) && EvalIs("(pp 'count)", "0") &&
              EvalIs("(pp 'emit! (list 0 0 0) (list 0 1 0))", "#t") && EvalIs("(pp 'count)", "1") &&
              EvalIs("(properties pp)", "(count life size-start size-end gravity drag)"),
          "a particle pool is made by name, takes a template property, emits, and counts what lives");
    Check(ScriptS7Eval("(pp 'clear!)", NULL) && EvalIs("(pp 'count)", "0"),
          "clear! empties a particle pool immediately");
    ScriptS7Close();
    ScriptHostFree(&host);
    GameplayWorldFree(&world);
}

static void WaypointsChecks(void)
{
    CoreWaypoints w = {0};

    /* Distance decides the route, not hop count. a-b-c is the fewest-hops way from a to c (2 hops)
       but b is a long way out to the side; a-d-e-c threads the short leg in three hops and is far
       shorter overall -- the route CoreWaypointsNextHop/Path must actually take. */
    int a = CoreWaypointsAdd(&w, (Vector3){0, 0, 0});
    int b = CoreWaypointsAdd(&w, (Vector3){0, 50, 1.5f});
    int c = CoreWaypointsAdd(&w, (Vector3){0, 0, 3});
    int d = CoreWaypointsAdd(&w, (Vector3){0, 0, 1});
    int e = CoreWaypointsAdd(&w, (Vector3){0, 0, 2});
    Check(a == 0 && b == 1 && c == 2 && d == 3 && e == 4, "points are added in order, indexed from zero");
    Check(CoreWaypointsLink(&w, a, b) && CoreWaypointsLink(&w, b, c) && CoreWaypointsLink(&w, a, d) &&
              CoreWaypointsLink(&w, d, e) && CoreWaypointsLink(&w, e, c),
          "points link both ways");
    Check(CoreWaypointsLink(&w, a, b), "linking an already-linked pair again still answers true");

    Check(CoreWaypointsNextHop(&w, a, c) == d,
          "the shortest route by distance is taken even though it costs more hops than the way via b");
    int path[8];
    int n = CoreWaypointsPath(&w, a, c, path, 8);
    Check(n == 4 && path[0] == a && path[1] == d && path[2] == e && path[3] == c,
          "the path follows that same distance-shortest route, from..to inclusive");
    Check(CoreWaypointsNextHop(&w, a, a) == a, "from == to answers from without searching");

    int far = CoreWaypointsAdd(&w, (Vector3){100, 100, 100}); /* linked to nothing */
    Check(CoreWaypointsNextHop(&w, a, far) == -1, "an unreachable point answers -1");
    Check(CoreWaypointsPath(&w, a, far, path, 8) == 0, "a path to an unreachable point writes nothing");

    Check(CoreWaypointsNextHop(&w, a, 99) == -1 && CoreWaypointsNextHop(&w, -1, a) == -1,
          "a bad index answers -1, not a wild read");
    Check(!CoreWaypointsLink(&w, a, 99) && !CoreWaypointsLink(&w, -1, a),
          "linking a bad index answers false");
    Check(!CoreWaypointsLink(&w, a, a), "a point cannot link to itself");
    Check(CoreWaypointsPath(&w, a, 99, path, 8) == 0 && CoreWaypointsPath(&w, a, c, path, 0) == 0,
          "a bad index or too small a capacity writes nothing");

    Check(CoreWaypointsNearest(&w, (Vector3){0, 0, 0.9f}) == d,
          "the nearest point is found by straight-line distance");
    CoreWaypoints empty = {0};
    Check(CoreWaypointsNearest(&empty, (Vector3){0, 0, 0}) == -1, "an empty graph has no nearest point");

    CoreWaypoints full = {0};
    int last = -1;
    for (int i = 0; i < CORE_WAYPOINTS_MAX; i++)
        last = CoreWaypointsAdd(&full, (Vector3){(float)i, 0, 0});
    Check(last == CORE_WAYPOINTS_MAX - 1, "the graph fills to its capacity");
    Check(CoreWaypointsAdd(&full, (Vector3){0, 0, 0}) == -1, "a full graph refuses another point");

    CoreWaypoints hub = {0};
    int center = CoreWaypointsAdd(&hub, (Vector3){0, 0, 0});
    int spokes[CORE_WAYPOINT_LINKS + 1];
    for (int i = 0; i < CORE_WAYPOINT_LINKS + 1; i++)
        spokes[i] = CoreWaypointsAdd(&hub, (Vector3){(float)(i + 1), 0, 0});
    bool allLinked = true;
    for (int i = 0; i < CORE_WAYPOINT_LINKS; i++)
        allLinked = allLinked && CoreWaypointsLink(&hub, center, spokes[i]);
    Check(allLinked, "a point links up to its capacity");
    Check(!CoreWaypointsLink(&hub, center, spokes[CORE_WAYPOINT_LINKS]),
          "a full link list refuses one more");
}

// appeared/vanished: a joiner is told about every object a snapshot adds or removes, once each,
// and the server -- which creates its own objects -- is never told at all.
static void NetAppearedVanishedChecks(void)
{
    SessionTestGame hostGame = {.seed = 9}, clientGame = {0};
    sessionHostGame = &hostGame;
    sessionClientGame = &clientGame;
    CoreNetSessionConfig hostConfig = SessionTestConfig(&hostGame, "appear-test", 1);
    CoreNetSessionConfig clientConfig = SessionTestConfig(&clientGame, "appear-test", 1);
    CoreNetSession host = {0}, client = {0};

    Check(CoreNetSessionHost(&host, &hostConfig, 0, true), "hosting starts, with the world object spawned");
    uint16_t port = CoreNetPort(&host.endpoint);
    Check(CoreNetSessionJoin(&client, &clientConfig, "127.0.0.1", port), "joining starts");
    Check(SessionPump(&host, &client, SessionClientSettled) && client.status == CORE_NET_SESSION_WELCOMED,
          "the joiner is welcomed");
    Check(CoreNetSessionReady(&client), "the joiner says it is ready");

    // Three objects are new to this joiner: the world (started), the host's own player (host!'s
    // player of its own, actor 1) and this joiner's own player (joined, once the server sees
    // MSG_READY). appeared must fire once for each.
    Check(SessionPump(&host, &client, SessionAllAppeared) && clientGame.appeared == 3,
          "appeared fires exactly once for each of the three objects the joiner did not have before");
    Check(hostGame.appeared == 0 && hostGame.vanished == 0,
          "the server is never told appeared or vanished; it creates its own objects");

    int appearedSoFar = clientGame.appeared;
    for (int i = 0; i < 30; i++)
    {
        CoreNetSessionStep(&host, 1.0 / 60.0, 2);
        CoreNetSessionStep(&client, 1.0 / 60.0, 2);
    }
    Check(clientGame.appeared == appearedSoFar,
          "further snapshots of the same objects do not announce them again");

    CoreNetSyncDespawn(&host.sync, 100); // the world object goes away
    Check(SessionPump(&host, &client, SessionWorldVanished) && clientGame.vanished == 1 &&
              clientGame.lastVanishedId == 100 && !CoreNetSyncFind(&client.sync, 100),
          "vanished fires once when a snapshot removes an object, which is then gone from the registry");
    Check(CoreNetSyncFind(&client.sync, 2) != NULL,
          "an object the snapshot did not remove is untouched by another one vanishing");

    CoreNetSessionLeave(&client);
    CoreNetSessionLeave(&host);
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
    NetworkBadSharedTypeChecks();
    NetObjectsChecks();
    ElapsedChecks();
    StorageChecks();
    SaveAndInputMapChecks();
    GameCallChecks();
    PlaybackChecks();
    SpriteSheetChecks();
    SpritePresentationChecks();
    RowNameChecks();
    SheetCacheChecks();
    IsoGridChecks();
    IsoMoveChecks();
    NetworkChecks();
    NetCommandChecks();
    NetPackedObjectChecks();
    NetClockChecks();
    NetDeltaChecks();
    NetSyncChecks();
    NetFieldsFromTypeChecks();
    NetOwnershipChecks();
    Collision3DChecks();
    NetSessionChecks();
    ParticlesChecks();
    WaypointsChecks();
    NetAppearedVanishedChecks();
    ObjectChecks();
    GameObjectChecks();
    NodeChecks();
    AudioChecks();
    FpsControllerChecks();
    RigFileChecks();
    failures += HeadlessChecks();
    failures += DrawPathChecks();
    UnloadRenderTexture(scratch);
    UiFree(&ui);
    CloseWindow();
    failures += StoreChecks();
    failures += World3DChecks();
    failures += GameChecks();
    failures += GameRunnerChecks();
    failures += NetChecks();
    printf("REGRESSION TEST failures=%d checks=%d\n", failures, checks);
    return failures ? 1 : 0;
}
