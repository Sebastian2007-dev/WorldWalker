#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push { FrameRef frame; } pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) flat in uvec2 vTex;
layout(location = 4) in float vHeight;
layout(location = 5) flat in float vFade;
layout(location = 6) in vec3 vCrownN;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 a = texture(textures[nonuniformEXT(vTex.x)], vUv);
    // Keep coverage in distant mips: sharpen alpha with the mip level.
    float lod = textureQueryLod(textures[nonuniformEXT(vTex.x)], vUv).x;
    float alpha = a.a * (1.0 + max(lod, 0.0) * 0.3);
    if (alpha < 0.5 || vFade < screenDither(gl_FragCoord.xy)) discard;

    FrameRef f = pc.frame;
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    vec4 nr = texture(textures[nonuniformEXT(vTex.y)], vUv);
    vec3 tn = vec3(nr.xy * 2.0 - 1.0, 0.0);
    tn.y = -tn.y;
    tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
    n = perturbNormal(n, vRel, vUv, tn);
    vec3 v = -normalize(vRel);

    // Crown self-occlusion: darker towards the bottom and inside. Shadow maps add the rest.
    float ao = mix(0.5, 1.0, smoothstep(0.1, 0.95, vHeight));
    float sh = sunShadow(f, vRel, normalize(vNormal), 2.0);
    vec3 nb = normalize(mix(n, normalize(vCrownN), 0.6));
    vec3 color = shadeFoliage(f, foliageAlbedo(a.rgb, 0.15), nb, v, ao, sh) * mix(0.75, 1.0, vHeight);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
