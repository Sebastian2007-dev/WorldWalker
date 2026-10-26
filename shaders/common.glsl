// Shared declarations. Mirrors src/render/GpuTypes.h (scalar block layout).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(buffer_reference, scalar) readonly buffer FrameRef {
    mat4 viewProj;
    mat4 invViewProj;
    mat4 view;
    vec4 frustum[6];
    vec3 camUp;        float time;
    vec3 sunDir;       float sunIntensity;
    vec3 sunColor;     float fogDensity;
    vec3 camOriginMod; float camAltitude;
    vec3 camForward;   float exposure;
    vec2 viewport;     float lodScale; float impostorDistance;
    uint groundAlbedo; uint groundNormal; float shadowPass; uint shadowMap;
    mat4 shadowMat[4];
    vec4 cascadeFar;
    vec4 cascadeTexel;
    vec3 skyAmbient;    uint skyLut;          // mean sky radiance over the upper hemisphere; sky-view LUT
    vec3 groundAmbient; float cloudCoverage;
    vec3 camMod64k;     float studio;         // camera ECEF mod 65536 m, for cloud noise; creator studio lights 0..1
};

struct TreeVertex { vec3 pos; vec3 normal; vec2 uv; };
layout(buffer_reference, scalar) readonly buffer TreeVertexRef { TreeVertex d[]; };

struct VegCell { vec3 originRel; uint firstInstance; vec3 east; uint instanceCount; vec3 north; float pad0; vec3 up; float pad1; };
layout(buffer_reference, scalar) readonly buffer VegCellRef { VegCell d[]; };

struct VisibleTree { vec3 pos; float scale; vec2 cs; uint cell; float fade; };
layout(buffer_reference, scalar) buffer VisibleRef { VisibleTree d[]; };

struct Variant { float height; float boundRadius; float impRadius; float impCenterZ; uint impAlbedo; uint impNormal; uint impGrid; uint pad; };
layout(buffer_reference, scalar) readonly buffer VariantRef { Variant d[]; };

struct DrawInfo { uint bucket; uint albedo; uint normal; uint variant; };
layout(buffer_reference, scalar) readonly buffer DrawInfoRef { DrawInfo d[]; };

const float PI = 3.14159265;

