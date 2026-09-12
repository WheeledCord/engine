/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H
/* The shader loader injects this same value after GLSL's #version line. */
#define CORE_BONE_CAPACITY 64
// Frame textures bind above raylib's own units: materials use 0..MAX_MATERIAL_MAPS-1 (12) in
// DrawMesh, and the batch claims 1..4 for its extra samplers. Nothing of raylib's touches these.
#define CORE_FRAME_TEXTURE_FIRST_UNIT 12
#endif
