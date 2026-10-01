;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; B5.1, E5 T4: a weak table's contents depend on when the collector runs.
(define-kind soldier
  (field count 0)
  (on (tick dt)
    (let ((h (make-weak-hash-table)))
      (hash-table-set! h (list 1) 1)
      (set! count (hash-table-entries h)))))

(spawn 'soldier)
