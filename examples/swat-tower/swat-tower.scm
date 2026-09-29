;;;; SWAT Tower, written against the PROPOSED Trench Engine API (design/proposal.md, part B).
;;;; This is a paper design; it does not run today. It is the whole game:
;;;;   - procedural floors of rooms and corridors;
;;;;   - two enemy types, one written as plain handlers and one with the optional state-machine sugar;
;;;;   - stairs to the next floor;
;;;;   - medkit, ammo and shotgun pickups, and weapon switching;
;;;;   - score, and a high score saved per player;
;;;;   - HUD;
;;;;   - co-op for up to 8 players.
;;;; There is no C and no main.c: `trench run swat-tower/` loads this file.
;;;;
;;;; Markers:
;;;;   ;; RULE: <rule> - a place where one of the proposal's rules got in the way, and what it cost.
;;;;   ;; NET:  ...    - a place where co-op needed something that single player would not.

;;; ---------------------------------------------------------------------------------------------
;;; Tuning. Top-level definitions are frozen once the game has loaded. Constants and tables are
;;; what the top level is for, so the freeze costs nothing here.

(define floor-cells 33)                      ; a floor is 33 x 33 cells
(define cell-size 3.0)                       ; metres per cell
(define gravity 20.0)

(define weapons
  '((pistol  :damage 25 :pellets 1 :spread 0.01 :cooldown 0.30 :refill 12 :sound "pistol.wav"  :mesh "pistol.glb")
    (shotgun :damage 12 :pellets 7 :spread 0.09 :cooldown 0.90 :refill 6  :sound "shotgun.wav" :mesh "shotgun.glb")))

(define (stat weapon key) (cadr (memq key (cdr (assq weapon weapons)))))

(define-actions                              ; action -> default key; players can rebind
  (forward "W") (back "S") (left "A") (right "D") (jump "Space") (run "LeftShift")
  (fire "Mouse1") (switch "Q") (host "H") (join "J") (restart "R"))

;;; ---------------------------------------------------------------------------------------------
;;; The game is the root of the world. It owns the floor, the score and the roster of who is playing.