vec3 octDecode(uint packed) {
    vec2 e = vec2(float(int(packed << 16) >> 16), float(int(packed) >> 16)) / 32767.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// ---------------------------------------------------------------- noise
// World-anchored value noise. Positions are ECEF metres modulo `span` (4096 for ground detail, 65536 for clouds);
// every lattice period divides the span so the pattern is continuous across the wrap.
float hash13(uvec3 p) {
    uint h = p.x * 1597334677u ^ p.y * 3812015801u ^ p.z * 2798796415u;
    h = (h ^ (h >> 16)) * 0x7feb352du;
    h = (h ^ (h >> 15)) * 0x846ca68bu;
    return float(h >> 8) / 16777216.0;
}

float tiledNoise(vec3 p, float cell, float span) {
    uint period = uint(span / cell);
    vec3 q = p / cell;
    vec3 i = floor(q);
    vec3 f = q - i;
    f = f * f * (3.0 - 2.0 * f);
    uvec3 b = uvec3(ivec3(i) + ivec3(period * 64u)) % period;
    uvec3 b1 = (b + 1u) % period;
    float n000 = hash13(b), n100 = hash13(uvec3(b1.x, b.y, b.z));
    float n010 = hash13(uvec3(b.x, b1.y, b.z)), n110 = hash13(uvec3(b1.x, b1.y, b.z));
    float n001 = hash13(uvec3(b.x, b.y, b1.z)), n101 = hash13(uvec3(b1.x, b.y, b1.z));
    float n011 = hash13(uvec3(b.x, b1.y, b1.z)), n111 = hash13(b1);
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float worldNoise(vec3 p, float cell) { return tiledNoise(p, cell, 4096.0); }

float screenDither(vec2 fragCoord) {
    return fract(52.9829189 * fract(dot(fragCoord, vec2(0.06711056, 0.00583715))));
}

// Vegetated ground colour per ESA WorldCover class with large drier patches. Shared by terrain and grass blades so
// both agree. n1 is worldNoise(worldMod, 8.0).
vec3 groundTint(uint cls, vec3 worldMod, float n1) {
    vec3 lush = mix(vec3(0.046, 0.078, 0.022), vec3(0.062, 0.080, 0.024), n1), dry = vec3(0.12, 0.105, 0.05);
    if (cls == 40u)      { lush = vec3(0.10, 0.12, 0.035); dry = vec3(0.21, 0.17, 0.075); }   // cropland
    else if (cls == 20u) { lush = vec3(0.07, 0.085, 0.035); dry = vec3(0.13, 0.11, 0.06); }   // shrubland
    else if (cls == 90u) { lush = vec3(0.045, 0.08, 0.03); dry = vec3(0.09, 0.09, 0.05); }    // wetland
    else if (cls == 100u){ lush = vec3(0.10, 0.11, 0.055); dry = vec3(0.15, 0.14, 0.09); }    // moss / lichen
    else if (cls == 10u) { lush = vec3(0.045, 0.06, 0.022); dry = vec3(0.08, 0.07, 0.04); }   // forest floor
    float dryness = smoothstep(0.3, 0.8, worldNoise(worldMod + 101.0, 64.0) * 0.75 + n1 * 0.25);
    return mix(lush, dry, dryness * 0.85);
}

// ---------------------------------------------------------------- sky & lighting
// Sky-view LUT (rendered each frame by sky_lut.frag): u = azimuth from the sun (0..PI, the sky is symmetric),
// v = elevation, with v = 0.5 on the true horizon and resolution concentrated around it.
float skyHorizonElevation(float altM) {
    const float r = 6360e3;
    return -acos(clamp(r / (r + max(altM, 0.0)), 0.0, 1.0));
}
vec2 skyLutUv(float az, float el, float hz) {
    float v = el >= hz ? 0.5 + 0.5 * sqrt((el - hz) / (0.5 * PI - hz)) : 0.5 - 0.5 * sqrt((hz - el) / (0.5 * PI + hz));
    return vec2(az / PI, v);
}
void skyLutAngles(vec2 uv, float hz, out float az, out float el) {
    az = uv.x * PI;
    float k = abs(uv.y - 0.5) * 2.0;
    el = uv.y >= 0.5 ? hz + (0.5 * PI - hz) * k * k : hz - (0.5 * PI + hz) * k * k;
}

vec3 skyRadiance(FrameRef f, vec3 dir) {
    vec3 up = f.camUp;
    float s = clamp(dot(dir, up), -1.0, 1.0);
    vec3 dh = dir - up * s, sh = f.sunDir - up * dot(f.sunDir, up);
    float lh = dot(dh, dh), ls = dot(sh, sh);
    float cosAz = lh > 1e-8 && ls > 1e-8 ? dot(dh, sh) * inversesqrt(lh * ls) : 1.0;
    vec2 uv = skyLutUv(acos(clamp(cosAz, -1.0, 1.0)), asin(s), skyHorizonElevation(f.camAltitude));
    return texture(textures[nonuniformEXT(f.skyLut)], uv).rgb + vec3(0.0015, 0.002, 0.004);  // + faint night sky
}

vec3 sunDisk(FrameRef f, vec3 dir) {
    float c = dot(dir, f.sunDir);
    return f.sunColor * f.sunIntensity * 60.0 * smoothstep(0.99996, 0.99999, c);
}

vec3 applyFog(FrameRef f, vec3 color, vec3 rel) {
    float dist = length(rel);
    vec3 dir = rel / max(dist, 1e-3);
    // Exponential height fog: thinner when the camera (and the ray) are high up.
    float camH = max(f.camAltitude, 0.0);
    float rayRise = dot(dir, f.camUp);
    float hScale = 1.0 / 2500.0;
    float k = rayRise * dist * hScale;
    float heightTerm = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
    float density = f.fogDensity * exp(-camH * hScale);
    float fog = 1.0 - exp(-dist * density * heightTerm);
    vec3 fogDir = normalize(dir - f.camUp * dot(dir, f.camUp) * 0.85);
    return mix(color, skyRadiance(f, fogDir), clamp(fog, 0.0, 1.0));
}

// Sky and ground bounce light, integrated on the CPU from the same atmosphere model (Renderer::render).
vec3 ambientLight(FrameRef f, vec3 n) {
    return mix(f.groundAmbient, f.skyAmbient, dot(n, f.camUp) * 0.5 + 0.5);
}

// ---------------------------------------------------------------- clouds
// One procedural cloud layer at a fixed altitude, drifting with the wind. Noise positions are ECEF metres
// modulo 65536 so the pattern is continuous everywhere.
const float kCloudAltitude = 3200.0;

vec3 cloudWind(FrameRef f) { return vec3(9.0, 5.0, 2.0) * f.time; }

float cloudDensity(FrameRef f, vec3 p, int octaves) {
    if (f.cloudCoverage < 0.01) return 0.0;
    float v = 0.0, a = 0.5, norm = 0.0, cell = 4096.0;
    for (int i = 0; i < octaves; ++i) {
        v += a * tiledNoise(p, cell, 65536.0);
        norm += a;
        a *= 0.5;
        cell *= 0.5;
    }
    float th = mix(0.72, 0.3, f.cloudCoverage);
    return smoothstep(th, th + 0.12, v / norm);
}

// Distance along a ray (s = cosine to the local up) to the cloud layer on a spherical Earth, -1 if missed.
float cloudLayerDistance(FrameRef f, float s) {
    float delta = kCloudAltitude - f.camAltitude;
    float d = s * s + 2.0 * delta / 6.371e6;
    if (d < 0.0) return -1.0;
    float q = sqrt(d);
    if (delta > 0.0) return 2.0 * delta / (s + q);
    return s < 0.0 ? -2.0 * delta / (q - s) : -1.0;
}

// Sunlight reaching a point below the layer.
float cloudShadow(FrameRef f, vec3 rel) {
    float sunUp = dot(f.sunDir, f.camUp);
    float alt = f.camAltitude + dot(rel, f.camUp);
    if (f.cloudCoverage < 0.01 || alt >= kCloudAltitude || sunUp <= 0.02) return 1.0;
    vec3 p = f.camMod64k + rel + f.sunDir * ((kCloudAltitude - alt) / sunUp) + cloudWind(f);
    return 1.0 - 0.8 * cloudDensity(f, p, 3);
}

vec3 shadeSurface(FrameRef f, vec3 albedo, vec3 n, vec3 v, float rough, float ao, float shadow) {
    float ndl = max(dot(n, f.sunDir), 0.0) * shadow;
    vec3 h = normalize(f.sunDir + v);
    float a = rough * rough;
    float ndh = max(dot(n, h), 0.0);
    float d = a * a / (PI * pow(ndh * ndh * (a * a - 1.0) + 1.0, 2.0));
    float spec = d * 0.04 * ndl * 0.25;
    vec3 direct = f.sunColor * f.sunIntensity * (albedo / PI * ndl + spec) * PI;
    return direct + ambientLight(f, n) * albedo * ao;
}

// Keeps the brightness of a foliage texture and pulls its hue towards green by `amount`. The baked impostors lean
// slightly olive; keep the pull small or conifers turn lime.
vec3 foliageAlbedo(vec3 a, float amount) {
    const vec3 lumaW = vec3(0.2126, 0.7152, 0.0722), green = vec3(0.48, 1.0, 0.28);
    return mix(a, dot(a, lumaW) * green / dot(green, lumaW), amount);
}

// Foliage: wrapped diffuse plus light transmitted through the leaves. Callers bend the normal towards the crown's
// outward direction, so a crown reads as one volume instead of individually lit cards.
vec3 shadeFoliage(FrameRef f, vec3 albedo, vec3 n, vec3 v, float ao, float shadow) {
    float wrap = max((dot(n, f.sunDir) + 0.35) / 1.35, 0.0) * shadow;
    float trans = pow(max(dot(-v, f.sunDir), 0.0), 4.0) * mix(0.25, 1.0, shadow);
    vec3 sun = f.sunColor * f.sunIntensity;
    return sun * albedo * (wrap + vec3(0.9, 1.0, 0.55) * trans * 0.35) + ambientLight(f, n) * albedo * ao;
}

#ifdef GL_FRAGMENT_SHADER
// Cascaded sun shadow: 4 cascades in a 2x2 atlas, reversed depth, 4x4 texel PCF from four gathers.
float sunShadow(FrameRef f, vec3 rel, vec3 n, float biasScale) {
    float viewZ = dot(rel, f.camForward);
    float clouds = cloudShadow(f, rel);
    if (viewZ > f.cascadeFar.w || f.sunIntensity <= 0.0) return clouds;
    int c = viewZ > f.cascadeFar.z ? 3 : (viewZ > f.cascadeFar.y ? 2 : (viewZ > f.cascadeFar.x ? 1 : 0));
    float texel = f.cascadeTexel[c] * biasScale;
    // Normal offset towards the light side of the surface keeps acne away without peter-panning.
    vec3 nl = dot(n, f.sunDir) < 0.0 ? -n : n;
    vec4 s = f.shadowMat[c] * vec4(rel + nl * texel * 1.5 + f.sunDir * texel, 1.0);
    // Tent filtered PCF over 3x3 texels from four gathers: weights fall off with the sub-texel position, so light
    // through small canopy gaps becomes soft spots instead of square texels.
    float atlasSize = float(textureSize(textures[nonuniformEXT(f.shadowMap)], 0).x);  // depends on shadow quality
    vec2 tc = s.xy * atlasSize - 0.5;
    vec2 base = floor(tc), fr = tc - base;
    vec4 wx = vec4(1.0 - fr.x, 1.0, 1.0, fr.x), wy = vec4(1.0 - fr.y, 1.0, 1.0, fr.y);
    float lit = 0.0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            // Gather returns texels (x0,y1), (x1,y1), (x1,y0), (x0,y0) of the 2x2 block around the coordinate.
            vec4 d = textureGather(textures[nonuniformEXT(f.shadowMap)], (base + vec2(2 * i, 2 * j)) / atlasSize, 0);
            vec4 w = vec4(wx[2 * i] * wy[2 * j + 1], wx[2 * i + 1] * wy[2 * j + 1], wx[2 * i + 1] * wy[2 * j],
                          wx[2 * i] * wy[2 * j]);
            lit += dot(step(d, vec4(s.z + 1e-6)), w);
        }
    lit /= 9.0;
    return mix(lit, 1.0, smoothstep(f.cascadeFar.w * 0.8, f.cascadeFar.w, viewZ)) * clouds;
}

// Cotangent frame normal mapping (no stored tangents needed).
vec3 perturbNormal(vec3 n, vec3 pos, vec2 uv, vec3 tn) {
    vec3 dp1 = dFdx(pos), dp2 = dFdy(pos);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(t, t), dot(b, b)) + 1e-20);
    mat3 tbn = mat3(t * invmax, b * invmax, n);
    return normalize(tbn * tn);
}
#endif
