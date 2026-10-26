#version 460
#include "common.glsl"
#include "skin.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 0) out vec4 outColor;

float ggx(float ndh, float rough) {
    float a2 = rough * rough * rough * rough;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float schlick(float f0, float c) { return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); }

// Skin: light scattered under the skin wraps further around the terminator in red than in blue and softens shadow
// edges the same way. Two specular lobes (broad sheen plus a tight oily highlight) and a fresnel rim from the sky.
vec3 shadeSkin(FrameRef f, vec3 albedo, vec3 n, vec3 v, float shadow) {
    float ndl = dot(n, f.sunDir);
    const vec3 wrap = vec3(0.2, 0.11, 0.08);
    vec3 diffuse = clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
    vec3 shade = mix(vec3(shadow), vec3(sqrt(shadow)), vec3(0.18, 0.07, 0.04));
    vec3 h = normalize(f.sunDir + v);
    float ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0), ndv = max(dot(n, v), 1e-3);
    float spec = (0.75 * ggx(ndh, 0.52) + 0.25 * ggx(ndh, 0.3)) * schlick(0.028, vdh) * 0.25 * max(ndl, 0.0) * shadow;
    vec3 sun = f.sunColor * f.sunIntensity;
    vec3 rim = ambientLight(f, reflect(-v, n)) * schlick(0.028, ndv) * 0.6;
    return sun * (albedo * diffuse * shade + spec * PI) + ambientLight(f, n) * albedo + rim;
}

// Hair cards (Kajiya-Kay): a white highlight close to the strand's specular cone and a second, coloured one shifted
// towards the root. Strands run along the texture's v axis.
vec3 shadeHair(FrameRef f, vec3 albedo, vec3 n, vec3 v, float shadow) {
    vec3 dp1 = dFdx(vRel), dp2 = dFdy(vRel);
    vec2 duv1 = dFdx(vUv), duv2 = dFdy(vUv);
    vec3 b = cross(dp2, n) * duv1.y + cross(n, dp1) * duv2.y;
    b -= n * dot(n, b);
    vec3 t = dot(b, b) > 1e-12 ? normalize(b) : normalize(cross(n, vec3(0.0, 0.0, 1.0)) + 1e-4);
    vec3 h = normalize(f.sunDir + v);
    float ndl = dot(n, f.sunDir);
    float d1 = dot(normalize(t + n * 0.08), h), d2 = dot(normalize(t - n * 0.12), h);
    float s1 = pow(sqrt(max(1.0 - d1 * d1, 0.0)), 200.0) * 0.04;
    float s2 = pow(sqrt(max(1.0 - d2 * d2, 0.0)), 40.0) * 0.15;
    float lit = smoothstep(-0.25, 0.35, ndl) * shadow;
    float diffuse = max((ndl + 0.3) / 1.3, 0.0) * shadow;
    vec3 sun = f.sunColor * f.sunIntensity;
    return sun * (albedo * diffuse + (vec3(s1) + albedo * s2) * lit) + ambientLight(f, n) * albedo * 0.8;
}

// Eyes: wet cornea with a sharp sun highlight and the sky mirrored at grazing angles.
vec3 shadeEyes(FrameRef f, vec3 albedo, vec3 n, vec3 v, float shadow) {
    vec3 h = normalize(f.sunDir + v);
    float ndl = max(dot(n, f.sunDir), 0.0);
    float spec = ggx(max(dot(n, h), 0.0), 0.1) * schlick(0.04, dot(v, h)) * 0.25 * ndl * shadow;
    vec3 sun = f.sunColor * f.sunIntensity;
    vec3 sky = ambientLight(f, reflect(-v, n)) * schlick(0.04, dot(n, v)) * 0.3;
    // The lids shade the eyeball; without it the sclera glows in the ambient light.
    return sun * (albedo * ndl * shadow + spec * PI) + ambientLight(f, n) * albedo * 0.45 + sky;
}

void main() {
    vec4 a = texture(textures[nonuniformEXT(pc.albedo)], vUv);
    // Hair: dithered alpha test, TAA turns it into soft strands.
    if (pc.alphaTest != 0u && a.a < mix(0.2, 0.8, screenDither(gl_FragCoord.xy))) discard;
    FrameRef f = pc.frame;
    uint model = pc.material & 0xFFu;
    vec3 albedo = (pc.material & kSkinRecolor) != 0u ? min(dot(a.rgb, vec3(0.2126, 0.7152, 0.0722)) * pc.tint, vec3(0.9))
                                                      : a.rgb * pc.tint;
    vec3 ng = normalize(vNormal);
    if (!gl_FrontFacing) ng = -ng;
    vec4 nr = texture(textures[nonuniformEXT(pc.normal)], vUv);
    vec3 tn = vec3(nr.xy * 2.0 - 1.0, 0.0);
    tn.y = -tn.y;  // V was flipped at export
    tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
    vec3 n = perturbNormal(ng, vRel, vUv, tn);
    vec3 v = -normalize(vRel);
    float sh = sunShadow(f, vRel, ng, 1.0);
    // Scattering already warms the skin; keep the texture's saturation from adding up to an orange cast.
    if (model == kSkinBody) albedo = mix(vec3(dot(albedo, vec3(0.2126, 0.7152, 0.0722))), albedo, 0.88);
    vec3 color = model == kSkinBody ? shadeSkin(f, albedo, n, v, sh)
               : model == kSkinHair ? shadeHair(f, albedo, n, v, sh)
               : model == kSkinEyes ? shadeEyes(f, albedo, n, v, sh)
                                    : shadeSurface(f, albedo, n, v, max(nr.z, 0.35), 1.0, sh);
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
