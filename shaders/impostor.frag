#version 460
#include "common.glsl"

layout(push_constant, scalar) uniform Push { FrameRef frame; } pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in uvec2 vTex;
layout(location = 3) flat in mat3 vToWorld;
layout(location = 6) flat in float vFade;
layout(location = 7) in float vHeight;
layout(location = 8) in vec3 vCrownN;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 a = texture(textures[nonuniformEXT(vTex.x)], vUv);
    if (a.a < 0.5 || vFade < screenDither(gl_FragCoord.xy)) discard;
    FrameRef f = pc.frame;
    vec3 nl = texture(textures[nonuniformEXT(vTex.y)], vUv).xyz * 2.0 - 1.0;
    vec3 n = normalize(vToWorld * nl);
    vec3 v = -normalize(vRel);
    float ao = mix(0.5, 1.0, smoothstep(0.1, 0.95, vHeight));
    float sh = sunShadow(f, vRel, n, 4.0);
    vec3 nb = normalize(mix(n, normalize(vCrownN), 0.6));
    vec3 color = shadeFoliage(f, foliageAlbedo(a.rgb, 0.15), nb, v, ao, sh) * mix(0.75, 1.0, vHeight);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
