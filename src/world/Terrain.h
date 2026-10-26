#pragma once
// Geographic quadtree terrain. Level 0 nodes are 1x1 degree cells; each level halves the
// cell size down to kMaxLevel (~200 m nodes, ~7 m vertex spacing). Chunks are generated on
// worker threads from streamed DEM/land-cover data and rendered camera-relative.
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "core/Jobs.h"
#include "gfx/Device.h"
#include "world/WorldData.h"

namespace world {

class Terrain {
public:
    static constexpr int kGrid = 32;           // quads per chunk side
    static constexpr int kMaxLevel = 9;
    static constexpr int kCoverRes = 64;       // land-cover texels per chunk side

    void init(gfx::Device& dev, WorldData& data, core::JobSystem& jobs);
    void shutdown();

    // Selects visible nodes, schedules generation, integrates finished chunks.
    void update(const glm::dvec3& camEcef, double camLat, double camLon);
    void draw(VkCommandBuffer cmd, VkPipelineLayout layout, VkDeviceAddress frameData,
              const glm::dvec3& camEcef, const glm::vec4 frustum[6]);
    // Lake surfaces (water.vert/.frag) of the chunks drawn last frame that have a lake level. Bind the water pipeline
    // first; draw after the opaque scene.
    void drawWater(VkCommandBuffer cmd, VkPipelineLayout layout, VkDeviceAddress frameData,
                   const glm::dvec3& camEcef, const glm::vec4 frustum[6]);

    size_t chunkCount() const { return chunks_.size(); }
    size_t drawnCount() const { return drawList_.size(); }
    size_t inFlight() const { return inFlight_.load(); }
    bool groundReady() const { return groundReady_; }

    float detail = 1.0f;  // scales the distance up to which nodes are refined (graphics quality)

private:
    struct Chunk {
        int level = 0;
        int64_t y = 0, x = 0;
        std::atomic<int> state{0};  // 0 = requested, 1 = built (CPU), 2 = ready (GPU), -1 = cancelled
        gfx::Buffer buffer;
        glm::dvec3 origin{0};
        double radius = 0;
        glm::vec3 up{0, 0, 1};     // local vertical at the chunk centre
        float waterAbove = -1e9f;  // lake level relative to the origin along `up`; -1e9 without water cover
        std::atomic<uint64_t> lastUsed{0};
        // CPU results handed from the worker to the main thread
        std::vector<uint8_t> payload;
        uint32_t vertexBytes = 0;
    };

    static uint64_t key(int level, int64_t y, int64_t x) {
        return (uint64_t(level) << 58) | (uint64_t(y) << 29) | uint64_t(x);
    }
    std::shared_ptr<Chunk> request(int level, int64_t y, int64_t x, float priority);
    bool visit(int level, int64_t y, int64_t x, const glm::dvec3& cam);
    void build(Chunk& c);
    void drawChunks(VkCommandBuffer cmd, VkPipelineLayout layout, VkDeviceAddress frameData, const glm::dvec3& camEcef,
                    const glm::vec4 frustum[6], bool lakesOnly);

    gfx::Device* dev_ = nullptr;
    WorldData* data_ = nullptr;
    core::JobSystem* jobs_ = nullptr;
    gfx::Buffer indexBuffer_;
    uint32_t indexCount_ = 0;

    std::unordered_map<uint64_t, std::shared_ptr<Chunk>> chunks_;
    std::vector<std::shared_ptr<Chunk>> drawList_;
    std::atomic<uint64_t> frame_{0};
    std::atomic<int> inFlight_{0};
    bool groundReady_ = false;
};

}  // namespace world
