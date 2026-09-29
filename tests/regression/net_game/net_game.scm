;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; The smallest networked game (docs/developer/store.md §9.6): each player gets a walker on an open
;;;; 16 x 16 floor, the host rolls five balls along x for ten seconds, and every walker says hello to
;;;; the game once. tests/regression/runner_checks.c runs a host and two clients of it over loopback
;;;; and compares their state hashes and the host's count of hellos.

(define-actions (left "A") (right "D") (forward "W") (back "S"))

(define-kind walker
  (is character)
  (on (start)                                  ; runs on the walker's owner: a client's comes over the wire
    (send (game) 'hello (local-player)))
  (on (tick dt)
    (let ((v (input-vector 'left 'right 'forward 'back)))
      (set! velocity (vec3 (* 3 (vx v)) 0 (* 3 (vz v))))
      (move-and-slide!))))

(define-kind ball
  (is node)
  (child look (model :local #t))               ; spawned on each machine as the ball arrives
  (on (tick dt)
    (when (< (tick-time) 10.0)
      (set! position (v+ position (vec3 0.1 0 0))))))

(define-kind game
  (field hellos 0)
  (child floor (tilemap :width 16 :depth 16))
  (on (start)
    (do ((i 0 (+ i 1))) ((= i 5))
      (spawn 'ball :at (vec3 2 0.5 (+ 4 (* 4 i))))))
  (on (player-joined p)
    (spawn 'walker :owner p :at (vec3 (+ 4 (* 3 p)) 0 12)))
  (on (hello p)
    (set! hellos (+ hellos 1))))
