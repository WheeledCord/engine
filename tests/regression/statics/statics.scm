;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Static batching (docs/developer/store.md §3.1): 200 :static crates of two tints on a 24x24-cell
;;;; tilemap (3x3 regions of 8x8 cells), seen from above. Run by tests/regression/present_checks.c
;;;; in a window with --bench, with and without --no-static-batch: the batched run draws far fewer
;;;; draws than it has crates and the same pixels. At tick 45 one static crate is moved (the error
;;;; naming :static) and a loose crate too (no error); at tick 50 a static crate is removed. Also at
;;;; tick 45: a plain node carrying a static crate is moved (the error naming that crate: an ancestor
;;;; moved it), and a green static crate is attached to player 2's node on the right of the view, which
;;;; moves it there and makes it player 2's, not this machine's (no error: here that is its owner
;;;; moving it; it is batched again where it now stands). A handler here cannot write the position of
;;;; a thing another machine owns, and attach! is the one change that both moves it and hands it over.

(define-kind floor-map (is tilemap :width 24 :depth 24 :cell-size 2 :height 60))
(define-kind crate (is model :mesh "crate.obj" :static #t :tint (vec3 0.8 0.6 0.3)))
(define-kind blue-crate (is crate :tint (vec3 0.3 0.5 0.8)))
(define-kind green-crate (is crate :tint (vec3 0.1 0.9 0.1)))
(define-kind shelf (is node))
(define-kind loose-crate (is model :mesh "crate.obj" :tint (vec3 0.8 0.6 0.3)))
(define-kind eye (is camera :for-owner #t :fov 70))

(define-kind game
  (field mover (ref crate))
  (field doomed (ref crate))
  (field loose (ref loose-crate))
  (field stranger (ref green-crate))
  (field holder (ref shelf))
  (field carrier (ref shelf))
  (field t 0)
  (on (start)
    (spawn 'floor-map)
    (spawn 'eye :owner 1 :at (vec3 24 40 24) :rotation (vec3 -1.5707964 0 0))
    (do ((i 0 (+ i 1))) ((= i 200))
      (let ((c (spawn (if (even? i) 'crate 'blue-crate)
                      :at (vec3 (+ 1.5 (* 2.3 (modulo i 20))) 0 (+ 1.5 (* 4.6 (quotient i 20)))))))
        (when (= i 0) (set! mover c))
        (when (= i 101) (set! doomed c))))
    (set! loose (spawn 'loose-crate :at (vec3 3.8 0 3.8)))
    (set! stranger (spawn 'green-crate :at (vec3 8 3 24)))
    (set! holder (spawn 'shelf :owner 2 :at (vec3 40 3 24)))
    (set! carrier (spawn 'shelf :at (vec3 30 3 24)))
    (spawn 'crate :parent carrier))
  (on (player-joined p) #t)
  (on (tick dt)
    (set! t (+ t 1))
    (when (= t 45)
      (set! (mover 'position) (vec3 1.5 0 3.5))
      (set! (loose 'position) (vec3 3.8 0 5.0))
      (set! (carrier 'position) (vec3 30 3 26))
      (attach! stranger holder :at (vec3 0 0 0)))
    (when (= t 50)
      (remove doomed))))
