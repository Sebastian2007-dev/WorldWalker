#version 460
#include "common.glsl"

layout(buffer_reference, scalar) readonly buffer CoverRef { uint d[]; };

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    uint64_t vertices;
    CoverRef cover;
    vec3 originRel;
    float level;
    vec3 originMod;
    float coverRes;
} pc;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec3 vWorldMod;
layout(location = 0) out vec4 outColor;

uint coverAt(ivec2 t) {
    int res = int(pc.coverRes);
    t = clamp(t, ivec2(0), ivec2(res - 1));
    uint i = uint(t.y * res + t.x);
    return (pc.cover.d[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu;
}

// ESA WorldCover classes -> base albedo (linear) and roughness.
void classMaterial(uint cls, float n, out vec3 albedo, out float rough) {
    rough = 0.9;
    if (cls == 10u)       albedo = mix(vec3(0.045, 0.055, 0.025), vec3(0.08, 0.07, 0.04), n);   // forest floor
    else if (cls == 20u)  albedo = mix(vec3(0.10, 0.10, 0.05), vec3(0.16, 0.14, 0.08), n);    // shrubland
    else if (cls == 30u)  albedo = mix(vec3(0.08, 0.13, 0.035), vec3(0.15, 0.17, 0.06), n);   // grassland
    else if (cls == 40u)  albedo = mix(vec3(0.17, 0.16, 0.07), vec3(0.11, 0.15, 0.05), n);    // cropland
    else if (cls == 50u)  albedo = mix(vec3(0.09, 0.12, 0.05), vec3(0.16, 0.15, 0.11), n);    // built-up: plain ground for now
    else if (cls == 60u)  albedo = mix(vec3(0.33, 0.28, 0.21), vec3(0.42, 0.36, 0.27), n);    // bare / sparse
    else if (cls == 70u) { albedo = vec3(0.80, 0.83, 0.88); rough = 0.5; }                      // snow & ice
    else if (cls == 80u) { albedo = vec3(0.01, 0.025, 0.035); rough = 0.06; }                  // water
    else if (cls == 90u)  albedo = mix(vec3(0.06, 0.09, 0.04), vec3(0.10, 0.12, 0.05), n);    // wetland
    else if (cls == 95u)  albedo = mix(vec3(0.04, 0.06, 0.03), vec3(0.07, 0.08, 0.04), n);    // mangroves
    else if (cls == 100u) albedo = mix(vec3(0.14, 0.14, 0.08), vec3(0.20, 0.19, 0.12), n);    // moss & lichen
    else                  albedo = vec3(0.12, 0.12, 0.08);
}

void main() {
    FrameRef f = pc.frame;
    vec3 n = normalize(vNormal);
    vec3 up = f.camUp;
    float dist = length(vRel);
    vec3 v = -vRel / max(dist, 1e-3);

    // Organic class borders: jitter the land-cover lookup with world noise.
    float res = pc.coverRes;
    vec2 jitter = (vec2(worldNoise(vWorldMod, 16.0), worldNoise(vWorldMod + 31.7, 16.0)) - 0.5) * 2.2 +
                  (vec2(worldNoise(vWorldMod, 4.0), worldNoise(vWorldMod + 17.3, 4.0)) - 0.5) * 0.8;
    ivec2 texel = ivec2(floor(vUv * res + jitter * smoothstep(5.0, 8.0, pc.level + 0.5)));
    uint cls = coverAt(texel);
    // Water only lies flat: where the land cover says water but the terrain slopes, the cover map (10 m) and the
    // DEM disagree about the shoreline. Show wet bank there instead of water climbing the hill.
    if (cls == 80u && 1.0 - dot(normalize(vNormal), f.camUp) > 0.03 + worldNoise(vWorldMod, 4.0) * 0.03) cls = 90u;

    float n1 = worldNoise(vWorldMod, 8.0);
    float n2 = worldNoise(vWorldMod, 1.0);
    float n3 = worldNoise(vWorldMod, 64.0);
    float detailFade = 1.0 - smoothstep(60.0, 400.0, dist);
    float variation = mix(n3, n1 * 0.6 + n2 * 0.4, detailFade);

    vec3 albedo;
    float rough;
    classMaterial(cls, variation, albedo, rough);

    // Photo ground texture (triplanar on world-anchored coordinates, two scales against tiling).
    vec3 tw = pow(abs(n), vec3(4.0));
    tw /= tw.x + tw.y + tw.z;
    vec3 groundTex = vec3(0.0);
    vec3 groundNrm = vec3(0.0);
    float groundRough = 0.0;
    const float scales[2] = float[](4.0, 13.0);
    for (int s = 0; s < 2; ++s) {
        vec3 p = vWorldMod / scales[s];
        vec2 uvs[3] = vec2[](p.yz, p.xz, p.xy);
        for (int a = 0; a < 3; ++a) {
            if (tw[a] < 0.01) continue;
            vec3 c = texture(textures[nonuniformEXT(f.groundAlbedo)], uvs[a]).rgb;
            vec4 nr = texture(textures[nonuniformEXT(f.groundNormal)], uvs[a]);
            groundTex += c * tw[a] * 0.5;
            groundRough += nr.z * tw[a] * 0.5;
            vec2 t = (nr.xy * 2.0 - 1.0) * tw[a] * 0.5;
            // Whiteout-style: offset the geometric normal along the projection plane axes.
            if (a == 0) groundNrm += vec3(0.0, t.x, t.y);
            else if (a == 1) groundNrm += vec3(t.x, 0.0, t.y);
            else groundNrm += vec3(t.x, t.y, 0.0);
        }
    }
    groundTex *= 0.8 + 0.4 * n3;  // macro variation

    // Classes that are vegetated ground use the photo texture, tinted per class.
    bool textured = cls == 10u || cls == 20u || cls == 30u || cls == 40u || cls == 50u || cls == 90u ||
                    cls == 95u || cls == 100u;
    if (textured) {
        // The photo is dry, brownish grass. Use it only for structure (ratio to its mean, which the last mip
        // gives us) and take the colour from the class, with large drier patches from world noise.
        vec3 texMean = textureLod(textures[nonuniformEXT(f.groundAlbedo)], vec2(0.5), 16.0).rgb;
        vec3 detail = pow(clamp(groundTex / max(texMean, vec3(1e-3)), vec3(0.0), vec3(3.0)), vec3(1.6));
        vec3 tex = groundTint(cls, vWorldMod, n1) * detail;
        if (cls == 10u) {                                      // forest floor: needles and litter
            vec3 litter = vec3(0.13, 0.085, 0.05) * (0.7 + 0.6 * n2);
            tex = mix(tex * 0.75, litter, 0.55);
        }
        albedo = tex;
        rough = mix(groundRough, 0.9, 0.3);
        n = normalize(n + groundNrm * 0.8 * detailFade);
    }

    // Steep slopes show rock, high ground gets snow.
    float slope = 1.0 - dot(n, up);
    vec3 rock = mix(vec3(0.16, 0.15, 0.14), vec3(0.26, 0.24, 0.22), n1);
    float rockMask = smoothstep(0.28, 0.42, slope + (n1 - 0.5) * 0.1);
    if (cls != 80u) {
        albedo = mix(albedo, rock, rockMask);
        float snowLine = 2800.0 + (n3 - 0.5) * 400.0;
        float alt = f.camAltitude + dot(vRel, up);
        float snow = smoothstep(snowLine, snowLine + 250.0, alt) * (1.0 - smoothstep(0.35, 0.6, slope));
        albedo = mix(albedo, vec3(0.8, 0.83, 0.88), snow);
    }

    // Micro shading from noise so close ground does not look flat.
    if (!textured) albedo *= mix(1.0, 0.8 + 0.4 * n2, detailFade);

    float sh = sunShadow(f, vRel, normalize(vNormal), 1.0);
    vec3 color;
    if (cls == 80u) {
        // Water: flat surface (the DEM is noisy over lakes) with two layers of wind ripples drifting across it.
        vec3 wn = up;
        vec3 east = normalize(cross(vec3(0.0, 0.0, 1.0), up)), north = cross(up, east);
        vec2 wind = vec2(0.8, 0.6) * f.time;
        float rippleFade = 1.0 - smoothstep(80.0, 600.0, dist);
        for (int k = 0; k < 2; ++k) {
            float cell = k == 0 ? 6.0 : 1.5;
            vec3 p = vWorldMod + (east * wind.x + north * wind.y) * (k == 0 ? 1.0 : -1.6);
            float e = cell * 0.25, h0 = worldNoise(p, cell);
            vec2 g = vec2(worldNoise(p + east * e, cell) - h0, worldNoise(p + north * e, cell) - h0) / e;
            wn -= (east * g.x + north * g.y) * (k == 0 ? 0.12 : 0.05) * cell * rippleFade;
        }
        wn = normalize(wn);
        float ndv = max(dot(wn, v), 0.0);
        float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
        vec3 r = reflect(-v, wn);
        if (dot(r, up) < 0.0) r = reflect(r, up);  // ripples must not reflect the underside
        // Without screen space reflections, low reflected rays would show the bright horizon sky; on a real lake they
        // mostly see the shore and its forest. Fade those towards a dark, lit-by-sky shore colour.
        float rEl = dot(r, up);
        vec3 shore = f.skyAmbient * vec3(0.10, 0.14, 0.09) + f.sunColor * f.sunIntensity * vec3(0.012, 0.018, 0.01);
        vec3 refl = mix(shore, skyRadiance(f, r), smoothstep(0.03, 0.25, rEl));
        // Sun glint on the ripples.
        vec3 hv = normalize(f.sunDir + v);
        float glint = pow(max(dot(wn, hv), 0.0), 600.0) * 60.0 * sh;
        refl += f.sunColor * f.sunIntensity * glint;
        // Body colour: light scattered back from inside the water, darker for steep (deep looking) views.
        vec3 body = (f.skyAmbient * 0.6 + f.sunColor * f.sunIntensity * max(dot(up, f.sunDir), 0.0) * 0.15) *
                    vec3(0.02, 0.05, 0.05) * mix(1.4, 0.7, ndv);
        color = mix(body, refl, fres);
    } else {
        float ao = cls == 10u ? 0.7 : 1.0;  // canopy shade on forest floor
        color = shadeSurface(f, albedo, n, v, rough, ao, sh);
        // Beyond the shadow range the canopy shade on forest floor is approximated.
        if (cls == 10u) color *= mix(1.0, 0.75, smoothstep(240.0, 360.0, dist));
    }

    outColor = vec4(applyFog(f, color, vRel), 1.0);
}
