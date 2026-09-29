;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Tick handlers that never end, like swat-tower's populate once did: their do loop's end test is
;;;; never true. The handler time limit (proposal B2.6) stops them every tick: the rusher's loop
;;;; calls an engine function, the shooter's a helper of its own.
(define-kind rusher
  (field ticks 0)
  (field last 0)
  (on (tick dt)
    (set! ticks (+ ticks 1))
    (do ((i 0 (+ i 1))) ((< i 0))
      (set! last (random 100)))))

(define-kind shooter
  (field ticks 0)
  (define (twice n) (+ n n))
  (on (tick dt)
    (set! ticks (+ ticks 1))
    (do ((i 0 (+ i 1))) ((< i 0))
      (twice i))))

(spawn 'rusher)
(spawn 'shooter)
