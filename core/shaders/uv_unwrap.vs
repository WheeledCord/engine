#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Texture baking: the mesh is drawn flat into its own UV layout rather than into the world, so each
// texel of the target lands on the surface point that owns it. The UV becomes the clip position,
// and the surface the material shader wants to look at -- object position and normal -- is handed
// across as varyings instead. A material written against object space therefore evaluates exactly
// as it does when drawn normally; only where the result is written changes.
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
attribute vec4 vertexColor;
uniform mat4 matModel;
varying vec2 fragTexCoord;
varying vec4 fragColor;
varying vec3 fragNormal;
varying vec3 fragPosition;      // object space
varying vec3 fragWorldPosition; // matModel applied, for materials that use it
void main()
{
    fragTexCoord=vertexTexCoord;
    fragColor=vertexColor;
    fragNormal=vertexNormal;                        // object space: unrotated, as baking needs
    fragPosition=vertexPosition;
    fragWorldPosition=(matModel*vec4(vertexPosition,1.0)).xyz;
    // UV 0..1 -> clip -1..1. No projection: the "camera" is the texture itself.
    gl_Position=vec4(vertexTexCoord*2.0-1.0,0.0,1.0);
}
