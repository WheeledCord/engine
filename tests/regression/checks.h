/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGRESSION_CHECKS_H
#define REGRESSION_CHECKS_H

/* Each module's checks are a plain function in tests/regression/<module>_checks.c. It prints
   "FAIL: <what>" for every check that fails and returns how many failed. main.c adds them up. */

/* A headless engine run (no window) and its tick timing. */
int HeadlessChecks(void);

/* Input routing between frames and updates, 3D and 2D transforms, 2D queries and the 2D camera. */
int SpaceChecks(void);

/* The draw path: keys, sort, cull, static batching, and a submit under the hidden window. */
int DrawPathChecks(void);

/* The skinned test rig (tests/regression/assets/rig/test_rig.gltf) as raylib loads it: bones by
   name and both clips. Needs the hidden window. */
int AnimationAssetChecks(void);

/* The store: kinds, fields, the tree, the tick order, rules, snapshot, hash, save and load. */
int StoreChecks(void);

/* The built-in 3D kinds: slide, areas, rays, paths, chunks, cached transforms, determinism. */
int World3DChecks(void);
/* The store's Scheme frontend: define-kind's code walk, the calls, and the five rule errors. */
int GameChecks(void);

/* The project runner: a headless bot session recorded and replayed to the same hash, another seed,
   recordings refused, and the store's pending commands. */
int GameRunnerChecks(void);
int NetInterruptChecks(void); /* only the Ctrl+C-on-a-client check (regression_test --net-interrupt) */
int NetChecks(void); /* networking on the store: host and two clients, in-memory and lossy (§9.7) */

/* What the runner draws and plays beyond models (§3.1): static batching, point lights, the viewmodel
   pass and sound emitters, from ./build/core/trench runs in a window and their screenshots. */
int PresentationChecks(void);

#endif
