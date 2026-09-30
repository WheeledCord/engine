;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Sound emitters (docs/developer/store.md §3.1): a `sound` thing looping hum.wav (0.1 s of a
;;;; 100 Hz tone) that moves, changes volume, stops and is removed, beside one naming a file that is
;;;; not there. Run by tests/regression/present_checks.c in a window, where this container has no
;;;; audio device: the run must end cleanly with one warning per emitter and no error.

(define-kind hum (is sound :stream "hum.wav" :volume 0.8 :playing #t))

(define-kind game
  (field h (ref hum))
  (field t 0)
  (on (start)
    (spawn 'camera :owner 1 :for-owner #t :at (vec3 0 1.6 0))
    (set! h (spawn 'hum :at (vec3 5 1 -5)))
    (spawn 'hum :stream "missing.wav" :at (vec3 -5 1 -5)))
  (on (player-joined p) #t)
  (on (tick dt)
    (set! t (+ t 1))
    (when (= t 10) (set! (h 'position) (vec3 -5 1 -5)))
    (when (= t 20) (set! (h 'volume) 0.3))
    (when (= t 30) (set! (h 'playing) #f))
    (when (= t 40) (remove h))))
