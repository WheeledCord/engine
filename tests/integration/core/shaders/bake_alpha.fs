#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Full-strength colour behind a half alpha. If the bake blends, the colour arrives halved.
void main() { gl_FragColor=vec4(1.0,1.0,1.0,0.5); }
