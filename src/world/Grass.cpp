#include "world/Grass.h"

#include <cmath>
#include <cstring>

#include "geo/Geodesy.h"

namespace world {
namespace {

constexpr double kRegionDeg = 1.0 / 64.0;  // blade grid anchor regions (~1.7 km); grass only re-seeds at their edges
constexpr double kRebuildDist = 10.0;      // rebuild the field once the camera is this far from its centre

struct FieldHeader {  // mirrors GrassFieldRef in shaders/grass.vert
    glm::vec3 east;  float x0;
    glm::vec3 north; float y0;
    glm::vec3 up;    float cell;
    glm::vec3 anchorMod; uint32_t res;
};
static_assert(sizeof(FieldHeader) == 64);

struct GrassPush {
    VkDeviceAddress frame, field;
    glm::vec3 anchorRel;  float spacing;
    glm::vec2 camLocal;   uint32_t gridN; float radius;
};
static_assert(sizeof(GrassPush) <= gfx::kPushConstantSize);

}  // namespace

void Grass::init(gfx::Device& dev, gfx::Bindless& bindless, WorldData& data, core::JobSystem& jobs,
                 VkFormat colorFormat, VkFormat depthFormat) {
    dev_ = &dev;
    bindless_ = &bindless;
    data_ = &data;
    jobs_ = &jobs;
    VkDeviceSize size = sizeof(FieldHeader) + VkDeviceSize(kRes) * kRes * sizeof(glm::vec2);
    for (auto& b : buffers_) b = dev.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, true);

    gfx::GraphicsPipelineDesc d;
    d.colorFormats = {colorFormat};
    d.depthFormat = depthFormat;
    d.vs = "grass.vert";
    d.fs = "grass.frag";
    d.cull = VK_CULL_MODE_NONE;
    d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    pipe_ = gfx::createGraphicsPipeline(dev, bindless.pipelineLayout(), d);
}

void Grass::shutdown() {
    if (!dev_) return;
    for (auto& b : buffers_) dev_->destroyNow(b);
    if (pipe_) vkDestroyPipeline(dev_->device(), pipe_, nullptr);
}

void Grass::build(WorldData& data, Field& f) {
    f.samples.resize(size_t(kRes) * kRes);
    for (int j = 0; j < kRes; ++j)
        for (int i = 0; i < kRes; ++i) {
            double x = f.x0 + i * double(kCell), y = f.y0 + j * double(kCell);
            double lat, lon, h;
            geo::fromEcef(f.anchor + f.east * x + f.north * y, lat, lon, h);
            glm::dvec3 ground = geo::toEcef(lat, lon, data.meshHeight(lat, lon));
            f.samples[size_t(j) * kRes + i] =
                glm::vec2(float(glm::dot(ground - f.anchor, f.up)), float(data.landCover(lat, lon)));
        }
    f.done = true;
}

void Grass::update(const glm::dvec3& camEcef) {
    if (pending_ && pending_->done) {
        // Upload into the next ring slot; the GPU only ever reads the current one or the one before it.
        int slot = (slot_ + 1) % kSlots;
        Field& f = *pending_;
        auto* hdr = static_cast<FieldHeader*>(buffers_[slot].mapped);
        hdr->east = glm::vec3(f.east);
        hdr->north = glm::vec3(f.north);
        hdr->up = glm::vec3(f.up);
        hdr->x0 = float(f.x0);
        hdr->y0 = float(f.y0);
        hdr->cell = kCell;
        hdr->anchorMod = glm::vec3(glm::mod(f.anchor, glm::dvec3(4096.0)));
        hdr->res = kRes;
        std::memcpy(hdr + 1, f.samples.data(), f.samples.size() * sizeof(glm::vec2));
        slot_ = slot;
        active_ = std::move(pending_);
    }
    if (pending_) return;

    double lat, lon, h;
    geo::fromEcef(camEcef, lat, lon, h);
    double aLat = (std::floor(lat / kRegionDeg) + 0.5) * kRegionDeg, aLon = (std::floor(lon / kRegionDeg) + 0.5) * kRegionDeg;
    if (active_ && active_->anchorLat == aLat && active_->anchorLon == aLon) {
        glm::dvec3 d = camEcef - active_->anchor;
        double cx = glm::dot(d, active_->east) - active_->x0 - kRes * kCell * 0.5;
        double cy = glm::dot(d, active_->north) - active_->y0 - kRes * kCell * 0.5;
        if (std::sqrt(cx * cx + cy * cy) < kRebuildDist) return;
    }

    auto f = std::make_shared<Field>();
    f->anchorLat = aLat;
    f->anchorLon = aLon;
    f->anchor = geo::toEcef(aLat, aLon, 0.0);
    geo::Enu enu = geo::enuFrame(aLat, aLon);
    f->east = enu.east;
    f->north = enu.north;
    f->up = enu.up;
    glm::dvec3 d = camEcef - f->anchor;
    // Field origin snapped to whole samples so heights do not shift between rebuilds.
    f->x0 = std::floor(glm::dot(d, f->east) / kCell) * kCell - kRes * kCell * 0.5;
    f->y0 = std::floor(glm::dot(d, f->north) / kCell) * kCell - kRes * kCell * 0.5;
    pending_ = f;
    jobs_->submit(0.0f, [data = data_, f] { build(*data, *f); });
}

void Grass::draw(VkCommandBuffer cmd, VkDeviceAddress frameData, const glm::dvec3& camEcef) {
    if (slot_ < 0) return;
    glm::dvec3 d = camEcef - active_->anchor;
    GrassPush p{};
    p.frame = frameData;
    p.field = buffers_[slot_].address;
    p.anchorRel = glm::vec3(-d);
    p.spacing = kSpacing;
    p.camLocal = glm::vec2(float(glm::dot(d, active_->east)), float(glm::dot(d, active_->north)));
    p.gridN = uint32_t(std::ceil(2.0f * kRadius / kSpacing));
    p.radius = kRadius;
    VkPipelineLayout layout = bindless_->pipelineLayout();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 7, p.gridN * p.gridN, 0, 0);
}

}  // namespace world
