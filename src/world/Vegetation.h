#pragma once
// Trees placed deterministically from ESA WorldCover classes + a simple climate model,
// stored per ~200 m cell on the GPU and culled/LOD-selected by a compute shader that
// writes indirect draw commands (mesh LOD0, mesh LOD1, octahedral impostor).
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "core/Jobs.h"
#include "gfx/Pipeline.h"
#include "render/GpuTypes.h"
#include "world/WorldData.h"

namespace world {

enum class TreeGroup { Conifer, ConiferYoung, UnderstoryConifer, Broadleaf, Shrub, Tropical, Arid, Count };

struct TreeVariant {
    std::string name;
    std::vector<TreeGroup> groups;
    float height = 1, crownRadius = 1;
    struct Sub { uint32_t firstIndex, indexCount; int32_t vertexOffset; uint32_t material; };
    std::vector<Sub> lods[2];
    float impRadius = 1, impCenterZ = 0;
    uint32_t impAlbedo = 0, impNormal = 0, impGrid = 8;
    float trunkRadius = 0.3f;
    float baseZ = 0.0f;        // bottom of the trunk in model space (some models start above their origin)
    float baseRadius = 0.0f;   // reach of the root flare, sunk into slopes so the downhill side never floats
};

struct Trunk {
    glm::dvec3 pos;
    float radius;
};

class Vegetation {
public:
    static constexpr int kCellLevel = 9;           // 1/512 degree cells (~217 m)
    static constexpr uint32_t kCellCapacity = 4096;
    static constexpr uint32_t kMaxCells = 1024;
    static constexpr uint32_t kLodCaps[3] = {6144, 49152, 196608};
    static constexpr uint32_t kShadowLodCaps[3] = {2048, 12288, 32768};
    static constexpr uint32_t kShadowViews = 4;    // one per shadow cascade, view 0 is the camera

    void init(gfx::Device& dev, gfx::Bindless& bindless, WorldData& data, core::JobSystem& jobs,
              const std::filesystem::path& assetDir, VkFormat colorFormat, VkFormat depthFormat);
    void shutdown();

    void update(const glm::dvec3& camEcef, double camLat, double camLon, float radius);
    // Largest radius whose cells fit into kMaxCells at this latitude (cells narrow towards the poles). Beyond it
    // slots run out and cells, near ones included, would be missing.
    static float maxRadius(double camLat);
    // Uploads this frame's camera relative cell table; call once per frame before cull().
    void beginFrame(uint32_t frameIndex, const glm::dvec3& camEcef);
    // Compute pass for one view (0 = camera, 1..kShadowViews = shadow cascades); outside of a render pass.
    // The view's frustum comes from frameData; minLod forces cheaper LODs for far shadow cascades.
    void cull(VkCommandBuffer cmd, VkDeviceAddress frameData, uint32_t view, uint32_t minLod = 0);
    // Draws inside the main render pass.
    void draw(VkCommandBuffer cmd, VkDeviceAddress frameData);
    // Depth-only draw of a shadow view inside the shadow render pass.
    void drawShadow(VkCommandBuffer cmd, VkDeviceAddress frameData, uint32_t view);

    void nearbyTrunks(const glm::dvec3& pos, double radius, std::vector<Trunk>& out);

    size_t activeCells() const { return readyCells_; }
    size_t instanceCount() const { return instanceTotal_; }
    size_t variantCount() const { return variants_.size(); }
    bool loaded() const { return !variants_.empty(); }

private:
    struct Cell {
        int64_t y = 0, x = 0;
        std::atomic<int> state{0};  // 0 queued, 1 built, 2 uploaded, -1 cancelled
        std::atomic<uint64_t> lastUsed{0};
        uint32_t slot = UINT32_MAX;
        glm::dvec3 origin{0};
        glm::vec3 east{1, 0, 0}, north{0, 1, 0}, up{0, 0, 1};
        std::vector<render::TreeInstance> instances;
        std::vector<uint32_t> variantCounts;  // instances per variant, for the per-frame capacity budget
    };

    void loadAssets(const std::filesystem::path& dir);
    // Per view: visible instances, LOD bucket counters and the indirect commands that draw them.
    struct View {
        uint32_t caps[3] = {};
        gfx::Buffer visible, counters, meshCmds, impCmds;
    };

    void build(Cell& c);
    int pickVariant(TreeGroup g, uint32_t h) const;
    uint32_t bucketBase(const uint32_t caps[3], uint32_t variant, uint32_t lod) const;
    void drawView(VkCommandBuffer cmd, VkDeviceAddress frameData, const View& v, VkPipeline bark, VkPipeline leaf,
                  VkPipeline imp);

    gfx::Device* dev_ = nullptr;
    gfx::Bindless* bindless_ = nullptr;
    WorldData* data_ = nullptr;
    core::JobSystem* jobs_ = nullptr;

    std::vector<TreeVariant> variants_;
    std::vector<bool> materialLeaf_;
    std::vector<uint32_t> materialAlbedo_, materialNormal_;
    std::vector<int> groupVariants_[int(TreeGroup::Count)];
    std::vector<gfx::Image> textures_;

    gfx::Buffer vertexBuffer_, indexBuffer_, variantBuffer_;
    gfx::Buffer instanceBuffer_, meshInfoBuffer_, impInfoBuffer_;
    View views_[1 + kShadowViews];
    gfx::Buffer cellBuffers_[gfx::kFramesInFlight];
    uint32_t barkCmdCount_ = 0, leafCmdCount_ = 0, impCmdCount_ = 0;
    uint32_t frameCellCount_ = 0;
    uint32_t cullFrameIndex_ = 0;

    VkPipeline cullPipe_ = VK_NULL_HANDLE, cmdPipe_ = VK_NULL_HANDLE;
    VkPipeline barkPipe_ = VK_NULL_HANDLE, leafPipe_ = VK_NULL_HANDLE, impPipe_ = VK_NULL_HANDLE;
    VkPipeline barkShadowPipe_ = VK_NULL_HANDLE, leafShadowPipe_ = VK_NULL_HANDLE, impShadowPipe_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, std::shared_ptr<Cell>> cells_;
    std::vector<uint32_t> freeSlots_;
    std::vector<std::pair<uint32_t, uint64_t>> coolingSlots_;  // slot, frame freed
    std::atomic<uint64_t> frame_{0};
    size_t readyCells_ = 0, instanceTotal_ = 0;
};

}  // namespace world
