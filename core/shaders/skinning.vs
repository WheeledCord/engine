#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// CORE_BONE_CAPACITY is injected by CoreLoadShaders from core/config.h.
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec4 vertexColor;
attribute vec4 vertexBoneIds;
attribute vec4 vertexBoneWeights;
uniform mat4 mvp;
uniform mat4 boneMatrices[CORE_BONE_CAPACITY];
varying vec2 fragTexCoord;
varying vec4 fragColor;
void main()
{
    vec4 p=vec4(vertexPosition,1.0);
    vec4 skinned=vertexBoneWeights.x*(boneMatrices[int(vertexBoneIds.x)]*p)
               +vertexBoneWeights.y*(boneMatrices[int(vertexBoneIds.y)]*p)
               +vertexBoneWeights.z*(boneMatrices[int(vertexBoneIds.z)]*p)
               +vertexBoneWeights.w*(boneMatrices[int(vertexBoneIds.w)]*p);
    fragTexCoord=vertexTexCoord;
    fragColor=vertexColor;
    gl_Position=mvp*skinned;
}
