;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 4: a handler stores a procedure in a field.
(define-kind grenade
  (field on-hit 'none)
  (on (tick dt) (set! on-hit (lambda () 'boom))))

(spawn 'grenade)
