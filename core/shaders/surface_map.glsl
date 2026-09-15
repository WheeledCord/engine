/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// A surface map is one texture carrying both what a surface looks like and how it sits: RGB is the
// colour, alpha is height. That pairing is what lets a baked material keep its relief -- colour
// alone bakes flat, and a separate normal map costs a second texture and a second fetch.
//
// The normal comes from the height rather than being stored: the slope of the height field across
// the surface is the tilt, and the frame to tilt in comes from how the texture is laid over the
// geometry, which the derivatives of the position and the UV already describe. No tangent attribute
// is needed, so a mesh that was never built for normal mapping still takes one.
//
// Include this and call CoreSurfaceNormal. Requires GLSL 120 derivatives, which are core.

float CoreSurfaceHeight(sampler2D surfaceMap, vec2 uv) { return texture2D(surfaceMap, uv).a; }

vec3 CoreSurfaceNormal(sampler2D surfaceMap, vec2 uv, vec3 position, vec3 normal, float strength)
{
    // Slope of the height field in screen space, then carried onto the surface through the frame
    // the UVs impose on it. dFdx/dFdy of a varying is the per-pixel step, so the two divide out and
    // the result does not depend on how close the camera is.
    float height = CoreSurfaceHeight(surfaceMap, uv);
    float dHdx = CoreSurfaceHeight(surfaceMap, uv + dFdx(uv)) - height;
    float dHdy = CoreSurfaceHeight(surfaceMap, uv + dFdy(uv)) - height;
    vec3 dPdx = dFdx(position), dPdy = dFdy(position);
    vec3 n = normalize(normal);
    // Gradient of the height along the surface, with the component along the normal removed so the
    // perturbation only tilts the normal and never flips it.
    vec3 gradient = dHdx * cross(dPdy, n) + dHdy * cross(n, dPdx);
    float area = dot(dPdx, cross(dPdy, n));
    if (abs(area) < 1e-12) return n;
    gradient /= area;
    return normalize(n - strength * (gradient - dot(gradient, n) * n));
}
