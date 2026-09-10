#include "capabilities.h"
#include "config.h"
#include "raylib.h"
#include "render_target.h"
#include "rlgl.h"
#include "shader.h"
#include <GL/gl.h>
#include <string.h>
static bool Has(const char *s, const char *name)
{
    if (!s)
        return false;
    size_t n = strlen(name);
    const char *p = s;
    while ((p = strstr(p, name)))
    {
        if ((p == s || p[-1] == ' ') && (!p[n] || p[n] == ' '))
            return true;
        p += n;
    }
    return false;
}
bool CoreCheckCapabilities(CoreRequirements r)
{
    TraceLog(LOG_INFO, "GPU: %s / %s / %s / GLSL %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
             glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    /* Mesa's forced GL21 override exposes raylib's cosmetic GL_NUM_EXTENSIONS query error. */
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR)
        TraceLog(LOG_WARNING, "raylib initialization GL error 0x%x (recorded before capability checks)",
                 error);
    if (rlGetVersion() != RL_OPENGL_21)
    {
        TraceLog(LOG_ERROR, "Core requires a raylib GRAPHICS_API_OPENGL_21 build");
        return false;
    }
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    bool fbo = Has(ext, "GL_ARB_framebuffer_object"), dep = Has(ext, "GL_ARB_depth_texture");
    TraceLog(LOG_INFO, "ARB_texture_float=%d ARB_framebuffer_object=%d ARB_depth_texture=%d",
             Has(ext, "GL_ARB_texture_float"), fbo, dep);
    if (r.vertexUniformComponents < CORE_BONE_CAPACITY * 16 + 16)
        r.vertexUniformComponents = CORE_BONE_CAPACITY * 16 + 16;
    if (r.fragmentUniformComponents < 5)
        r.fragmentUniformComponents = 5;
    if (r.textureUnits < 1)
        r.textureUnits = 1;
    if (r.varyingFloats < 6)
        r.varyingFloats = 6;
    const GLenum keys[] = {GL_MAX_VERTEX_UNIFORM_COMPONENTS, GL_MAX_FRAGMENT_UNIFORM_COMPONENTS,
                           GL_MAX_TEXTURE_IMAGE_UNITS, GL_MAX_VARYING_FLOATS, GL_MAX_VERTEX_ATTRIBS};
    const char *names[] = {"GL_MAX_VERTEX_UNIFORM_COMPONENTS", "GL_MAX_FRAGMENT_UNIFORM_COMPONENTS",
                           "GL_MAX_TEXTURE_IMAGE_UNITS", "GL_MAX_VARYING_FLOATS", "GL_MAX_VERTEX_ATTRIBS"};
    int need[] = {r.vertexUniformComponents, r.fragmentUniformComponents, r.textureUnits, r.varyingFloats, 9};
    bool ok = fbo && dep;
    for (int i = 0; i < 5; i++)
    {
        GLint value = 0;
        glGetIntegerv(keys[i], &value);
        TraceLog(LOG_INFO, "%s=%d required=%d", names[i], value, need[i]);
        if (value < need[i])
        {
            TraceLog(LOG_ERROR, "Insufficient %s: required %d, available %d", names[i], need[i], value);
            ok = false;
        }
    }
    if (!fbo || !dep)
        TraceLog(LOG_ERROR,
                 "Required ARB_framebuffer_object / ARB_depth_texture unavailable (FBO=%d depth=%d)", fbo,
                 dep);
    if (!ok)
        return false;
    RenderTexture probe;
    if (!MakeRT(&probe, 16, 16, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, true))
        return false;
    CoreUnloadRT(&probe);
    const ShaderFile requiredShader = {"core/shaders/skinning.vs", "core/shaders/textured.fs"};
    Shader skin = {0};
    if (!CoreLoadShaders(&requiredShader, 1, &skin))
        return false;
    bool gpu = skin.locs[SHADER_LOC_BONE_MATRICES] >= 0 && skin.locs[SHADER_LOC_VERTEX_BONEIDS] >= 0 &&
               skin.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] >= 0;
    CoreUnloadShaders(&skin, 1);
    if (!gpu)
    {
        TraceLog(LOG_ERROR, "Required GPU skinning shader interface unavailable");
        return false;
    }
    error = glGetError();
    if (error)
    {
        TraceLog(LOG_ERROR, "Capability probe GL error: 0x%x", error);
        return false;
    }
    return true;
}

bool CoreCheckGraphicsErrors(const char *where)
{
    bool ok = true;
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
    {
        TraceLog(LOG_ERROR, "GL error at %s: 0x%x", where, e);
        ok = false;
    }
    return ok;
}
