/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "node.h"

#include "raymath.h"
#include "transform.h" // TransformMatrix: one scale-rotation-translation order for the whole engine

#include <math.h>

// A chain walk (parent-of-parent-of-...) never looks past this many steps, live cycle or not.
#define NODE_MAX_DEPTH 256

static bool NodeSameId(EngineObjectId a, EngineObjectId b)
{
    return a.index == b.index && a.generation == b.generation;
}

// world = parent's world, with local composed into it the way core/skeleton.c's RigToGlobal
// composes a bone's local pose into its parent's -- scale, then rotate, then translate.
static Transform NodeCompose(Transform parent, Transform local)
{
    return (Transform){
        Vector3Add(parent.translation,
                  Vector3RotateByQuaternion(Vector3Multiply(local.translation, parent.scale), parent.rotation)),
        QuaternionNormalize(QuaternionMultiply(parent.rotation, local.rotation)),
        Vector3Multiply(parent.scale, local.scale),
    };
}

// Walks from a local transform up through live node3d parents to build a world transform. Takes
// the local fields rather than a handle so it still works for a node mid-destroy, whose own slot
// is already stale but whose storage (and whose still-live ancestors) are not.
static Transform NodeWorldOf(const EngineObjects *objects, Vector3 position, Quaternion rotation,
                             Vector3 scale, EngineObjectId parent, int depth)
{
    Transform local = {position, rotation, scale};
    if (depth >= NODE_MAX_DEPTH)
        return local;
    const CoreNode *p = EngineObjectData(objects, parent, &CoreNodeType);
    if (!p)
        return local; // no live parent: local is world
    Transform parentWorld = NodeWorldOf(objects, p->position, p->rotation, p->scale, p->parent, depth + 1);
    return NodeCompose(parentWorld, local);
}

static bool NodeInvertibleScale(Vector3 s)
{
    return fabsf(s.x) > 0.000001f && fabsf(s.y) > 0.000001f && fabsf(s.z) > 0.000001f;
}

// The local transform that composes with parentWorld to make world, mirroring core/skeleton.c's
// RigToLocal. Degenerate parent scale is left untouched rather than divided by.
static Transform NodeLocalFor(Transform parentWorld, Transform world)
{
    if (!NodeInvertibleScale(parentWorld.scale))
        return world;
    Quaternion inverseRotation = QuaternionInvert(parentWorld.rotation);
    return (Transform){
        Vector3Divide(Vector3RotateByQuaternion(Vector3Subtract(world.translation, parentWorld.translation),
                                                inverseRotation),
                     parentWorld.scale),
        QuaternionNormalize(QuaternionMultiply(inverseRotation, world.rotation)),
        Vector3Divide(world.scale, parentWorld.scale),
    };
}

bool CoreNodeWorld(const EngineObjects *objects, EngineObjectId node, Matrix *out)
{
    const CoreNode *n = EngineObjectData(objects, node, &CoreNodeType);
    if (!n || !out)
        return false;
    *out = TransformMatrix(NodeWorldOf(objects, n->position, n->rotation, n->scale, n->parent, 0));
    return true;
}

bool CoreNodeWorldPosition(const EngineObjects *objects, EngineObjectId node, Vector3 *out)
{
    const CoreNode *n = EngineObjectData(objects, node, &CoreNodeType);
    if (!n || !out)
        return false;
    *out = NodeWorldOf(objects, n->position, n->rotation, n->scale, n->parent, 0).translation;
    return true;
}

bool CoreNodeSetWorldPosition(EngineObjects *objects, EngineObjectId node, Vector3 world)
{
    CoreNode *n = EngineObjectData(objects, node, &CoreNodeType);
    if (!n)
        return false;
    const CoreNode *p = EngineObjectData(objects, n->parent, &CoreNodeType);
    if (!p)
    {
        n->position = world;
        return true;
    }
    Transform parentWorld = NodeWorldOf(objects, p->position, p->rotation, p->scale, p->parent, 0);
    if (!NodeInvertibleScale(parentWorld.scale))
        return false;
    n->position = Vector3Divide(
        Vector3RotateByQuaternion(Vector3Subtract(world, parentWorld.translation),
                                 QuaternionInvert(parentWorld.rotation)),
        parentWorld.scale);
    return true;
}

// True when parent is node itself, or is reached by walking up node's would-be new ancestors --
// the two ways CoreNodeSetParent must refuse, checked by the one walk.
static bool NodeWouldCycle(const EngineObjects *objects, EngineObjectId node, EngineObjectId parent)
{
    int depth = 0;
    for (EngineObjectId walk = parent; !EngineObjectIdIsNull(walk) && depth < NODE_MAX_DEPTH; depth++)
    {
        if (NodeSameId(walk, node))
            return true;
        const CoreNode *w = EngineObjectData(objects, walk, &CoreNodeType);
        if (!w)
            break;
        walk = w->parent;
    }
    return false;
}

bool CoreNodeSetParent(EngineObjects *objects, EngineObjectId node, EngineObjectId parent, bool keepWorld)
{
    CoreNode *n = EngineObjectData(objects, node, &CoreNodeType);
    if (!n)
        return false;
    const CoreNode *p = EngineObjectIdIsNull(parent) ? NULL : EngineObjectData(objects, parent, &CoreNodeType);
    if (!EngineObjectIdIsNull(parent) && !p)
        parent = ENGINE_OBJECT_NULL; // a handle that no longer names a live node counts as no parent
    else if (p && NodeWouldCycle(objects, node, parent))
        return false;

    Transform before = {0};
    if (keepWorld)
        before = NodeWorldOf(objects, n->position, n->rotation, n->scale, n->parent, 0);

    n->parent = parent;

    if (keepWorld)
    {
        Transform local = p ? NodeLocalFor(NodeWorldOf(objects, p->position, p->rotation, p->scale, p->parent, 0),
                                          before)
                           : before;
        n->position = local.translation;
        n->rotation = local.rotation;
        n->scale = local.scale;
    }
    return true;
}

