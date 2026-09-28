/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_PARTICLES_H
#define CORE_PARTICLES_H

#include "object.h"
#include "raylib.h"
#include <stdbool.h>

/* A pool of physically simulated, camera-facing billboards: sparks, dust, smoke, anything a game
   throws off a point and lets age out. The pool owns the physics (gravity, drag, curl-noise
   turbulence, an optional pull toward an anchor, rotation and a spin-up ramp) and a default
   billboard look; a game supplies the rest -- textures, colours, what a kind means, and two
   optional hooks. `step` runs after a particle has moved, so a game can add its own behaviour
   (sticking to a surface, changing kind, spawning something else) and remove the particle by
   answering false. `look` runs while drawing, so a game can replace a particle's size, colour, the
   billboard's up vector (to stretch it along velocity) or its rotation, or skip it by answering
   false. */

typedef enum CoreParticleBlend
{
    CORE_PARTICLE_ALPHA,
    CORE_PARTICLE_ADDITIVE
} CoreParticleBlend;

typedef struct CoreParticle
{
    Vector3 position, velocity;
    float age, life; /* seconds */
    float size0, size1; /* billboard size, lerped over life */
    Color color0, color1; /* tint, lerped over life */
    float gravity; /* downward accel, m/s^2; positive falls */
    float drag; /* velocity damping per second */
    float rotation, rotationSpeed; /* billboard Z-rotation and its speed, in degrees */
    float spinUp; /* rotationSpeed is multiplied by 1 + spinUp*age/life, for a spin that ramps up */
    Vector3 turbulenceStrength; /* per-axis curl-noise strength added to velocity; zero is none */
    float turbulenceScale; /* world-to-noise-space scale the position is sampled at */
    Vector3 anchor, pull; /* per-axis pull of velocity toward anchor, applied after turbulence */
    Texture2D texture;
    CoreParticleBlend blend;
    int kind; /* the game's tag; the engine never interprets it */
} CoreParticle;

/* A fixed-capacity array of particles, live ones packed at the front. A caller that owns the array
   itself may set items/capacity directly instead of calling CoreParticlesInit, and must then not
   call CoreParticlesFree. clock is the pool's own elapsed time, advanced by CoreParticlesUpdate and
   fed to the turbulence noise in place of a wall clock, so two pools never fight over one phase. */
typedef struct CoreParticles
{
    CoreParticle *items;
    int count, capacity;
    float clock;
} CoreParticles;

/** @brief Allocates a pool's particle storage.
 * @param pool Storage to initialize.
 * @param capacity Maximum live particles; must be more than zero.
 * @return True on success; false leaves pool safe to free. */
bool CoreParticlesInit(CoreParticles *pool, int capacity);

/** @brief Releases a pool's particle storage.
 * @param pool Pool allocated by CoreParticlesInit; NULL is accepted. A pool whose storage the
 * caller owns directly must not be passed here.
 * @return No value. */
void CoreParticlesFree(CoreParticles *pool);

/** @brief Adds one particle, copied from the caller's description.
 * @param pool Pool to add to.
 * @param particle Particle values to copy in; age is not reset by this call.
 * @return The new particle's storage, or NULL when the pool is at capacity. */
CoreParticle *CoreParticleEmit(CoreParticles *pool, const CoreParticle *particle);

/* Runs after a particle has aged, been pushed by gravity/drag/turbulence/pull, turned and moved.
   previous is where it was before this update's movement. Returning false removes it, by swapping
   in the last live particle -- the same removal every particle sees, so a game cannot tell whether
   this or expiry removed it. */
typedef bool (*CoreParticleStepFn)(CoreParticle *particle, Vector3 previous, float dt, void *user);

/** @brief Advances every particle by one step: ages it, removing it when its life is up; applies
 * gravity, drag, turbulence and the anchor pull to velocity; turns it; moves it; then, if step is
 * not NULL, calls step and removes the particle when step answers false.
 * @param pool Pool to advance.
 * @param dt Seconds to advance.
 * @param step Optional per-particle hook run after the particle has moved; NULL skips it.
 * @param user Passed through to step unchanged.
 * @return No value. */
void CoreParticlesUpdate(CoreParticles *pool, float dt, CoreParticleStepFn step, void *user);

/* What a particle draws as: size and color already default to the lerped values, up to the world
   Y axis, and rotation to the particle's own. look may change any of them, or answer false to skip
   drawing this particle. */
typedef struct CoreParticleLook
{
    Vector2 size;
    Color color;
    Vector3 up;
    float rotation;
} CoreParticleLook;
typedef bool (*CoreParticleLookFn)(const CoreParticle *particle, float t, CoreParticleLook *out,
                                   void *user);

/** @brief Draws every particle as a billboard, depth-tested against the scene but not writing
 * depth, in one alpha-blended pass followed by one additive pass, each in pool order.
 * @param pool Pool to draw.
 * @param camera Camera the billboards face.
 * @param look Optional per-particle look hook; NULL draws every particle with its default look.
 * @param user Passed through to look unchanged.
 * @return No value. */
void CoreParticlesDraw(const CoreParticles *pool, Camera camera, CoreParticleLookFn look, void *user);

/* A particle pool as an engine type, "particles", created with an optional capacity (default 512):
   count (read-only), and a template a script sets before emitting -- life, size-start, size-end,
   gravity, drag. emit! copies the template with a given position and velocity; clear! empties the
   pool. Drawing is a caller-owned pool and camera, so it is not reachable from a script. */
extern const EngineType CoreParticlesType;

#endif
