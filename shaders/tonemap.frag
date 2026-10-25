#version 460
#include "common.glsl"

layout(buffer_reference, scalar) readonly buffer ExposureRef { float value; };  // written by exposure.comp
layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    uint hdrIndex;
    float exposure;
    float menuFade;
    ExposureRef autoExposure;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

// ACES filmic approximation (Narkowicz).
vec3 aces(vec3 x) {
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 hdr;
    if (pc.menuFade > 0.001) {
        // Pause menu backdrop: rotated golden-angle disc blur, radius grows with the fade.
        FrameRef f = pc.frame;
        vec2 px = 1.0 / f.viewport;
        float radius = 16.0 * pc.menuFade * (f.viewport.y / 1080.0);
        float rot = screenDither(gl_FragCoord.xy) * 6.2831853;
        const int N = 32;
        hdr = vec3(0.0);
        for (int i = 0; i < N; ++i) {
            float r = sqrt((float(i) + 0.5) / float(N)) * radius;
            float a = float(i) * 2.39996323 + rot;
            hdr += texture(textures[nonuniformEXT(pc.hdrIndex)], vUv + vec2(cos(a), sin(a)) * r * px).rgb;
        }
        hdr /= float(N);
    } else {
        hdr = texture(textures[nonuniformEXT(pc.hdrIndex)], vUv).rgb;
    }
    vec3 c = aces(hdr * pc.exposure * pc.autoExposure.value * 0.6);
    if (pc.menuFade > 0.001) {
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(c, vec3(luma), 0.4 * pc.menuFade) * (1.0 - 0.45 * pc.menuFade);
    }
    c += (screenDither(gl_FragCoord.xy) - 0.5) / 255.0;  // break up banding
    outColor = vec4(c, 1.0);  // swapchain is sRGB: hardware applies the transfer curve
}
