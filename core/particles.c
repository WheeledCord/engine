/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "particles.h"
#include "raymath.h"
#include "rlgl.h"
#include <math.h>
#include <stdlib.h>

// ---- curl noise: divergence-free turbulence, so particles swirl and fold instead of clumping at
// sinks or scattering from sources. Cheap 3D value noise (integer hash + trilinear smoothstep),
// then the curl of a vector potential built from three offset noise fields. ------------------------
static float CoreParticleNoiseHash(int x, int y, int z)
{
    unsigned n = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663) ^ (unsigned)(z * 83492791);
    n = (n ^ (n >> 13)) * 1274126177u;
    return (n & 0x7FFFFF) * (1.0f / 8388607.0f);
}
static float CoreParticleNoise3D(float x, float y, float z)
{
    int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
    float u = x - xi, v = y - yi, w = z - zi;
    u = u * u * (3 - 2 * u);
    v = v * v * (3 - 2 * v);
    w = w * w * (3 - 2 * w);
    return CoreParticleNoiseHash(xi, yi, zi) * (1 - u) * (1 - v) * (1 - w) +
           CoreParticleNoiseHash(xi, yi, zi + 1) * (1 - u) * (1 - v) * w +
           CoreParticleNoiseHash(xi, yi + 1, zi) * (1 - u) * v * (1 - w) +
           CoreParticleNoiseHash(xi, yi + 1, zi + 1) * (1 - u) * v * w +
           CoreParticleNoiseHash(xi + 1, yi, zi) * u * (1 - v) * (1 - w) +
           CoreParticleNoiseHash(xi + 1, yi, zi + 1) * u * (1 - v) * w +
           CoreParticleNoiseHash(xi + 1, yi + 1, zi) * u * v * (1 - w) +
           CoreParticleNoiseHash(xi + 1, yi + 1, zi + 1) * u * v * w;
}
// curl of psi = (N(x,y,z), N(x,y+31.4,z), N(x,y,z+57.1)) -- 12 noise samples.
static Vector3 CoreParticleCurlNoise(Vector3 p, float time)
{
    float e = 0.15f, s = 1.0f;
    float x = p.x * s, y = p.y * s, z = p.z * s + time * 0.3f;
    float inv = 1.0f / (2.0f * e);
    Vector3 v;
    v.x = (CoreParticleNoise3D(x, y + e, z + 57.1f) - CoreParticleNoise3D(x, y - e, z + 57.1f) -
           CoreParticleNoise3D(x, y + 31.4f, z + e) + CoreParticleNoise3D(x, y + 31.4f, z - e)) *
          inv;
    v.y = (CoreParticleNoise3D(x, y, z + e) - CoreParticleNoise3D(x, y, z - e) -
           CoreParticleNoise3D(x + e, y, z + 57.1f) + CoreParticleNoise3D(x - e, y, z + 57.1f)) *
          inv;
    v.z = (CoreParticleNoise3D(x + e, y + 31.4f, z) - CoreParticleNoise3D(x - e, y + 31.4f, z) -
           CoreParticleNoise3D(x, y + e, z) + CoreParticleNoise3D(x, y - e, z)) *
          inv;
    return v;
}

bool CoreParticlesInit(CoreParticles *pool, int capacity)
{
    if (!pool || capacity <= 0)
        return false;
    CoreParticle *items = calloc((size_t)capacity, sizeof(CoreParticle));
    if (!items)
        return false;
    pool->items = items;
    pool->count = 0;
    pool->capacity = capacity;
    pool->clock = 0.0f;
    return true;
}

void CoreParticlesFree(CoreParticles *pool)
{
    if (!pool)
        return;
    free(pool->items);
    *pool = (CoreParticles){0};
}

CoreParticle *CoreParticleEmit(CoreParticles *pool, const CoreParticle *particle)
{
    if (!pool || !particle || pool->count >= pool->capacity)
        return NULL;
    CoreParticle *slot = &pool->items[pool->count++];
    *slot = *particle;
    return slot;
}

void CoreParticlesUpdate(CoreParticles *pool, float dt, CoreParticleStepFn step, void *user)
{
    if (!pool)
        return;
    pool->clock += dt;
    for (int i = 0; i < pool->count;)
    {
        CoreParticle *p = &pool->items[i];
        p->age += dt;
        if (p->age >= p->life)
        {
            pool->items[i] = pool->items[--pool->count];
            continue;
        }
        p->velocity.y -= p->gravity * dt;
        if (p->drag > 0.0f)
            p->velocity = Vector3Scale(p->velocity, 1.0f / (1.0f + p->drag * dt));
        if (p->turbulenceStrength.x != 0.0f || p->turbulenceStrength.y != 0.0f ||
            p->turbulenceStrength.z != 0.0f)
        {
            Vector3 curl = CoreParticleCurlNoise(Vector3Scale(p->position, p->turbulenceScale), pool->clock);
            p->velocity.x += curl.x * p->turbulenceStrength.x * dt;
            p->velocity.y += curl.y * p->turbulenceStrength.y * dt;
            p->velocity.z += curl.z * p->turbulenceStrength.z * dt;
        }
        p->velocity.x += (p->anchor.x - p->position.x) * p->pull.x * dt;
        p->velocity.y += (p->anchor.y - p->position.y) * p->pull.y * dt;
        p->velocity.z += (p->anchor.z - p->position.z) * p->pull.z * dt;
        p->rotation += p->rotationSpeed * (1.0f + p->spinUp * p->age / p->life) * dt;
        Vector3 previous = p->position;
        p->position = Vector3Add(p->position, Vector3Scale(p->velocity, dt));
        if (step && !step(p, previous, dt, user))
        {
            pool->items[i] = pool->items[--pool->count];
            continue;
        }
        i++;
    }
}

