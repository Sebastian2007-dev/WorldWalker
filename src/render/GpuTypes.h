#pragma once
// CPU mirrors of structures read by shaders (scalar block layout, see shaders/common.glsl).
#include <glm/glm.hpp>

#include <cstdint>

namespace render {

struct FrameData {
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::mat4 view;
    glm::vec4 frustum[6];
    glm::vec3 camUp;       float time;
    glm::vec3 sunDir;      float sunIntensity;
    glm::vec3 sunColor;    float fogDensity;
    glm::vec3 camOriginMod; float camAltitude;  // camera ECEF mod 4096 m (seamless world-space noise)
    glm::vec3 camForward;  float exposure;
    glm::vec2 viewport;    float lodScale;  float impostorDistance;
    uint32_t groundAlbedo; uint32_t groundNormal; float shadowPass; uint32_t shadowMap;
    glm::mat4 shadowMat[4];    // camera relative position -> shadow atlas uv + depth (reversed: 1 = towards the sun)
    glm::vec4 cascadeFar;      // view depth where each cascade ends
    glm::vec4 cascadeTexel;    // world size of one shadow texel per cascade
    glm::vec3 skyAmbient;    uint32_t skyLut;    // mean sky radiance over the upper hemisphere; sky-view LUT
    glm::vec3 groundAmbient; float cloudCoverage;
    glm::vec3 camMod64k;     float studio;       // camera ECEF mod 65536 m, for cloud noise; creator studio lights 0..1
};

struct TerrainVertex {
    glm::vec3 pos;      // relative to chunk origin
    uint32_t normal;    // octahedral snorm16x2
    glm::vec2 uv;       // 0..1 across the chunk
};

struct TreeVertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct TreeInstance {          // per tree, stored per vegetation cell
    glm::vec3 pos;             // relative to cell origin
    float yaw;
    float scale;
    uint32_t variant;
    float pad[2];
};

struct VegCell {               // per active vegetation cell, refreshed every frame
    glm::vec3 originRel;  uint32_t firstInstance;   // origin relative to camera
    glm::vec3 east;       uint32_t instanceCount;
    glm::vec3 north;      float pad0;
    glm::vec3 up;         float pad1;
};

struct VariantGpu {
    float height, boundRadius, impRadius, impCenterZ;
    uint32_t impAlbedo, impNormal, impGrid, pad;
};

struct DrawInfo {              // per indirect draw command
    uint32_t bucket;
    uint32_t albedo;
    uint32_t normal;
    uint32_t variant;
};

inline uint32_t octEncode(glm::vec3 n) {
    n /= (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
    glm::vec2 p(n.x, n.y);
    if (n.z < 0) {
        p = glm::vec2((1.0f - std::abs(n.y)) * (n.x >= 0 ? 1.0f : -1.0f),
                      (1.0f - std::abs(n.x)) * (n.y >= 0 ? 1.0f : -1.0f));
    }
    auto q = [](float v) { return uint32_t(int32_t(std::round(glm::clamp(v, -1.0f, 1.0f) * 32767.0f)) & 0xFFFF); };
    return q(p.x) | (q(p.y) << 16);
}

}  // namespace render
