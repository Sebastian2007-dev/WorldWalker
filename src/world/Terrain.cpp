#include "world/Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "geo/Geodesy.h"
#include "render/GpuTypes.h"

namespace world {
namespace {

constexpr int kVertsPerSide = Terrain::kGrid + 1;
constexpr int kGridVerts = kVertsPerSide * kVertsPerSide;
constexpr int kSkirtVerts = 4 * kVertsPerSide;
constexpr double kSplitFactor = 2.2;   // split when distance < nodeSize * factor
constexpr int kRootRadius = 2;         // 1-degree cells around the camera
constexpr uint64_t kEvictAfterFrames = 600;

struct TerrainPush {
    VkDeviceAddress frame;
    VkDeviceAddress vertices;
    VkDeviceAddress cover;
    glm::vec3 originRel;
    float level;
    glm::vec3 originMod;
    float coverRes;
};
static_assert(sizeof(TerrainPush) <= 128);

double nodeSizeDeg(int level) { return 1.0 / double(1 << level); }
static_assert(WorldData::kMeshSpacingDeg == 1.0 / double((1 << Terrain::kMaxLevel) * Terrain::kGrid),
              "WorldData::meshHeight must sample the finest terrain grid");

}  // namespace

void Terrain::init(gfx::Device& dev, WorldData& data, core::JobSystem& jobs) {
    dev_ = &dev;
    data_ = &data;
    jobs_ = &jobs;

    // Shared index buffer: grid + skirts. Vertex (i, j): i east, j north.
    std::vector<uint32_t> idx;
    auto v = [](int i, int j) { return uint32_t(j * kVertsPerSide + i); };
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            idx.insert(idx.end(), {v(i, j), v(i + 1, j), v(i + 1, j + 1)});
            idx.insert(idx.end(), {v(i, j), v(i + 1, j + 1), v(i, j + 1)});
        }
    // Skirt vertex s of edge e is at kGridVerts + e * kVertsPerSide + s.
    auto edgeTop = [&](int e, int s) {
        switch (e) {
            case 0: return v(s, 0);
            case 1: return v(kGrid, s);
            case 2: return v(kGrid - s, kGrid);
            default: return v(0, kGrid - s);
        }
    };
    for (int e = 0; e < 4; ++e)
        for (int s = 0; s < kGrid; ++s) {
            uint32_t a = edgeTop(e, s), b = edgeTop(e, s + 1);
            uint32_t sa = uint32_t(kGridVerts + e * kVertsPerSide + s), sb = sa + 1;
            idx.insert(idx.end(), {a, sa, sb, a, sb, b});
        }
    indexCount_ = uint32_t(idx.size());
    indexBuffer_ = dev.createBuffer(idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY, false, false);
    dev.uploadBuffer(indexBuffer_, 0, idx.data(), idx.size() * 4);
}

void Terrain::shutdown() {
    for (auto& [k, c] : chunks_) dev_->destroyNow(c->buffer);
    chunks_.clear();
    dev_->destroyNow(indexBuffer_);
}

std::shared_ptr<Terrain::Chunk> Terrain::request(int level, int64_t y, int64_t x, float priority) {
    uint64_t k = key(level, y, x);
    auto it = chunks_.find(k);
    if (it != chunks_.end()) {
        it->second->lastUsed = frame_.load();
        if (it->second->state.load() == -1) {  // cancelled earlier, ask again
            it->second->state = 0;
        } else {
            return it->second;
        }
    } else {
        auto c = std::make_shared<Chunk>();
        c->level = level;
        c->y = y;
        c->x = x;
        c->lastUsed = frame_.load();
        it = chunks_.emplace(k, c).first;
    }
    auto c = it->second;
    ++inFlight_;
    jobs_->submit(priority, [this, c] {
        // Skip work for chunks that went out of view while queued.
        if (frame_ - c->lastUsed > 30) {
            c->state = -1;
        } else {
            build(*c);
            c->state = 1;
        }
        --inFlight_;
    });
    return c;
}

