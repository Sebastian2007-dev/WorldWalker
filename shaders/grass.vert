#version 460
#include "common.glsl"

// Procedural grass: one instance per cell of a world-anchored grid around the camera, one blade per instance
// (7 vertex triangle strip). Ground height and land cover come from a small field sampled on the CPU.

layout(buffer_reference, scalar) readonly buffer GrassFieldRef {
    vec3 east;  float x0;
    vec3 north; float y0;
    vec3 up;    float cell;
    vec3 anchorMod; uint res;
    vec2 s[];   // (height above the anchor tangent plane, land cover class)
};

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    GrassFieldRef field;
    vec3 anchorRel;   // field anchor relative to the camera
    float spacing;
    vec2 camLocal;    // camera in anchor tangent plane coordinates
    uint gridN;
    float radius;
} pc;

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;
layout(location = 3) out float vT;   // 0 at the root, 1 at the tip

vec2 fieldAt(GrassFieldRef g, ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(int(g.res) - 1));
    return g.s[p.y * int(g.res) + p.x];
}

// Blade density (0..1) and height in metres per land cover class.
void grassParams(uint cls, out float density, out float height) {
    density = 0.0;
    height = 0.4;
    if (cls == 30u)      { density = 1.0;  height = 0.45; }  // grassland
    else if (cls == 40u) { density = 0.9;  height = 0.6; }   // cropland
    else if (cls == 20u) { density = 0.7;  height = 0.5; }   // shrubland
    else if (cls == 90u) { density = 0.9;  height = 0.55; }  // wetland
    else if (cls == 10u) { density = 0.1;  height = 0.25; }  // forest floor: mostly needles and moss
    else if (cls == 50u) { density = 0.35; height = 0.25; }  // built-up
    else if (cls == 100u){ density = 0.4;  height = 0.12; }  // moss / lichen
    else if (cls == 95u) { density = 0.2;  height = 0.4; }   // mangroves
    else if (cls == 60u) { density = 0.06; height = 0.25; }  // bare / sparse
}

void main() {
    FrameRef f = pc.frame;
    GrassFieldRef g = pc.field;
    int n = int(pc.gridN);
    ivec2 cell = ivec2(floor(pc.camLocal / pc.spacing)) - n / 2 + ivec2(gl_InstanceIndex % n, gl_InstanceIndex / n);
    uvec3 key = uvec3(cell + (1 << 20), 0u);
    float r0 = hash13(key), r1 = hash13(key + uvec3(0, 0, 1)), r2 = hash13(key + uvec3(0, 0, 2));
    float r3 = hash13(key + uvec3(0, 0, 3)), r4 = hash13(key + uvec3(0, 0, 4));
    vec2 local = (vec2(cell) + vec2(r0, r1)) * pc.spacing;
    float dist = length(local - pc.camLocal);

    // Ground from the field: bilinear height, nearest land cover, normal from the height gradient.
    vec2 gp = (local - vec2(g.x0, g.y0)) / g.cell;
    ivec2 gi = ivec2(floor(gp));
    vec2 gf = gp - vec2(gi);
    vec2 s00 = fieldAt(g, gi), s10 = fieldAt(g, gi + ivec2(1, 0));
    vec2 s01 = fieldAt(g, gi + ivec2(0, 1)), s11 = fieldAt(g, gi + ivec2(1, 1));
    float z = mix(mix(s00.x, s10.x, gf.x), mix(s01.x, s11.x, gf.x), gf.y);
    uint cls = uint(fieldAt(g, ivec2(round(gp))).y + 0.5);
    vec3 ng = normalize(g.up * g.cell + g.east * (mix(s00.x, s01.x, gf.y) - mix(s10.x, s11.x, gf.y)) +
                        g.north * (mix(s00.x, s10.x, gf.x) - mix(s01.x, s11.x, gf.x)));
    vec3 root = pc.anchorRel + g.east * local.x + g.north * local.y + g.up * z;
    vec3 worldMod = g.anchorMod + g.east * local.x + g.north * local.y + g.up * z;

    float density, height;
    grassParams(cls, density, height);
    density *= 1.0 - smoothstep(0.2, 0.35, 1.0 - dot(ng, g.up));  // no grass on rock faces
    float snowLine = 2800.0 + (worldNoise(worldMod, 64.0) - 0.5) * 400.0;
    density *= 1.0 - smoothstep(snowLine - 150.0, snowLine + 50.0, f.camAltitude + dot(root, f.camUp));
    // Far blades get sparser and wider, then sink into the ground at the edge of the radius.
    float thin = mix(1.0, 0.3, smoothstep(6.0, pc.radius, dist));
    bool inField = all(greaterThanEqual(gp, vec2(1.0))) && all(lessThan(gp, vec2(float(g.res) - 2.0)));
    bool visible = inField && dist < pc.radius && r2 < density * thin;
    for (int k = 0; k < 5 && visible; ++k)
        visible = dot(f.frustum[k].xyz, root) + f.frustum[k].w > -1.0;
    if (!visible) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);  // outside the clip volume, the whole strip collapses
        return;
    }

    float hgt = height * (0.45 + 1.1 * r3 * r3) * (1.0 - smoothstep(pc.radius * 0.7, pc.radius, dist));
    float width = (0.022 + 0.012 * r4) / sqrt(thin);
    int v = gl_VertexIndex;
    float t = float(v / 2) / 3.0;
    float side = v == 6 ? 0.0 : float(v & 1) * 2.0 - 1.0;
    float ang = r4 * 6.2831853 + r0 * 3.0;
    vec3 right = g.east * cos(ang) + g.north * sin(ang);
    vec3 fwd = cross(g.up, right);

    // Wind: slow gusts rolling across the field plus a little per-blade flutter.
    vec2 windDir = normalize(vec2(0.8, 0.6));
    float gust = sin(f.time * 1.3 - dot(local, windDir) * 0.35) * 0.5 + 0.5;
    float flutter = sin(f.time * 4.0 + r0 * 20.0) * 0.08;
    vec3 wind = (g.east * windDir.x + g.north * windDir.y) * (0.1 + 0.35 * gust) + fwd * flutter;
    vec3 bend = (fwd * (0.1 + 0.3 * r1 * r1) + wind) * hgt;

    vec3 p = root + g.up * (hgt * t) + bend * (t * t) + right * (side * width * (1.0 - t) * 0.5);
    vec3 tangent = g.up * hgt + bend * (2.0 * t);
    vec3 bn = normalize(cross(right, tangent));
    if (dot(bn, -p) < 0.0) bn = -bn;  // two sided: face the viewer
    vNormal = normalize(mix(bn, ng, 0.6));

    // Living blades are greener than the ground mix underneath (which includes soil and litter).
    vec3 base = groundTint(cls, worldMod, worldNoise(worldMod, 8.0)) * vec3(0.8, 1.0, 0.75) *
                (0.8 + 0.4 * r2 / max(density * thin, 1e-3));
    vColor = mix(base, base * vec3(1.08, 1.05, 0.85), t * 0.5);
    vT = t;
    vRel = p;
    gl_Position = f.viewProj * vec4(p, 1.0);
}
