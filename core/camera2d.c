/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "camera2d.h"

#include <math.h>
#include "raymath.h"

static Vector2 ClampPosition(const CoreCamera2D *camera, Vector2 position)
{
    if (!camera->clampBounds || camera->zoom <= 0)
        return position;
    float halfX = camera->viewport.x / (2 * camera->zoom);
    float halfY = camera->viewport.y / (2 * camera->zoom);
    if (camera->bounds.width <= 2 * halfX)
        position.x = camera->bounds.x + camera->bounds.width * .5f;
    else
        position.x = Clamp(position.x, camera->bounds.x + halfX,
                           camera->bounds.x + camera->bounds.width - halfX);
    if (camera->bounds.height <= 2 * halfY)
        position.y = camera->bounds.y + camera->bounds.height * .5f;
    else
        position.y = Clamp(position.y, camera->bounds.y + halfY,
                           camera->bounds.y + camera->bounds.height - halfY);
    return position;
}

CoreCamera2D CoreCamera2DDefault(void)
{
    return (CoreCamera2D){.viewport = {960, 540}, .zoom = 1};
}
void CoreCamera2DSetViewport(CoreCamera2D *camera, Vector2 viewport)
{
    if (!camera)
        return;
    camera->viewport = viewport;
    camera->position = ClampPosition(camera, camera->position);
    camera->previous = camera->position;
}
void CoreCamera2DFollow(CoreCamera2D *camera, Vector2 target, float dt)
{
    if (!camera)
        return;
    camera->previous = camera->position;
    float t = camera->followRate > 0 && dt > 0 ? 1 - expf(-camera->followRate * dt) : 1;
    camera->position = Vector2Lerp(camera->position, target, t);
    camera->position = ClampPosition(camera, camera->position);
}
Vector2 CoreCamera2DInterpolated(const CoreCamera2D *camera, float alpha)
{
    if (!camera)
        return (Vector2){0};
    return Vector2Lerp(camera->previous, camera->position, Clamp(alpha, 0, 1));
}
Vector2 CoreCamera2DWorldToScreen(const CoreCamera2D *camera, float alpha, Vector2 world)
{
    if (!camera || camera->zoom <= 0)
        return world;
    Vector2 position = CoreCamera2DInterpolated(camera, alpha);
    return (Vector2){camera->viewport.x * .5f + (world.x - position.x) * camera->zoom,
                     camera->viewport.y * .5f + (world.y - position.y) * camera->zoom};
}
Vector2 CoreCamera2DScreenToWorld(const CoreCamera2D *camera, float alpha, Vector2 screen)
{
    if (!camera || camera->zoom <= 0)
        return screen;
    Vector2 position = CoreCamera2DInterpolated(camera, alpha);
    return (Vector2){position.x + (screen.x - camera->viewport.x * .5f) / camera->zoom,
                     position.y + (screen.y - camera->viewport.y * .5f) / camera->zoom};
}

// ---- as an engine type ---------------------------------------------------------------------------
static bool CameraCreate(EngineCall *call)
{
    *(CoreCamera2D *)call->data = CoreCamera2DDefault();
    return true;
}
static bool CameraSetViewport(void *object, const EngineValue *value)
{
    CoreCamera2DSetViewport(object, value->as.vector2);
    return true;
}
static bool CameraGetViewport(const void *object, EngineValue *out)
{
    *out = EngineVector2(((const CoreCamera2D *)object)->viewport);
    return true;
}
static bool CameraFollow(EngineCall *call)
{
    CoreCamera2DFollow(call->data, call->arguments[0].as.vector2, call->arguments[1].as.number);
    return true;
}
static bool CameraWorldToScreen(EngineCall *call)
{
    call->result = EngineVector2(
        CoreCamera2DWorldToScreen(call->data, call->objects->alpha, call->arguments[0].as.vector2));
    return true;
}
static bool CameraScreenToWorld(EngineCall *call)
{
    call->result = EngineVector2(
        CoreCamera2DScreenToWorld(call->data, call->objects->alpha, call->arguments[0].as.vector2));
    return true;
}
static bool CameraInterpolated(EngineCall *call)
{
    call->result = EngineVector2(CoreCamera2DInterpolated(call->data, call->objects->alpha));
    return true;
}
static const EngineProperty cameraProperties[] = {
    ENGINE_FIELD("position", CoreCamera2D, position, ENGINE_VECTOR2, ENGINE_PROPERTY_SAVE,
                 "the world point at the centre of the view"),
    ENGINE_FIELD("zoom", CoreCamera2D, zoom, ENGINE_FLOAT, ENGINE_PROPERTY_SAVE, "magnification"),
    ENGINE_FIELD("follow-rate", CoreCamera2D, followRate, ENGINE_FLOAT, 0,
                 "how quickly follow! closes the gap, per second; zero snaps"),
    ENGINE_FIELD("clamp-bounds", CoreCamera2D, clampBounds, ENGINE_BOOL, 0,
                 "keep the view inside its bounds"),
    ENGINE_COMPUTED("viewport", ENGINE_VECTOR2, 0, CameraGetViewport, CameraSetViewport,
                    "the screen size used for world and screen conversion"),
};
static const EngineMethod cameraMethods[] = {
    {"follow!", ENGINE_NONE, {ENGINE_VECTOR2, ENGINE_FLOAT}, 2, CameraFollow,
     "move toward a target over dt seconds"},
    {"world->screen", ENGINE_VECTOR2, {ENGINE_VECTOR2}, 1, CameraWorldToScreen,
     "a world point in screen pixels, as drawn this frame"},
    {"screen->world", ENGINE_VECTOR2, {ENGINE_VECTOR2}, 1, CameraScreenToWorld,
     "screen pixels as a world point, as drawn this frame"},
    {"interpolated", ENGINE_VECTOR2, {ENGINE_NONE}, 0, CameraInterpolated,
     "where the camera is drawn this frame, between steps"},
};
const EngineType CoreCamera2DType = {
    .name = "camera2d",
    .size = sizeof(CoreCamera2D),
    .properties = cameraProperties,
    .propertyCount = sizeof cameraProperties / sizeof cameraProperties[0],
    .methods = cameraMethods,
    .methodCount = sizeof cameraMethods / sizeof cameraMethods[0],
    .create = CameraCreate,
    .help = "a 2D camera that follows, clamps and converts between world and screen",
};
