#version 120
varying vec2 fragTexCoord;
uniform sampler2D texture0;
uniform float nearPlane;
uniform float farPlane;
void main() {
    float d=texture2D(texture0,fragTexCoord).r;
    float z=d*2.0-1.0;
    float linear=(2.0*nearPlane*farPlane)/(farPlane+nearPlane-z*(farPlane-nearPlane));
    gl_FragColor=vec4(vec3(1.0/(1.0+linear*0.12)),1.0);
}
