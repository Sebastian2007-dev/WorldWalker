#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push { FrameRef frame; } pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in uvec2 vTex;
layout(location = 4) in float vHeight;
layout(location = 5) flat in float vFade;
layout(location = 0) out vec4 outColor;

void main() {
    if (vFade < screenDither(gl_FragCoord.xy)) discard;
    FrameRef f = pc.frame;
    vec3 albedo = texture(textures[nonuniformEXT(vTex.x)], vUv).rgb;
    vec4 nr = texture(textures[nonuniformEXT(vTex.y)], vUv);
    vec3 tn = vec3(nr.xy * 2.0 - 1.0, 0.0);
    tn.y = -tn.y;  // V was flipped at export
    tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
    vec3 n = perturbNormal(normalize(vNormal), vRel, vUv, tn);
    vec3 v = -normalize(vRel);
    float ao = mix(0.45, 1.0, smoothstep(0.0, 0.6, vHeight));
    float sh = sunShadow(f, vRel, normalize(vNormal), 1.0);
    vec3 color = shadeSurface(f, albedo, n, v, max(nr.z, 0.4), ao, sh) * mix(0.7, 1.0, vHeight);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
