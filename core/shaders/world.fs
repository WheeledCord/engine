#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// One directional light over an ambient term, then exponential fog by distance from the eye.
// lightDir is the direction the light travels, in world space, normalised.
varying vec2 fragTexCoord;
varying vec3 fragNormal;
varying vec3 fragPosition;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambient;
uniform vec3 fogColor;
uniform float fogDensity;
uniform vec3 viewPos;
void main()
{
    vec4 base=texture2D(texture0,fragTexCoord)*colDiffuse;
    vec3 n=normalize(fragNormal);
    vec3 lit=base.rgb*(ambient+lightColor*max(dot(n,-lightDir),0.0));
    float fog=exp(-fogDensity*length(fragPosition-viewPos));
    gl_FragColor=vec4(mix(fogColor,lit,clamp(fog,0.0,1.0)),base.a);
}