void Terrain::build(Chunk& c) {
    const double size = nodeSizeDeg(c.level);
    const double lat0 = -90.0 + double(c.y) * size;
    const double lon0 = -180.0 + double(c.x) * size;
    const double d = size / kGrid;
    const int S = kVertsPerSide + 2;  // with a 1-sample border for normals

    std::vector<glm::dvec3> p(size_t(S) * S);
    double hMin = 1e9, hMax = -1e9;
    for (int j = 0; j < S; ++j)
        for (int i = 0; i < S; ++i) {
            double lat = std::clamp(lat0 + (j - 1) * d, -89.999, 89.999);
            double lon = lon0 + (i - 1) * d;
            double h = data_->heightForSpacing(lat, lon, d);
            if (i > 0 && j > 0 && i < S - 1 && j < S - 1) {
                hMin = std::min(hMin, h);
                hMax = std::max(hMax, h);
            }
            p[size_t(j) * S + i] = geo::toEcef(lat, lon, h);
        }

    const double clat = lat0 + size * 0.5, clon = lon0 + size * 0.5;
    c.origin = geo::toEcef(clat, clon, (hMin + hMax) * 0.5);
    double halfDiag = std::sqrt(std::pow(size * 0.5 * geo::metersPerDegLat(clat), 2) +
                                std::pow(size * 0.5 * geo::metersPerDegLon(std::min(std::abs(lat0), std::abs(lat0 + size))), 2));
    c.radius = std::sqrt(halfDiag * halfDiag + std::pow((hMax - hMin) * 0.5, 2)) + 10.0;
    geo::Enu enu = geo::enuFrame(clat, clon);
    double skirt = std::max(4.0, size * geo::metersPerDegLat(clat) / kGrid * 0.6);

    std::vector<render::TerrainVertex> verts(kGridVerts + kSkirtVerts);
    for (int j = 0; j < kVertsPerSide; ++j)
        for (int i = 0; i < kVertsPerSide; ++i) {
            auto at = [&](int ii, int jj) { return p[size_t(jj + 1) * S + (ii + 1)]; };
            glm::dvec3 de = at(i + 1, j) - at(i - 1, j);
            glm::dvec3 dn = at(i, j + 1) - at(i, j - 1);
            glm::vec3 n = glm::vec3(glm::normalize(glm::cross(de, dn)));
            auto& v = verts[size_t(j) * kVertsPerSide + i];
            v.pos = glm::vec3(at(i, j) - c.origin);
            v.normal = render::octEncode(n);
            v.uv = glm::vec2(float(i) / kGrid, float(j) / kGrid);
        }
    for (int e = 0; e < 4; ++e)
        for (int s = 0; s <= kGrid; ++s) {
            int i, j;
            switch (e) {
                case 0: i = s; j = 0; break;
                case 1: i = kGrid; j = s; break;
                case 2: i = kGrid - s; j = kGrid; break;
                default: i = 0; j = kGrid - s; break;
            }
            auto v = verts[size_t(j) * kVertsPerSide + i];
            v.pos -= glm::vec3(enu.up * skirt);
            verts[kGridVerts + e * kVertsPerSide + s] = v;
        }

    // Land cover, row 0 = south.
    std::vector<uint8_t> cover(kCoverRes * kCoverRes);
    double cd = size / kCoverRes;
    for (int y = 0; y < kCoverRes; ++y)
        for (int x = 0; x < kCoverRes; ++x)
            cover[size_t(y) * kCoverRes + x] = data_->landCover(lat0 + (y + 0.5) * cd, lon0 + (x + 0.5) * cd, cd);

    c.vertexBytes = uint32_t(verts.size() * sizeof(render::TerrainVertex));
    c.payload.resize(c.vertexBytes + cover.size());
    std::memcpy(c.payload.data(), verts.data(), c.vertexBytes);
    std::memcpy(c.payload.data() + c.vertexBytes, cover.data(), cover.size());
}

