;;;; A carried item, written against the PROPOSED Trench Engine API (proposal.md, third round).
;;;; Paper design: it does not run today. This is Trenchfoot's hard case, the one the medkit
;;;; example avoided: a crate you pick up, hold visibly in your hand, carry, and drop onto a
;;;; surface, in co-op, with two players grabbing it in the same tick, at 300 ms round trip.
;;;;
;;;; What each player sees, frame by frame, is in proposal.md part C4. The rules this file
;;;; relies on, all new in the third round (proposal B3.1, B3.5, B1):
;;;;   - A thing is owned by whoever owns the root of its tree. Reparenting is therefore how
;;;;     ownership moves, and the only way it moves. `attach!` and `detach!` are the two operations.
;;;;   - Lifetime is separate from ownership: a thing is removed with its spawner, not its owner.
;;;;     A thing attached under a soldier is a guest there; when that soldier goes, the guest is
;;;;     detached, not removed, and `(on (orphaned))` runs on its new owner.
;;;;   - A parent change is a jump for interpolation: no machine ever blends a world-space
;;;;     position into a hand-space one.
;;;;   - A `socket` is a node that follows a bone of a sibling model, so a thing under it rides
;;;;     the animation. For the local player it follows the first-person arms instead.
;;;;
;;;; Markers, as in swat-tower.scm:
;;;;   ;; RULE: ... where a rule got in the way and what it cost.
;;;;   ;; NET:  ... where co-op needed something single player would not.
;;;;   ;; FLAW: ... a design flaw this example exposed, fixed in the proposal (not here).

(define-actions (interact "E") (drop "Q"))
(define reach 2.5)                                    ; metres; how far you can grab or place
(define hold-time 0.5)                                ; the ring takes this long to fill (Trenchfoot: 0.5 s)

;;; ---------------------------------------------------------------------------------------------
;;; The soldier: one per player, owned by that player. Its hands are a socket. What it holds is
;;; whatever is attached under that socket, so there is no `held` field to keep in step with the
;;; tree: `(held)` reads the tree.

