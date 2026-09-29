;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; define-kind's code walk and the store frontend's calls (docs/developer/store.md §5), loaded by
;;;; tests/regression/game_checks.c. Handlers record what they saw in fields; the check ticks the
;;;; store and reads them back. A field ending in -ok is #t only when every case in it held.

(define limit 3)                                   ; a top-level constant a handler reads
(define (twice x) (* 2 x))                         ; a top-level function a handler calls

(define-actions (fire "Mouse1") (jump "Space") (forward "W") (back "S") (left "A") (right "D"))

(define-kind game
  (field notes 0)
  (on (noted) (set! notes (+ notes 1))))

(define-kind body
  (field position (vec3 0 0 0))
  (field hits 0)
  (define (bump n) (set! hits (+ hits n))))        ; a helper that writes a field

(define-kind probe
  (is body)
  (field label "")                                 ; the first own field: (probe "x") sets it
  (field power 0))

(define-kind walker
  (is body :hits 1)                                ; a base field's default, overridden
  (field health 100)
  (field ticks 0)
  (field shadows-ok #f)
  (field helpers-ok #f)
  (field children-ok #f)
  (field map-ok #f)
  (field grid-ok #f)
  (field tree-ok #f)
  (field pinged 0)
  (field poked 0)
  (field seen-was 'unset :local)
  (field frames 0 :local)
  (field rolls (list-of int :max 8))
  (field carried (list-of symbol :max 4) :init '(pistol))
  (field ammo (map-of symbol int :max 4) :init '((shotgun . 0) (pistol . 36)))
  (field cells (grid-of int 4 3) :init '((1 1 1 1) (2 2 2 2)))
  (field friend (ref walker))
  (child eye (probe :at (vec3 0 1.6 0) :power 3 :label "eye")
    (child gun (probe "gun" :power 7)))

  (on (start)
    (after 0.05 'ping 7)
    (set! rolls (list (random 1000) (random 1000) (random 1000))))

  (on (ping n) (set! pinged n))
  (on (poke n) (set! poked (+ poked n)))

  (on (tick dt)
    (set! ticks (+ ticks 1))
    (set! health (- health 1))
    (when (= ticks 1) (check-once))
    (when friend (send friend 'poke 2)))

  (define (check-once)
    (set! shadows-ok
          (and (let ((health 5)) (set! health 6) (= health 6))
               ((lambda (health) (set! health 7) (= health 7)) 0)
               (let* ((ticks 10) (health (+ ticks 1))) (= health 11))
               (letrec ((health (lambda () 12))) (= (health) 12))
               (do ((health 0 (+ health 1))) ((= health 3) (= health 3)))
               (let loop ((health 0)) (if (< health 4) (loop (+ health 1)) (= health 4)))
               (let () (define health 13) (set! health 14) (= health 14))
               (eq? (car '(health)) 'health)
               (case 'health ((health) #t) (else #f))
               (= health 99)))                     ; the field itself, after this tick's set!
    (bump 2)                                       ; an inherited helper
    (for-each bump '(1 1))                         ; a helper passed as a value
    (set! helpers-ok (and (= hits 5) (= (twice limit) 6)))
    (set! (eye 'power) (+ (eye 'power) 1))
    (set! children-ok
          (and (= (eye 'power) 4) (= (gun 'power) 7) (equal? (gun 'label) "gun")
               (equal? (eye 'label) "eye") (< (abs (- (vy (eye 'position)) 1.6)) 1e-6)
               (eq? (parent gun) eye) (eq? (child self 'eye) eye) (eq? (child self 'gun) gun)
               (is? gun 'body) (not (is? gun 'walker)) (eq? (kind-of gun) 'probe)
               (eq? (first-child self) eye) (equal? (children eye) (list gun)) (thing? gun)))
    (set! (ammo 'pistol) (- (ammo 'pistol) 1))
    (set! map-ok
          (and (= (ammo 'pistol) 35) (equal? (map-keys ammo) '(pistol shotgun))
               (equal? (map-values ammo) '(35 0)) (not (ammo 'rifle))
               (begin (map-remove! ammo 'shotgun) (equal? (map-keys ammo) '(pistol)))
               (begin (set! ammo '((rifle . 2) (pistol . 5))) (equal? (map-keys ammo) '(pistol rifle)))
               (let ((seen '())) (map-for-each (lambda (k v) (set! seen (cons k seen))) ammo) (= (length seen) 2))
               (begin (set! carried (append carried '(rifle))) (equal? carried '(pistol rifle)))))
    (grid-set! cells 1 2 7)
    (set! grid-ok
          (and (= (grid-ref cells 1 2) 7) (= (grid-ref cells 3 1) 2) (= (grid-ref cells 0 0) 1)
               (= (grid-width cells) 4) (= (grid-height cells) 3) (= (cells 1 2) 7)
               (begin (grid-fill-rect! cells 0 0 2 2 3) (and (= (grid-ref cells 1 1) 3) (= (grid-ref cells 2 1) 2)))))
    (set! tree-ok
          (and (eq? (game) (car (things 'game))) (= (local-player) 1) (equal? (players) '(1))
               (= (length (things 'walker)) 2) (= (length (things 'body)) 6)
               (= (clamp 5 0 limit) 3))))

  (on (health-changed was now)                     ; presentation: writes local fields only
    (when (eq? seen-was 'unset) (set! seen-was (if was 'value 'false)))
    (set! frames (+ frames 1))))

(define-kind guard
  (field count 0)
  (field entered 0)
  (field exited 0)
  (field alert-ticks 0)
  (states idle
    (idle (on (tick dt) (set! count (+ count 1)) (when (= count 2) (go 'alert))))
    (alert (on (enter) (set! entered (+ entered 1)))
           (on (exit) (set! exited (+ exited 1)))
           (on (tick dt)
             (set! alert-ticks (+ alert-ticks 1))
             (when (= alert-ticks 3) (go 'idle))))))

(define-kind victim
  (on (poke n) (send (game) 'noted)))

(define-kind remover
  (field victim-gone #f)
  (on (tick dt)
    (let ((victims (things 'victim)))
      (when (pair? victims)
        (send (car victims) 'poke 1)                 ; queued, then dropped by the remove
        (remove (car victims))
        (set! victim-gone (null? (things 'victim)))))))

;; A :local child (proposal B1): presentation handlers write it, gameplay handlers can't read it, and
;; its own children are local too.
(define-kind gun-model
  (field mesh ""))

(define-kind trooper
  (field weapon 'pistol)
  (field gun-refused #f)
  (child gun (gun-model :local #t)
    (child flash (gun-model)))
  (on (tick dt)
    (set! gun-refused
          (catch #t
            (lambda () (gun 'mesh) #f)
            (lambda (type info)
              (eqv? 0 (string-position "mesh on gun is local (this screen only). The tick handler of trooper can't read it"
                                       (apply format #f info)))))))
  (on (weapon-changed was now)
    (set! (gun 'mesh) (if (eq? now 'pistol) "pistol.glb" "rifle.glb"))))

(spawn 'game)
(spawn 'trooper)
(let ((a (spawn 'walker))
      (b (spawn 'walker :at (vec3 1 2 3) :health 50)))
  (set! (a 'friend) b))
(spawn 'guard)
(spawn 'victim)
(spawn 'remover)