// ---- the engine type ----------------------------------------------------------------------------
static bool NodeCreate(EngineCall *call)
{
    CoreNode *node = call->data;
    node->scale = (Vector3){1, 1, 1};
    node->rotation = QuaternionIdentity();
    node->parent = ENGINE_OBJECT_NULL;
    node->objects = call->objects;
    node->self = call->self;
    return true;
}

// Every child whose parent was this node is reparented to none, kept exactly where it was in the
// world: its local transform becomes what its world transform already was, computed while this
// node's own storage (though its slot is already stale) and every ancestor above it are still good.
static void NodeDestroy(void *data)
{
    CoreNode *node = data;
    EngineObjects *objects = node->objects;
    if (!objects)
        return;
    Transform gone = NodeWorldOf(objects, node->position, node->rotation, node->scale, node->parent, 0);
    for (size_t i = 0; i < objects->slotCount; i++)
    {
        EngineObjectSlot *slot = &objects->slots[i];
        if (!slot->live || !EngineTypeIs(slot->type, &CoreNodeType))
            continue;
        CoreNode *child = slot->data;
        if (!NodeSameId(child->parent, node->self))
            continue;
        Transform childWorld =
            NodeCompose(gone, (Transform){child->position, child->rotation, child->scale});
        child->parent = ENGINE_OBJECT_NULL;
        child->position = childWorld.translation;
        child->rotation = childWorld.rotation;
        child->scale = childWorld.scale;
    }
}

static bool NodeGetRotation(const void *object, EngineValue *out)
{
    *out = EngineVector3(QuaternionToEuler(((const CoreNode *)object)->rotation));
    return true;
}

static bool NodeSetRotation(void *object, const EngineValue *value)
{
    Vector3 e = value->as.vector3;
    ((CoreNode *)object)->rotation = QuaternionFromEuler(e.x, e.y, e.z);
    return true;
}

static bool NodeGetParent(const void *object, EngineValue *out)
{
    *out = EngineObject(((const CoreNode *)object)->parent);
    return true;
}

static bool NodeSetParent(void *object, const EngineValue *value)
{
    CoreNode *node = object;
    return CoreNodeSetParent(node->objects, node->self, value->as.object, true);
}

static bool NodeGetWorldPosition(const void *object, EngineValue *out)
{
    const CoreNode *node = object;
    Vector3 world;
    if (!CoreNodeWorldPosition(node->objects, node->self, &world))
        return false;
    *out = EngineVector3(world);
    return true;
}

static bool NodeSetWorldPositionMethod(EngineCall *call)
{
    CoreNode *node = call->data;
    return CoreNodeSetWorldPosition(node->objects, node->self, call->arguments[0].as.vector3);
}

static bool NodeTranslate(EngineCall *call)
{
    CoreNode *node = call->data;
    Transform t = {node->position, node->rotation, node->scale};
    TransformMoveLocal(&t, call->arguments[0].as.vector3);
    node->position = t.translation;
    return true;
}

static bool NodeRotate(EngineCall *call)
{
    CoreNode *node = call->data;
    Transform t = {node->position, node->rotation, node->scale};
    TransformRotateLocal(&t, call->arguments[0].as.vector3, call->arguments[1].as.number);
    node->rotation = t.rotation;
    return true;
}

static const EngineProperty nodeProperties[] = {
    ENGINE_FIELD("position", CoreNode, position, ENGINE_VECTOR3, ENGINE_PROPERTY_SAVE,
                "local position, relative to the parent"),
    ENGINE_FIELD("scale", CoreNode, scale, ENGINE_VECTOR3, ENGINE_PROPERTY_SAVE,
                "local scale; starts at (1,1,1)"),
    ENGINE_COMPUTED("rotation", ENGINE_VECTOR3, ENGINE_PROPERTY_SAVE, NodeGetRotation, NodeSetRotation,
                    "local rotation as Euler radians about X, then Y, then Z"),
    ENGINE_COMPUTED("parent", ENGINE_OBJECT, 0, NodeGetParent, NodeSetParent,
                    "the node this one sits under; setting it keeps this node's world transform, and "
                    "#f detaches it"),
    ENGINE_COMPUTED("world-position", ENGINE_VECTOR3, ENGINE_PROPERTY_READ_ONLY, NodeGetWorldPosition, NULL,
                    "position in world space, the whole parent chain applied"),
};
static const EngineMethod nodeMethods[] = {
    {"set-world-position!", ENGINE_NONE, {ENGINE_VECTOR3}, 1, NodeSetWorldPositionMethod,
     "moves the node so its world position becomes the one given"},
    {"translate!", ENGINE_NONE, {ENGINE_VECTOR3}, 1, NodeTranslate, "moves the node along its own local axes"},
    {"rotate!", ENGINE_NONE, {ENGINE_VECTOR3, ENGINE_FLOAT}, 2, NodeRotate,
     "turns the node about a local axis by radians"},
};

const EngineType CoreNodeType = {
    .name = "node3d",
    .size = sizeof(CoreNode),
    .properties = nodeProperties,
    .propertyCount = sizeof nodeProperties / sizeof nodeProperties[0],
    .methods = nodeMethods,
    .methodCount = sizeof nodeMethods / sizeof nodeMethods[0],
    .create = NodeCreate,
    .destroy = NodeDestroy,
    .help = "a local position, rotation and scale under an optional parent, whose world transform is "
            "the parent's times its own",
};