(define-kind soldier
  (is character :radius 0.35 :height 1.8)
  (field yaw 0.0)
  (field pitch 0.0)
  (field reaching (ref carryable))                    ; a grab we have asked for and not heard back on
  (field ring 0.0 :local)                             ; the hold ring on this screen only
  (child eye (camera :at (vec3 0 1.6 0) :fov 75 :for-owner #t)
    (child arms (model "arms.glb" :for-owner #t :viewmodel #t)))   ; first person, this machine only
  (child body (model "soldier.glb" :animation 'idle :hidden-for-owner #t))
  (child hand (socket :bone "hand.R" :of (arms body)))            ; follows whichever of the two is drawn here

  (define (held) (first-child hand))                  ; #f when the hand is empty

  (on (tick dt)
    (look)
    (walk dt)
    (let ((item (held)))
      (cond
        ;; Ask for the item the moment the key goes down, not when the ring fills: the round trip
        ;; then hides inside the 0.5 s hold. Trenchfoot asks at the end of the hold, which is
        ;; why its pickups feel late over a real link (research 0.8).
        ((and (not item) (not reaching) (pressed? 'interact))
         (let ((target (aimed-at 'carryable reach)))    ; nearest carryable under the crosshair, in reach
           (when target
             (set! reaching target)
             ;; NET: the crate belongs to the host while it lies free. Only its owner may decide who
             ;; gets it, so this is a message, and the answer is the crate arriving in our hand.
             (send target 'grab self))))
        ((and reaching (or (not (held? 'interact)) (not (aimed-at 'carryable reach))))
         (send reaching 'ungrab self)                   ; let go of the key or looked away: withdraw
         (set! reaching #f))
        ((and item (pressed? 'drop))
         ;; Where it lands is decided here, against the world as this machine sees it. The item is
         ;; ours while it is in our hand (it is under our root), so `drop` runs on this machine,
         ;; this tick, and we see it land at once.
         (let ((hit (raycast (world-position eye) (aim yaw pitch) reach :ignore (list self item))))
           (send item 'drop
                 (if hit (hit-point hit) (v+ (world-position hand) (v* (aim yaw pitch) 1.0)))
                 (if hit (hit-normal hit) (vec3 0 1 0))
                 yaw))))))

  (on (grab-failed item)                               ; someone else got there first
    (when (eq? item reaching) (set! reaching #f)))

  (on (got item)                                       ; the item is in our hand (sent by the item, B3.4 below)
    (when (eq? item reaching) (set! reaching #f)))

  (define (look)
    (let ((m (mouse-motion)))
      (set! yaw (- yaw (* 0.0025 (vx m))))
      (set! pitch (clamp (- pitch (* 0.0025 (vy m))) -1.4 1.4))
      (set! rotation (vec3 0 yaw 0))
      (set! (eye 'rotation) (vec3 pitch 0 0))))

  (define (walk dt)
    (let ((wish (rotate-y (input-vector 'left 'right 'forward 'back) yaw)))
      (set! velocity (vec3 (* 4.5 (vx wish)) (- (vy velocity) (* 20.0 dt)) (* 4.5 (vz wish))))
      (move-and-slide!)))

  ;; Presentation: the ring, and the arms.
  (on (frame dt)
    (set! ring (if reaching (min 1.0 (+ ring (/ dt hold-time))) 0.0)))
  (on (draw-hud)
    (when reaching (draw-ring (/ (screen-width) 2) (/ (screen-height) 2) 28 ring :color 'white))
    (let ((item (held)))
      (when item (draw-text (item 'label) 24 (- (screen-height) 48) :size 22)))))

;;; ---------------------------------------------------------------------------------------------
;;; The item. The host spawns crates into the world, so it owns them while they lie free. Held, a
;;; crate is a child of a hand socket, so it is owned by that soldier's player: its handlers run
;;; there, and only there. `holder` is not needed for ownership; it names who has it for game logic.

(define-kind carryable
  (is area :shape (box 0.4 0.3 0.4))                   ; aimed at and walked through, not collided with
  (field label "crate")
  (field holder (ref soldier))
  (child look (model "crate.glb"))

  ;; Runs on whoever owns the crate: the host while it is free, the holder while it is held.
  ;; Two grabs in the same tick arrive as two messages; the queue is first in, first out, and
  ;; arrival order is part of the recording, so the second always sees `holder` set.
  (on (grab by)
    (cond ((and (not holder) (< (vdistance (world-position self) (by 'position)) (* 1.5 reach)))
           (set! holder by)
           ;; The tree does the ownership hand-over. After this line the crate is under `by`'s
           ;; root, so `by`'s machine owns it: it will run this crate's handlers, draw it in the
           ;; hand with no lag, and decide the drop. This handler finishes on the old owner first.
           (attach! self (child by 'hand) :at (vec3 0 0 0) :rotation (vec3 0 0 0))
           (send by 'got self))
          (else (send by 'grab-failed self))))

  (on (ungrab by)                                      ; the player withdrew before it arrived: nothing to undo
    #t)

  ;; Runs on the holder's machine, which owns the crate while it is held. Detaching puts it under
  ;; the world root, so the host owns it again as soon as this state reaches the host.
  (on (drop point normal yaw)
    (when holder
      (set! holder #f)
      (detach! self :at point :up normal :yaw yaw)))    ; a jump: nobody interpolates from the hand to the floor

  ;; The holder left the game (or was removed) while carrying it. The engine has already detached
  ;; the crate at the hand's last world transform and handed it to the host; this runs there.
  (on (orphaned)
    (set! holder #f)
    (let ((ground (raycast (world-position self) (vec3 0 -1 0) 5.0)))
      (when ground (detach! self :at (hit-point ground) :up (hit-normal ground) :yaw 0.0))))

  ;; Presentation, on every machine. `parent-changed` also fires when the crate first appears on
  ;; a machine (was = #f), so a joiner who arrives mid-carry sees it in the right hand.
  (on (parent-changed was now)
    (when (and was now) (play-sound "pickup.wav" :at (world-position self)))
    (when (and was (not now)) (play-sound "drop.wav" :at (world-position self)))))

(define-kind crate (is carryable) (field label "CRATE"))
(define-kind medkit-box (is carryable) (field label "MEDKIT") (child look (model "medkit.glb")))

;;; ---------------------------------------------------------------------------------------------
;;; Notes from writing it.
;;;
;;; RULE: authority (rule 5). The soldier cannot write the crate's parent while the host owns the
;;;   crate, so a pickup is a message and a round trip. That is the same as Trenchfoot today
;;;   (NET_CMD_PICKUP) and as Fusion's RPC_Collect; the difference is that once it is in the hand,
;;;   the drop is local. Cost: one message and the `grab-failed`/`got` pair.
;;;
;;; FLAW (fixed in B3.1): the second-round proposal had no way to move ownership, and removed a
;;;   leaver's *owned* things. Under that rule a crate could never be dropped without a round trip,
;;;   and a crate handed to a player would vanish when they left. Ownership now follows the tree,
;;;   lifetime follows the spawner, and `orphaned` covers the leaver.
;;;
;;; FLAW (fixed in B3.4): a message can reach a machine that has just stopped owning the thing
;;;   (P3's `grab` arrives at P2 after P2 dropped the crate). The host now forwards a message to the
;;;   owner it knows; a machine that receives one for a thing it no longer owns sends it back to the
;;;   host once, and the host delivers it to the current owner. Two hops, then it is dropped with a
;;;   warning.
;;;
;;; FLAW (fixed in B3.5): interpolating a proxy across a parent change blended a world position
;;;   into a hand offset and the crate flew through the floor for 100 ms. A parent change is now a
;;;   jump, like `teleport!`.
;;;
;;; FLAW (fixed in B6): there was no way to hang a thing off a bone, and no way to show a thing in
;;;   the first-person hand for its owner and in the third-person hand for everyone else. `socket`
;;;   with `:of (arms body)` and `:viewmodel` models do both.
;;;
;;; FLAW (fixed in B2.2): `attach!` from the host's `grab` handler hands the crate away in the
;;;   middle of a handler. The rule is now: the handler runs to the end on the old owner, its
;;;   writes are the last the old owner makes, and the old owner runs no more handlers for it.
;;;
;;; NET: `reaching` is a shared field, not `:local`, because it is gameplay state: a replay must
;;;   send the same `ungrab`. It costs a reference in the snapshot only while a grab is pending.

;;; ---------------------------------------------------------------------------------------------
;;; ENGINE: the paper version has no game kind; this one is added so the example runs (a floor, a
;;; crate and a medkit box in front of where each soldier stands, a soldier per player).

;; ENGINE: walk reads left, right, forward and back, which the paper version never declares; a
;; define-actions form replaces the one before it, so this one repeats interact and drop.
(define-actions (interact "E") (drop "Q") (left "A") (right "D") (forward "W") (back "S"))

(define-kind game
  (child floor (tilemap :width 16 :depth 16))
  (on (start)
    (spawn 'crate :at (vec3 8 0.3 6))
    (spawn 'medkit-box :at (vec3 10 0.3 6)))
  (on (player-joined p)
    (spawn 'soldier :owner p :at (vec3 (+ 6 (* 2 p)) 0 8))))
