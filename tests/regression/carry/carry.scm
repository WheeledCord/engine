;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; What carrying needs from the Scheme layer (proposal B1, B3.7, B6, C4), on one machine: a socket
;;;; declared with :of (arms body), a soldier with one given to another player, aimed-at from a
;;;; handler, raycast ignoring a list, attach! and detach! with their placements, first-child,
;;;; parent-changed, orphaned, a socket with no bone lookup at its local transform, and a box area.
;;;; Run by tests/regression/runner_checks.c with --headless --present; every line it checks is
;;;; printed as "check NAME #t" or "event ...", and a #f is a failure.

(define-actions (interact "E"))

(define (check name ok) (format #t "check ~A ~A~%" name (if ok #t #f)))
(define (refused? thunk) (catch #t (lambda () (thunk) #f) (lambda args #t)))
(define (message-of thunk)
  (catch #t (lambda () (thunk) "") (lambda (type info) (if (pair? info) (apply format #f info) ""))))
(define (near? a b) (< (vdistance a b) 1e-4))
(define (name-of t) (if t (kind-of t) #f))

(define-kind soldier
  (is character :radius 0.35 :height 1.8)
  (child eye (camera :at (vec3 0 1.6 0) :for-owner #t)
    (child arms (model "arms.glb" :for-owner #t :viewmodel #t)))
  (child body (model "soldier.glb" :hidden-for-owner #t))
  (child hand (socket :bone "hand.R" :of (arms body) :at (vec3 0.3 1.0 -0.4)))
  (on (tick dt)
    (when (= (tick-time-ticks) 3)
      (let ((c (car (things 'crate))))
        (check 'aimed-at-in-reach (eq? (aimed-at 'crate 2.5) c))
        (check 'aimed-at-out-of-reach (not (aimed-at 'crate 1.0)))
        (check 'aimed-at-with-camera (eq? (aimed-at eye 'crate 2.5) c)))))
  (on (drop-it item point yaw)                        ; the holder owns what it holds
    (send item 'drop point (vec3 0 1 0) yaw))
  (on (let-go item)
    (detach! item :keep-world #t))
  (on (leave)
    (remove self)))

(define (tick-time-ticks) (round (* (tick-time) 60)))

(define-kind crate
  (is area :radius 0.3)
  (on (drop point normal yaw)
    (detach! self :at point :up normal :yaw yaw))
  (on (orphaned)
    (format #t "event orphaned ~A owner-root ~A~%" (name-of (parent self)) (vy (world-position self)))
    (detach! self :at (vec3 9 0.3 9) :up (vec3 0 1 0) :yaw 0.0))
  (on (parent-changed was now)                        ; presentation: it may not write shared state
    (format #t "event parent-changed ~A ~A~%" (name-of was) (name-of now))
    (check 'parent-changed-is-presentation (refused? (lambda () (set! radius 1.0))))))

;; A box area (B6): the sphere its default radius of 1 gives would not reach x 21.7, the box does.
(define-kind shelf
  (is area :shape (box 4 1 1))
  (field touches 0)
  (field leaves 0)
  (on (touched other) (set! touches (+ touches 1)))
  (on (untouched other) (set! leaves (+ leaves 1))))

(define-kind walker (is character))

(define-kind bag                                       ; a list setting on a spawn given away
  (is node)
  (field tags (list-of symbol :max 2)))

(define-kind game
  (field t 0)
  (field s (ref soldier))
  (field s2 (ref soldier))
  (field c (ref crate))
  (field c3 (ref crate))
  (field sh (ref shelf))
  (field w (ref walker))
  (on (start)
    (set! s (spawn 'soldier :owner 1 :at (vec3 5 0 5)))
    ;; Given to a player who is not here: its socket's :of was written by the spawn anyway.
    (set! s2 (spawn 'soldier :owner 2 :at (vec3 12 0 5)))
    (set! c (spawn 'crate :at (vec3 5 1.6 3.5)))
    (set! c3 (spawn 'crate :owner 1 :at (vec3 2 0.3 2))))
  (on (player-joined p) #t)
  (on (tick dt)
    (set! t (+ t 1))
    (case t
      ((1)
       (check 'socket-of-names (equal? ((child s 'hand) 'of) '(arms body)))
       (check 'socket-of-given-away (equal? ((child s2 'hand) 'of) '(arms body)))
       (check 'socket-of-not-owner-refused (refused? (lambda () (set! ((child s2 'hand) 'of) '(body)))))
       (check 'first-child-empty (not (first-child (child s 'hand))))
       (set! sh (spawn 'shelf :at (vec3 20 0.5 20)))
       (set! w (spawn 'walker :at (vec3 21.7 0 23)))
       (check 'box-kind-setting (and (eq? (sh 'shape) 'box) (near? (sh 'size) (vec3 4 1 1))))
       (let ((a (spawn 'area :shape (box 1 2 3))))
         (check 'box-spawn-setting (and (eq? (a 'shape) 'box) (near? (a 'size) (vec3 1 2 3))))
         (remove a))
       (check 'shape-unknown-refused
              (and (string-position "cylinder" (message-of (lambda () (spawn 'area :shape 'cylinder))))
                   (string-position "cone" (message-of (lambda () (set! (sh 'shape) 'cone))))
                   (eq? (sh 'shape) 'box)))
       (check 'spawn-list-given-away (equal? ((spawn 'bag :owner 2 :tags '(a b)) 'tags) '(a b)))
       (check 'spawn-list-over-capacity-refused (refused? (lambda () (spawn 'bag :owner 2 :tags '(a b c)))))
       (check 'aimed-at-needs-a-camera
              (string-position "has none" (message-of (lambda () (aimed-at 'crate 2.0)))))
       ;; A ray from the soldier's eye along +x meets s2 unless s2 is in the ignore list too.
       (let ((from (world-position (child s 'eye))))
         (check 'raycast-ignore-list-hits (eq? (hit-thing (raycast from (vec3 1 0 0) 20 :ignore (list s))) s2))
         (check 'raycast-ignore-list-skips (not (raycast from (vec3 1 0 0) 20 :ignore (list s s2))))
         (check 'raycast-ignore-list-refuses-a-number (refused? (lambda () (raycast from (vec3 1 0 0) 20 :ignore (list 5)))))))
      ((3)
       (teleport! w (vec3 21.7 0 20)))
      ((5)
       (check 'box-area-touched (and (= (sh 'touches) 1) (= (sh 'leaves) 0)
                                     (equal? (overlapping sh 'walker) (list w))))
       (teleport! w (vec3 21.7 0 23))
       (attach! c (child s 'hand) :at (vec3 0 0 0))
       (check 'first-child-holds (eq? (first-child (child s 'hand)) c))
       (check 'socket-local-transform (near? (world-position c) (vec3 5.3 1.0 4.6))))
      ((8)
       (check 'box-area-untouched (and (= (sh 'touches) 1) (= (sh 'leaves) 1) (null? (overlapping sh 'walker))))
       (send s 'drop-it c (vec3 7 0.3 7) 1.5))
      ((10)
       (check 'dropped-at (and (not (parent c)) (near? (c 'position) (vec3 7 0.3 7))
                               (< (abs (- (vy (c 'rotation)) 1.5)) 1e-4)))
       (check 'detach-root-no-placement-refused (refused? (lambda () (detach! c))))
       (check 'detach-root-not-owner-refused (refused? (lambda () (detach! c3 :at (vec3 0 0 0)))))
       (detach! c :at (vec3 8 0.3 8))
       (check 'detach-root-placed (near? (c 'position) (vec3 8 0.3 8)))
       (attach! c (child s 'hand) :at (vec3 0 0 0)))
      ((12)
       (check 'detach-held-not-owner-refused (refused? (lambda () (detach! c :keep-world #t))))
       (send s 'let-go c))
      ((13)
       (check 'keep-world (and (not (parent c)) (near? (c 'position) (vec3 5.3 1.0 4.6)))))
      ((14)
       (attach! c (child s 'hand) :at (vec3 0 0.5 0)))
      ((16)
       (send s 'leave))
      ((17)
       (check 'orphan-detached (and (not (parent c)) (near? (c 'position) (vec3 5.3 1.5 4.6)))))
      ((19)
       (check 'orphaned-seated (near? (c 'position) (vec3 9 0.3 9))))))
  (on (draw-hud)
    (when (= t 2)
      (check 'draw-ring-fill (draw-ring 10 10 5 0.5 :color 'white))
      (check 'draw-ring-fill-refuses-a-symbol (refused? (lambda () (draw-ring 10 10 5 'half)))))))
