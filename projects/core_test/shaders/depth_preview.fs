#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
varying vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float nearPlane;
uniform float farPlane;
void main() {
    float d=texture2D(texture0,fragTexCoord).r;
    float z=d*2.0-1.0;
    float linear=(2.0*nearPlane*farPlane)/(farPlane+nearPlane-z*(farPlane-nearPlane));
    gl_FragColor=vec4(vec3(1.0/(1.0+linear*0.12)),1.0);
}
