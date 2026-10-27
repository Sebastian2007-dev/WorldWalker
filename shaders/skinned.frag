#version 460
#include "common.glsl"
#include "skin.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in float vOcclusion;
layout(location = 0) out vec4 outColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

struct Light { vec3 dir; vec3 radiance; float shadow; };

float ggx(float ndh, float rough) {
    float a2 = rough * rough * rough * rough;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float schlick(float f0, float c) { return f0 + (1.0 - f0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); }

// Creator preview lights, fixed to the camera so the character reads well from every side: a warm key from the front
// left, a cool fill from the right and a rim from behind. Scaled to the daylight level so auto exposure stays put.
Light studioLight(FrameRef f, int i) {
    vec3 fwd = f.camForward, right = normalize(cross(fwd, f.camUp)), up = cross(right, fwd);
    float level = max(dot(f.sunColor, kLuma) * f.sunIntensity, dot(f.skyAmbient, kLuma) * 8.0) * f.studio;
    if (i == 0) return Light(normalize(-fwd * 0.7 - right * 0.6 + up * 0.5), vec3(1.0, 0.95, 0.88) * level * 0.3, 1.0);
    if (i == 1) return Light(normalize(-fwd * 0.8 + right * 0.6 + up * 0.1), vec3(0.8, 0.88, 1.0) * level * 0.05, 1.0);
    return Light(normalize(fwd * 0.8 + right * 0.4 + up * 0.45), vec3(1.0, 0.96, 0.9) * level * 0.35, 1.0);
}

// ---------------------------------------------------------------- skin
// Hash based value noise with its derivative.
float hash12(vec2 p) {
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
    return float((q.x ^ q.y) * 1597334673u) * (1.0 / 4294967295.0);
}
vec3 valueNoised(vec2 x) {
    vec2 i = floor(x), f = x - i;
    vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    float a = hash12(i), b = hash12(i + vec2(1, 0)), c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
    float k = a - b - c + d;
    return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y, du * (vec2(b - a, c - a) + k * u.yx));
}

// Micro relief (pores, fine creases) in texture space: the body textures have no normal map. Each octave fades out
// before its bumps get smaller than a pixel, so distant skin stays calm under TAA.
vec2 skinRelief(vec2 uv) {
    vec2 slope = vec2(0);
    float footprint = length(fwidth(uv));
    const vec2 octaves[2] = vec2[](vec2(850.0, 0.07), vec2(2300.0, 0.045));
    for (int o = 0; o < 2; ++o) {
        float fade = 1.0 - smoothstep(0.25, 0.6, footprint * octaves[o].x);
        if (fade > 0.0) slope += valueNoised(uv * octaves[o].x + float(o) * 17.0).yz * octaves[o].y * fade;
    }
    return slope;
}

// Light scattered under the skin wraps further around the terminator in red than in blue and softens shadow edges the
// same way. Two specular lobes (broad sheen plus a tight oily highlight).
vec3 skinLight(vec3 albedo, vec3 n, vec3 v, Light l, float occlusion) {
    float ndl = dot(n, l.dir);
    const vec3 wrap = vec3(0.17, 0.1, 0.075);
    vec3 diffuse = clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
    vec3 shade = mix(vec3(l.shadow), vec3(sqrt(l.shadow)), vec3(0.15, 0.06, 0.035));
    vec3 h = normalize(l.dir + v);
    float ndh = max(dot(n, h), 0.0), vdh = max(dot(v, h), 0.0);
    float spec = (0.75 * ggx(ndh, 0.5) + 0.25 * ggx(ndh, 0.3)) * schlick(0.028, vdh) * 0.25 * max(ndl, 0.0) * l.shadow;
    return l.radiance * (albedo * diffuse * shade * mix(1.0, occlusion, 0.6) + spec * PI * occlusion * occlusion);
}

vec3 skinAmbient(FrameRef f, vec3 albedo, vec3 n, vec3 v, float occlusion) {
    float ndv = max(dot(n, v), 1e-3);
    vec3 rim = ambientLight(f, reflect(-v, n)) * schlick(0.028, ndv) * 0.6;
    return (ambientLight(f, n) * albedo + rim) * occlusion;
}

// ---------------------------------------------------------------- hair
// Hair cards (Kajiya-Kay): a white highlight close to the strand's specular cone and a second, coloured one shifted
// towards the root. Strands run along the texture's v axis.
vec3 hairTangent(vec3 n) {
    vec3 dp1 = dFdx(vRel), dp2 = dFdy(vRel);
    vec2 duv1 = dFdx(vUv), duv2 = dFdy(vUv);
    vec3 b = cross(dp2, n) * duv1.y + cross(n, dp1) * duv2.y;
    b -= n * dot(n, b);
    return dot(b, b) > 1e-12 ? normalize(b) : normalize(cross(n, vec3(0.0, 0.0, 1.0)) + 1e-4);
}

vec3 hairLight(vec3 albedo, vec3 n, vec3 t, vec3 v, Light l) {
    vec3 h = normalize(l.dir + v);
    float ndl = dot(n, l.dir);
    float d1 = dot(normalize(t + n * 0.08), h), d2 = dot(normalize(t - n * 0.12), h);
    float s1 = pow(sqrt(max(1.0 - d1 * d1, 0.0)), 200.0) * 0.04;
    float s2 = pow(sqrt(max(1.0 - d2 * d2, 0.0)), 40.0) * 0.15;
    float lit = smoothstep(-0.25, 0.35, ndl) * l.shadow;
    float diffuse = max((ndl + 0.3) / 1.3, 0.0) * l.shadow;
    return l.radiance * (albedo * diffuse + (vec3(s1) + albedo * s2) * lit);
}