(define-kind game
  (field floor 0)
  (field score 0)
  (field over #f)
  (field spawn-point (vec3 0 0 0))
  (field roster (map-of int (ref soldier) :max 8))   ; player -> that player's soldier
  (child level (tilemap :width floor-cells :depth floor-cells :cell-size cell-size :height 3.0
                        :floor-texture "tiles.png" :ceiling-texture "ceiling.png"))
  (child sun (light :type 'ambient :energy 0.35))

  (on (start)
    (send self 'next-floor))

  (on (player-joined player)                         ; at start this also fires for the local player
    (set! (roster player) (spawn 'soldier :owner player :at (spot-near spawn-point player))))

  (on (player-left player)                           ; the engine removes the things that player owned
    (map-remove! roster player))

  (on (next-floor)
    (set! floor (+ floor 1))
    (for-each remove (things 'enemy))
    (for-each remove (things 'pickup))
    (for-each remove (things 'stairs))
    (let ((rooms (carve-floor! (level 'cells) (+ 4 (min floor 6)))))
      (set! spawn-point (room-center (car rooms)))
      (spawn 'stairs :at (room-center (car (reverse rooms))))
      (for-each populate (cdr rooms))
      (for-each (lambda (player soldier)
                  ;; RULE: authority. Each soldier belongs to its player, so the game can't set its
                  ;; position; it asks. That costs one message and one small handler (`arrive`).
                  (send soldier 'arrive (spot-near spawn-point player)))
                (map-keys roster) (map-values roster))))

  (define (populate room)                            ; a helper; like a handler, it sees the fields
    (let ((n (+ 1 (random (min 3 floor)))))
      (do ((i 0 (+ i 1))) ((= i n))
        (spawn (if (< (random 100) (* 12 floor)) 'shooter 'rusher) :at (random-spot room))))
    (when (< (random 100) 60)
      (spawn (list-ref '(medkit ammo-box shotgun-crate) (random 3)) :at (random-spot room))))

  (define (room-center room) (cell->world level (room-cx room) (room-cz room)))
  (define (random-spot room)
    (cell->world level (+ (car room) (random (caddr room))) (+ (cadr room) (random (cadddr room)))))

  (on (enemy-killed by points)
    (set! score (+ score points))
    (when by (send by 'credit points)))              ; a reference to a thing that is gone reads as #f

  (on (soldier-down)
    (set! over (not (member #t (map (lambda (s) (s 'up)) (map-values roster))))))

  (on (restart)                                      ; sent by a player pressing R, below
    (when over
      (set! over #f)
      (set! score 0)
      (set! floor 0)
      (for-each (lambda (s) (send s 'revive)) (map-values roster))
      (send self 'next-floor)))

  ;; Presentation. These handlers run on every machine, may read anything, and write only local state.
  (on (over-changed was now)
    (when now (profile-set! 'highscore (max score (profile-ref 'highscore 0)))))

  (on (draw-hud)
    (let ((me (roster (local-player)))
          (w (screen-width)) (h (screen-height)))
      (when me
        (draw-text (format #f "HEALTH ~A" (me 'health)) 24 (- h 48) :size 28)
        (draw-text (format #f "~A ~A" (me 'weapon) ((me 'ammo) (me 'weapon))) (- w 24) (- h 48)
                   :size 28 :align 'right)
        (draw-rect (- (/ w 2) 2) (- (/ h 2) 2) 4 4 :color 'white)
        (when (> (me 'hurt) 0.0)
          (draw-rect 0 0 w h :color (rgba 200 0 0 (* 300 (me 'hurt))))))
      (draw-text (format #f "FLOOR ~A   SCORE ~A   BEST ~A" floor score (profile-ref 'highscore 0)) 24 24
                 :size 22)
      (cond (over
             (draw-text (format #f "EVERYONE IS DOWN - SCORE ~A - R TO RESTART" score) (/ w 2) (/ h 2)
                        :size 34 :align 'center))
            ((null? (things 'enemy))
             (draw-text "Floor clear - find the stairs" (/ w 2) 80 :size 26 :align 'center)))))

  (on (frame dt)                                     ; keys that are not gameplay
    (cond ((and over (pressed? 'restart)) (send self 'restart)))))  ; a player command: recorded, replayed

;;; ---------------------------------------------------------------------------------------------
;;; Floor generation: plain functions over the level's grid. `random` draws from the stream of the
;;; thing whose handler is running (the game here), so every floor comes out the same on replay.

(define (carve-floor! cells room-count)
  (grid-fill! cells 1)
  (let loop ((rooms '()) (tries 0))
    (if (or (= (length rooms) room-count) (= tries 300))
        (let ((rooms (reverse rooms)))
          (for-each (lambda (a b) (corridor! cells a b)) rooms (cdr rooms))
          rooms)
        (let* ((w (+ 3 (* 2 (random 3))))
               (d (+ 3 (* 2 (random 3))))
               (room (list (+ 1 (* 2 (random (quotient (- floor-cells w 1) 2))))
                           (+ 1 (* 2 (random (quotient (- floor-cells d 1) 2))))
                           w d)))
          (if (member room rooms rooms-touch?)
              (loop rooms (+ tries 1))
              (begin (grid-fill-rect! cells (car room) (cadr room) w d 0)
                     (loop (cons room rooms) (+ tries 1))))))))

(define (rooms-touch? a b)                           ; a room is (x z width depth), in cells
  (not (or (> (car a) (+ (car b) (caddr b))) (> (car b) (+ (car a) (caddr a)))
           (> (cadr a) (+ (cadr b) (cadddr b))) (> (cadr b) (+ (cadr a) (cadddr a))))))

(define (room-cx room) (+ (car room) (quotient (caddr room) 2)))
(define (room-cz room) (+ (cadr room) (quotient (cadddr room) 2)))

(define (corridor! cells a b)                        ; an L: along x, then along z
  (let ((ax (room-cx a)) (az (room-cz a)) (bx (room-cx b)) (bz (room-cz b)))
    (grid-fill-rect! cells (min ax bx) az (+ 1 (abs (- ax bx))) 1 0)
    (grid-fill-rect! cells bx (min az bz) 1 (+ 1 (abs (- az bz))) 0)))

(define (spot-near point player)                     ; players arrive standing in a small ring
  (v+ point (vec3 (* 0.9 (cos player)) 0 (* 0.9 (sin player)))))

;;; ---------------------------------------------------------------------------------------------
;;; The soldier. There is one per player, owned by that player, so it moves on that player's machine
;;; with no delay.

(define-kind soldier
  (is character :radius 0.35 :height 1.8)
  (field health 100)
  (field up #t)
  (field yaw 0.0)
  (field pitch 0.0)
  (field weapon 'pistol)
  (field carried (list-of symbol :max 4) :init '(pistol))                ; the inventory
  (field ammo (map-of symbol int :max 4) :init '((pistol . 36) (shotgun . 0)))
  (field cooldown 0.0)
  (field credits 0)
  (field hurt 0.0 :local)                                                 ; red flash on this screen only
  (child eye (camera :at (vec3 0 1.6 0) :fov 75 :for-owner #t)
    (child gun (model (stat 'pistol :mesh) :at (vec3 0.25 -0.25 -0.45) :local #t)))

  (on (tick dt)
    (when up
      (look)
      (walk dt)
      (set! cooldown (max 0.0 (- cooldown dt)))
      (when (pressed? 'switch) (switch-weapon))
      (when (and (held? 'fire) (= cooldown 0.0) (> (ammo weapon) 0))
        (fire))))

  (define (look)
    (let ((m (mouse-motion)))
      (set! yaw (- yaw (* 0.0025 (vx m))))
      (set! pitch (clamp (- pitch (* 0.0025 (vy m))) -1.4 1.4))
      (set! rotation (vec3 0 yaw 0))
      (set! (eye 'rotation) (vec3 pitch 0 0))))

  (define (walk dt)
    (let* ((wish (rotate-y (input-vector 'left 'right 'forward 'back) yaw))
           (speed (if (held? 'run) 7.0 4.5))
           (up-speed (if (and on-floor (pressed? 'jump)) 6.0 (- (vy velocity) (* gravity dt)))))
      (set! velocity (vec3 (* speed (vx wish)) up-speed (* speed (vz wish))))
      (move-and-slide!)))

  (define (fire)
    (set! (ammo weapon) (- (ammo weapon) 1))
    (set! cooldown (stat weapon :cooldown))
    (play-sound (stat weapon :sound) :at position)     ; effects from gameplay code are shown on every machine
    (burst 'muzzle-flash :at (world-position gun))
    (do ((i 0 (+ i 1))) ((= i (stat weapon :pellets)))
      (let ((hit (raycast (world-position eye) (spread (aim yaw pitch) (stat weapon :spread)) 80.0
                          :ignore self)))
        (when hit
          (burst (if (is? (hit-thing hit) 'enemy) 'blood 'dust) :at (hit-point hit))
          (when (is? (hit-thing hit) 'enemy)
            ;; NET: the enemy belongs to the host. This machine decides the hit against the enemy as
            ;; drawn here and tells the owner. That favours the shooter, which suits co-op.
            (send (hit-thing hit) 'damage (stat weapon :damage) self))))))

  (define (switch-weapon)
    (let ((rest (cdr (or (memq weapon carried) (list weapon)))))
      (set! weapon (if (pair? rest) (car rest) (car carried)))))

  (on (damage amount by)
    (when up
      (set! health (max 0 (- health amount)))
      (when (= health 0)
        (set! up #f)
        (send (game) 'soldier-down))))

  (on (collect what)
    (case what
      ((medkit)   (set! health (min 100 (+ health 40))))
      ((ammo-box) (set! (ammo weapon) (+ (ammo weapon) (stat weapon :refill))))
      ((shotgun)  (unless (memq 'shotgun carried) (set! carried (append carried '(shotgun))))
                  (set! (ammo 'shotgun) (+ (ammo 'shotgun) (stat 'shotgun :refill)))
                  (set! weapon 'shotgun)))
    (play-sound "pickup.wav" :at position))

  (on (arrive where)                                   ; the game moves us between floors
    (teleport! where)                                  ; like set! position, but other machines don't slide
    (set! velocity (vec3 0 0 0)))                      ; the soldier across the map to get there

  (on (revive)
    (set! health 100)
    (set! up #t)
    (set! carried '(pistol))
    (set! ammo '((pistol . 36) (shotgun . 0)))
    (set! weapon 'pistol))

  (on (credit points)
    (set! credits (+ credits points)))

  ;; Presentation.
  (on (health-changed was now)
    (when (and was (< now was))
      (set! hurt 0.35)
      (play-sound "hurt.wav" :at position)))
  ;; RULE: presentation writes only local state. The first draft declared `gun` as an ordinary child,
  ;; so it was shared, and this handler failed with: "mesh on gun is shared state; a presentation handler
  ;; can't write it...". The fix the message offers is to make the gun a local child (`:local #t`
  ;; above). That is right anyway, because everyone can work out the gun from `weapon`. Cost: one keyword.
  ;; `-changed` handlers also run when a thing first appears on a machine (`was` is #f), so a player
  ;; who joins or loads a save sees the right gun without extra code.
  (on (weapon-changed was now)
    (set! (gun 'mesh) (stat now :mesh)))
  (on (frame dt)
    (set! hurt (max 0.0 (- hurt dt)))))

;;; ---------------------------------------------------------------------------------------------
;;; Enemies. Both types share `enemy`; they differ only in how they think. The host owns them.

(define-kind enemy
  (is character :radius 0.4 :height 1.8)
  (field health 50)
  (field points 100)
  (field reload 0.0)
  (child body (model "grunt.glb" :animation 'idle))

  (on (damage amount by)                   ; after `remove`, messages still queued for it are dropped,
    (set! health (- health amount))        ; so seven pellets can't score one kill seven times
    (burst 'blood :at (v+ position (vec3 0 1.2 0)))
    (when (<= health 0)
      (send (game) 'enemy-killed by points)
      (remove self)))

  (define (target)                         ; the nearest soldier this enemy can see
    (nearest 'soldier position
             :where (lambda (s) (and (s 'up) (line-of-sight? (v+ position (vec3 0 1.6 0))
                                                             (v+ (s 'position) (vec3 0 1.2 0)))))))

  (define (step-toward point speed dt)     ; walk the level's grid path toward point, under gravity
    (let* ((next (path-next (child (game) 'level) position point))
           (flat (vec3 (- (vx next) (vx position)) 0 (- (vz next) (vz position))))
           (dir (if (> (vlength flat) 0.1) (vnormalize flat) (vec3 0 0 0))))
      (when (> speed 0) (set! rotation (vec3 0 (heading dir) 0)))
      (set! velocity (vec3 (* speed (vx dir)) (- (vy velocity) (* gravity dt)) (* speed (vz dir))))
      (move-and-slide!)
      (set! (body 'animation) (if (> speed 0) 'run 'idle)))))

;; Plain handlers: the rusher is one tick handler with an `if`.
(define-kind rusher
  (is enemy)
  (field health 60)

  (on (tick dt)
    (set! reload (max 0.0 (- reload dt)))
    (let ((s (target)))
      (if (not s)
          (step-toward position 0 dt)
          (begin
            (step-toward (s 'position) 5.5 dt)
            (when (and (< (vdistance position (s 'position)) 1.5) (= reload 0.0))
              (send s 'damage 15 self)
              (play-sound "swipe.wav" :at position)
              (set! reload 0.8)))))))

;; The same idea written with the optional state-machine sugar: the shooter hunts, then stands and fires.
(define-kind shooter
  (is enemy)
  (field points 150)
  (field prey (ref soldier))

  (states hunt
    (hunt
      (on (tick dt)
        (let ((s (target)))
          (if (and s (< (vdistance position (s 'position)) 14.0))
              (begin (set! prey s) (go 'aim))
              (step-toward (if s (s 'position) position) (if s 3.5 0) dt)))))
    (aim
      (on (enter) (set! reload 0.6))
      (on (tick dt)
        (set! reload (max 0.0 (- reload dt)))
        (step-toward position 0 dt)
        (set! (body 'animation) 'aim)
        (cond ((or (not prey) (not (prey 'up))) (go 'hunt))
              ((> (vdistance position (prey 'position)) 16.0) (go 'hunt))
              ((= reload 0.0)
               (play-sound "rifle.wav" :at position)
               (when (< (random 100) 55)          ; 55% of shots land
                 (send prey 'damage 8 self))
               (set! reload 1.2)))))))

;;; ---------------------------------------------------------------------------------------------
;;; Things you walk into. The host owns these too.

(define-kind pickup
  (is area :radius 0.8)
  (field gives 'medkit)
  (field taken #f)

  (on (touched other)
    (when (and (not taken) (is? other 'soldier) (other 'up) (useful-to? other))
      (set! taken #t)                      ; if two soldiers touch it in the same tick, the first wins
      (send other 'collect gives)
      (remove self)))

  (define (useful-to? s)
    (not (and (eq? gives 'medkit) (= (s 'health) 100)))))

(define-kind medkit        (is pickup) (field gives 'medkit)   (child look (model "medkit.glb" :spin 1.5)))
(define-kind ammo-box      (is pickup) (field gives 'ammo-box) (child look (model "ammo.glb" :spin 1.5)))
(define-kind shotgun-crate (is pickup) (field gives 'shotgun)  (child look (model "shotgun.glb" :spin 1.5)))

(define-kind stairs
  (is area :radius 1.4)
  (field used #f)
  (child look (model "stairs.glb"))

  (on (tick dt)                            ; standing on them when the last enemy dies counts
    (when (and (not used) (null? (things 'enemy)) (pair? (overlapping self 'soldier)))
      (set! used #t)
      (send (game) 'next-floor))))
