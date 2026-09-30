#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// CORE_BONE_CAPACITY is injected by CoreLoadShaders from core/config.h. SKINNING_BONES sizes the
// bone array: CORE_BONE_CAPACITY unless the loader defines it (CoreLoadShaderDefined). The project
// runner defines 24, which with mvp, matModel and matNormal fits GLSL 120's guaranteed 512 vertex
// uniform components (proposal B9.3); models with more bones are skinned on the CPU.
// Its outputs are world.vs's plus fragColor, so world.fs lights and fogs a skinned model exactly as
// it does a static one; fragments that do not read fragPosition or fragColor are unaffected.
#ifndef SKINNING_BONES
#define SKINNING_BONES CORE_BONE_CAPACITY
#endif
attribute vec3 vertexPosition;
attribute vec2 vertexTexCoord;
attribute vec3 vertexNormal;
attribute vec4 vertexColor;
attribute vec4 vertexBoneIds;
attribute vec4 vertexBoneWeights;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
uniform mat4 boneMatrices[SKINNING_BONES];
varying vec2 fragTexCoord;
varying vec4 fragColor;
varying vec3 fragNormal;
varying vec3 fragPosition;
void main()
{
    vec4 p=vec4(vertexPosition,1.0);
    vec4 skinned=vertexBoneWeights.x*(boneMatrices[int(vertexBoneIds.x)]*p)
               +vertexBoneWeights.y*(boneMatrices[int(vertexBoneIds.y)]*p)
               +vertexBoneWeights.z*(boneMatrices[int(vertexBoneIds.z)]*p)
               +vertexBoneWeights.w*(boneMatrices[int(vertexBoneIds.w)]*p);
    // Bones assumed rigid/uniform-scale: the 3x3 part of each bone matrix skins the normal
    // the same way it skins the position, with no per-bone inverse-transpose.
    vec3 n=vertexBoneWeights.x*(mat3(boneMatrices[int(vertexBoneIds.x)])*vertexNormal)
          +vertexBoneWeights.y*(mat3(boneMatrices[int(vertexBoneIds.y)])*vertexNormal)
          +vertexBoneWeights.z*(mat3(boneMatrices[int(vertexBoneIds.z)])*vertexNormal)
          +vertexBoneWeights.w*(mat3(boneMatrices[int(vertexBoneIds.w)])*vertexNormal);
    fragTexCoord=vertexTexCoord;
    fragColor=vertexColor;
    fragNormal=normalize(mat3(matNormal)*n);
    fragPosition=vec3(matModel*skinned);
    gl_Position=mvp*skinned;
}
