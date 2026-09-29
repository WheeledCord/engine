;;;; This Source Code Form is subject to the terms of the Mozilla Public
;;;; License, v. 2.0. If a copy of the MPL was not distributed with this
;;;; file, You can obtain one at https://mozilla.org/MPL/2.0/.

;;;; Rule 3's cousin: gameplay code opens a session, which a replay or another machine would do again.
(define-kind soldier
  (on (tick dt) (host-game 7777)))

(spawn 'soldier)
