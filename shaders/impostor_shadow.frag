#version 460
#include "common.glsl"

// Shadow pass for impostors: depth only, alpha tested.
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uvec2 vTex;
layout(location = 6) flat in float vFade;

void main() {
    if (texture(textures[nonuniformEXT(vTex.x)], vUv).a < 0.5 || vFade < screenDither(gl_FragCoord.xy)) discard;
}
