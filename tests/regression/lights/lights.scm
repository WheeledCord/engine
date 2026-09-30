;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Point lights (docs/developer/store.md §3.1): a dark room (ambient 0.05, a directional light of
;;;; energy 0, since with none the runner lights the world with a default sun) seen from 12 m straight
;;;; above (16 0 16), with five point lights of range 2 on the floor's side. Four are the nearest the
;;;; camera: a (over the middle), b (over a skinned rig tinted magenta), c and d; e, the farthest,
;;;; is left out. Run by tests/regression/present_checks.c in a window: at tick 30 the lights are on,
;;;; and from tick 31 every energy is 0.

(define-kind room (is tilemap :width 16 :depth 16 :cell-size 2 :height 30))
(define-kind lamp (is light :type 'point :range 2 :energy 4))
(define-kind rig
  (is node)
  (child body (model "../assets/rig/test_rig.gltf" :animation 'idle :tint (vec3 1 0 1))))
(define-kind eye (is camera :for-owner #t :fov 60))

(define-kind game
  (field t 0)
  (on (start)
    (spawn 'room)
    (spawn 'light :type 'ambient :energy 0.05)
    (spawn 'light :type 'directional :energy 0)
    (spawn 'eye :owner 1 :at (vec3 16 12 16) :rotation (vec3 -1.5707964 0 0))
    (spawn 'rig :at (vec3 12 0 16))
    (spawn 'lamp :at (vec3 16 1 16))                         ; a: 11 m from the camera
    (spawn 'lamp :at (vec3 12 3 16) :range 3 :energy 3)      ; b: 9.8 m, over the rig
    (spawn 'lamp :at (vec3 16 1 10))                         ; c: 12.5 m
    (spawn 'lamp :at (vec3 10 1 22))                         ; d: 13.9 m
    (spawn 'lamp :at (vec3 27 1 21)))                        ; e: 16.3 m, the fifth
  (on (player-joined p) #t)
  (on (tick dt)
    (set! t (+ t 1))
    (when (= t 31)
      (for-each (lambda (l) (set! (l 'energy) 0)) (things 'lamp)))))
