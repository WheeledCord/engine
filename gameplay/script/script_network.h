/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef GAMEPLAY_SCRIPT_NETWORK_H
#define GAMEPLAY_SCRIPT_NETWORK_H
#include "core/net_session.h"

/* Networking as a Scheme-facing engine type, on top of core/net_session.h and core/net_sync.h: a
   game marks a property ENGINE_PROPERTY_SHARED (core/object.h) on a type it already declared for
   scripts, and a "network" object takes it from there.

   Made with (make 'network "game-name" version). At creation it registers one net schema for every
   type already registered in the same object pool that has at least one shared property
   (CoreNetFieldsFromType), in registration order -- schema id is that position plus one, name is
   the type's name. Every machine runs the same script, so every machine registers the same types in
   the same order and agrees on the ids without a word being said over the wire. A type whose shared
   property cannot go on the wire -- a string, an object, a computed one, or too many to fit -- fails
   the whole make rather than quietly leaving that type unreplicated.

   status (string: "off", "connecting", "welcomed", "active", "failed"), actor (int), server (bool),
   refusal (string, CoreNetRefusalText), port (int: the bound UDP port once hosting, useful when
   host! was given 0 and the system picked one).

   host!(port) and join!(address port) answer whether the attempt is out, the way CoreNetSessionHost
   and CoreNetSessionJoin do; ready! and leave! wrap CoreNetSessionReady and CoreNetSessionLeave.
   share!(object) is server only: it spawns a replicated object whose state IS object's own storage
   (CoreNetSyncBindState with EngineObjectData) and remembers the pairing; mine?(object) says
   whether this machine decides the paired replicated object, the way CoreNetSessionIsMine does.

   On a joiner, an object a snapshot brings becomes an engine object of the matching type (its
   schema's name, looked up in the same pool), bound the same way, and the signal "appeared" carries
   it; one a snapshot takes away destroys the engine object it was paired with. Destroying the
   network object, or leave!, destroys every object it created this way -- an object share! was
   given is the caller's, and is never destroyed here.

   Signals: "joined" (actor) and "left" (actor) on the server, "welcomed" and "appeared" (object) on
   a joiner. A command or an event from Scheme is not part of this: see docs/user/networking.md. */
extern const EngineType ScriptNetworkType;

#endif
