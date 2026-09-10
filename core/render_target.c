/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "render_target.h"
#include "rlgl.h"
#include <GL/gl.h>
#include <stddef.h>
void CoreUnloadRT(RenderTexture *rt)
{
    if (rt->id)
        UnloadRenderTexture(*rt);
    *rt = (RenderTexture){0};
}
bool MakeRT(RenderTexture *out, int w, int h, int format, bool depth)
{
    *out = (RenderTexture){0};
    if (w <= 0 || h <= 0 || (!format && !depth))
        return false;
    unsigned int previous = rlGetActiveFramebuffer();
    RenderTexture t = {0};
    t.id = rlLoadFramebuffer();
    if (!t.id)
        return false;
    rlEnableFramebuffer(t.id);
    t.texture = (Texture){0, w, h, 1, format};
    if (format)
    {
        t.texture.id = rlLoadTexture(NULL, w, h, format, 1);
        rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    }
    else
    {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    if (depth)
    {
        t.depth = (Texture){rlLoadTextureDepth(w, h, false), w, h, 1, 19};
        rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    }
    bool ok = (!format || t.texture.id) && (!depth || (t.depth.id && glIsTexture(t.depth.id))) &&
              rlFramebufferComplete(t.id);
    rlEnableFramebuffer(previous);
    if (!ok)
    {
        TraceLog(LOG_ERROR, "Required framebuffer failed: %dx%d colour=%d sampleable-depth=%d", w, h, format,
                 depth);
        CoreUnloadRT(&t);
        return false;
    }
    *out = t;
    return true;
}

void CoreDepthRange(float *n, float *f)
{
    *n = (float)rlGetCullDistanceNear();
    *f = (float)rlGetCullDistanceFar();
}
