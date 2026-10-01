/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// One check per bug that has been fixed here, so none of them can come back unnoticed. Each one
// asserts on a value read back: a pixel, a rectangle, a return code. Run with no arguments for the
// checks; --present and --net-interrupt run one group on its own.
#define _DEFAULT_SOURCE
#include "checks.h"
#include "core/collision3d.h"
#include "core/engine.h"
#include "core/fps_camera.h"
#include "core/frame_uniforms.h"
#include "core/iso_grid.h"
#include "core/network.h"
#include "core/net_clock.h"
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
#include "core/transform.h"
#include "core/ui.h"
#include "core/ui_document.h"
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

int main(int argc, char **argv)
{
    SetTraceLogLevel(LOG_WARNING);
    if (argc > 1 && !strcmp(argv[1], "--present")) /* only the presentation checks, which are also in the full run */
        return PresentationChecks() ? 1 : 0;
    if (argc > 1 && !strcmp(argv[1], "--net-interrupt")) /* only the Ctrl+C-on-a-client check, also in the full run */
        return NetInterruptChecks() ? 1 : 0;
    /* raylib 5.5 carries on after GLFW fails and crashes in rlglInit, so say why before that happens */
    if (!getenv("DISPLAY") || !*getenv("DISPLAY"))
    {
        fprintf(stderr, "regression_test needs an X display for its hidden window; run it under Xvfb "
                        "(Xvfb :99 & DISPLAY=:99 make smoke)\n");
        return 1;
    }
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
    SaveAndInputMapChecks();
    PlaybackChecks();
    SpriteSheetChecks();
    SpritePresentationChecks();
    IsoGridChecks();
    NetworkChecks();
    NetClockChecks();
    Collision3DChecks();
    ParticlesChecks();
    WaypointsChecks();
    AudioChecks();
    FpsControllerChecks();
    RigFileChecks();
    failures += HeadlessChecks();
    failures += SpaceChecks();
    failures += DrawPathChecks();
    failures += AnimationAssetChecks();
    UnloadRenderTexture(scratch);
    UiFree(&ui);
    CloseWindow();
    failures += StoreChecks();
    failures += World3DChecks();
    failures += GameChecks();
    failures += GameRunnerChecks();
    failures += PresentationChecks();
    failures += NetChecks();
    printf("REGRESSION TEST failures=%d checks=%d\n", failures, checks);
    return failures ? 1 : 0;
}
