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
    snprintf(path, sizeof path, "%s", Scratch("regression_malformed.ui"));
    FILE *f = fopen(path, "w");
    fprintf(f, "core_ui_document 6\nsurface 200 100 0\t\n"
               "0 10 10 80 24 0 0 7 0 0 0 0 0 1 1 -1 -\tGood\n"
               "this line is not an element at all\n");
    fclose(f);
    UiDocument d = {0};
    Check(!UiDocumentLoad(&d, path) && d.count == 0,
          "a layout with a line that cannot be read refuses to load");
    UiDocumentFree(&d);

    GameplayWorld world = {0};
    GameplayWorldInit(&world, (GameplayWorldConfig){8, 16, 0.1});
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
    GameplayWorldInit(&world, (GameplayWorldConfig){16, ScriptEntitySize(), 0.1});
    ScriptHost host;
    ScriptHostInit(&host, &world);
    Check(ScriptS7Open(&host), "the Scheme frontend registers the binding table");
    bool declared = ScriptS7Eval(
        "(begin"
        " (define (t-spawn) (think-next))"
        " (define (t-think) (move-world! (vec (* (get \"speed\") (dt)) 0)) (think-next))"
        " (define-entity \"tester\" '((\"speed\" \"float\"))"
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
    ConventionChecks();
    DeferredChecks();
    ScriptChecks();
    UnloadRenderTexture(scratch);
    UiFree(&ui);
    CloseWindow();
    printf("REGRESSION TEST failures=%d checks=%d\n", failures, checks);
    return failures ? 1 : 0;
}
