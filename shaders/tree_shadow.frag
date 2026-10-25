#version 460
#include "common.glsl"

// Shadow pass for alpha-tested leaves: depth only, same coverage rules as tree_leaf.frag.
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in uvec2 vTex;
layout(location = 5) flat in float vFade;

void main() {
    vec4 a = texture(textures[nonuniformEXT(vTex.x)], vUv);
    float lod = textureQueryLod(textures[nonuniformEXT(vTex.x)], vUv).x;
    float alpha = a.a * (1.0 + max(lod, 0.0) * 0.3);
    if (alpha < 0.5 || vFade < screenDither(gl_FragCoord.xy)) discard;
}
