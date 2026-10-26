#version 460
#include "common.glsl"
#include "terrain.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec3 vWorldMod;
layout(location = 3) in float vDepth;
layout(location = 0) out vec4 outColor;

void main() {
    // Only lakes: other ground of the chunk below the level (a valley further down) stays dry.
    if (!nearWaterCover(ivec2(floor(vUv * pc.coverRes)))) discard;
    // The DEM has no lake depths, only a surface that strays a few metres from the level. Ground far below it is not a
    // lake bed but a valley next to the lake (an outflowing river), which must not be flooded at lake height.
    if (vDepth > 6.0) discard;
    // Shallow water near the shore is transparent; the wet lake bed shows through.
    float alpha = 1.0 - exp(-max(vDepth, 0.0) * 2.2);
    if (alpha < 0.01) discard;
    FrameRef f = pc.frame;
    float dist = length(vRel);
    vec3 v = -vRel / max(dist, 1e-3);
    vec3 color = shadeWater(f, vWorldMod, v, dist, sunShadow(f, vRel, f.camUp, 1.0));
    outColor = vec4(applyFog(f, color, vRel), alpha);
}
