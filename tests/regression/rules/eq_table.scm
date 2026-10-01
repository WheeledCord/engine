;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; B5.4: an eq? table iterates in memory order, so games can't make one.
(define-kind soldier
  (field seen 0)
  (on (tick dt)
    (let ((h (make-hash-table 8 eq?)))
      (hash-table-set! h 'a 1)
      (set! seen (length (map car h))))))

(spawn 'soldier)
