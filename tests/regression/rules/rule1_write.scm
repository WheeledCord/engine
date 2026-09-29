;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 1, write: a presentation handler writes a shared field of a child.
(define-kind gun-model
  (field mesh ""))

(define-kind soldier
  (field weapon 'pistol)
  (child gun (gun-model))
  (on (frame dt) (set! (gun 'mesh) "shotgun.glb")))

(spawn 'soldier)
