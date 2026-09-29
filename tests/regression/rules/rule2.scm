;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 2: a handler changes a top-level definition, frozen once the game has loaded.
(define score 0)

(define-kind soldier
  (on (tick dt) (set! score (+ score 1))))

(spawn 'soldier)
