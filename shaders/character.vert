#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push {
    mat4 model;   // camera relative
    FrameRef frame;
    TreeVertexRef vertices;
    vec4 color;
} pc;

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vNormal;

void main() {
    TreeVertex v = pc.vertices.d[gl_VertexIndex];
    vec4 rel = pc.model * vec4(v.pos, 1.0);
    vRel = rel.xyz;
    vNormal = normalize(transpose(inverse(mat3(pc.model))) * v.normal);
    gl_Position = pc.frame.viewProj * rel;
}
