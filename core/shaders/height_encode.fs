#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// World height above the ground packed into red over 8 metres (SPRITE_HEIGHT_RANGE in sprite_bake.h),
// read back on the CPU so the dither can shade a wall toward its foot and a backdrop fade as it rises.
varying vec2 fragTexCoord;
varying float fragHeight;
uniform sampler2D texture0;
uniform int clipGround;
varying float fragClipHeight;
void main()
{
    if(clipGround!=0 && fragClipHeight<0.0) discard;
    if(texture2D(texture0,fragTexCoord).a<=0.0) discard;
    gl_FragColor=vec4(clamp(fragHeight/8.0,0.0,1.0),0.0,0.0,1.0);
}
