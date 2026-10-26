#version 460
#include "common.glsl"
#include "terrain.glsl"

layout(location = 0) out vec3 vRel;      // camera relative position
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vWorldMod; // ECEF mod 4096, for seamless noise
layout(location = 4) out float vAboveWater; // height above the chunk's lake level (metres)

void main() {
    TerrainVertex v = pc.vertices.d[gl_VertexIndex];
    vec3 pos = v.pos;
    if (hasLake()) {
        float above = heightAboveLake(pos);
        float bed = lakeBed(above, v.uv);
        pos += pc.up * (bed - above);
        vAboveWater = bed;
    } else {
        vAboveWater = 1e9;
    }
    vec3 rel = pc.originRel + pos;
    vRel = rel;
    vNormal = octDecode(v.normal);
    vUv = v.uv;
    vWorldMod = pc.originMod + pos;
    gl_Position = pc.frame.viewProj * vec4(rel, 1.0);
}
