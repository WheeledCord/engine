#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The project runner's world pass (gameplay/game.c): lit.vs plus the world-space position the
// fragment shader measures fog by.
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
varying vec2 fragTexCoord;
varying vec3 fragNormal;
varying vec3 fragPosition;
void main()
{
    fragTexCoord=vertexTexCoord;
    fragNormal=normalize(mat3(matNormal)*vertexNormal);
    fragPosition=vec3(matModel*vec4(vertexPosition,1.0));
    gl_Position=mvp*vec4(vertexPosition,1.0);
}
