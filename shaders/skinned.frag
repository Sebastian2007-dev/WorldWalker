#version 460
#include "common.glsl"
#include "skin.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 a = texture(textures[nonuniformEXT(pc.albedo)], vUv);
    // Hair: dithered alpha test, TAA turns it into soft strands.
    if (pc.alphaTest != 0u && a.a < mix(0.2, 0.8, screenDither(gl_FragCoord.xy))) discard;
    FrameRef f = pc.frame;
    vec3 ng = normalize(vNormal);
    if (!gl_FrontFacing) ng = -ng;
    vec4 nr = texture(textures[nonuniformEXT(pc.normal)], vUv);
    vec3 tn = vec3(nr.xy * 2.0 - 1.0, 0.0);
    tn.y = -tn.y;  // V was flipped at export
    tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
    vec3 n = perturbNormal(ng, vRel, vUv, tn);
    vec3 v = -normalize(vRel);
    float sh = sunShadow(f, vRel, ng, 1.0);
    vec3 color = shadeSurface(f, a.rgb, n, v, max(nr.z, 0.35), 1.0, sh);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
