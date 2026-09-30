;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; The carried item of proposal C4 over the network. The carryable is examples/carried-item's,
;;;; unchanged but for one print in its orphaned handler; the soldier is the part of the example's
;;;; soldier that carrying needs (the hand socket, reaching, got and grab-failed); and carrier is a
;;;; test-only behaviour in place of a player at the keys: from its tick handler it walks to the
;;;; nearest carryable with no parent, sends grab within 1.5 m, and after holding it for hold-for
;;;; seconds drops it 3 m ahead with (send item 'drop point normal yaw), as the example's soldier
;;;; does. From still-at seconds it stands still, and every machine prints its report once a second.
;;;; tests/regression/runner_checks.c runs a host and two clients of it and compares the last report
;;;; each machine prints; for the leaving check it appends new values of hold-for and still-at.

(define reach 2.5)                                    ; as in the example
(define hold-for 2.0)                                 ; seconds a carrier holds what it got
(define still-at 38.0)                                ; (2400 - 120) / 60: the last 120 ticks are still

;;; ---- the example's carryable (examples/carried-item/carried-item.scm), with one print in
;;; orphaned ---------------------------------------------------------------------------------------

(define-kind carryable
  (is area :radius 0.3)
  (field label "crate")
  (field number 0)                                    ; test only: the same name on every machine
  (field holder (ref soldier))
  (child look (model "crate.glb"))

  (on (grab by)
    (cond ((and (not holder) (< (vdistance (world-position self) (by 'position)) (* 1.5 reach)))
           (set! holder by)
           (attach! self (child by 'hand) :at (vec3 0 0 0) :rotation (vec3 0 0 0))
           (send by 'got self))
          (else (send by 'grab-failed self))))

  (on (ungrab by)
    #t)

  (on (drop point normal yaw)
    (when holder
      (set! holder #f)
      (detach! self :at point :up normal :yaw yaw)))

  (on (orphaned)
    (set! holder #f)
    (let ((ground (raycast (world-position self) (vec3 0 -1 0) 5.0)))
      (when ground (detach! self :at (hit-point ground) :up (hit-normal ground) :yaw 0.0)))
    ;; test only: which machine ran it (handlers run only where their thing is owned), where the
    ;; item ended, and that it hangs from nothing
    (format #t "orphaned item ~A on player ~A parent ~A y ~A~%" number (local-player) (parent self)
            (vy (world-position self))))

  (on (parent-changed was now)
    (when (and was now) (play-sound "pickup.wav" :at (world-position self)))
    (when (and was (not now)) (play-sound "drop.wav" :at (world-position self)))))

(define-kind crate (is carryable) (field label "CRATE"))
(define-kind medkit-box (is carryable) (field label "MEDKIT") (child look (model "medkit.glb")))

;;; ---- the soldier: its hand, and what it counts ------------------------------------------------

(define-kind soldier
  (is character :radius 0.35 :height 1.8)
  (field player 0)                                    ; test only: who it was spawned for
  (field yaw 0.0)
  (field reaching (ref carryable))
  (field sent 0) (field got 0) (field failed 0) (field drops 0)
  (child eye (camera :at (vec3 0 1.6 0) :fov 75 :for-owner #t)
    (child arms (model "arms.glb" :for-owner #t :viewmodel #t)))
  (child body (model "soldier.glb" :animation 'idle :hidden-for-owner #t))
  (child hand (socket :bone "hand.R" :of (arms body)))

  (on (grab-failed item)
    (set! failed (+ failed 1))
    (when (eq? item reaching) (set! reaching #f)))

  (on (got item)
    (set! got (+ got 1))
    (when (eq? item reaching) (set! reaching #f))))

;;; ---- the test-only carrier --------------------------------------------------------------------

(define (flat v) (vec3 (vx v) 0 (vz v)))
(define (inside x) (clamp x 2.0 30.0))

(define-kind carrier
  (is soldier)
  (field held-for 0.0)
  (field waited 0.0)
  (field shown -1 :local)                             ; the item number last printed as in hand

  (on (tick dt)
    (set! velocity (vec3 0 0 0))
    (when (< (tick-time) still-at)
      (let ((item (first-child hand)))
        (cond
          (item
           (set! held-for (+ held-for dt))
           (when (>= held-for hold-for)
             (let ((ahead (v+ position (rotate-y (vec3 0 0 -3) yaw))))
               (set! held-for 0.0)
               (set! drops (+ drops 1))
               (send item 'drop (vec3 (inside (vx ahead)) 0.3 (inside (vz ahead))) (vec3 0 1 0) yaw)
               (set! yaw (+ yaw 2.4)))))
          (reaching                                   ; asked, not answered yet
           (set! waited (+ waited dt))
           (when (> waited 3.0)
             (send reaching 'ungrab self)
             (set! reaching #f)))
          (else
           (let ((target (nearest 'carryable position :where (lambda (c) (not (parent c))))))
             (when target
               (let ((to (flat (v- (world-position target) position))))
                 (if (< (vlength to) 1.5)
                     (begin
                       (set! reaching target)
                       (set! waited 0.0)
                       (set! sent (+ sent 1))
                       (send target 'grab self))
                     (set! velocity (vscale (vnormalize to) 3.0)))))))))
      (move-and-slide!)))

  ;; On the machine that plays this carrier: what is in its hand, each time that changes, so the
  ;; last such line of a player who left says what it was carrying.
  (on (frame dt)
    (when (= player (local-player))
      (let* ((item (first-child hand)) (n (if item (item 'number) 0)))
        (unless (= n shown)
          (set! shown n)
          (format #t "hand player ~A item ~A~%" player n))))))

;;; ---- the game: three carryables, a carrier per player, and the report --------------------------

(define (root-of t) (let up ((t t)) (if (parent t) (up (parent t)) t)))
(define (holder-of item)                              ; the player whose soldier it hangs from, or 0
  (let ((r (root-of item))) (if (is? r 'soldier) (r 'player) 0)))

(define-kind game
  (field reported 0 :local)                           ; the last whole second reported
  (child floor (tilemap :width 16 :depth 16))
  (on (start)
    ;; Players 2 and 3 start as far from item 1 as each other (their carriers stand at x 16 and 20,
    ;; z 8), so their first grabs can meet there (C4's trace 1); player 1's, at x 12, arrives later.
    (spawn 'crate :at (vec3 18 0.3 12) :number 1)
    (spawn 'medkit-box :at (vec3 10 0.3 20) :number 2)
    (spawn 'crate :at (vec3 22 0.3 20) :number 3))
  (on (player-joined p)
    (spawn 'carrier :owner p :player p :at (vec3 (+ 8 (* 4 p)) 0 8)))
  (on (player-left p)                                 ; after the leaver's guests' orphaned (B3.7)
    (for-each (lambda (c) (format #t "after player-left ~A item ~A holder ~A parent ~A~%"
                                  p (c 'number) (holder-of c) (parent c)))
              (things 'carryable)))
  ;; Every machine reports once a second from the moment the carriers stand still: each player's
  ;; counts, then who holds items 1, 2 and 3 (0: on the floor). The runner reads the last report.
  (on (frame dt)
    (let ((second (truncate (tick-time))))       ; floor names the tilemap here
      (when (and (>= (tick-time) still-at) (> second reported))
        (set! reported second)
        (for-each (lambda (s)
                    (format #t "carry player ~A sent ~A got ~A failed ~A drops ~A~%"
                            (s 'player) (s 'sent) (s 'got) (s 'failed) (s 'drops)))
                  (things 'soldier))
        (format #t "carry holders ~A~%"
                (map (lambda (n)
                       (let loop ((cs (things 'carryable)))
                         (cond ((null? cs) 'gone) ((= ((car cs) 'number) n) (holder-of (car cs)))
                               (else (loop (cdr cs))))))
                     '(1 2 3)))))))
