#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// World-space normal packed into a colour, read back on the CPU to reconstruct the surface
// normal a diffuse pixel belongs to. Pairs with skinning.vs's fragNormal; no lighting model here.
varying vec2 fragTexCoord;
varying vec3 fragNormal;
uniform sampler2D texture0;
void main()
{
    if(texture2D(texture0,fragTexCoord).a<=0.0) discard;
    vec3 n=normalize(fragNormal);
    gl_FragColor=vec4(n*0.5+0.5,1.0);
}
