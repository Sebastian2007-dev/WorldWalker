#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    uint64_t vertices;
    VisibleRef visible;
    VegCellRef cells;
    DrawInfoRef info;
    VariantRef variants;
    uint infoBase;
} pc;

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out uvec2 vTex;
layout(location = 3) flat out mat3 vToWorld;  // tree local -> world (ECEF axes)
layout(location = 6) flat out float vFade;
layout(location = 7) out float vHeight;
layout(location = 8) out vec3 vCrownN;

// Must match hemi_oct_decode / frame_basis in tools/blender/bake_tree.py.
vec3 hemiOctDecode(vec2 uv) {
    vec2 p = vec2(uv.x + uv.y, uv.x - uv.y) * 0.5;
    return normalize(vec3(p, 1.0 - abs(p.x) - abs(p.y)));
}
vec2 hemiOctEncode(vec3 d) {
    vec2 p = d.xy / (abs(d.x) + abs(d.y) + d.z);
    return vec2(p.x + p.y, p.x - p.y);
}

void main() {
    VisibleTree t = pc.visible.d[gl_InstanceIndex];
    VegCell c = pc.cells.d[t.cell];
    DrawInfo di = pc.info.d[gl_DrawID];
    Variant var = pc.variants.d[di.variant];

    vec3 tx = c.east * t.cs.x + c.north * t.cs.y;
    vec3 ty = -c.east * t.cs.y + c.north * t.cs.x;
    vec3 tz = c.up;
    mat3 toWorld = mat3(tx, ty, tz);

    vec3 center = t.pos + tz * (var.impCenterZ * t.scale);
    // In the shadow pass the impostor faces the sun instead of the camera.
    vec3 toCam = pc.frame.shadowPass > 0.5 ? pc.frame.sunDir : normalize(-center);
    vec3 dl = vec3(dot(toCam, tx), dot(toCam, ty), max(dot(toCam, tz), 0.0));
    dl = normalize(dl + vec3(0, 0, 1e-4));

    float grid = float(var.impGrid);
    vec2 cell = clamp(floor((hemiOctEncode(dl) * 0.5 + 0.5) * grid), vec2(0.0), vec2(grid - 1.0));
    vec3 d = hemiOctDecode((cell + 0.5) / grid * 2.0 - 1.0);
    vec3 right = abs(d.z) < 0.999 ? normalize(cross(-d, vec3(0, 0, 1))) : vec3(1, 0, 0);
    vec3 up = normalize(cross(right, -d));

    const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 q = corners[gl_VertexIndex];
    float r = var.impRadius * t.scale;
    vec3 rel = center + toWorld * (right * q.x + up * q.y) * r;

    vCrownN = toWorld * (right * q.x + up * q.y + d * 0.7);

    vRel = rel;
    vUv = vec2(cell.x + q.x * 0.5 + 0.5, cell.y + 0.5 - q.y * 0.5) / grid;
    vTex = uvec2(var.impAlbedo, var.impNormal);
    vToWorld = toWorld;
    vFade = t.fade;
    vHeight = clamp((var.impCenterZ + q.y * var.impRadius) / max(var.height, 0.01), 0.0, 1.0);
    gl_Position = pc.frame.viewProj * vec4(rel, 1.0);
}
