/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "fps_camera.h"
#include "raymath.h"
FpsCameraConfig FpsCameraDefaults(void)
{
    return (FpsCameraConfig){.sensitivity = 0.0026f,
                             .pitchLimit = 1.45f,
                             .walkSpeed = 3,
                             .runSpeed = 6,
                             .crouchSpeed = 1.5f,
                             .acceleration = 12,
                             .deceleration = 16,
                             .eyeHeight = 1.7f,
                             .crouchEye = 1,
                             .eyeFollow = 12,
                             .stepWalk = 0.8f,
                             .stepRun = 1.1f,
                             .bobResponse = 8,
                             .bobRunScale = 1.4f,
                             .bobVertical = 0.025f,
                             .bobSide = 0.015f,
                             .bobForward = 0.01f,
                             .bobRoll = 0.006f,
                             .bobPitch = 0.006f,
                             .breathRate = 0.3f,
                             .breathAmount = 0.005f,
                             .fov = 65,
                             .viewmodel = {.side = 0.022f,
                                           .down = 0.016f,
                                           .forward = 0.01f,
                                           .pitch = 0.012f,
                                           .yaw = 0.018f,
                                           .roll = 0.025f,
                                           .swayMax = 0.055f,
                                           .swayResponse = 9,
                                           .swayRoll = 0.3f}};
}
static Vector3 Forward(float yaw, float pitch)
{
    return (Vector3){cosf(pitch) * sinf(yaw), sinf(pitch), cosf(pitch) * cosf(yaw)};
}
void FpsCameraInit(FpsCamera *s, Vector3 pos, float yaw, float pitch, float fov)
{
    *s = (FpsCamera){0};
    s->position = pos;
    s->yaw = yaw;
    s->pitch = pitch;
    s->current = (Camera){pos, Vector3Add(pos, Forward(yaw, pitch)), {0, 1, 0}, fov, CAMERA_PERSPECTIVE};
    s->previous = s->current;
    s->vmRotation = s->previousVmRotation = QuaternionIdentity();
}
int FpsCameraUpdate(FpsCamera *s, const FpsCameraConfig *c, FpsInput input, FpsWorld world, float dt)
{
    if (dt <= 0)
        return 0;
    s->previous = s->current;
    s->previousVmOffset = s->vmOffset;
    s->previousVmRotation = s->vmRotation;
    float oldYaw = s->yaw, oldPitch = s->pitch;
    s->yaw -= input.lookDelta.x * c->sensitivity;
    s->pitch = Clamp(s->pitch - input.lookDelta.y * c->sensitivity, -c->pitchLimit, c->pitchLimit);
    float yawDelta = s->yaw - oldYaw, pitchDelta = s->pitch - oldPitch;
    s->yaw = atan2f(sinf(s->yaw), cosf(s->yaw));
    float response = fmaxf(c->viewmodel.swayResponse, 0.01f), limit = fmaxf(c->viewmodel.swayMax, 0.0001f),
          follow = 1 - expf(-response * dt);
    s->lagYaw += (-limit * tanhf(yawDelta / dt / response / limit) - s->lagYaw) * follow;
    s->lagPitch += (-limit * tanhf(pitchDelta / dt / response / limit) - s->lagPitch) * follow;
    Vector3 f = Forward(s->yaw, s->pitch), flat = Forward(s->yaw, 0),
            right = Vector3Normalize(Vector3CrossProduct(f, (Vector3){0, 1, 0}));
    Vector3 movement = input.move;
    if (Vector3LengthSqr(movement) > 1)
        movement = Vector3Normalize(movement);
    float speed = input.crouch ? c->crouchSpeed : (input.sprint ? c->runSpeed : c->walkSpeed);
    Vector3 wish = Vector3Scale(
        Vector3Add(Vector3Scale(right, movement.x), Vector3Scale(input.noclip ? f : flat, movement.z)),
        speed);
    if (input.noclip)
        wish.y += movement.y * speed;
    if (Vector3Length(wish) > speed && speed > 0)
        wish = Vector3Scale(Vector3Normalize(wish), speed);
    float rate = Vector3LengthSqr(movement) > 0 ? c->acceleration : c->deceleration;
    s->velocity = Vector3Lerp(s->velocity, wish, 1 - expf(-rate * dt));
    Vector3 old = s->position;
    Vector3 desired = Vector3Add(old, Vector3Scale(s->velocity, dt));
    s->position =
        (!input.noclip && world.resolveMove) ? world.resolveMove(world.context, old, desired) : desired;
    if (!input.noclip)
    {
        if (fabsf(s->position.x - old.x) < fabsf(desired.x - old.x) * 0.5f)
            s->velocity.x = 0;
        if (fabsf(s->position.z - old.z) < fabsf(desired.z - old.z) * 0.5f)
            s->velocity.z = 0;
        if (world.groundHeight)
        {
            float target = world.groundHeight(world.context, s->position.x, s->position.z) +
                           (input.crouch ? c->crouchEye : c->eyeHeight);
            s->position.y += (target - s->position.y) * (1 - expf(-c->eyeFollow * dt));
        }
    }
    Vector3 delta = Vector3Subtract(s->position, old);
    float travelled = input.noclip ? 0 : sqrtf(delta.x * delta.x + delta.z * delta.z),
          actual = travelled / dt;
    float run = Clamp((actual - c->walkSpeed) / fmaxf(c->runSpeed - c->walkSpeed, 0.01f), 0, 1);
    s->bobBlend +=
        (Clamp(actual / fmaxf(c->walkSpeed, 0.01f), 0, 1) - s->bobBlend) * (1 - expf(-c->bobResponse * dt));
    float next = s->phase + travelled * PI / fmaxf(Lerp(c->stepWalk, c->stepRun, run), 0.01f);
    s->footfalls = (int)(next / PI) - (int)(s->phase / PI);
    s->phase = fmodf(next, 2 * PI);
    s->elapsed += dt;
    float amp = s->bobBlend * Lerp(1, c->bobRunScale, run), wave = sinf(2 * s->phase);
    float y = c->bobVertical * amp * (-0.5f * cosf(2 * s->phase) + 0.1f * sinf(4 * s->phase));
    float roll = c->bobRoll * amp * cosf(s->phase), nod = c->bobPitch * amp * wave;
    Vector3 view = Vector3Add(s->position, Vector3Add(Vector3Scale(right, c->bobSide * amp * cosf(s->phase)),
                                                      Vector3Scale(flat, c->bobForward * amp * wave)));
    view.y += y + sinf((float)s->elapsed * 2 * PI * c->breathRate) * c->breathAmount * (1 - s->bobBlend);
    f = Vector3Normalize(Vector3RotateByAxisAngle(f, right, nod));
    Vector3 up = Vector3Normalize(Vector3CrossProduct(right, f));
    s->current = (Camera){view, Vector3Add(view, f), Vector3RotateByAxisAngle(up, f, roll), c->fov,
                          CAMERA_PERSPECTIVE};
    float side = amp * cosf(s->phase), step = amp * (0.5f + 0.5f * cosf(2 * s->phase));
    ViewmodelMotion m = c->viewmodel;
    s->vmOffset = (Vector3){m.side * side, -m.down * step, m.forward * amp * wave};
    s->vmRotation = QuaternionFromEuler(s->lagPitch + m.pitch * step, s->lagYaw + m.yaw * side,
                                        m.roll * side + m.swayRoll * s->lagYaw);
    return s->footfalls;
}
Camera FpsCameraInterpolated(const FpsCamera *s, float alpha)
{
    alpha = Clamp(alpha, 0, 1);
    Camera c = s->current;
    c.position = Vector3Lerp(s->previous.position, s->current.position, alpha);
    Vector3 a = Vector3Subtract(s->previous.target, s->previous.position),
            b = Vector3Subtract(s->current.target, s->current.position);
    Vector3 f = Vector3Normalize(Vector3Lerp(a, b, alpha));
    if (Vector3LengthSqr(f) < 0.000001f)
        f = b;
    c.target = Vector3Add(c.position, f);
    c.up = Vector3Normalize(Vector3Lerp(s->previous.up, s->current.up, alpha));
    return c;
}
void FpsViewmodelInterpolated(const FpsCamera *s, float alpha, Vector3 *offset, Quaternion *rotation)
{
    *offset = Vector3Lerp(s->previousVmOffset, s->vmOffset, Clamp(alpha, 0, 1));
    *rotation = QuaternionSlerp(s->previousVmRotation, s->vmRotation, Clamp(alpha, 0, 1));
}
