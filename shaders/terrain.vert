#version 460
#include "common.glsl"

struct TerrainVertex { vec3 pos; uint normal; vec2 uv; };
layout(buffer_reference, scalar) readonly buffer TerrainVertexRef { TerrainVertex d[]; };

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    TerrainVertexRef vertices;
    uint64_t cover;
    vec3 originRel;
    float level;
    vec3 originMod;
    float coverRes;
} pc;

layout(location = 0) out vec3 vRel;      // camera relative position
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vWorldMod; // ECEF mod 4096, for seamless noise

void main() {
    TerrainVertex v = pc.vertices.d[gl_VertexIndex];
    vec3 rel = pc.originRel + v.pos;
    vRel = rel;
    vNormal = octDecode(v.normal);
    vUv = v.uv;
    vWorldMod = pc.originMod + v.pos;
    gl_Position = pc.frame.viewProj * vec4(rel, 1.0);
}
