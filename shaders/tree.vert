#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    TreeVertexRef vertices;
    VisibleRef visible;
    VegCellRef cells;
    DrawInfoRef info;
    VariantRef variants;
    uint infoBase;
} pc;

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) flat out uvec2 vTex;
layout(location = 4) out float vHeight;   // 0 at the base, 1 at the top
layout(location = 5) flat out float vFade;
layout(location = 6) out vec3 vCrownN;    // outward direction from the crown centre

void main() {
    VisibleTree t = pc.visible.d[gl_InstanceIndex];
    VegCell c = pc.cells.d[t.cell];
    DrawInfo di = pc.info.d[pc.infoBase + gl_DrawID];
    Variant var = pc.variants.d[di.variant];
    TreeVertex v = pc.vertices.d[gl_VertexIndex];

    vec3 lp = v.pos * t.scale;
    float hf = clamp(v.pos.z / var.height, 0.0, 1.0);

    // Wind, all in the world wind direction (same as the grass). The phase must come from something fixed per
    // tree: t.pos is camera relative and would speed the animation up or down while walking.
    FrameRef f = pc.frame;
    float treePhase = atan(t.cs.y, t.cs.x) * 3.0 + t.scale * 17.0;  // yaw and scale are random per instance
    float gust = 0.6 + 0.4 * sin(f.time * 0.37 + treePhase * 0.3);
    // Whole tree bends, more towards the top.
    float sway = (sin(f.time * 0.9 + treePhase) * 0.6 + sin(f.time * 2.1 + treePhase * 1.7) * 0.25) * gust;
    float bend = hf * hf * var.height * t.scale * 0.006;
    // Branches swing as a whole, more the further they reach out from the trunk. The phase varies smoothly
    // around the trunk (integer multiple of the angle, so no seam) so leaf cards are not torn apart.
    float branchPhase = treePhase + 2.0 * atan(v.pos.y, v.pos.x) + v.pos.z * 0.4;
    float branch = sin(f.time * 1.6 + branchPhase) * 0.02 * length(v.pos.xy) * t.scale * gust;
    // Leaves only shiver a few millimetres.
    float leaf = sin(f.time * 4.5 + dot(v.pos, vec3(1.7, 2.3, 3.1))) * 0.005 * t.scale * hf;

    vec2 r = vec2(lp.x * t.cs.x - lp.y * t.cs.y, lp.x * t.cs.y + lp.y * t.cs.x);
    vec2 rn = vec2(v.normal.x * t.cs.x - v.normal.y * t.cs.y, v.normal.x * t.cs.y + v.normal.y * t.cs.x);
    vec3 windDir = c.east * 0.8 + c.north * 0.6;
    vec3 rel = t.pos + c.east * r.x + c.north * r.y + c.up * lp.z + windDir * (sway * bend + branch) +
               c.up * (branch * 0.4 + leaf);

    vec3 co = v.pos - vec3(0.0, 0.0, var.height * 0.6);
    co.z *= 0.6;  // crowns are taller than wide
    vec2 rc = vec2(co.x * t.cs.x - co.y * t.cs.y, co.x * t.cs.y + co.y * t.cs.x);
    vCrownN = c.east * rc.x + c.north * rc.y + c.up * co.z;

    vRel = rel;
    vNormal = c.east * rn.x + c.north * rn.y + c.up * v.normal.z;
    vUv = v.uv;
    vTex = uvec2(di.albedo, di.normal);
    vHeight = hf;
    vFade = t.fade;
    gl_Position = f.viewProj * vec4(rel, 1.0);
}
