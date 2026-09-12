; The authoring demo's Mover, in Scheme. Every call here comes from the engine's binding table;
; nothing about this file is specific to s7 beyond the language it is written in.

(define (mover-spawn)
  (think-next))

(define (mover-think)
  (let* ((speed (get "speed"))
         (per-step (* speed (dt)))
         (turn (- (if (key-down? (key "e")) 1 0)
                  (if (key-down? (key "q")) 1 0))))
    (move-world! (vec* (input-vector) per-step))
    (rotate! (* turn 2.0 (dt)))
    (when (key-down? (key "space"))
      (move-local! (vec per-step 0)))
    (think-next)))

(define (mover-draw)
  (let ((body (rgba 102 191 255 255))
        (arrow (rgba 0 82 172 255)))
    (draw-rect-rotated (interpolated) (vec 32 32) (interpolated-rotation) body)
    ; The arrow points along local +X, so Space follows it whichever way the square faces.
    (draw-triangle (to-local (vec 12 0)) (to-local (vec -6 -8)) (to-local (vec -6 8)) arrow)))

(define-entity "mover"
  '(("speed" "float"))
  '(("spawn" "mover-spawn") ("think" "mover-think") ("draw" "mover-draw")))
