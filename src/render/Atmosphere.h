#pragma once
// Physically based single scattering sky (Rayleigh, Mie, ozone; coefficients from Hillaire 2020) with a cheap
// isotropic multiple scattering term. Mirrored in shaders/atmosphere.glsl, keep both in sync.
// Units: km, planet centred at the origin. Radiance is in shading units: shadeSurface() lights with
// albedo * E * ndl, so the sun's physical irradiance is PI * E.
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace render::atmo {

constexpr float kRg = 6360.0f, kRt = 6460.0f;
constexpr float kSunTOA = 3.8f;          // sun irradiance above the atmosphere, shading units
constexpr float kMultiScatter = 0.15f;   // isotropic stand-in for higher scattering orders
constexpr float kMieG = 0.8f;
const glm::vec3 kRayleigh(5.802e-3f, 13.558e-3f, 33.1e-3f);
constexpr float kMieScat = 3.996e-3f, kMieExt = 4.44e-3f;
const glm::vec3 kOzone(0.650e-3f, 1.881e-3f, 0.085e-3f);
constexpr float kPi = 3.14159265f;

inline glm::vec3 extinction(float h, glm::vec3& rayScat, float& mieScat) {
    float dR = std::exp(-h / 8.0f), dM = std::exp(-h / 1.2f);
    float dO = std::max(0.0f, 1.0f - std::abs(h - 25.0f) / 15.0f);
    rayScat = kRayleigh * dR;
    mieScat = kMieScat * dM;
    return rayScat + kMieExt * dM + kOzone * dO;
}

// Nearest positive intersection with a sphere around the origin, -1 if none.
inline float raySphere(glm::vec3 ro, glm::vec3 rd, float r) {
    float b = glm::dot(ro, rd), c = glm::dot(ro, ro) - r * r, d = b * b - c;
    if (d < 0.0f) return -1.0f;
    float s = std::sqrt(d), t0 = -b - s, t1 = -b + s;
    return t0 > 0.0f ? t0 : (t1 > 0.0f ? t1 : -1.0f);
}

inline glm::vec3 sunTransmittance(glm::vec3 p, glm::vec3 sun, int steps = 12) {
    if (raySphere(p, sun, kRg) > 0.0f) return glm::vec3(0.0f);
    float tMax = raySphere(p, sun, kRt);
    if (tMax <= 0.0f) return glm::vec3(1.0f);
    glm::vec3 depth(0.0f), rs;
    float ms, dt = tMax / float(steps);
    for (int i = 0; i < steps; ++i) {
        glm::vec3 q = p + sun * ((float(i) + 0.5f) * dt);
        depth += extinction(glm::length(q) - kRg, rs, ms) * dt;
    }
    return glm::exp(-depth);
}

inline glm::vec3 skyRadiance(glm::vec3 ro, glm::vec3 rd, glm::vec3 sun, int steps = 32) {
    float tTop = raySphere(ro, rd, kRt), tGround = raySphere(ro, rd, kRg);
    float tMax = tGround > 0.0f ? tGround : tTop;
    if (tMax <= 0.0f) return glm::vec3(0.0f);
    float mu = glm::dot(rd, sun);
    float phaseR = 3.0f / (16.0f * kPi) * (1.0f + mu * mu);
    float g = kMieG;
    float phaseM = 3.0f / (8.0f * kPi) * ((1.0f - g * g) * (1.0f + mu * mu)) /
                   ((2.0f + g * g) * std::pow(1.0f + g * g - 2.0f * g * mu, 1.5f));
    glm::vec3 L(0.0f), T(1.0f), rs;
    float ms;
    for (int i = 0; i < steps; ++i) {
        // Quadratic step distribution: dense near the camera where Mie haze lives.
        float t0 = tMax * float(i * i) / float(steps * steps), t1 = tMax * float((i + 1) * (i + 1)) / float(steps * steps);
        float dt = t1 - t0;
        glm::vec3 p = ro + rd * (0.5f * (t0 + t1));
        glm::vec3 ext = extinction(glm::length(p) - kRg, rs, ms);
        glm::vec3 Ts = sunTransmittance(p, sun, 6);
        glm::vec3 scat = (rs * phaseR + glm::vec3(ms * phaseM)) * Ts + (rs + glm::vec3(ms)) * kMultiScatter * Ts;
        glm::vec3 stepT = glm::exp(-ext * dt);
        L += T * scat * (glm::vec3(1.0f) - stepT) / glm::max(ext, glm::vec3(1e-7f));
        T *= stepT;
    }
    return L * (kPi * kSunTOA);
}

}  // namespace render::atmo
