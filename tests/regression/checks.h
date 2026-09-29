/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGRESSION_CHECKS_H
#define REGRESSION_CHECKS_H

/* Each module's checks are a plain function in tests/regression/<module>_checks.c. It prints
   "FAIL: <what>" for every check that fails and returns how many failed. main.c adds them up. */

/* A headless engine run (no window), and the network byte counters over loopback. */
int HeadlessChecks(void);

/* The draw path: keys, sort, cull, static batching, and a submit under the hidden window. */
int DrawPathChecks(void);

/* The store: kinds, fields, the tree, the tick order, rules, snapshot, hash, save and load. */
int StoreChecks(void);

/* The store's Scheme frontend: define-kind's code walk, the calls, and the five rule errors. */
int GameChecks(void);

#endif
