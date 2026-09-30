#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// The project runner's world pass (docs/developer/store.md §3.1): an ambient term, one directional
// light, and up to four point lights lit per pixel in a fixed loop of four (an unused slot has zero
// colour), then exponential fog by distance from the eye. lightDir is the direction the light
// travels, in world space, normalised. A point light's colour is its colour times its energy, and
// it falls off as max(0, 1 - d / range)^2. Four is the budget on the target GPU, where pixels, not
// calls, are the limit (proposal B9.6, Part E question 10).
#define POINT_LIGHTS 4
varying vec2 fragTexCoord;
varying vec3 fragNormal;
varying vec3 fragPosition;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambient;
uniform vec3 pointPosition[POINT_LIGHTS];
uniform vec3 pointColor[POINT_LIGHTS];
uniform float pointRange[POINT_LIGHTS];
uniform vec3 fogColor;
uniform float fogDensity;
uniform vec3 viewPos;
void main()
{
    vec4 base=texture2D(texture0,fragTexCoord)*colDiffuse;
    vec3 n=normalize(fragNormal);
    vec3 light=ambient+lightColor*max(dot(n,-lightDir),0.0);
    for(int i=0;i<POINT_LIGHTS;i++)
    {
        vec3 toLight=pointPosition[i]-fragPosition;
        float d=length(toLight);
        float fall=max(1.0-d/pointRange[i],0.0);
        light+=pointColor[i]*(fall*fall)*max(dot(n,toLight/max(d,0.0001)),0.0);
    }
    vec3 lit=base.rgb*light;
    float fog=exp(-fogDensity*length(fragPosition-viewPos));
    gl_FragColor=vec4(mix(fogColor,lit,clamp(fog,0.0,1.0)),base.a);
}
