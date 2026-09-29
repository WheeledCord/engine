;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 1, read: a gameplay handler reads a :local field.
(define-kind soldier
  (field health 100)
  (field hurt 0.0 :local)
  (on (tick dt) (set! health (- health hurt))))

(spawn 'soldier)
