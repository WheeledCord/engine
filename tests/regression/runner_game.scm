;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; The smallest game the runner (gameplay/game.c) runs: an open 8 x 8 tilemap and a player who
;;;; walks and jumps on it. tests/regression/runner_checks.c records a bot session of it, replays it
;;;; and compares hashes; tests/regression/engine.project names it.

(define-actions (left "A") (right "D") (forward "W") (back "S") (jump "Space"))

(define-kind player
  (is character)
  (child eye (camera :at (vec3 0 1.6 0) :for-owner #t))
  (child body (model :at (vec3 0 0.9 0) :hidden-for-owner #t))
  (on (tick dt)
    (let ((v (input-vector 'left 'right 'forward 'back))
          (up (if (and on-floor (pressed? 'jump)) 5 (- (vy velocity) (* 9.8 dt)))))
      (set! velocity (vec3 (* 4 (vx v)) up (* 4 (vz v))))
      (move-and-slide!)))
  (on (draw-hud)
    (draw-text (format #f "~,2F ~,2F" (vx position) (vz position)) 10 10 :size 20 :color 'white)))

(define-kind game
  (child map (tilemap :width 8 :depth 8))
  (on (start)
    (spawn 'player :owner 1 :at (vec3 8 0.5 8))))
