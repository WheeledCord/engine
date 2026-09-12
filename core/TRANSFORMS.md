# Movement and transform helpers

Include `core/transform.h`. These are optional plain-value helpers: they do not introduce components,
parenting, automatic updates, rendering, collision response or ownership. 3D uses raylib's existing
`Transform`; 2D has a small `Transform2D` value. Existing UI integer layout is unaffected.

## Moving forward

The direct Unity C# equivalent for a non-physics object is:

```csharp
if (Input.GetKey(KeyCode.W))
    transform.Translate(Vector3.forward * speed * Time.deltaTime, Space.Self);
```

For an already scheduled entity here:

```c
static void Think(EntityContext *e)
{
    Mover *self = e->data; // Mover contains a raylib Transform transform and a float speed.
    if (e->input->down[KEY_W])
        TransformMoveLocal(&self->transform, (Vector3){0, 0, self->speed * (float)e->dt});
    EntityThinkNext(e);
}
```

Initialize the transform with `TransformIdentity()` or explicit identity defaults before use. A zeroed
raylib Transform has zero scale and an invalid rotation; it is not an identity. No helper hides that
mistake by silently changing the scale. Spawn should schedule the first Think.

Forward is **+Z**, up is +Y, right is -X, which is what a right-handed system with +Z forward
gives you. FpsCamera's zero yaw, ActorLookAt's yaw and glTF models all face +Z as well, so camera
orientation, actor aiming and these helpers can be connected without a conversion. Unity uses +Z
forward too, but it is left-handed, so its right is +X. Movement takes a
displacement, not a velocity; multiply by dt once in the caller. World movement ignores rotation;
local movement follows rotation but ignores scale, so scaling a model cannot change its travel speed.
All inputs are finite values; 3D rotations are unit quaternions. Rotation helpers preserve normalization.

For a 2D ship facing right at rest:

```c
Transform2DRotate(&ship, turnSpeed * dt);
Transform2DMoveLocal(&ship, (Vector2){speed * dt, 0});
```

2D follows raylib screen coordinates: +X right, +Y down, positive rotation clockwise. The 2D LookAt
helper points local +X at the target. Initialize with `Transform2DIdentity()`. Angles are **radians**,
matching raymath. Raylib's drawing calls and Camera2D use degrees; convert with RAD2DEG at that boundary.
The authoring_demo shows world movement, local movement and rotation with interpolated rendering.

## What each call is for

| Task | API | Reason |
|---|---|---|
| Move in object or world axes | TransformMoveLocal / TransformMoveWorld, 2D equivalents | Avoid repeating rotation-plus-add code and confusing scale with speed. |
| Rotate around local or world axes | TransformRotateLocal / TransformRotateWorld | Encapsulates quaternion multiplication order. 2D uses Transform2DRotate. |
| Read facing axes | TransformForward / Right / Up | Consistent axis convention across callers. |
| Face a target | TransformLookAt / Transform2DLookAt | Object orientation, distinct from a camera view matrix. 3D points +Z at the target, 2D points +X. |
| Convert a point between spaces | TransformPoint / TransformInversePoint, 2D equivalents | Includes translation, rotation and scale. Useful for attachment positions and picking. |
| Convert a direction between spaces | TransformDirection / TransformInverseDirection, 2D equivalents | Rotation only; preserves length. |
| Build a drawing matrix | TransformMatrix / Transform2DMatrix | One consistent scale-rotation-translation order. Build once for many points. |
| Approach scalar/angle/rotation at a bounded speed | MoveTowards / AngleMoveTowards / QuaternionRotateTowards | Stops at the target rather than overshooting; angles take the shortest path. |
| Compare or interpolate headings across the wrap | AngleDelta | Avoids a nearly full turn when crossing -PI/PI. |

LookAt returns false and leaves rotation unchanged for coincident targets. 3D also rejects a zero up
vector or an up vector parallel to the viewing direction; the caller chooses the appropriate up axis.
Inverse point conversion returns false without modifying its output when any scale component is zero.
Negative scales are supported. Maximum movement/turn steps must be nonnegative; zero or negative steps
leave the current value unchanged. AngleDelta's exact half-turn chooses -PI. AngleMoveTowards returns an
equivalent angle near the current angle, not necessarily within [-PI, PI).

## Use what raylib/raymath already provides

| Familiar Unity task | Existing API here |
|---|---|
| Vector2/Vector3.MoveTowards | Vector2MoveTowards / Vector3MoveTowards |
| Lerp, quaternion Slerp | Lerp / Vector2Lerp / Vector3Lerp / QuaternionSlerp |
| Normalize, distance, dot, cross | Vector2/Vector3Normalize, Distance, DotProduct; Vector3CrossProduct |
| Clamp magnitude | Vector2ClampValue / Vector3ClampValue (minimum 0) |
| World/screen camera conversion | GetWorldToScreen / GetWorldToScreen2D / GetScreenToWorld2D |
| Basic overlap and ray tests | Raylib CheckCollision* / GetRayCollision* functions |

Raymath Lerp is not clamped like Unity's Mathf.Lerp: clamp the interpolation parameter when required.
Lerp expresses interpolation by fraction; MoveTowards expresses a maximum distance per call. They are
not interchangeable. No duplicate vector, camera, collision or interpolation framework was added.
Rigidbody.MovePosition, CharacterController.Move and AddForce are physics operations and have not been
imitated with simple position writes. SmoothDamp, orbit helpers and general tween scheduling are left
out of this change; they require additional state/policy beyond these small transform operations.

## Where the shapes of these calls came from

Unity teaches Translate/Rotate in its introductory gameplay scripting material. That demonstrates
common use cases, not a measured frequency or a reason to reproduce its entire API.

- [Unity Learn: Translate and Rotate](https://learn.unity.com/tutorial/translate-and-rotate?version=2019.3)
- [Translate](https://docs.unity.cn/6000.4/Documentation/ScriptReference/Transform.Translate.html)
- [TransformPoint: includes scale](https://docs.unity.cn/ScriptReference/Transform.TransformPoint.html)
- [TransformDirection: excludes translation and scale](https://docs.unity.cn/ScriptReference/Transform.TransformDirection.html)
- [Quaternion.RotateTowards](https://docs.unity.cn/ScriptReference/Quaternion.RotateTowards.html)
- [Rigidbody2D.MovePosition: physics movement](https://docs.unity.cn/ScriptReference/Rigidbody2D.MovePosition.html)

Implementation delegates vector/quaternion/matrix operations to the vendored raymath. Unity-inspired
convenience does not change the engine's raylib coordinate conventions or imply Unity API compatibility.
