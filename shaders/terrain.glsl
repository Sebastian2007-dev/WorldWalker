// Terrain chunks: push constants, land cover lookups and lakes. Shared by terrain.vert/.frag (ground) and
// water.vert/.frag (lake surfaces). Mirrors TerrainPush in src/world/Terrain.cpp.
struct TerrainVertex { vec3 pos; uint normal; vec2 uv; };
layout(buffer_reference, scalar) readonly buffer TerrainVertexRef { TerrainVertex d[]; };
layout(buffer_reference, scalar) readonly buffer CoverRef { uint d[]; };

layout(push_constant, scalar) uniform Push {
    FrameRef frame;
    TerrainVertexRef vertices;
    CoverRef cover;
    vec3 originRel;
    float level;
    vec3 originMod;
    float coverRes;
    vec3 up;            // chunk vertical
    float waterAbove;   // lake level above the chunk origin, -1e9 without water
} pc;

const uint kWater = 80u;

bool hasLake() { return pc.waterAbove > -1e8; }

uint coverAt(ivec2 t) {
    int res = int(pc.coverRes);
    t = clamp(t, ivec2(0), ivec2(res - 1));
    uint i = uint(t.y * res + t.x);
    return (pc.cover.d[i >> 2] >> ((i & 3u) * 8u)) & 0xFFu;
}

// Water fraction at a position in texel units, bilinear between texel centres: thresholding it gives rounded shores
// instead of the raster's staircase.
float waterMask(vec2 t) {
    vec2 b = floor(t - 0.5), fr = t - 0.5 - b;
    ivec2 i = ivec2(b);
    float w00 = coverAt(i) == kWater ? 1.0 : 0.0, w10 = coverAt(i + ivec2(1, 0)) == kWater ? 1.0 : 0.0;
    float w01 = coverAt(i + ivec2(0, 1)) == kWater ? 1.0 : 0.0, w11 = coverAt(i + ivec2(1, 1)) == kWater ? 1.0 : 0.0;
    return mix(mix(w00, w10, fr.x), mix(w01, w11, fr.x), fr.y);
}

// Any water cover within two texels.
bool nearWaterCover(ivec2 texel) {
    if (coverAt(texel) == kWater) return true;
    const ivec2 ring[8] = ivec2[](ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2), ivec2(1, 1), ivec2(-1, 1),
                                  ivec2(1, -1), ivec2(-1, -1));
    for (int k = 0; k < 8; ++k)
        if (coverAt(texel + ring[k]) == kWater) return true;
    return false;
}

// Height of a chunk-relative position above the chunk's lake level, including the Earth's curvature drop so the lake
// stays level across large chunks.
float heightAboveLake(vec3 pos) {
    float along = dot(pos, pc.up);
    float horizontal2 = max(dot(pos, pos) - along * along, 0.0);
    return along + horizontal2 / (2.0 * 6371000.0) - pc.waterAbove;
}

// Lake bed: the DEM over lakes is not flat (it varies by metres), so under water cover the ground is pulled below the
// lake surface. Ground clearly above the level stays: rivers fall along their course and keep their own height.
// Returns the new height above the lake.
float lakeBed(float above, vec2 uv) {
    if (!hasLake() || above > 3.0) return above;
    float m = waterMask(uv * pc.coverRes);
    if (m <= 0.0) return above;
    return mix(above, min(above, -0.3 - 1.7 * m), smoothstep(0.0, 0.5, m));
}

#ifdef GL_FRAGMENT_SHADER
// Water surface: wind ripples, sky reflection (darkened towards the horizon where a real lake mirrors its shore), sun
// glint and the light scattered back from the water body.
vec3 shadeWater(FrameRef f, vec3 worldMod, vec3 v, float dist, float shadow) {
    vec3 up = f.camUp;
    vec3 wn = up;
    vec3 east = normalize(cross(vec3(0.0, 0.0, 1.0), up)), north = cross(up, east);
    vec2 wind = vec2(0.8, 0.6) * f.time;
    // Three octaves, gentle slopes: strong ones flip the reflection between sky and shore in big blotches.
    const float cells[3] = float[](7.0, 2.2, 0.7), strength[3] = float[](0.035, 0.03, 0.025);
    for (int k = 0; k < 3; ++k) {
        float cell = cells[k];
        vec3 p = worldMod + (east * wind.x + north * wind.y) * (k == 1 ? -1.6 : 1.0 + float(k));
        float e = cell * 0.25, h0 = worldNoise(p, cell);
        vec2 g = vec2(worldNoise(p + east * e, cell) - h0, worldNoise(p + north * e, cell) - h0) / e;
        // Fine octaves fade out first with distance, before they alias.
        float fade = 1.0 - smoothstep(cell * 12.0, cell * 80.0, dist);
        wn -= (east * g.x + north * g.y) * strength[k] * cell * fade;
    }
    wn = normalize(wn);
    float ndv = max(dot(wn, v), 0.0);
    float fres = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
    vec3 r = reflect(-v, wn);
    if (dot(r, up) < 0.0) r = reflect(r, up);  // ripples must not reflect the underside
    // Without screen space reflections, low reflected rays would show the bright horizon sky; on a real lake they
    // mostly see the shore and its forest. A wide transition: at grazing views small ripples change the elevation a lot.
    vec3 shore = f.skyAmbient * vec3(0.10, 0.14, 0.09) + f.sunColor * f.sunIntensity * vec3(0.012, 0.018, 0.01);
    vec3 refl = mix(shore, skyRadiance(f, r), smoothstep(-0.05, 0.55, dot(r, up)));
    vec3 hv = normalize(f.sunDir + v);
    refl += f.sunColor * f.sunIntensity * pow(max(dot(wn, hv), 0.0), 600.0) * 60.0 * shadow;
    vec3 body = (f.skyAmbient * 0.6 + f.sunColor * f.sunIntensity * max(dot(up, f.sunDir), 0.0) * 0.15) *
                vec3(0.02, 0.05, 0.05) * mix(1.4, 0.7, ndv);
    return mix(body, refl, fres);
}
#endif
