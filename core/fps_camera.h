/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_FPS_CAMERA_H
#define CORE_FPS_CAMERA_H
#include "raylib.h"
#include <stdbool.h>
typedef struct ViewmodelMotion
{
    float side, down, forward, pitch, yaw, roll, swayMax, swayResponse, swayRoll;
} ViewmodelMotion;
typedef struct FpsCameraConfig
{
    float sensitivity, pitchLimit, walkSpeed, runSpeed, crouchSpeed, acceleration, deceleration;
    float eyeHeight, crouchEye, eyeFollow, stepWalk, stepRun, bobResponse, bobRunScale;
    float bobVertical, bobSide, bobForward, bobRoll, bobPitch, breathRate, breathAmount, fov;
    ViewmodelMotion viewmodel;
} FpsCameraConfig;
typedef struct FpsInput
{
    Vector3 move;
    Vector2 lookDelta;
    bool sprint, crouch, noclip;
} FpsInput;
typedef struct FpsWorld
{
    void *context;
    Vector3 (*resolveMove)(void *, Vector3 from, Vector3 desired);
    float (*groundHeight)(void *, float x, float z);
} FpsWorld;
typedef struct FpsCamera
{
    Vector3 position, velocity;
    float yaw, pitch, phase, bobBlend, lagYaw, lagPitch;
    double elapsed;
    Camera previous, current;
    Vector3 previousVmOffset, vmOffset;
    Quaternion previousVmRotation, vmRotation;
    int footfalls;
} FpsCamera;
FpsCameraConfig FpsCameraDefaults(void);
void FpsCameraInit(FpsCamera *state, Vector3 eyePosition, float yaw, float pitch, float fov);
/* Keys and gameplay speed restrictions are resolved by the caller. Returns footfall count. */
int FpsCameraUpdate(FpsCamera *s, const FpsCameraConfig *c, FpsInput input, FpsWorld world, float dt);
Camera FpsCameraInterpolated(const FpsCamera *s, float alpha);
void FpsViewmodelInterpolated(const FpsCamera *s, float alpha, Vector3 *offset, Quaternion *rotation);
#endif