bool Terrain::visit(int level, int64_t y, int64_t x, const glm::dvec3& cam) {
    double size = nodeSizeDeg(level);
    double clat = -90.0 + (double(y) + 0.5) * size, clon = -180.0 + (double(x) + 0.5) * size;
    double sizeM = size * geo::metersPerDegLat(clat);

    glm::dvec3 approx = geo::toEcef(clat, clon, 0.0);
    double approxDist = std::max(0.0, glm::length(cam - approx) - sizeM * 0.75);
    auto c = request(level, y, x, float(approxDist / sizeM) - float(level) * 0.01f);
    bool ready = c->state.load() == 2;
    double dist = ready ? std::max(0.0, glm::length(cam - c->origin) - c->radius) : approxDist;
    float prio = float(dist / sizeM) - float(level) * 0.01f;

    bool wantSplit = level < kMaxLevel && dist < sizeM * kSplitFactor;
    if (wantSplit) {
        bool allReady = true;
        for (int k = 0; k < 4; ++k) {
            auto ch = request(level + 1, y * 2 + (k >> 1), x * 2 + (k & 1), prio);
            allReady &= ch->state.load() == 2;
        }
        if (allReady) {
            for (int k = 0; k < 4; ++k) visit(level + 1, y * 2 + (k >> 1), x * 2 + (k & 1), cam);
            return true;
        }
    }
    if (ready) {
        drawList_.push_back(c);
        return true;
    }
    return false;
}

void Terrain::update(const glm::dvec3& camEcef, double camLat, double camLon) {
    ++frame_;

    // Integrate finished chunks (uploads are flushed at the start of this frame's command buffer).
    for (auto& [k, c] : chunks_) {
        if (c->state.load() == 1) {
            c->buffer = dev_->createBuffer(c->payload.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VMA_MEMORY_USAGE_GPU_ONLY);
            dev_->uploadBuffer(c->buffer, 0, c->payload.data(), c->payload.size());
            c->payload.clear();
            c->payload.shrink_to_fit();
            c->state = 2;
        }
    }

    drawList_.clear();
    int cy = int(std::floor(camLat + 90.0));
    int cx = int(std::floor(camLon + 180.0));
    int rx = std::min(180, int(std::ceil(kRootRadius / std::max(0.05, std::cos(camLat * geo::kDeg)))));
    for (int dy = -kRootRadius; dy <= kRootRadius; ++dy) {
        int y = cy + dy;
        if (y < 0 || y >= 180) continue;
        for (int dx = -rx; dx <= rx; ++dx) {
            int x = ((cx + dx) % 360 + 360) % 360;
            visit(0, y, x, camEcef);
        }
    }

    // Evict chunks that have not been needed for a while.
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        auto& c = it->second;
        int s = c->state.load();
        bool idle = s == 2 || s == -1;
        if (idle && frame_ - c->lastUsed > kEvictAfterFrames) {
            dev_->destroyLater(c->buffer);
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }
}

void Terrain::draw(VkCommandBuffer cmd, VkPipelineLayout layout, VkDeviceAddress frameData, const glm::dvec3& camEcef,
                   const glm::vec4 frustum[6]) {
    vkCmdBindIndexBuffer(cmd, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (auto& c : drawList_) {
        glm::vec3 rel = glm::vec3(c->origin - camEcef);
        bool visible = true;
        for (int i = 0; i < 6 && visible; ++i)
            visible = glm::dot(glm::vec3(frustum[i]), rel) + frustum[i].w > -float(c->radius);
        if (!visible) continue;
        TerrainPush pc{};
        pc.frame = frameData;
        pc.vertices = c->buffer.address;
        pc.cover = c->buffer.address + c->vertexBytes;
        pc.originRel = rel;
        pc.level = float(c->level);
        pc.originMod = glm::vec3(glm::mod(c->origin, glm::dvec3(4096.0)));
        pc.coverRes = float(kCoverRes);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
    }
}

}  // namespace world
