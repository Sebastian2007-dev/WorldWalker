#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push { FrameRef frame; } pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;
layout(location = 3) in float vT;
layout(location = 0) out vec4 outColor;

void main() {
    FrameRef f = pc.frame;
    vec3 n = normalize(vNormal);
    vec3 v = -normalize(vRel);
    float sh = sunShadow(f, vRel, n, 1.5);
    float ao = mix(0.6, 1.0, vT);  // blades shade each other near the ground
    vec3 color = shadeFoliage(f, vColor, n, v, ao, sh) * mix(0.75, 1.0, vT);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
