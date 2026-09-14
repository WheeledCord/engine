#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The same surface interface as skinning.vs, for meshes that are not skinned: a static prop has no
// bone weights, and skinning one would collapse every vertex onto the origin. Pairs with the same
// fragment shaders, which only ever asked for a normal and never cared where it came from.
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
attribute vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matNormal;
varying vec2 fragTexCoord;
varying vec4 fragColor;
varying vec3 fragNormal;
void main()
{
    fragTexCoord=vertexTexCoord;
    fragColor=vertexColor;
    fragNormal=normalize(mat3(matNormal)*vertexNormal);
    gl_Position=mvp*vec4(vertexPosition,1.0);
}
