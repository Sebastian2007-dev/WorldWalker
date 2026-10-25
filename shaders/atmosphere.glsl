// Physically based single scattering sky. Mirrors src/render/Atmosphere.h, keep both in sync.
// Units: km, planet centred at the origin, radiance in shading units (sun irradiance PI * kSunTOA).

const float kRg = 6360.0, kRt = 6460.0;
const float kSunTOA = 3.8;
const float kMultiScatter = 0.15;
const float kMieG = 0.8;
const vec3 kRayleigh = vec3(5.802e-3, 13.558e-3, 33.1e-3);
const float kMieScat = 3.996e-3, kMieExt = 4.44e-3;
const vec3 kOzone = vec3(0.650e-3, 1.881e-3, 0.085e-3);

vec3 atmoExtinction(float h, out vec3 rayScat, out float mieScat) {
    float dR = exp(-h / 8.0), dM = exp(-h / 1.2);
    float dO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
    rayScat = kRayleigh * dR;
    mieScat = kMieScat * dM;
    return rayScat + kMieExt * dM + kOzone * dO;
}

float raySphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd), c = dot(ro, ro) - r * r, d = b * b - c;
    if (d < 0.0) return -1.0;
    float s = sqrt(d), t0 = -b - s, t1 = -b + s;
    return t0 > 0.0 ? t0 : (t1 > 0.0 ? t1 : -1.0);
}

vec3 atmoSunTransmittance(vec3 p, vec3 sun, int steps) {
    if (raySphere(p, sun, kRg) > 0.0) return vec3(0.0);
    float tMax = raySphere(p, sun, kRt);
    if (tMax <= 0.0) return vec3(1.0);
    vec3 depth = vec3(0.0), rs;
    float ms, dt = tMax / float(steps);
    for (int i = 0; i < steps; ++i) {
        vec3 q = p + sun * ((float(i) + 0.5) * dt);
        depth += atmoExtinction(length(q) - kRg, rs, ms) * dt;
    }
    return exp(-depth);
}

vec3 atmoSkyRadiance(vec3 ro, vec3 rd, vec3 sun, int steps) {
    float tTop = raySphere(ro, rd, kRt), tGround = raySphere(ro, rd, kRg);
    float tMax = tGround > 0.0 ? tGround : tTop;
    if (tMax <= 0.0) return vec3(0.0);
    float mu = dot(rd, sun);
    float phaseR = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    float g = kMieG;
    float phaseM = 3.0 / (8.0 * PI) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    vec3 L = vec3(0.0), T = vec3(1.0), rs;
    float ms;
    float n2 = float(steps * steps);
    for (int i = 0; i < steps; ++i) {
        float t0 = tMax * float(i * i) / n2, t1 = tMax * float((i + 1) * (i + 1)) / n2;
        float dt = t1 - t0;
        vec3 p = ro + rd * (0.5 * (t0 + t1));
        vec3 ext = atmoExtinction(length(p) - kRg, rs, ms);
        vec3 Ts = atmoSunTransmittance(p, sun, 6);
        vec3 scat = (rs * phaseR + vec3(ms * phaseM)) * Ts + (rs + vec3(ms)) * kMultiScatter * Ts;
        vec3 stepT = exp(-ext * dt);
        L += T * scat * (1.0 - stepT) / max(ext, vec3(1e-7));
        T *= stepT;
    }
    return L * (PI * kSunTOA);
}
