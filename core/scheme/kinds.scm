;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; The prelude of the store's Scheme frontend (gameplay/script/game_s7.c), loaded into the rootlet
;;;; before any game: define-kind and its code walk, and the few calls easier said in Scheme.
;;;; docs/developer/store.md §5 is the contract. Everything named %... is the frontend's own.

;;; ---- the code walk (§5.2) -------------------------------------------------------------------
;;; Inside a kind's handlers and helpers its fields, children and helpers are plain names. The walk
;;; rewrites a free field name into (%field self i), (set! field v) into (%set-field! self i v), a
;;; child name into (%child self slot), and a helper call (h args) into ((%helper-ref self 'h) self
;;; args). A name bound by lambda, let, let*, letrec, do, named let or an inner define is left
;;; alone, and nothing under quote is touched (s7's reader has already turned a quasiquote into
;;; calls, so its unquoted parts are walked like any code). ctx is #(fields children helpers):
;;; fields and children are alists from name to index, helpers a list of names.

(define (%binding-name b) (if (pair? b) (car b) b))

(define (%param-names params)                 ; (a b . c), a, and lambda*'s (a (b 1)) :rest c
  (cond ((symbol? params) (if (keyword? params) '() (list params)))
        ((pair? params) (append (%param-names (%binding-name (car params))) (%param-names (cdr params))))
        (else '())))

(define (%body-defines body)                  ; inner defines scope over the whole body
  (let loop ((b body) (names '()))
    (if (not (pair? b))
        names
        (let ((f (car b)))
          (loop (cdr b)
                (if (and (pair? f) (memq (car f) '(define define* define-constant define-macro))
                         (pair? (cdr f)))
                    (cons (%binding-name (cadr f)) names)
                    names))))))

(define (%walk x ctx bound)
  (cond ((symbol? x) (%walk-symbol x ctx bound))
        ((pair? x) (%walk-pair x ctx bound))
        (else x)))

(define (%walk-symbol x ctx bound)
  (cond ((memq x bound) x)
        ((assq x (vector-ref ctx 0)) => (lambda (p) (list '%field 'self (cdr p))))
        ((assq x (vector-ref ctx 1)) => (lambda (p) (list '%child 'self (cdr p))))
        ((memq x (vector-ref ctx 2))                 ; a helper used as a value: bind self now
         (list 'lambda '%args (list 'apply (list '%helper-ref 'self (list 'quote x)) 'self '%args)))
        (else x)))

(define (%walk-list xs ctx bound)             ; every element; the list may be improper
  (cond ((pair? xs) (cons (%walk (car xs) ctx bound) (%walk-list (cdr xs) ctx bound)))
        ((null? xs) xs)
        (else (%walk xs ctx bound))))

(define (%walk-body body ctx bound)
  (%walk-list body ctx (append (%body-defines body) bound)))

(define (%walk-params params ctx bound)       ; lambda* defaults are walked in the outer scope
  (cond ((pair? params)
         (cons (if (pair? (car params))
                   (cons (caar params) (%walk-list (cdar params) ctx bound))
                   (car params))
               (%walk-params (cdr params) ctx bound)))
        (else params)))

(define (%walk-bindings bindings ctx bound)
  (map (lambda (b) (if (pair? b) (cons (car b) (%walk-list (cdr b) ctx bound)) b)) bindings))

(define (%walk-pair x ctx bound)
  (let ((head (car x)))
    (cond
     ((eq? head #_quote) x)                   ; the reader turns 'x into (#_quote x)
     ((or (not (symbol? head)) (memq head bound) (not (list? x)))
      (%walk-list x ctx bound))
     (else
        (case head
          ((quote quasiquote) x)
          ((lambda lambda*)
           (if (pair? (cdr x))
               (let ((params (cadr x)))
                 (cons head (cons (%walk-params params ctx bound)
                                  (%walk-body (cddr x) ctx (append (%param-names params) bound)))))
               x))
          ((define define* define-constant)
           (cond ((and (pair? (cdr x)) (pair? (cadr x)))   ; (define (f . params) body ...)
                  (let ((params (cdadr x)))
                    (cons head (cons (cons (caadr x) (%walk-params params ctx bound))
                                     (%walk-body (cddr x) ctx (append (%param-names params) bound))))))
                 ((pair? (cdr x)) (cons head (cons (cadr x) (%walk-list (cddr x) ctx bound))))
                 (else x)))
          ((let)
           (if (and (pair? (cdr x)) (symbol? (cadr x)))   ; named let
               (let ((name (cadr x)) (bindings (caddr x)))
                 (cons 'let (cons name (cons (%walk-bindings bindings ctx bound)
                                             (%walk-body (cdddr x) ctx
                                                         (cons name (append (map %binding-name bindings) bound)))))))
               (let ((bindings (cadr x)))
                 (cons 'let (cons (%walk-bindings bindings ctx bound)
                                  (%walk-body (cddr x) ctx (append (map %binding-name bindings) bound)))))))
          ((let*)
           (let loop ((bs (cadr x)) (out '()) (inner bound))
             (if (null? bs)
                 (cons 'let* (cons (reverse out) (%walk-body (cddr x) ctx inner)))
                 (let ((b (car bs)))
                   (loop (cdr bs)
                         (cons (if (pair? b) (cons (car b) (%walk-list (cdr b) ctx inner)) b) out)
                         (cons (%binding-name b) inner))))))
          ((letrec letrec*)
           (let ((inner (append (map %binding-name (cadr x)) bound)))
             (cons head (cons (%walk-bindings (cadr x) ctx inner) (%walk-body (cddr x) ctx inner)))))
          ((do)                                              ; (do ((v init step) ...) (test res ...) body ...)
           (let ((inner (append (map %binding-name (cadr x)) bound)))
             (cons 'do (cons (map (lambda (s)
                                    (if (and (pair? s) (pair? (cdr s)))
                                        (cons (car s) (cons (%walk (cadr s) ctx bound)
                                                            (%walk-list (cddr s) ctx inner)))
                                        s))
                                  (cadr x))
                             (%walk-list (cddr x) ctx inner)))))
          ((set!)
           (let ((target (cadr x)) (value (%walk-list (cddr x) ctx bound)))
             (cond ((or (not (symbol? target)) (memq target bound))
                    (cons 'set! (cons (%walk target ctx bound) value)))
                   ((assq target (vector-ref ctx 0))
                    => (lambda (p) (cons '%set-field! (cons 'self (cons (cdr p) value)))))
                   ((assq target (vector-ref ctx 1))
                    (error 'game-error "can't set! ~A: it is a child. Set one of its fields: (set! (~A 'field) value)"
                           target target))
                   (else (cons 'set! (cons target value))))))
          ((case)                                            ; the datums are data
           (cons 'case (cons (%walk (cadr x) ctx bound)
                             (map (lambda (clause)
                                    (if (pair? clause) (cons (car clause) (%walk-list (cdr clause) ctx bound)) clause))
                                  (cddr x)))))
          (else
           (if (memq head (vector-ref ctx 2))
               (cons (list '%helper-ref 'self (list 'quote head)) (cons 'self (%walk-list (cdr x) ctx bound)))
               (%walk-list x ctx bound))))))))

;;; ---- define-kind (§5.2) ---------------------------------------------------------------------
;;; Expands into (%kind-declare 'name 'base fields children settings), then one %kind-helper per
;;; helper, %kind-states, and one %kind-handler per handler. Field defaults, :init values, child
;;; settings and is settings are expressions, evaluated once when the kind is declared.

;;; A child setting whose value is a list of the kind's own child names, such as a socket's
;;; :of (arms body), names those children: it is quoted rather than called (B6).
(define (%names-of-children? v names)
  (and (pair? v) (list? v) (not (null? names))
       (let loop ((v v)) (or (null? v) (and (symbol? (car v)) (memq (car v) names) (loop (cdr v)))))))

(define* (%settings-expr kind args (names '())) ; (:k v ... positional) -> (list (cons 'k v) (cons #f p))
  (let loop ((a args) (out '()))
    (cond ((null? a) (cons 'list (reverse out)))
          ((keyword? (car a))
           (if (null? (cdr a))
               (error 'game-error "define-kind ~A: ~A needs a value" kind (car a)))
           (loop (cddr a) (cons (list 'cons (list 'quote (keyword->symbol (car a)))
                                      (if (%names-of-children? (cadr a) names) (list 'quote (cadr a)) (cadr a)))
                                out)))
          (else (loop (cdr a) (cons (list 'cons #f (car a)) out))))))

(define (%spec-expr x)                        ; a declared type; its counts are expressions
  (case (car x)
    ((ref) (list 'quote x))
    ((map-of) (cons 'list (cons ''map-of (cons (list 'quote (cadr x)) (cons (list 'quote (caddr x)) (cdddr x))))))
    (else (cons 'list (cons (list 'quote (car x)) (cons (list 'quote (cadr x)) (cddr x)))))))

(define (%field-expr kind f)                  ; (field name default-or-type [:local [#t]] [:init v])
  (unless (and (pair? (cdr f)) (symbol? (cadr f)) (pair? (cddr f)))
    (error 'game-error "define-kind ~A: ~S should be (field name default)" kind f))
  (let* ((x (caddr f))
         (spec? (and (pair? x) (memq (car x) '(list-of set-of map-of grid-of ref))))
         (local #f)
         (init '()))
    (let loop ((o (cdddr f)))
      (when (pair? o)
        (case (car o)
          ((:local) (if (and (pair? (cdr o)) (boolean? (cadr o)))
                        (begin (set! local (cadr o)) (loop (cddr o)))
                        (begin (set! local #t) (loop (cdr o)))))
          ((:init) (when (null? (cdr o)) (error 'game-error "define-kind ~A: :init needs a value" kind))
                   (set! init (list (cadr o)))
                   (loop (cddr o)))
          (else (error 'game-error "define-kind ~A: field ~A has an unknown option ~S" kind (cadr f) (car o))))))
    (list 'list (list 'quote (cadr f)) (if spec? (%spec-expr x) #f) (if spec? #f x)
          (if local ''(local) ''()) (if (null? init) ''() (list 'list (car init))))))

(define* (%child-expr kind c (names '()))     ; (child name (kind setting ...) child ...)
  (unless (and (pair? (cdr c)) (symbol? (cadr c)) (pair? (cddr c)) (pair? (caddr c)) (symbol? (caaddr c)))
    (error 'game-error "define-kind ~A: ~S should be (child name (kind :setting value ...))" kind c))
  (cons 'list (cons (list 'quote (cadr c))
                    (cons (list 'quote (caaddr c))
                          (cons (%settings-expr kind (cdaddr c) names)
                                (map (lambda (n) (%child-expr kind n names)) (cdddr c)))))))

(define (%child-names c)                      ; a child and every child declared inside it
  (cons (cadr c) (apply append (map %child-names (cdddr c)))))

(define (%lambda-expr clause ctx params)
  (cons 'lambda (cons (cons 'self params)
                      (%walk-body (cddr clause) ctx (cons 'self (%param-names params))))))

(define (%handler-expr kind clause ctx state)  ; (on (event . params) body ...)
  (unless (and (pair? (cdr clause)) (pair? (cadr clause)) (symbol? (caadr clause)))
    (error 'game-error "define-kind ~A: ~S should be (on (event arg ...) body ...)" kind clause))
  (let ((line (or (pair-line-number clause) 0)))
    (append (list '%kind-handler (list 'quote kind) (list 'quote (caadr clause))
                  (%lambda-expr clause ctx (cdadr clause)) line)
            (if state (list (list 'quote state)) '()))))

(define (%expand-kind name clauses)
  (unless (symbol? name) (error 'game-error "define-kind needs a name: (define-kind name clause ...)"))
  (let ((base #f) (settings ''()) (fields '()) (children '()) (handlers '()) (helpers '()) (states #f))
    (for-each
     (lambda (c)
       (unless (and (pair? c) (symbol? (car c)))
         (error 'game-error "define-kind ~A: ~S is not a clause (is, field, child, on, define or states)" name c))
       (case (car c)
         ((is) (unless (and (pair? (cdr c)) (symbol? (cadr c)))
                 (error 'game-error "define-kind ~A: ~S should be (is kind :setting value ...)" name c))
               (set! base (cadr c))
               (set! settings (%settings-expr name (cddr c))))
         ((field) (set! fields (cons c fields)))
         ((child) (set! children (cons c children)))
         ((on) (set! handlers (cons (cons #f c) handlers)))
         ((define) (unless (and (pair? (cdr c)) (pair? (cadr c)) (symbol? (caadr c)))
                     (error 'game-error "define-kind ~A: a helper is (define (name arg ...) body ...), not ~S" name c))
                   (set! helpers (cons c helpers)))
         ((states) (unless (and (pair? (cdr c)) (symbol? (cadr c)))
                     (error 'game-error "define-kind ~A: ~S should be (states initial (state (on ...) ...) ...)" name c))
                   (set! states c))
         (else (error 'game-error "define-kind ~A: ~S is not a clause (is, field, child, on, define or states)" name c))))
     clauses)
    (let* ((fields (reverse fields)) (children (reverse children)) (helpers (reverse helpers))
           (base-fields (if base
                            (or (%kind-field-names base)
                                (error 'game-error "define-kind ~A: it extends ~A, but there is no kind named ~A" name base base))
                            '()))
           (field-alist (let loop ((names base-fields) (i 0) (out '()))
                          (if (null? names) (reverse out) (loop (cdr names) (+ i 1) (cons (cons (car names) i) out)))))
           (count (length base-fields))
           (field-exprs (map (lambda (f) (%field-expr name f)) fields)))
      (for-each (lambda (f)
                  (unless (assq (cadr f) field-alist)
                    (set! field-alist (append field-alist (list (cons (cadr f) count))))
                    (set! count (+ count 1))))
                fields)
      (when states
        (unless (assq 'state field-alist)
          (set! field-alist (append field-alist (list (cons 'state count)))))
        (set! field-exprs (append field-exprs (list (list 'list ''state #f (list 'quote (cadr states)) ''(hidden) ''())))))
      (let* ((child-alist (map (lambda (n) (cons n (%child-slot n)))
                               (append (if base (%kind-child-names base) '())
                                       (apply append (map %child-names children)))))
             (ctx (vector field-alist child-alist
                          (append (map caadr helpers) (if base (%kind-helper-names base) '())))))
        (when states
          (for-each (lambda (st)
                      (unless (and (pair? st) (symbol? (car st)))
                        (error 'game-error "define-kind ~A: ~S should be (state-name (on ...) ...)" name st))
                      (for-each (lambda (c)
                                  (unless (and (pair? c) (eq? (car c) 'on))
                                    (error 'game-error "define-kind ~A: state ~A holds only (on ...) clauses, not ~S" name (car st) c))
                                  (set! handlers (cons (cons (car st) c) handlers)))
                                (cdr st)))
                    (cddr states)))
        (append
         (list 'begin
               (list '%kind-declare (list 'quote name) (list 'quote base) (cons 'list field-exprs)
                     (cons 'list (map (lambda (c) (%child-expr name c (map car child-alist))) children)) settings))
         (if states
             (list (list '%kind-states (list 'quote name) (list 'quote (cadr states)) (list 'quote (map car (cddr states)))))
             '())
         (map (lambda (h) (list '%kind-helper (list 'quote name) (list 'quote (caadr h)) (%lambda-expr h ctx (cdadr h))))
              helpers)
         (map (lambda (h) (%handler-expr name (cdr h) ctx (car h))) (reverse handlers))
         (list (list 'quote name)))))))

(define-macro (define-kind name . clauses) (%expand-kind name clauses))

;;; ---- the rest of the surface that is simpler in Scheme -----------------------------------------

(define-macro (define-actions . actions)      ; (define-actions (forward "W") (fire "Mouse1") ...)
  (list '%define-actions (list 'quote actions)))

(define (clamp x lo hi) (max lo (min hi x)))

(define (map-for-each f m)                     ; entries naming removed things are skipped
  (for-each f (map-keys m) (map-values m)))

;;; ---- running game code: handlers, loading, the REPL, the freeze (§5.4, §5.7) --------------------

;;; An error is reported by C (%report context type info text file line), which rewords s7's
;;; immutable error as rule 2 and adds the kind, thing and event of the running handler.
;;; Context 0 is a handler, 1 a load, 2 the REPL (which only answers the message).

(define (%error-string info)
  (if (and (pair? info) (string? (car info)))
      (catch #t (lambda () (apply format #f info)) (lambda args (object->string info)))
      (object->string info)))

(define (%dispatch proc args)
  (catch #t
    (lambda () (apply proc args) #t)
    (lambda (type info)
      (%report 0 type info (%error-string info) ((owlet) 'error-file) ((owlet) 'error-line))
      #f)))

(define (%load-file path env)
  (catch #t
    (lambda () (load path env) #t)
    (lambda (type info)
      (%report 1 type info (%error-string info) ((owlet) 'error-file) ((owlet) 'error-line))
      #f)))

(define (%repl text env)
  (catch #t
    (lambda () (cons #t (eval-string text env)))
    (lambda (type info)
      (cons #f (%report 2 type info (%error-string info) ((owlet) 'error-file) ((owlet) 'error-line))))))

;; Rule 3: the clock and files, shadowed in the game environment. Outside gameplay handlers they do
;; what they always did; load reads into the game environment unless told otherwise.
(define (%guard name original env)
  (lambda args
    (cond ((%gameplay?) (%rule-3 name))
          ((not original) (error 'game-error "~A is not available to games" name))
          ((and (eq? name 'load) (pair? args) (null? (cdr args))) (original (car args) env))
          (else (apply original args)))))

;; Rule 2: every binding of the game environment, the environment itself, the environments of the
;; closures reachable from it (E5's freeze-let!, with a visited list so a closure that names itself
;; ends the walk), and the vectors, tables and strings bound at top level.
(define (%freeze! top)
  (let ((seen '()))
    (define (freeze e)
      (unless (or (not (let? e)) (eq? e (rootlet)) (memq e seen))
        (set! seen (cons e seen))
        (for-each (lambda (b)
                    (let ((v (cdr b)))
                      (when (or (vector? v) (hash-table? v) (string? v)) (immutable! v))
                      (when (procedure? v)                ; a C function's funclet is the rootlet
                        (let up ((f (funclet v)))
                          (unless (or (not (let? f)) (eq? f top) (eq? f (rootlet)))
                            (freeze f)
                            (up (outlet f)))))
                      (immutable! (car b) e)))
                  e)
        (immutable! e)))
    (freeze top)))
