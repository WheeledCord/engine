#version 120
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
// Writes the object-space position straight out as colour, so a baked texture can be checked
// against the geometry that produced it rather than against another rendering of it.
varying vec3 fragPosition;
void main() { gl_FragColor=vec4(fragPosition,1.0); }
