#version 460
#include "common.glsl"
#include "terrain.glsl"

// Lake surface: the chunk's terrain grid flattened onto its lake level. The terrain drawn before it hides the water
// wherever the ground is higher, so the shore is where the level surface meets the terrain.
layout(location = 0) out vec3 vRel;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec3 vWorldMod;
layout(location = 3) out float vDepth;  // water depth over the (lake bed) ground

void main() {
    TerrainVertex v = pc.vertices.d[gl_VertexIndex];
    float above = heightAboveLake(v.pos);
    vDepth = -lakeBed(above, v.uv);
    vec3 pos = v.pos - pc.up * above;
    vRel = pc.originRel + pos;
    vUv = v.uv;
    vWorldMod = pc.originMod + pos;
    gl_Position = pc.frame.viewProj * vec4(vRel, 1.0);
}
