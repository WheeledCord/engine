;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; The viewmodel pass (docs/developer/store.md §3.1). Player 1's head stands 0.2 m from a wall,
;;;; looking at it, with a box (no mesh: the runner's magenta 0.25 m cube) held 0.4 m in front of
;;;; the eye, which is inside the wall. Run by tests/regression/present_checks.c in a window with
;;;; --shot-every 20: at tick 20 the box is an ordinary model and the wall hides it; from tick 21 it
;;;; is a viewmodel and is drawn over the wall (tick 40); from tick 41 the head turns round to face
;;;; two viewmodel boxes of player 2's, the left one :for-owner (not drawn here) and the right one
;;;; not (drawn like any model), with its own box hidden (tick 60).

(define-kind room (is tilemap :width 4 :depth 4 :cell-size 2 :height 3))
;; Player 1's things run their handlers for player 1, so the head changes itself.
(define-kind head
  (is node)
  (field t 0)
  (child eye (camera :for-owner #t :viewmodel-fov 60))
  (child held (model :for-owner #t :at (vec3 0 -0.1 -0.4)))
  (on (tick dt)
    (set! t (+ t 1))
    (when (= t 21)
      (set! (held 'viewmodel) #t))
    (when (= t 41)
      (set! (held 'visible) #f)
      (set! rotation (vec3 0 3.14159265 0)))))
(define-kind stranger (is model :viewmodel #t))

(define-kind game
  (on (start)
    (let ((m (spawn 'room)))
      (grid-fill-rect! (m 'cells) 0 0 4 1 1))           ; cells z 0 are solid: a wall face at z = 2
    (spawn 'head :owner 1 :at (vec3 3 1.5 2.2))
    (spawn 'stranger :owner 2 :for-owner #t :at (vec3 3.5 1.4 3.7))  ; left, seen from the far side
    (spawn 'stranger :owner 2 :at (vec3 2.5 1.4 3.7)))              ; right
  (on (player-joined p) #t))
