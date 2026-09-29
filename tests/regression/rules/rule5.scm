;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 5: player 1's soldier writes a field of player 2's.
(define-kind soldier
  (field health 100)
  (on (tick dt)
    (for-each (lambda (other) (unless (eq? other self) (set! (other 'health) 5)))
              (things 'soldier))))

(spawn 'soldier :owner 1)
(spawn 'soldier :owner 2)
