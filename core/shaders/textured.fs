#version 120
varying vec2 fragTexCoord;
varying vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
void main() { gl_FragColor=texture2D(texture0,fragTexCoord)*colDiffuse*fragColor; }
