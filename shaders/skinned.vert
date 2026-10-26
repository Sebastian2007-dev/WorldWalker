#version 460
#include "common.glsl"
#include "skin.glsl"

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out float vOcclusion;  // body: crease occlusion, stored as the normal's length (render/Human)

void main() {
    SkinVertex v = pc.vertices.d[gl_VertexIndex];
    vec4 w = unpackUnorm4x8(v.weights);
    uvec4 j = uvec4(v.joints & 255u, (v.joints >> 8) & 255u, (v.joints >> 16) & 255u, v.joints >> 24);
    mat4 skin = pc.joints.m[j.x] * w.x + pc.joints.m[j.y] * w.y + pc.joints.m[j.z] * w.z + pc.joints.m[j.w] * w.w;
    vec4 p = pc.model * (skin * vec4(v.pos, 1.0));
    vRel = p.xyz;
    vNormal = mat3(pc.model) * (mat3(skin) * v.normal);
    vUv = v.uv;
    vOcclusion = min(length(v.normal), 1.0);
    gl_Position = pc.frame.viewProj * p;
}
