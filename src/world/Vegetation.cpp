#include "world/Vegetation.h"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

#include "geo/Geodesy.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace world {
namespace {

struct CullPush {
    VkDeviceAddress frame, cells, instances, variants, visible, counters;
    uint32_t cellCount, variantCount;
    uint32_t caps[3];
    uint32_t minLod;
};
struct CmdPush {
    VkDeviceAddress counters, meshCmds, meshInfo, impCmds, impInfo;
    uint32_t meshCount, impCount;
    uint32_t caps[3];
};
struct DrawPush {
    VkDeviceAddress frame, vertices, visible, cells, info, variants;
    uint32_t infoBase;
};

TreeGroup groupFromTag(const std::string& t) {
    if (t == "conifer") return TreeGroup::Conifer;
    if (t == "conifer_young") return TreeGroup::ConiferYoung;
    if (t == "understory_conifer") return TreeGroup::UnderstoryConifer;
    if (t == "broadleaf") return TreeGroup::Broadleaf;
    if (t == "shrub") return TreeGroup::Shrub;
    if (t == "tropical") return TreeGroup::Tropical;
    if (t == "arid") return TreeGroup::Arid;
    return TreeGroup::Broadleaf;
}

double smoothNoise(double x, double y) {
    double fx = std::floor(x), fy = std::floor(y);
    int64_t ix = int64_t(fx), iy = int64_t(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    double a = hashFloat(hash2(ix, iy) ^ 0x51ED27u), b = hashFloat(hash2(ix + 1, iy) ^ 0x51ED27u);
    double c = hashFloat(hash2(ix, iy + 1) ^ 0x51ED27u), d = hashFloat(hash2(ix + 1, iy + 1) ^ 0x51ED27u);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// How far below the ground surface a tree's origin goes: puts the trunk bottom on the ground and sinks the root
// flare into slopes so its downhill side does not hang in the air.
double sinkDepth(const TreeVariant& v, float scale, double slope) {
    return (double(v.baseZ) + 0.05 + std::min(slope, 1.0) * double(v.baseRadius) * 0.8) * scale;
}

}  // namespace

constexpr uint32_t Vegetation::kLodCaps[3];
constexpr uint32_t Vegetation::kShadowLodCaps[3];

uint32_t Vegetation::bucketBase(const uint32_t caps[3], uint32_t variant, uint32_t lod) const {
    uint32_t per = caps[0] + caps[1] + caps[2];
    uint32_t off = 0;
    for (uint32_t l = 0; l < lod; ++l) off += caps[l];
    return variant * per + off;
}

void Vegetation::loadAssets(const fs::path& dir) {
    std::ifstream idxFile(dir / "index.json");
    if (!idxFile) {
        std::fprintf(stderr, "No baked trees in %s (run tools/bake_trees.py)\n", dir.string().c_str());
        return;
    }
    json index = json::parse(idxFile);

    std::vector<render::TreeVertex> verts;
    std::vector<uint32_t> indices;

    auto loadTex = [&](const fs::path& p, bool srgb, VkSampler sampler) -> uint32_t {
        int w, h, n;
        stbi_uc* px = stbi_load(p.string().c_str(), &w, &h, &n, 4);
        if (!px) {
            std::fprintf(stderr, "missing texture %s\n", p.string().c_str());
            static uint32_t white = 0xFFFFFFFFu;
            gfx::Image img = dev_->createTexture2D(&white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
            textures_.push_back(img);
            return bindless_->add(img, sampler);
        }
        gfx::Image img = dev_->createTexture2D(px, uint32_t(w), uint32_t(h),
                                               srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, true);
        stbi_image_free(px);
        textures_.push_back(img);
        return bindless_->add(img, sampler);
    };

    for (auto& spName : index["species"]) {
        fs::path sdir = dir / spName.get<std::string>();
        std::ifstream sf(sdir / "species.json");
        if (!sf) continue;
        json sp = json::parse(sf);
        uint32_t matBase = uint32_t(materialLeaf_.size());
        for (auto& m : sp["materials"]) {
            materialLeaf_.push_back(m["leaf"].get<bool>());
            materialAlbedo_.push_back(loadTex(sdir / m["albedo"].get<std::string>(), true, bindless_->linearRepeat()));
            materialNormal_.push_back(loadTex(sdir / m["normal"].get<std::string>(), false, bindless_->linearRepeat()));
        }
        std::vector<TreeGroup> groups;
        for (auto& b : sp["biomes"]) groups.push_back(groupFromTag(b.get<std::string>()));

        for (auto& v : sp["variants"]) {
            TreeVariant tv;
            tv.name = v["name"].get<std::string>();
            tv.groups = groups;
            tv.height = v["height"].get<float>();
            tv.crownRadius = v["crown_radius"].get<float>();
            for (int l = 0; l < 2; ++l) {
                std::ifstream mf(sdir / v["lods"][l]["mesh"].get<std::string>(), std::ios::binary);
                char magic[4];
                uint32_t nv, ni, ns;
                float bounds[6];
                mf.read(magic, 4);
                mf.read(reinterpret_cast<char*>(&nv), 4);
                mf.read(reinterpret_cast<char*>(&ni), 4);
                mf.read(reinterpret_cast<char*>(&ns), 4);
                mf.read(reinterpret_cast<char*>(bounds), 24);
                std::vector<uint32_t> subs(size_t(ns) * 3);
                mf.read(reinterpret_cast<char*>(subs.data()), std::streamsize(subs.size() * 4));
                int32_t vbase = int32_t(verts.size());
                uint32_t ibase = uint32_t(indices.size());
                verts.resize(verts.size() + nv);
                mf.read(reinterpret_cast<char*>(&verts[size_t(vbase)]), std::streamsize(size_t(nv) * sizeof(render::TreeVertex)));
                indices.resize(indices.size() + ni);
                mf.read(reinterpret_cast<char*>(&indices[ibase]), std::streamsize(size_t(ni) * 4));
                for (uint32_t s = 0; s < ns; ++s)
                    tv.lods[l].push_back({ibase + subs[s * 3], subs[s * 3 + 1], vbase, matBase + subs[s * 3 + 2]});
                if (l == 0) {
                    // Trunk bottom: lowest vertex near the axis (drooping branches further out do not count).
                    float lowAxis = 1e9f, lowAny = 1e9f;
                    for (uint32_t k = 0; k < nv; ++k) {
                        const glm::vec3& q = verts[size_t(vbase) + k].pos;
                        lowAny = std::min(lowAny, q.z);
                        if (q.x * q.x + q.y * q.y < 0.36f) lowAxis = std::min(lowAxis, q.z);
                    }
                    tv.baseZ = lowAxis < 1e8f ? lowAxis : lowAny;
                    for (uint32_t k = 0; k < nv; ++k) {
                        const glm::vec3& q = verts[size_t(vbase) + k].pos;
                        if (q.z < tv.baseZ + 0.25f)
                            tv.baseRadius = std::max(tv.baseRadius, std::sqrt(q.x * q.x + q.y * q.y));
                    }
                    tv.baseRadius = std::min(tv.baseRadius, 1.5f);
                }
            }
            auto& imp = v["impostor"];
            tv.impRadius = imp["radius"].get<float>();
            tv.impCenterZ = imp["center_z"].get<float>();
            tv.impGrid = imp["grid"].get<uint32_t>();
            tv.impAlbedo = loadTex(sdir / imp["albedo"].get<std::string>(), true, bindless_->linearClamp());
            tv.impNormal = loadTex(sdir / imp["normal"].get<std::string>(), false, bindless_->linearClamp());
            bool big = tv.height > 6.0f;
            tv.trunkRadius = tv.height < 2.0f ? 0.0f : (big ? 0.35f : 0.2f);
            int vi = int(variants_.size());
            for (auto g : groups) groupVariants_[int(g)].push_back(vi);
            variants_.push_back(std::move(tv));
        }
    }
    std::printf("Trees: %zu variants, %zu vertices, %zu triangles\n", variants_.size(), verts.size(), indices.size() / 3);
    if (variants_.empty()) return;

    vertexBuffer_ = dev_->createBuffer(verts.size() * sizeof(render::TreeVertex),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_GPU_ONLY);
    dev_->uploadBuffer(vertexBuffer_, 0, verts.data(), vertexBuffer_.size);
    indexBuffer_ = dev_->createBuffer(indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_GPU_ONLY, false, false);
    dev_->uploadBuffer(indexBuffer_, 0, indices.data(), indexBuffer_.size);
}

void Vegetation::init(gfx::Device& dev, gfx::Bindless& bindless, WorldData& data, core::JobSystem& jobs,
                      const fs::path& assetDir, VkFormat colorFormat, VkFormat depthFormat) {
    dev_ = &dev;
    bindless_ = &bindless;
    data_ = &data;
    jobs_ = &jobs;
    loadAssets(assetDir);
    if (variants_.empty()) return;

    // Variant table.
    std::vector<render::VariantGpu> vg;
    for (auto& v : variants_)
        vg.push_back({v.height, std::max(v.height * 0.6f, v.crownRadius), v.impRadius, v.impCenterZ, v.impAlbedo,
                      v.impNormal, v.impGrid, 0});
    variantBuffer_ = dev.createBuffer(vg.size() * sizeof(render::VariantGpu),
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VMA_MEMORY_USAGE_GPU_ONLY);
    dev.uploadBuffer(variantBuffer_, 0, vg.data(), variantBuffer_.size);

    // Static draw info: bark (opaque) first, then leaves (alpha tested, two sided). Each view gets its own copy of
    // the indirect commands because instance offsets depend on the view's LOD capacities.
    struct MeshSub { uint32_t variant, lod; const TreeVariant::Sub* sub; };
    std::vector<MeshSub> meshSubs;
    std::vector<render::DrawInfo> meshInfo;
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t v = 0; v < variants_.size(); ++v)
            for (uint32_t l = 0; l < 2; ++l)
                for (auto& s : variants_[v].lods[l]) {
                    if (materialLeaf_[s.material] != (pass == 1)) continue;
                    meshSubs.push_back({v, l, &s});
                    meshInfo.push_back({v * 3 + l, materialAlbedo_[s.material], materialNormal_[s.material], v});
                    (pass == 0 ? barkCmdCount_ : leafCmdCount_)++;
                }
    std::vector<render::DrawInfo> impInfo;
    for (uint32_t v = 0; v < variants_.size(); ++v)
        impInfo.push_back({v * 3 + 2, variants_[v].impAlbedo, variants_[v].impNormal, v});
    impCmdCount_ = uint32_t(impInfo.size());
    auto makeBuf = [&](const void* data, size_t size, VkBufferUsageFlags extra) {
        gfx::Buffer b = dev.createBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra,
                                         VMA_MEMORY_USAGE_GPU_ONLY);
        dev.uploadBuffer(b, 0, data, size);
        return b;
    };
    meshInfoBuffer_ = makeBuf(meshInfo.data(), meshInfo.size() * sizeof(meshInfo[0]), 0);
    impInfoBuffer_ = makeBuf(impInfo.data(), impInfo.size() * sizeof(impInfo[0]), 0);

    for (uint32_t vi = 0; vi <= kShadowViews; ++vi) {
        View& view = views_[vi];
        const uint32_t* caps = vi == 0 ? kLodCaps : kShadowLodCaps;
        std::copy(caps, caps + 3, view.caps);
        std::vector<VkDrawIndexedIndirectCommand> meshCmds;
        for (auto& m : meshSubs)
            meshCmds.push_back({m.sub->indexCount, 0, m.sub->firstIndex, m.sub->vertexOffset, bucketBase(caps, m.variant, m.lod)});
        std::vector<VkDrawIndirectCommand> impCmds;
        for (uint32_t v = 0; v < variants_.size(); ++v) impCmds.push_back({6, 0, 0, bucketBase(caps, v, 2)});
        view.meshCmds = makeBuf(meshCmds.data(), meshCmds.size() * sizeof(meshCmds[0]), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        view.impCmds = makeBuf(impCmds.data(), impCmds.size() * sizeof(impCmds[0]), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        uint32_t per = caps[0] + caps[1] + caps[2];
        view.visible = dev.createBuffer(VkDeviceSize(per) * variants_.size() * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                        VMA_MEMORY_USAGE_GPU_ONLY);
        view.counters = dev.createBuffer(variants_.size() * 3 * 4,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VMA_MEMORY_USAGE_GPU_ONLY);
    }
    instanceBuffer_ = dev.createBuffer(VkDeviceSize(kMaxCells) * kCellCapacity * sizeof(render::TreeInstance),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VMA_MEMORY_USAGE_GPU_ONLY);
    for (auto& b : cellBuffers_)
        b = dev.createBuffer(kMaxCells * sizeof(render::VegCell), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, true);
    for (uint32_t i = 0; i < kMaxCells; ++i) freeSlots_.push_back(kMaxCells - 1 - i);

    VkPipelineLayout layout = bindless.pipelineLayout();
    cullPipe_ = gfx::createComputePipeline(dev, layout, "tree_cull.comp");
    cmdPipe_ = gfx::createComputePipeline(dev, layout, "tree_cmds.comp");
    gfx::GraphicsPipelineDesc d;
    d.colorFormats = {colorFormat};
    d.depthFormat = depthFormat;
    d.vs = "tree.vert";
    d.fs = "tree_bark.frag";
    barkPipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.fs = "tree_leaf.frag";
    d.cull = VK_CULL_MODE_NONE;
    leafPipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.vs = "impostor.vert";
    d.fs = "impostor.frag";
    impPipe_ = gfx::createGraphicsPipeline(dev, layout, d);

    gfx::GraphicsPipelineDesc sd;
    sd.depthFormat = depthFormat;
    sd.cull = VK_CULL_MODE_NONE;
    sd.vs = "tree.vert";
    barkShadowPipe_ = gfx::createGraphicsPipeline(dev, layout, sd);
    sd.fs = "tree_shadow.frag";
    leafShadowPipe_ = gfx::createGraphicsPipeline(dev, layout, sd);
    sd.vs = "impostor.vert";
    sd.fs = "impostor_shadow.frag";
    impShadowPipe_ = gfx::createGraphicsPipeline(dev, layout, sd);
}

void Vegetation::shutdown() {
    if (!dev_) return;
    for (auto& t : textures_) dev_->destroyNow(t);
    for (gfx::Buffer* b : {&vertexBuffer_, &indexBuffer_, &variantBuffer_, &instanceBuffer_, &meshInfoBuffer_,
                           &impInfoBuffer_})
        dev_->destroyNow(*b);
    for (auto& v : views_)
        for (gfx::Buffer* b : {&v.visible, &v.counters, &v.meshCmds, &v.impCmds}) dev_->destroyNow(*b);
    for (auto& b : cellBuffers_) dev_->destroyNow(b);
    for (VkPipeline p : {cullPipe_, cmdPipe_, barkPipe_, leafPipe_, impPipe_, barkShadowPipe_, leafShadowPipe_,
                         impShadowPipe_})
        if (p) vkDestroyPipeline(dev_->device(), p, nullptr);
}

int Vegetation::pickVariant(TreeGroup g, uint32_t h) const {
    const auto* list = &groupVariants_[int(g)];
    if (list->empty()) list = &groupVariants_[int(TreeGroup::Broadleaf)];
    if (list->empty()) list = &groupVariants_[int(TreeGroup::Conifer)];
    if (list->empty()) return -1;
    return (*list)[h % list->size()];
}

void Vegetation::build(Cell& c) {
    const double size = 1.0 / double(1 << kCellLevel);
    const double lat0 = -90.0 + double(c.y) * size, lon0 = -180.0 + double(c.x) * size;
    const double clat = lat0 + size * 0.5, clon = lon0 + size * 0.5;
    c.origin = geo::toEcef(clat, clon, data_->dem().height(clat, clon, 0));
    geo::Enu enu = geo::enuFrame(clat, clon);
    c.east = glm::vec3(enu.east);
    c.north = glm::vec3(enu.north);
    c.up = glm::vec3(enu.up);

    const double absLat = std::abs(clat);
    const double mLat = geo::metersPerDegLat(clat), mLon = geo::metersPerDegLon(clat);
    const double spacing = 4.0;
    const int nLat = int(std::ceil(size * mLat / spacing)), nLon = int(std::ceil(size * mLon / spacing));
    const double dLat = size / nLat, dLon = size / nLon;

    c.instances.clear();
    for (int a = 0; a < nLat; ++a)
        for (int b = 0; b < nLon; ++b) {
            if (c.instances.size() >= kCellCapacity) break;
            uint32_t h = hash2(c.y * 4096 + a, c.x * 4096 + b);
            double lat = lat0 + (a + 0.5 + (hashFloat(hash32(h ^ 0x1)) - 0.5) * 0.9) * dLat;
            double lon = lon0 + (b + 0.5 + (hashFloat(hash32(h ^ 0x2)) - 0.5) * 0.9) * dLon;
            uint8_t cls = data_->landCover(lat, lon);

            float density = 0;
            switch (cls) {
                case data::lc::Tree: density = 0.80f; break;
                case data::lc::Mangrove: density = 0.70f; break;
                case data::lc::Shrub: density = 0.14f; break;
                case data::lc::Wetland: density = 0.05f; break;
                case data::lc::Built: density = 0.03f; break;
                case data::lc::Grass: density = 0.012f; break;
                case data::lc::Moss: density = 0.01f; break;
                case data::lc::Crop: density = 0.002f; break;
                case data::lc::Bare: density = absLat < 40 ? 0.004f : 0.0f; break;
                default: density = 0;
            }
            if (hashFloat(hash32(h ^ 0x3)) >= density) continue;

            double slope = 0.0;
            double hgt = data_->meshHeight(lat, lon, &slope);
            double y = lat * 111320.0, x = lon * 111320.0 * std::cos(clat * geo::kDeg);

            // Climate: conifer share rises with latitude and elevation. Stands of ~120 m keep
            // conifer and broadleaf patches together instead of a salt-and-pepper mix.
            double coniferShare;
            if (absLat > 55) coniferShare = 0.9;
            else if (absLat > 40) coniferShare = 0.45 + std::clamp((hgt - 500.0) / 1000.0, 0.0, 0.5);
            else if (absLat > 23.5) coniferShare = 0.3 + std::clamp((hgt - 1200.0) / 1500.0, 0.0, 0.6);
            else coniferShare = std::clamp((hgt - 2000.0) / 1500.0, 0.0, 0.8);
            double stand = smoothNoise(x / 120.0, y / 120.0) * 0.85 + hashFloat(hash32(h ^ 0x4)) * 0.15;
            bool conifer = stand < coniferShare;

            TreeGroup g;
            switch (cls) {
                case data::lc::Tree:
                case data::lc::Mangrove:
                    if (conifer) g = hashFloat(hash32(h ^ 0x5)) < 0.12f ? TreeGroup::ConiferYoung : TreeGroup::Conifer;
                    else g = absLat < 23.5 ? TreeGroup::Tropical : TreeGroup::Broadleaf;
                    if (absLat < 23.5 && !conifer && hashFloat(hash32(h ^ 0x6)) < 0.6f) g = TreeGroup::Broadleaf;
                    break;
                case data::lc::Shrub:
                case data::lc::Bare:
                    g = (absLat > 12 && absLat < 38) ? TreeGroup::Arid : (conifer ? TreeGroup::UnderstoryConifer : TreeGroup::Shrub);
                    break;
                case data::lc::Moss:
                    g = TreeGroup::UnderstoryConifer;
                    break;
                default:
                    g = conifer ? TreeGroup::Conifer : TreeGroup::Broadleaf;
            }
            int v = pickVariant(g, hash32(h ^ 0x7));
            if (v < 0) continue;
            const TreeVariant& tv = variants_[v];

            float rnd = hashFloat(hash32(h ^ 0x8));
            float targetH;
            switch (g) {
                case TreeGroup::Conifer: targetH = 16.0f + rnd * 16.0f; break;
                case TreeGroup::Broadleaf: targetH = 9.0f + rnd * 9.0f; break;
                case TreeGroup::Tropical: targetH = 12.0f + rnd * 12.0f; break;
                default: targetH = tv.height * (0.75f + rnd * 0.5f);
            }
            float scale = std::clamp(targetH / tv.height, 0.6f, 2.4f);

            glm::dvec3 p = geo::toEcef(lat, lon, hgt - sinkDepth(tv, scale, slope)) - c.origin;
            render::TreeInstance inst{};
            inst.pos = glm::vec3(p);
            inst.yaw = hashFloat(hash32(h ^ 0x9)) * 6.2831853f;
            inst.scale = scale;
            inst.variant = uint32_t(v);
            c.instances.push_back(inst);

            // Saplings under conifer canopy.
            if (cls == data::lc::Tree && conifer && hashFloat(hash32(h ^ 0xA)) < 0.25f &&
                c.instances.size() < kCellCapacity) {
                int sv = pickVariant(TreeGroup::UnderstoryConifer, hash32(h ^ 0xB));
                if (sv >= 0) {
                    double slat = lat + (hashFloat(hash32(h ^ 0xC)) - 0.5) * dLat * 0.8;
                    double slon = lon + (hashFloat(hash32(h ^ 0xD)) - 0.5) * dLon * 0.8;
                    double sslope = 0.0;
                    double sh = data_->meshHeight(slat, slon, &sslope);
                    float sscale = 0.7f + hashFloat(hash32(h ^ 0xF)) * 0.9f;
                    glm::dvec3 sp = geo::toEcef(slat, slon, sh - sinkDepth(variants_[sv], sscale, sslope)) - c.origin;
                    render::TreeInstance s{};
                    s.pos = glm::vec3(sp);
                    s.yaw = hashFloat(hash32(h ^ 0xE)) * 6.2831853f;
                    s.scale = sscale;
                    s.variant = uint32_t(sv);
                    c.instances.push_back(s);
                }
            }
        }
    c.variantCounts.assign(variants_.size(), 0);
    for (auto& inst : c.instances) ++c.variantCounts[inst.variant];
}

float Vegetation::maxRadius(double camLat) {
    const double size = 1.0 / double(1 << kCellLevel);
    double cellArea = size * geo::metersPerDegLat(camLat) * size * std::max(1.0, geo::metersPerDegLon(camLat));
    // 90 % of the slots, minus the margin update() adds around the radius.
    double r = std::sqrt(0.9 * kMaxCells * cellArea / 3.14159265358979) - 0.7 * size * geo::metersPerDegLat(camLat);
    return float(std::max(r, 300.0));
}

void Vegetation::update(const glm::dvec3& camEcef, double camLat, double camLon, float radius) {
    radius = std::min(radius, maxRadius(camLat));
    if (variants_.empty()) return;
    uint64_t frame = ++frame_;

    // Release slots whose last GPU use is certainly finished.
    for (auto it = coolingSlots_.begin(); it != coolingSlots_.end();) {
        if (frame - it->second > gfx::kFramesInFlight + 1) {
            freeSlots_.push_back(it->first);
            it = coolingSlots_.erase(it);
        } else {
            ++it;
        }
    }

    const double size = 1.0 / double(1 << kCellLevel);
    double rLat = radius / geo::metersPerDegLat(camLat);
    double rLon = radius / std::max(1.0, geo::metersPerDegLon(camLat));
    int64_t y0 = int64_t(std::floor((camLat - rLat + 90.0) / size)), y1 = int64_t(std::floor((camLat + rLat + 90.0) / size));
    int64_t x0 = int64_t(std::floor((camLon - rLon + 180.0) / size)), x1 = int64_t(std::floor((camLon + rLon + 180.0) / size));
    int64_t xCount = int64_t(360.0 / size);

    for (int64_t y = std::max<int64_t>(y0, 0); y <= y1; ++y)
        for (int64_t xx = x0; xx <= x1; ++xx) {
            int64_t x = ((xx % xCount) + xCount) % xCount;
            double clat = -90.0 + (double(y) + 0.5) * size, clon = -180.0 + (double(xx) + 0.5) * size;
            double dy = (clat - camLat) * geo::metersPerDegLat(camLat);
            double dx = (clon - camLon) * geo::metersPerDegLon(camLat);
            double dist = std::sqrt(dx * dx + dy * dy);
            if (dist > radius + size * geo::metersPerDegLat(camLat) * 0.7) continue;
            uint64_t k = (uint64_t(y) << 32) | uint64_t(x);
            auto it = cells_.find(k);
            if (it != cells_.end()) {
                it->second->lastUsed = frame;
                if (it->second->state.load() != -1) continue;
                it->second->state = 0;
            } else {
                auto c = std::make_shared<Cell>();
                c->y = y;
                c->x = x;
                c->lastUsed = frame;
                it = cells_.emplace(k, c).first;
            }
            auto c = it->second;
            jobs_->submit(float(dist / 200.0) + 0.5f, [this, c] {
                if (frame_ - c->lastUsed > 30) {
                    c->state = -1;
                    return;
                }
                build(*c);
                c->state = 1;
            });
        }

    // Upload finished cells, evict stale ones.
    for (auto it = cells_.begin(); it != cells_.end();) {
        auto& c = it->second;
        int s = c->state.load();
        if (s == 1) {
            if (!freeSlots_.empty()) {
                c->slot = freeSlots_.back();
                freeSlots_.pop_back();
                if (!c->instances.empty())
                    dev_->uploadBuffer(instanceBuffer_, VkDeviceSize(c->slot) * kCellCapacity * sizeof(render::TreeInstance),
                                       c->instances.data(), c->instances.size() * sizeof(render::TreeInstance));
                c->state = 2;
            }
        }
        if (s != 0 && frame - c->lastUsed > 3) {
            if (c->slot != UINT32_MAX) coolingSlots_.push_back({c->slot, frame});
            it = cells_.erase(it);
            continue;
        }
        ++it;
    }
}

void Vegetation::beginFrame(uint32_t frameIndex, const glm::dvec3& camEcef) {
    if (variants_.empty()) return;
    // Per-frame cell table, camera relative.
    auto* cells = static_cast<render::VegCell*>(cellBuffers_[frameIndex].mapped);
    // Nearest cells first, and only as many as the visible-tree buckets can hold: in a dense forest with a long view
    // distance the far cells are left out instead of arbitrary ones (which used to drop trees right next to the
    // camera). Every instance ends in at most one bucket of its variant, so a variant's total never overflows the
    // coarsest bucket.
    std::vector<std::pair<double, Cell*>> ready;
    for (auto& [k, c] : cells_)
        if (c->state.load() == 2 && !c->instances.empty()) ready.push_back({glm::length(c->origin - camEcef), c.get()});
    std::sort(ready.begin(), ready.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<uint32_t> budget(variants_.size(), kLodCaps[2]);
    uint32_t n = 0;
    size_t total = 0;
    for (auto& [dist, c] : ready) {
        bool fits = true;
        for (size_t v = 0; v < c->variantCounts.size(); ++v) fits &= c->variantCounts[v] <= budget[v];
        if (!fits) break;
        for (size_t v = 0; v < c->variantCounts.size(); ++v) budget[v] -= c->variantCounts[v];
        render::VegCell& vc = cells[n++];
        vc.originRel = glm::vec3(c->origin - camEcef);
        vc.firstInstance = c->slot * kCellCapacity;
        vc.east = c->east;
        vc.instanceCount = uint32_t(c->instances.size());
        vc.north = c->north;
        vc.up = c->up;
        total += c->instances.size();
    }
    frameCellCount_ = n;
    cullFrameIndex_ = frameIndex;
    readyCells_ = n;
    instanceTotal_ = total;
}

void Vegetation::cull(VkCommandBuffer cmd, VkDeviceAddress frameData, uint32_t view, uint32_t minLod) {
    if (variants_.empty()) return;
    View& v = views_[view];
    uint32_t n = frameCellCount_;

    // Previous frame's indirect draws must be done before counters/commands are rewritten.
    gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdFillBuffer(cmd, v.counters.buffer, 0, VK_WHOLE_SIZE, 0);
    gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    VkPipelineLayout layout = bindless_->pipelineLayout();
    VkDescriptorSet set = bindless_->set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    if (n > 0) {
        CullPush cp{frameData, cellBuffers_[cullFrameIndex_].address, instanceBuffer_.address, variantBuffer_.address,
                    v.visible.address, v.counters.address, n, uint32_t(variants_.size()),
                    {v.caps[0], v.caps[1], v.caps[2]}, minLod};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipe_);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(cp), &cp);
        vkCmdDispatch(cmd, n, 1, 1);
    }
    gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    CmdPush mp{v.counters.address, v.meshCmds.address, meshInfoBuffer_.address, v.impCmds.address,
               impInfoBuffer_.address, barkCmdCount_ + leafCmdCount_, impCmdCount_, {v.caps[0], v.caps[1], v.caps[2]}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cmdPipe_);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(mp), &mp);
    vkCmdDispatch(cmd, (mp.meshCount + mp.impCount + 63) / 64, 1, 1);
    gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                       VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Vegetation::draw(VkCommandBuffer cmd, VkDeviceAddress frameData) {
    drawView(cmd, frameData, views_[0], barkPipe_, leafPipe_, impPipe_);
}

void Vegetation::drawShadow(VkCommandBuffer cmd, VkDeviceAddress frameData, uint32_t view) {
    drawView(cmd, frameData, views_[view], barkShadowPipe_, leafShadowPipe_, impShadowPipe_);
}

void Vegetation::drawView(VkCommandBuffer cmd, VkDeviceAddress frameData, const View& v, VkPipeline bark,
                          VkPipeline leaf, VkPipeline imp) {
    if (variants_.empty() || frameCellCount_ == 0) return;
    VkPipelineLayout layout = bindless_->pipelineLayout();
    VkDescriptorSet set = bindless_->set();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, indexBuffer_.buffer, 0, VK_INDEX_TYPE_UINT32);
    DrawPush p{frameData, vertexBuffer_.address, v.visible.address, cellBuffers_[cullFrameIndex_].address,
               meshInfoBuffer_.address, variantBuffer_.address, 0};
    constexpr uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bark);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    vkCmdDrawIndexedIndirect(cmd, v.meshCmds.buffer, 0, barkCmdCount_, stride);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, leaf);
    p.infoBase = barkCmdCount_;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    vkCmdDrawIndexedIndirect(cmd, v.meshCmds.buffer, VkDeviceSize(barkCmdCount_) * stride, leafCmdCount_, stride);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, imp);
    p.info = impInfoBuffer_.address;
    p.infoBase = 0;
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    vkCmdDrawIndirect(cmd, v.impCmds.buffer, 0, impCmdCount_, sizeof(VkDrawIndirectCommand));
}

void Vegetation::nearbyTrunks(const glm::dvec3& pos, double radius, std::vector<Trunk>& out) {
    out.clear();
    for (auto& [k, c] : cells_) {
        if (c->state.load() != 2) continue;
        if (glm::length(c->origin - pos) > 250.0 + radius) continue;
        for (auto& inst : c->instances) {
            const TreeVariant& v = variants_[inst.variant];
            if (v.trunkRadius <= 0) continue;
            glm::dvec3 p = c->origin + glm::dvec3(inst.pos);
            if (glm::length(p - pos) < radius) out.push_back({p, v.trunkRadius * inst.scale});
        }
    }
}

}  // namespace world