// ---------------------------------------------------------------- eyes
// Wet cornea with a sharp highlight and the sky mirrored at grazing angles.
vec3 eyesLight(vec3 albedo, vec3 n, vec3 v, Light l) {
    vec3 h = normalize(l.dir + v);
    float ndl = max(dot(n, l.dir), 0.0);
    float spec = ggx(max(dot(n, h), 0.0), 0.1) * schlick(0.04, dot(v, h)) * 0.25 * ndl * l.shadow;
    return l.radiance * (albedo * ndl * l.shadow + spec * PI);
}

// ---------------------------------------------------------------- cloth
vec3 clothLight(vec3 albedo, vec3 n, vec3 v, float rough, Light l) {
    float ndl = max(dot(n, l.dir), 0.0) * l.shadow;
    vec3 h = normalize(l.dir + v);
    float a = rough * rough, ndh = max(dot(n, h), 0.0);
    float d = a * a / (PI * pow(ndh * ndh * (a * a - 1.0) + 1.0, 2.0));
    return l.radiance * (albedo / PI * ndl + d * 0.04 * ndl * 0.25) * PI;
}

void main() {
    uint model = pc.material & 0xFFu;
    // Underwear: the edges are where the signed distance in uv.x crosses zero.
    if (model == kSkinUnderwear && vUv.x < 0.0) discard;
    vec4 a = texture(textures[nonuniformEXT(pc.albedo)], vUv);
    // Hair: dithered alpha test, TAA turns it into soft strands.
    if (pc.alphaTest != 0u && a.a < mix(0.2, 0.8, screenDither(gl_FragCoord.xy))) discard;
    FrameRef f = pc.frame;
    vec3 albedo = (pc.material & kSkinRecolor) != 0u ? min(dot(a.rgb, kLuma) * pc.tint, vec3(0.9)) : a.rgb * pc.tint;
    vec3 ng = normalize(vNormal);
    if (!gl_FrontFacing) ng = -ng;
    vec4 nr = texture(textures[nonuniformEXT(pc.normal)], vUv);
    vec3 tn = vec3(nr.xy * 2.0 - 1.0, 0.0);
    tn.y = -tn.y;  // V was flipped at export
    tn.xy *= pc.normalScale;
    float rough = max(nr.z, 0.35);
    if (model == kSkinBody) tn.xy -= skinRelief(vUv);
    if (model == kSkinUnderwear) {
        // Plain cotton jersey with a slightly lighter elastic band along the edges.
        float band = 1.0 - smoothstep(0.007, 0.01, vUv.x);
        albedo = pc.tint * (1.0 + band * 0.8);
        rough = 0.85;
    }
    tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
    vec3 n = perturbNormal(ng, vRel, vUv, tn);
    vec3 v = -normalize(vRel);
    float occlusion = vOcclusion;
    // Scattering already warms the skin; keep the texture's saturation from adding up to an orange cast.
    if (model == kSkinBody) albedo = mix(vec3(dot(albedo, kLuma)), albedo, 0.88);

    Light lights[4];
    // The studio lights take over part of the daylight in the creator.
    // Near the terminator the shadow map sees the body's facets, not its smooth shading: there the light's own falloff
    // shades the body and the map would only add polygon shaped edges (the shadow terminator problem).
    float sunShade = mix(1.0, sunShadow(f, vRel, ng, 2.0), smoothstep(0.0, 0.3, dot(ng, f.sunDir)));
    lights[0] = Light(f.sunDir, f.sunColor * f.sunIntensity * (1.0 - 0.55 * f.studio), sunShade);
    int count = f.studio > 0.0 ? 4 : 1;
    for (int i = 1; i < count; ++i) lights[i] = studioLight(f, i - 1);
    float ambientScale = 1.0 - 0.3 * f.studio;

    vec3 color = vec3(0);
    if (model == kSkinBody) {
        for (int i = 0; i < count; ++i) color += skinLight(albedo, n, v, lights[i], occlusion);
        color += skinAmbient(f, albedo, n, v, occlusion) * ambientScale;
    } else if (model == kSkinHair) {
        vec3 t = hairTangent(n);
        for (int i = 0; i < count; ++i) color += hairLight(albedo, n, t, v, lights[i]);
        color += ambientLight(f, n) * albedo * 0.8 * ambientScale;
    } else if (model == kSkinEyes) {
        for (int i = 0; i < count; ++i) color += eyesLight(albedo, n, v, lights[i]);
        // The lids shade the eyeball; without it the sclera glows in the ambient light.
        vec3 sky = ambientLight(f, reflect(-v, n)) * schlick(0.04, dot(n, v)) * 0.3;
        color += (ambientLight(f, n) * albedo * 0.45 + sky) * ambientScale;
    } else {
        for (int i = 0; i < count; ++i) color += clothLight(albedo, n, v, rough, lights[i]);
        color += ambientLight(f, n) * albedo * ambientScale;
        // Fabric sheen: fibres catch light at grazing angles.
        if (model == kSkinUnderwear) color += ambientLight(f, n) * pow(1.0 - max(dot(n, v), 0.0), 4.0) * 0.25;
    }
    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
