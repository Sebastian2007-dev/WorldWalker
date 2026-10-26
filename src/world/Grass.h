#pragma once
// Grass blades around the camera. A worker samples ground height and land cover into a small field (80 x 80 m)
// around the camera; the vertex shader builds blades procedurally on a world-anchored grid from that field, so
// there are no per-blade buffers and blades never pop while walking.
#include <atomic>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "core/Jobs.h"
#include "gfx/Pipeline.h"
#include "world/WorldData.h"

namespace world {

class Grass {
public:
    static constexpr int kRes = 160;           // field samples per side
    static constexpr float kCell = 0.5f;       // metres between field samples
    static constexpr float kRadius = 30.0f;    // blades are drawn up to this distance
    static constexpr float kSpacing = 0.13f;   // blade grid spacing in metres

    void init(gfx::Device& dev, gfx::Bindless& bindless, WorldData& data, core::JobSystem& jobs, VkFormat colorFormat,
              VkFormat depthFormat);
    void shutdown();

    // Rebuilds the field in the background when the camera wandered off its centre.
    void update(const glm::dvec3& camEcef);
    // Draws inside the main render pass.
    // density 0..1 thins the blade grid and pulls in the radius (lower graphics qualities).
    void draw(VkCommandBuffer cmd, VkDeviceAddress frameData, const glm::dvec3& camEcef, float density = 1.0f);

private:
    struct Field {
        double anchorLat = 0, anchorLon = 0;
        glm::dvec3 anchor{0};                   // region origin; the blade grid is anchored to its tangent plane
        glm::dvec3 east{1, 0, 0}, north{0, 1, 0}, up{0, 0, 1};
        double x0 = 0, y0 = 0;                  // tangent plane coordinates of sample 0
        std::vector<glm::vec2> samples;         // (height above the tangent plane, land cover class)
        std::atomic<bool> done{false};
    };
    static void build(WorldData& data, Field& f);  // runs on a worker; must not touch the Grass object

    gfx::Device* dev_ = nullptr;
    gfx::Bindless* bindless_ = nullptr;
    WorldData* data_ = nullptr;
    core::JobSystem* jobs_ = nullptr;

    // Ring of field buffers: a new field never overwrites one that a frame in flight still reads.
    static constexpr int kSlots = gfx::kFramesInFlight + 1;
    gfx::Buffer buffers_[kSlots];
    int slot_ = -1;
    std::shared_ptr<Field> active_, pending_;
    VkPipeline pipe_ = VK_NULL_HANDLE;
};

}  // namespace world
