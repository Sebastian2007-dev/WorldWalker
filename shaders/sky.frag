#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push { FrameRef frame; uint hdrIndex; float exposure; } pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// Cloud layer seen along `dir`: rgb = radiance, a = coverage.
vec4 cloudLayer(FrameRef f, vec3 dir) {
    float t = cloudLayerDistance(f, dot(dir, f.camUp));
    if (t <= 0.0 || t > 120000.0) return vec4(0.0);
    vec3 p = f.camMod64k + dir * t + cloudWind(f);
    float dens = cloudDensity(f, p, 5);
    if (dens <= 0.0) return vec4(0.0);
    // Self shadowing: how much cloud lies between this point and the sun.
    float d1 = cloudDensity(f, p + f.sunDir * 300.0, 4), d2 = cloudDensity(f, p + f.sunDir * 900.0, 3);
    float light = exp(-(d1 * 1.0 + d2 * 0.6) * 1.4);
    // Forward scattering gives the bright rims around clouds in front of the sun.
    float mu = max(dot(dir, f.sunDir), 0.0);
    float phase = 0.5 + 1.5 * pow(mu, 6.0) + 4.0 * pow(mu, 60.0) * (1.0 - dens);
    vec3 col = f.sunColor * f.sunIntensity * light * phase * 0.55 + f.skyAmbient * (1.2 - 0.5 * dens);
    // Distant clouds fade into the sky (aerial perspective).
    col = mix(col, skyRadiance(f, dir), 1.0 - exp(-t / 35000.0));
    return vec4(col, dens * (1.0 - smoothstep(70000.0, 120000.0, t)));
}

void main() {
    FrameRef f = pc.frame;
    vec4 p = f.invViewProj * vec4(vUv * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w);
    vec3 sky = skyRadiance(f, dir) + sunDisk(f, dir);
    vec4 clouds = cloudLayer(f, dir);
    outColor = vec4(mix(sky, clouds.rgb, clouds.a), 1.0);
}
