/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Go/no-go condition 6 (docs/developer/store.md section 8): the draw path's CPU cost per frame,
// measured with the design's E6 method. No window: an EGL surfaceless context renders into a
// 640x480 framebuffer object, and each frame ends in glFinish so the driver's work is counted.
// 400 draw items over 12 cube meshes and 3 materials of one shader; 120 are in view, the rest
// behind the camera. The cubes in view are small and far, so almost nothing is rasterised and
// the time is the submit's. Run pinned: LP_NUM_THREADS=1 taskset -c 3 ./build/core/draw_bench
#define _POSIX_C_SOURCE 199309L

#include "core/draw_path.h"
#include "raymath.h"
#include "rlgl.h"
#define EGL_NO_X11
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#define GL_RENDERER_ENUM 0x1F01
#define GL_VERSION_ENUM 0x1F02

enum { WIDTH = 640, HEIGHT = 480, WARMUP = 30, FRAMES = 600, ITEMS = 400, MESHES = 12, MATERIALS = 3 };

typedef void (*FinishFn)(void);
typedef const unsigned char *(*GetStringFn)(unsigned int);
static FinishFn glFinishPtr;

static double Now(clockid_t clock)
{
    struct timespec t;
    clock_gettime(clock, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int CompareDoubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Sorts v and answers the p-th percentile, as E6 does. */
static double Percentile(double *v, int n, double p)
{
    qsort(v, (size_t)n, sizeof *v, CompareDoubles);
    return v[(int)(p * (n - 1) + 0.5)];
}

// ---- EGL, copied from E6's bench.c ------------------------------------------------------------
static bool EglInit(void)
{
    EGLDisplay display = EGL_NO_DISPLAY;
    const char *path = "";
    PFNEGLGETPLATFORMDISPLAYEXTPROC getDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!getDisplay)
        getDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplay");
    if (getDisplay)
        display = getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display != EGL_NO_DISPLAY)
        path = "EGL_PLATFORM_SURFACELESS_MESA";
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
    {
        printf("surfaceless display failed (err 0x%x); trying EGL_PLATFORM_DEVICE_EXT\n", eglGetError());
        PFNEGLQUERYDEVICESEXTPROC queryDevices =
            (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
        EGLDeviceEXT devices[8];
        EGLint count = 0;
        if (!getDisplay || !queryDevices || !queryDevices(8, devices, &count) || count < 1)
        {
            printf("no EGL devices\n");
            return false;
        }
        display = getDisplay(EGL_PLATFORM_DEVICE_EXT, devices[0], NULL);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL))
        {
            printf("device platform failed\n");
            return false;
        }
        path = "EGL_PLATFORM_DEVICE_EXT";
    }
    EGLint major = 0, minor = 0;
    eglInitialize(display, &major, &minor);
    printf("EGL path: %s  EGL %d.%d  vendor=%s\n", path, major, minor, eglQueryString(display, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_API);
    EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    if (!eglChooseConfig(display, attributes, &config, 1, &configs) || configs < 1)
    {
        EGLint fewer[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        if (!eglChooseConfig(display, fewer, &config, 1, &configs) || configs < 1)
        {
            printf("no config\n");
            return false;
        }
    }
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    if (context == EGL_NO_CONTEXT)
    {
        printf("eglCreateContext failed 0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
    {
        printf("eglMakeCurrent surfaceless failed 0x%x\n", eglGetError());
        return false;
    }
    return true;
}

// ---- the scene --------------------------------------------------------------------------------
static DrawPath path;
static DrawItem items[ITEMS];
static Camera3D camera = {{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 60.0f, CAMERA_PERSPECTIVE};

/* The first `visible` items on a grid in view at 30 units, the rest mirrored behind the camera. */
static void Place(const uint32_t *meshes, const uint32_t *materials, int visible)
{
    for (int i = 0; i < ITEMS; i++)
    {
        int column = i % 20, row = i / 20;
        Vector3 at = {(float)column * 1.5f - 14.25f, (float)row * 1.2f - 11.4f, -30.0f};
        if (i >= visible)
            at.z = 30.0f;
        uint32_t mesh = meshes[i % MESHES];
        items[i] = (DrawItem){mesh, materials[i % MATERIALS], MatrixTranslate(at.x, at.y, at.z), at,
                              path.meshes[mesh - 1].radius, DRAW_LAYER_OPAQUE};
    }
}

static void Run(const char *name, int visible, const uint32_t *meshes, const uint32_t *materials)
{
    static double cpu[FRAMES], wall[FRAMES], cull[FRAMES], sort[FRAMES], submit[FRAMES];
    DrawStats stats = {0};
    Place(meshes, materials, visible);
    for (int f = -WARMUP; f < FRAMES; f++)
    {
        double w0 = Now(CLOCK_MONOTONIC), c0 = Now(CLOCK_PROCESS_CPUTIME_ID);
        DrawPathBegin(&path, camera, WIDTH, HEIGHT);
        for (int i = 0; i < ITEMS; i++)
            DrawPathAdd(&path, &items[i]);
        stats = DrawPathEnd(&path);
        glFinishPtr();
        double c1 = Now(CLOCK_PROCESS_CPUTIME_ID), w1 = Now(CLOCK_MONOTONIC);
        if (f < 0)
            continue;
        cpu[f] = c1 - c0;
        wall[f] = w1 - w0;
        cull[f] = stats.cullMicros;
        sort[f] = stats.sortMicros;
        submit[f] = stats.submitMicros;
    }
    double cp50 = Percentile(cpu, FRAMES, 0.5), cp99 = Percentile(cpu, FRAMES, 0.99), cmax = cpu[FRAMES - 1];
    double wp50 = Percentile(wall, FRAMES, 0.5), wp99 = Percentile(wall, FRAMES, 0.99);
    double sp50 = Percentile(submit, FRAMES, 0.5);
    printf("RESULT %s: items=%d visible=%d draws=%d shaderSwitches=%d textureSwitches=%d\n", name,
           stats.items, stats.visible, stats.draws, stats.shaderSwitches, stats.textureSwitches);
    printf("  frame cpu_us p50=%.1f p99=%.1f max=%.1f | wall_us p50=%.1f p99=%.1f\n", cp50, cp99, cmax, wp50, wp99);
    printf("  DrawStats p50/p99 us: cull %.1f/%.1f  sort %.1f/%.1f  submit %.1f/%.1f  (submit %.0f ns per draw)\n",
           Percentile(cull, FRAMES, 0.5), Percentile(cull, FRAMES, 0.99), Percentile(sort, FRAMES, 0.5),
           Percentile(sort, FRAMES, 0.99), sp50, Percentile(submit, FRAMES, 0.99),
           stats.draws ? sp50 * 1000.0 / stats.draws : 0.0);
    fflush(stdout);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    SetTraceLogLevel(LOG_WARNING);
    const char *threads = getenv("LP_NUM_THREADS");
    printf("draw_bench: FBO %dx%d, %d warm-up + %d timed frames, LP_NUM_THREADS=%s\n", WIDTH, HEIGHT,
           WARMUP, FRAMES, threads ? threads : "(unset)");
    if (!EglInit())
        return 1;
    /* rlLoadExtensions takes the loader as a data pointer; copy the function pointer's bits. */
    PFNEGLGETPROCADDRESSPROC getProc = eglGetProcAddress;
    void *loader;
    memcpy(&loader, &getProc, sizeof loader);
    rlLoadExtensions(loader);
    glFinishPtr = (FinishFn)eglGetProcAddress("glFinish");
    GetStringFn getString = (GetStringFn)eglGetProcAddress("glGetString");
    if (!glFinishPtr || !getString)
    {
        printf("glFinish or glGetString not found through eglGetProcAddress\n");
        return 1;
    }
    printf("GL_RENDERER: %s\nGL_VERSION: %s\n", getString(GL_RENDERER_ENUM), getString(GL_VERSION_ENUM));
    rlglInit(WIDTH, HEIGHT);

    unsigned int fbo = rlLoadFramebuffer();
    unsigned int colour = rlLoadTexture(NULL, WIDTH, HEIGHT, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    unsigned int depth = rlLoadTextureDepth(WIDTH, HEIGHT, true);
    rlFramebufferAttach(fbo, colour, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(fbo, depth, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    bool complete = rlFramebufferComplete(fbo);
    printf("FBO %u complete: %s\n", fbo, complete ? "yes" : "no");
    if (!complete)
        return 1;
    rlEnableFramebuffer(fbo);
    rlViewport(0, 0, WIDTH, HEIGHT);
    rlEnableDepthTest();
    /* What BeginMode3D would leave in rlgl; the draw path builds the same matrices itself. */
    double top = rlGetCullDistanceNear() * tan(camera.fovy * 0.5 * DEG2RAD), right = top * WIDTH / HEIGHT;
    rlSetMatrixProjection(MatrixFrustum(-right, right, -top, top, rlGetCullDistanceNear(), rlGetCullDistanceFar()));
    rlSetMatrixModelview(MatrixLookAt(camera.position, camera.target, camera.up));

    DrawPathInit(&path);
    uint32_t meshes[MESHES], materials[MATERIALS];
    for (int k = 0; k < MESHES; k++)
    {
        float size = 0.02f + 0.002f * (float)k; /* about a pixel at 30 units */
        Mesh cube = GenMeshCube(size, size, size);
        meshes[k] = DrawPathMesh(&path, &cube);
        UnloadMesh(cube);
    }
    Shader shader = {rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
    for (int k = 0; k < MATERIALS; k++)
    {
        unsigned char pixel[4] = {(unsigned char)(80 * k), 255, 255, 255};
        Texture2D texture = {rlLoadTexture(pixel, 1, 1, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1), 1, 1, 1,
                             RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        materials[k] = DrawPathMaterial(&path, shader, texture, WHITE, -1);
    }
    if (!meshes[MESHES - 1] || !materials[MATERIALS - 1])
    {
        printf("mesh or material upload failed\n");
        return 1;
    }
    printf("vertex array objects: %s; cube: %d vertices, %d triangles\n", path.meshes[0].vao ? "yes" : "no (per-draw attribute fallback)",
           path.meshes[0].vertexCount, path.meshes[0].triangleCount);

    Run("400 items, 120 in view", 120, meshes, materials);
    Run("400 items, all in view", ITEMS, meshes, materials);
    DrawPathFree(&path);
    return 0;
}
