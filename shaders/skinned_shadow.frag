#version 460
#include "common.glsl"
#include "skin.glsl"

// Shadow pass for the character: depth only, hair cut out by its alpha.
layout(location = 2) in vec2 vUv;

void main() {
    if (pc.alphaTest != 0u && texture(textures[nonuniformEXT(pc.albedo)], vUv).a < 0.5) discard;
}
