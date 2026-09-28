/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_NET_CLOCK_H
#define CORE_NET_CLOCK_H

#include "object.h"
#include <stdbool.h>
#include <stdint.h>

/* Two rates, not one. A server simulates in fixed steps and sends world snapshots at its own,
   lower rate -- Source runs 66 ticks per second and sends about 20 snapshots, and never sends more
   snapshots than it has simulated ticks. Sending on every arriving packet instead makes the traffic
   grow with the square of the player count and ties the world's cadence to whoever is talking. */
#define CORE_NET_TICK_RATE 60
#define CORE_NET_SEND_RATE 20
/* How far behind the newest snapshot a client draws, as a multiple of the snapshot interval. Two
   means a snapshot can be lost outright and there is still a later one to interpolate toward. */
#define CORE_NET_INTERP_SNAPSHOTS 2.0
/* How hard to retime playback when the drawn moment has drifted from where it should be, and how
   far from normal speed that is ever allowed to push it. Ten percent is imperceptible; a jump of
   the same size is not. */
#define CORE_NET_RETIME_GAIN  0.10
#define CORE_NET_RETIME_LIMIT 0.10

/** @brief Monotonic seconds, for driving a clock where there is no frame to take a delta from.
 *
 * A dedicated server has no window, so raylib's GetTime -- which the windowed engine loop uses --
 * is not available to it. This is the same measurement without one: monotonic, so it never jumps
 * when the wall clock is adjusted, and with an arbitrary origin, so only differences mean anything.
 * @return Seconds since an unspecified point, increasing. */
double CoreNetClockNow(void);

typedef struct CoreNetClock
{
    double tickInterval, sendInterval;
    double tickAccumulator, sendAccumulator;
    uint32_t tick;
} CoreNetClock;

/** @brief Starts a fixed-step server clock.
 * @param clock Caller-owned clock, overwritten.
 * @param tickRate Simulation steps per second; must be positive.
 * @param sendRate Snapshots per second; clamped to at most tickRate, since a snapshot of a step
 * that has not been simulated has nothing new in it.
 * @return True on success; false when an argument is unusable, leaving the clock zeroed. */
bool CoreNetClockInit(CoreNetClock *clock, int tickRate, int sendRate);

/** @brief Adds real time and reports how many fixed steps are now due.
 *
 * Time is banked rather than rounded, so a frame that takes longer than one step runs several and a
 * short frame runs none; the simulation keeps its fixed rate whatever the frame rate does.
 * @param clock Clock to advance.
 * @param dt Seconds since the last call; negative values are ignored.
 * @param maxTicks Ceiling on the steps returned, so a long stall cannot demand an unbounded catch
 * up; the unrun time is discarded rather than accumulating into a spiral.
 * @return Number of fixed steps to run now. Call CoreNetClockTicked once per step run. */
int CoreNetClockAdvance(CoreNetClock *clock, double dt, int maxTicks);

/** @brief Counts one simulated step.
 * @param clock Clock to advance; its tick number increases by one.
 * @return The tick just simulated. */
uint32_t CoreNetClockTicked(CoreNetClock *clock);

/** @brief Says whether a snapshot is due, consuming the slot when it is.
 * @param clock Clock to ask; call once per frame after advancing.
 * @return True at most once per send interval. */
bool CoreNetClockShouldSend(CoreNetClock *clock);

/* A client's estimate of where the server is, and of the moment it should be drawing.

   The client draws the world in the past -- far enough back that both snapshots bracketing the
   moment it is drawing have arrived. Source computes that as render time = now minus an
   interpolation delay, with the delay the larger of a floor and a multiple of the snapshot
   interval; Gaffer On Games arrives at the same place by asking that two snapshots in a row can be
   lost and there still be something to interpolate toward. */
typedef struct CoreNetInterpolator
{
    double tickInterval, delayTicks;
    double latestTick;    /* newest server tick seen, in ticks */
    double renderTick;    /* what to sample at; trails latestTick by delayTicks */
    double rate;          /* playback speed, nudged to close drift without moving the render point */
    bool started;
} CoreNetInterpolator;

/** @brief Starts a client interpolation clock.
 * @param interp Caller-owned clock, overwritten.
 * @param tickRate The server's simulation rate, which both sides agree on.
 * @param sendRate The server's snapshot rate, which sets how far back to draw.
 * @return True on success; false when an argument is unusable, leaving the clock zeroed. */
bool CoreNetInterpolatorInit(CoreNetInterpolator *interp, int tickRate, int sendRate);

/** @brief Records the server tick of a snapshot as it arrives.
 * @param interp Clock to update.
 * @param tick The snapshot's server tick. Ticks older than the newest seen are ignored, so a packet
 * that overtook another cannot drag the clock backwards.
 * @return No value. */
void CoreNetInterpolatorSnapshot(CoreNetInterpolator *interp, uint32_t tick);

/** @brief Advances the moment being drawn by real time.
 *
 * The render point runs forward on the client's own clock between snapshots. When it drifts from
 * where it should be, playback is retimed -- run a little fast or a little slow until it lines up
 * -- rather than the point being moved, because moving it makes the whole world jump by the
 * correction. It is snapped only when a long way out, which is a join or a stall, not jitter.
 * @param interp Clock to advance.
 * @param dt Seconds since the last call; negative values are ignored.
 * @return No value. */
void CoreNetInterpolatorAdvance(CoreNetInterpolator *interp, double dt);

/** @brief The server tick to sample replicated objects at.
 * @param interp Borrowed clock.
 * @return A fractional server tick, for CoreNetObjectSampleAt. Zero before the first snapshot. */
double CoreNetInterpolatorRenderTick(const CoreNetInterpolator *interp);

/* The two clocks as engine types. "net-clock" is created with a tick rate and a send rate:
   advance!, ticked!, should-send? and tick. "net-interpolator" likewise: snapshot!, advance! and
   render-tick. */
extern const EngineType CoreNetClockType;
extern const EngineType CoreNetInterpolatorType;

#endif
