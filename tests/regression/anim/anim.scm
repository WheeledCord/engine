;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Animation playback and a socket that follows a bone in drawing only (docs/developer/store.md §3):
;;;; the skinned test rig (tests/regression/assets/rig/test_rig.gltf) waves, and a box hangs in a
;;;; socket on its hand.R bone. Run by tests/regression/runner_checks.c in a window with
;;;; --shot-every 30 --print-draw-position box: the runner prints where the box was drawn, and the
;;;; tick handler below prints where gameplay reads it, which must not move (rule 1).

(define-kind rig
  (is node)
  (child body (model "../assets/rig/test_rig.gltf" :animation 'wave))
  ;; At the hand's rest position, so the rest pose and gameplay agree on where the hand is.
  (child hand (socket :bone "hand.R" :of (body) :at (vec3 0 2 0))))

(define-kind box (is model :tint (vec3 1 0.8 0.2)))  ; no mesh: the runner's 0.25 m cube

(define-kind game
  (field r (ref rig))
  (field b (ref box))
  (field t 0)
  (on (start)
    (set! r (spawn 'rig :at (vec3 0 0 -2)))
    (set! b (spawn 'box))
    (attach! b (child r 'hand) :at (vec3 0 0 0))
    ;; A clip the file does not have: the runner warns once and holds the rest pose.
    (set! ((child (spawn 'rig :at (vec3 -4 0 -2)) 'body) 'animation) 'dance))
  (on (player-joined p) #t)
  (on (tick dt)
    (set! t (+ t 1))
    (when (memv t '(30 60))
      (let ((p (world-position b)))
        (format #t "gameplay-position box ~A ~A ~A ~A~%" t (vx p) (vy p) (vz p))))))
