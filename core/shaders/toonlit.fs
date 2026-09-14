#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Hard two-band toon lighting: a fragment is either in the light or in the shadow band, no
// gradient between them. Used live and as the diffuse pass of the sprite bake.
varying vec2 fragTexCoord;
varying vec4 fragColor;
varying vec3 fragNormal;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 lightDir;    // world-space, points from the surface toward the light
uniform vec3 lightColor;
uniform vec3 ambientColor;
uniform float toonCutoff; // dot(N,L) at or above this is lit
void main()
{
    vec4 tex=texture2D(texture0,fragTexCoord);
    if(tex.a<=0.0) discard;
    vec3 n=normalize(fragNormal);
    float ndotl=dot(n,normalize(lightDir));
    vec3 band=ndotl>=toonCutoff?lightColor:ambientColor;
    gl_FragColor=vec4(tex.rgb*colDiffuse.rgb*fragColor.rgb*band,tex.a*colDiffuse.a*fragColor.a);
}