static void CoreParticleDrawOne(const CoreParticle *p, Camera camera, CoreParticleLookFn look,
                                void *user, CoreParticleBlend pass)
{
    if (p->blend != pass)
        return;
    float t = p->age / p->life;
    float sz = Lerp(p->size0, p->size1, t);
    float aspect = p->texture.height ? fabsf((float)p->texture.width / (float)p->texture.height) : 1.0f;
    CoreParticleLook out;
    out.size = (Vector2){sz * aspect, sz};
    out.color = ColorLerp(p->color0, p->color1, t);
    out.up = (Vector3){0, 1, 0};
    out.rotation = p->rotation;
    if (look && !look(p, t, &out, user))
        return;
    Rectangle source = {0, 0, (float)p->texture.width, (float)p->texture.height};
    DrawBillboardPro(camera, p->texture, source, p->position, out.up, out.size,
                     Vector2Scale(out.size, 0.5f), out.rotation, out.color);
}

void CoreParticlesDraw(const CoreParticles *pool, Camera camera, CoreParticleLookFn look, void *user)
{
    if (!pool)
        return;
    rlDisableDepthMask(); // depth-test, don't depth-write: particles don't occlude each other
    BeginBlendMode(BLEND_ALPHA);
    for (int i = 0; i < pool->count; i++)
        CoreParticleDrawOne(&pool->items[i], camera, look, user, CORE_PARTICLE_ALPHA);
    EndBlendMode();
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < pool->count; i++)
        CoreParticleDrawOne(&pool->items[i], camera, look, user, CORE_PARTICLE_ADDITIVE);
    EndBlendMode();
    rlEnableDepthMask();
}

// ---- as an engine type ---------------------------------------------------------------------------
#define CORE_PARTICLES_DEFAULT_CAPACITY 512

// What a script makes: the pool, and the particle a script edits and emit! copies from.
typedef struct CoreParticlesScript
{
    CoreParticles pool;
    CoreParticle template;
} CoreParticlesScript;

static bool ParticlesScriptCreate(EngineCall *call)
{
    CoreParticlesScript *object = call->data;
    int capacity = call->count > 0 ? call->arguments[0].as.integer : CORE_PARTICLES_DEFAULT_CAPACITY;
    if (capacity <= 0)
    {
        call->error = "a particle pool's capacity must be more than zero";
        return false;
    }
    if (!CoreParticlesInit(&object->pool, capacity))
    {
        call->error = "out of memory";
        return false;
    }
    object->template.life = 1.0f;
    object->template.size0 = object->template.size1 = 1.0f;
    object->template.color0 = object->template.color1 = WHITE;
    return true;
}
static void ParticlesScriptDestroy(void *data) { CoreParticlesFree(&((CoreParticlesScript *)data)->pool); }
static bool ParticlesScriptEmit(EngineCall *call)
{
    CoreParticlesScript *object = call->data;
    CoreParticle particle = object->template;
    particle.position = call->arguments[0].as.vector3;
    particle.velocity = call->arguments[1].as.vector3;
    particle.age = 0.0f;
    call->result = EngineBool(CoreParticleEmit(&object->pool, &particle) != NULL);
    return true;
}
static bool ParticlesScriptClear(EngineCall *call)
{
    ((CoreParticlesScript *)call->data)->pool.count = 0;
    return true;
}

static const EngineProperty particlesProperties[] = {
    ENGINE_FIELD("count", CoreParticlesScript, pool.count, ENGINE_INT, ENGINE_PROPERTY_READ_ONLY,
                 "how many particles are alive"),
    ENGINE_FIELD("life", CoreParticlesScript, template.life, ENGINE_FLOAT, 0,
                 "seconds a particle emitted now will live"),
    ENGINE_FIELD("size-start", CoreParticlesScript, template.size0, ENGINE_FLOAT, 0,
                 "billboard size at birth"),
    ENGINE_FIELD("size-end", CoreParticlesScript, template.size1, ENGINE_FLOAT, 0,
                 "billboard size at death"),
    ENGINE_FIELD("gravity", CoreParticlesScript, template.gravity, ENGINE_FLOAT, 0,
                 "downward accel in m/s^2; positive falls"),
    ENGINE_FIELD("drag", CoreParticlesScript, template.drag, ENGINE_FLOAT, 0,
                 "velocity damping per second"),
};
static const EngineMethod particlesMethods[] = {
    {"emit!", ENGINE_BOOL, {ENGINE_VECTOR3, ENGINE_VECTOR3}, 2, ParticlesScriptEmit,
     "emit one particle from the template at a position and velocity; #f when the pool is full"},
    {"clear!", ENGINE_NONE, {ENGINE_NONE}, 0, ParticlesScriptClear, "remove every live particle immediately"},
};
const EngineType CoreParticlesType = {
    .name = "particles",
    .size = sizeof(CoreParticlesScript),
    .properties = particlesProperties,
    .propertyCount = sizeof particlesProperties / sizeof particlesProperties[0],
    .methods = particlesMethods,
    .methodCount = sizeof particlesMethods / sizeof particlesMethods[0],
    .createArguments = {ENGINE_INT},
    .createArgumentCount = 1,
    .createRequired = 0,
    .create = ParticlesScriptCreate,
    .destroy = ParticlesScriptDestroy,
    .help = "a pool of physically simulated billboard particles, emitted from a template",
};
