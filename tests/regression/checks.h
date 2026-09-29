/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef REGRESSION_CHECKS_H
#define REGRESSION_CHECKS_H

/* Checks kept in files of their own beside main.c. Each prints a "FAIL: ..." line per failed check
   and answers how many failed; main() adds that to its own count. The GL ones run under the hidden
   window main() opens before calling them. */

int DrawPathChecks(void);

#endif
