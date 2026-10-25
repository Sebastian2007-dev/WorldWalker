#version 460
#include "common.glsl"

// Screen space ambient occlusion from the depth buffer. Noisy on purpose (8 samples, rotated per pixel and per
// frame): the TAA resolve applies it and averages the noise away over time.
layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    uint depth;
    uint frameIndex;
    float strength;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 positionAt(FrameRef f, vec2 pixel, float d) {
    vec4 p = f.invViewProj * vec4(pixel / f.viewport * 2.0 - 1.0, d, 1.0);
    return p.xyz / p.w;
}
float depthAt(ivec2 px) { return texelFetch(textures[nonuniformEXT(pc.depth)], px, 0).r; }

void main() {
    FrameRef f = pc.frame;
    ivec2 px = ivec2(gl_FragCoord.xy);
    float d = depthAt(px);
    if (d <= 0.0) {  // sky
        outColor = vec4(1.0);
        return;
    }
    vec2 c = vec2(px) + 0.5;
    vec3 P = positionAt(f, c, d);
    float dist = length(P);
    if (dist > 200.0) {
        outColor = vec4(1.0);
        return;
    }
    // Normal from the depth buffer: take the smaller difference on each axis so edges do not bleed.
    vec3 pr = positionAt(f, c + vec2(1, 0), depthAt(px + ivec2(1, 0))) - P;
    vec3 pl = P - positionAt(f, c - vec2(1, 0), depthAt(px - ivec2(1, 0)));
    vec3 pu = positionAt(f, c + vec2(0, 1), depthAt(px + ivec2(0, 1))) - P;
    vec3 pd = P - positionAt(f, c - vec2(0, 1), depthAt(px - ivec2(0, 1)));
    vec3 n = normalize(cross(dot(pr, pr) < dot(pl, pl) ? pr : pl, dot(pu, pu) < dot(pd, pd) ? pu : pd));
    if (dot(n, P) > 0.0) n = -n;

    // Tangent frame around the normal, rotated per pixel and per frame.
    float rot = (screenDither(gl_FragCoord.xy) + float(pc.frameIndex % 8u) * 0.618034) * 6.2831853;
    vec3 t = normalize(abs(n.z) < 0.9 ? cross(n, vec3(0, 0, 1)) : cross(n, vec3(1, 0, 0)));
    vec3 b = cross(n, t);
    float radius = clamp(dist * 0.03, 0.4, 2.5);
    const int N = 8;
    float occ = 0.0;
    for (int i = 0; i < N; ++i) {
        // Golden angle spiral over the hemisphere, samples denser near the centre.
        float u = (float(i) + 0.5) / float(N);
        float a = float(i) * 2.39996323 + rot;
        float sinT = sqrt(u), cosT = sqrt(1.0 - u);
        vec3 dir = (t * cos(a) + b * sin(a)) * sinT + n * cosT;
        vec3 S = P + dir * radius * mix(0.2, 1.0, u * u);
        vec4 clip = f.viewProj * vec4(S, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 spx = (clip.xy / clip.w * 0.5 + 0.5) * f.viewport;
        if (any(lessThan(spx, vec2(0.0))) || any(greaterThanEqual(spx, f.viewport))) continue;
        float sd = depthAt(ivec2(spx));
        if (sd <= clip.z / clip.w) continue;  // reversed Z: the scene there is further away than the sample
        vec3 Q = positionAt(f, spx, sd);
        // Range check: geometry far in front of the sample (thin blades, tree trunks) does not occlude.
        occ += 1.0 - smoothstep(radius * 0.5, radius * 1.5, length(P) - length(Q));
    }
    float ao = 1.0 - occ / float(N) * pc.strength;
    ao = mix(ao, 1.0, smoothstep(120.0, 200.0, dist));
    outColor = vec4(ao, ao, ao, 1.0);
}
