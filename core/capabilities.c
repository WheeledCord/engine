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

static bool MeetsLimit(GLenum key, const char *name, int need)
{
    if (need <= 0)
        return true;
    GLint value = 0;
    glGetIntegerv(key, &value);
    TraceLog(LOG_INFO, "%s=%d required=%d", name, value, need);
    if (value >= need)
        return true;
    TraceLog(LOG_ERROR, "Insufficient %s: required %d, available %d", name, need, value);
    return false;
}

static bool HasExtension(const char *ext, const char *name, const char *reason)
{
    if (Has(ext, name))
        return true;
    TraceLog(LOG_ERROR, "%s unavailable, required for %s", name, reason);
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
    /* Core's own build contract, not a project's declaration. */
    if (rlGetVersion() != RL_OPENGL_21)
    {
        TraceLog(LOG_ERROR, "Core requires a raylib GRAPHICS_API_OPENGL_21 build");
        return false;
    }

    /* GPU skinning is core's own path, so core knows what it costs to run. */
    if (r.gpuSkinning)
    {
        int bones = CORE_BONE_CAPACITY * 16 + 16;
        if (r.vertexUniformComponents < bones)
            r.vertexUniformComponents = bones;
        if (r.vertexAttributes < 9)
            r.vertexAttributes = 9;
    }

    bool ok = MeetsLimit(GL_MAX_VERTEX_UNIFORM_COMPONENTS, "GL_MAX_VERTEX_UNIFORM_COMPONENTS",
                         r.vertexUniformComponents);
    ok &= MeetsLimit(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, "GL_MAX_FRAGMENT_UNIFORM_COMPONENTS",
                     r.fragmentUniformComponents);
    ok &= MeetsLimit(GL_MAX_TEXTURE_IMAGE_UNITS, "GL_MAX_TEXTURE_IMAGE_UNITS", r.textureUnits);
    ok &= MeetsLimit(GL_MAX_VARYING_FLOATS, "GL_MAX_VARYING_FLOATS", r.varyingFloats);
    ok &= MeetsLimit(GL_MAX_VERTEX_ATTRIBS, "GL_MAX_VERTEX_ATTRIBS", r.vertexAttributes);

    if (r.renderTargets || r.sampleableDepth)
    {
        const char *ext = (const char *)glGetString(GL_EXTENSIONS);
        ok &= HasExtension(ext, "GL_ARB_framebuffer_object", "render targets");
        if (r.sampleableDepth)
            ok &= HasExtension(ext, "GL_ARB_depth_texture", "sampleable depth");
    }
    if (!ok)
        return false;

    if (r.renderTargets || r.sampleableDepth)
    {
        RenderTexture probe;
        if (!MakeRT(&probe, 16, 16, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, r.sampleableDepth))
            return false;
        CoreUnloadRT(&probe);
    }

    if (r.gpuSkinning)
    {
        const ShaderFile requiredShader = {"core/shaders/skinning.vs", "core/shaders/textured.fs"};
        Shader skin = {0};
        if (!CoreLoadShaders(&requiredShader, 1, &skin))
            return false;
        bool gpu = skin.locs[SHADER_LOC_BONE_MATRICES] >= 0 &&
                   skin.locs[SHADER_LOC_VERTEX_BONEIDS] >= 0 &&
                   skin.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] >= 0;
        CoreUnloadShaders(&skin, 1);
        if (!gpu)
        {
            TraceLog(LOG_ERROR, "Required GPU skinning shader interface unavailable");
            return false;
        }
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
