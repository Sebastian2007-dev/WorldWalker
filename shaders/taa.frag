#version 460
#include "common.glsl"

// Temporal anti-aliasing resolve: reproject last frame's result through the depth buffer, clip it to the colour
// range of the current 3x3 neighbourhood (kills ghosting) and blend a little of the new jittered frame in.
layout(push_constant, scalar) uniform Push {
    mat4 reproject;   // current camera relative position -> previous clip space (unjittered)
    FrameRef frame;
    uint current;     // this frame's HDR image
    uint history;     // last frame's resolved image
    uint depth;
    float blend;      // weight of the current frame, 1 = no history
    uint ao;          // ambient occlusion image, ~0u when disabled
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// Work on a compressed range so a few very bright pixels do not dominate the neighbourhood statistics.
vec3 toWork(vec3 c) {
    c /= 1.0 + max(c.r, max(c.g, c.b));
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 fromWork(vec3 y) {
    vec3 c = vec3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
    return c / max(1.0 - max(c.r, max(c.g, c.b)), 1e-4);
}

vec3 fetchHistory(vec2 uv) { return texture(textures[nonuniformEXT(pc.history)], uv).rgb; }

// Catmull-Rom history fetch from 5 bilinear taps: keeps the image sharp over many frames of resampling.
vec3 sampleHistory(vec2 uv, vec2 size) {
    vec2 pos = uv * size, c = floor(pos - 0.5) + 0.5, f = pos - c;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2, t0 = (c - 1.0) / size, t3 = (c + 2.0) / size, t12 = (c + w2 / w12) / size;
    vec3 r = fetchHistory(vec2(t12.x, t0.y)) * (w12.x * w0.y) + fetchHistory(vec2(t0.x, t12.y)) * (w0.x * w12.y) +
             fetchHistory(t12) * (w12.x * w12.y) + fetchHistory(vec2(t3.x, t12.y)) * (w3.x * w12.y) +
             fetchHistory(vec2(t12.x, t3.y)) * (w12.x * w3.y);
    float wsum = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(r / wsum, vec3(0.0));
}

vec3 currentAt(ivec2 px) {
    vec3 c = texelFetch(textures[nonuniformEXT(pc.current)], px, 0).rgb;
    return pc.ao == ~0u ? c : c * texelFetch(textures[nonuniformEXT(pc.ao)], px, 0).r;
}

void main() {
    FrameRef f = pc.frame;
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec3 cur = currentAt(px);
    if (pc.blend >= 1.0) {
        outColor = vec4(cur, 1.0);
        return;
    }

    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 c = toWork(currentAt(px + ivec2(x, y)));
            m1 += c;
            m2 += c * c;
        }
    vec3 mean = m1 / 9.0, sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // Homogeneous reprojection: works for geometry and for the sky at infinite depth (w = 0) alike.
    float d = texelFetch(textures[nonuniformEXT(pc.depth)], px, 0).r;
    vec2 ndc = (vec2(px) + 0.5) / f.viewport * 2.0 - 1.0;
    vec4 prev = pc.reproject * (f.invViewProj * vec4(ndc, d, 1.0));
    vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
    if (prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
        outColor = vec4(cur, 1.0);
        return;
    }

    vec3 hist = toWork(sampleHistory(prevUv, f.viewport));
    // Clip the history towards the neighbourhood mean instead of clamping per channel (keeps hue).
    vec3 dir = hist - mean;
    vec3 ext = max(sigma * 1.25, vec3(1e-5));
    float k = max(max(abs(dir.x) / ext.x, abs(dir.y) / ext.y), abs(dir.z) / ext.z);
    if (k > 1.0) hist = mean + dir / k;

    vec3 res = mix(hist, toWork(cur), pc.blend);
    outColor = vec4(fromWork(res), 1.0);
}
