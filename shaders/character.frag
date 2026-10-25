#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push {
    mat4 model;
    FrameRef frame;
    TreeVertexRef vertices;
    vec4 color;
} pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 outColor;

void main() {
    FrameRef f = pc.frame;
    vec3 n = normalize(vNormal);
    vec3 v = -normalize(vRel);
    vec3 color = shadeSurface(f, pc.color.rgb * 0.6, n, v, 0.7, 1.0, sunShadow(f, vRel, n, 1.0));
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
