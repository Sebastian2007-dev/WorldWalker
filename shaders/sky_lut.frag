#version 460
#include "common.glsl"
#include "atmosphere.glsl"

// Renders the sky-view LUT for the current camera altitude and sun elevation (see skyLutUv in common.glsl).
layout(push_constant, scalar) uniform Push { FrameRef frame; } pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    FrameRef f = pc.frame;
    float az, el;
    skyLutAngles(vUv, skyHorizonElevation(f.camAltitude), az, el);
    // Local frame: z up, sun in the xz plane.
    float mu = clamp(dot(f.sunDir, f.camUp), -1.0, 1.0);
    vec3 sun = vec3(sqrt(1.0 - mu * mu), 0.0, mu);
    vec3 rd = vec3(cos(az) * cos(el), sin(az) * cos(el), sin(el));
    vec3 ro = vec3(0.0, 0.0, kRg + max(f.camAltitude, 1.0) * 1e-3);
    outColor = vec4(atmoSkyRadiance(ro, rd, sun, 32), 1.0);
}
