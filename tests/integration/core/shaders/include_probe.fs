#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Compiles only if the include was expanded: the function it calls is defined nowhere else.
#include "tests/integration/core/shaders/include_fragment.glsl"
void main() { gl_FragColor=vec4(IncludedValue(),0.0,0.0,1.0); }
