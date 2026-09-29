;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 3: gameplay code reads the wall clock.
(define-kind soldier
  (field started 0.0)
  (on (tick dt) (set! started (real-time))))

(spawn 'soldier)
