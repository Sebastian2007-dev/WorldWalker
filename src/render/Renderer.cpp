#include "render/Renderer.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "geo/Geodesy.h"
#include "render/Atmosphere.h"

#include <stb_image.h>
#include <stb_image_write.h>

namespace render {
namespace {

struct FullscreenPush {
    VkDeviceAddress frame;
    uint32_t hdrIndex;
    float exposure;
    float menuFade;
    VkDeviceAddress autoExposure;  // eye adaptation state written by exposure.comp
};
struct ExposurePush {
    VkDeviceAddress state;
    uint32_t hdr;
    float dt;
    uint32_t enabled;
};

struct TaaPush {
    glm::mat4 reproject;
    VkDeviceAddress frame;
    uint32_t current, history, depth;
    float blend;
    uint32_t ao;
};
struct SsaoPush {
    VkDeviceAddress frame;
    uint32_t depth, frameIndex;
    float strength;
};
static_assert(sizeof(TaaPush) <= 128);

float halton(uint32_t i, uint32_t base) {
    float f = 1.0f, r = 0.0f;
    for (; i > 0; i /= base) {
        f /= float(base);
        r += f * float(i % base);
    }
    return r;
}

struct CharacterPush {
    glm::mat4 model;  // camera relative
    VkDeviceAddress frame;
    VkDeviceAddress vertices;
    glm::vec4 color;
};
static_assert(sizeof(CharacterPush) <= 128);

glm::mat4 reversedInfinitePerspective(float fovY, float aspect, float zNear) {
    float f = 1.0f / std::tan(fovY * 0.5f);
    glm::mat4 m(0.0f);
    m[0][0] = f / aspect;
    m[1][1] = -f;  // Vulkan clip space has +Y down
    m[2][3] = -1.0f;
    m[3][2] = zNear;
    return m;
}

void extractFrustum(const glm::mat4& m, glm::vec4 out[6]) {
    glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    out[0] = r3 + r0;
    out[1] = r3 - r0;
    out[2] = r3 + r1;
    out[3] = r3 - r1;
    out[4] = r3 - r2;  // near (reversed Z: depth <= 1)
    out[5] = glm::vec4(0, 0, 0, 1);  // infinite far plane: always inside
    for (int i = 0; i < 5; ++i) {
        float l = glm::length(glm::vec3(out[i]));
        if (l > 0) out[i] /= l;
    }
}

}  // namespace

void Renderer::init(gfx::Device& dev, gfx::Bindless& bindless, GLFWwindow* window, const std::filesystem::path& assetDir) {
    dev_ = &dev;
    bindless_ = &bindless;

    // Ground material (baked by tools/blender/bake_ground.py).
    auto loadTex = [&](const std::filesystem::path& p, bool srgb) -> uint32_t {
        int w = 1, h = 1, n;
        stbi_uc* px = stbi_load(p.string().c_str(), &w, &h, &n, 4);
        const uint32_t fallback =srgb ? 0xFF406040u : 0xFFE08080u;
        gfx::Image img = dev.createTexture2D(px ? (const void*)px : (const void*)&fallback, px ? uint32_t(w) : 1,
                                             px ? uint32_t(h) : 1,
                                             srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, px != nullptr);
        if (px) stbi_image_free(px);
        else std::fprintf(stderr, "missing ground texture %s\n", p.string().c_str());
        groundTextures_.push_back(img);
        return bindless.add(img, bindless.linearRepeat());
    };
    groundAlbedo_ = loadTex(assetDir / "baked" / "ground" / "grass_ground_albedo.png", true);
    groundNormal_ = loadTex(assetDir / "baked" / "ground" / "grass_ground_nrm.png", false);
    createTargets();
    createShadowAtlas(kShadowTile);
    {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physical(), &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(dev.physical(), &qn, qs.data());
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev.physical(), &props);
        if (qs[dev.queueFamily()].timestampValidBits > 0 && props.limits.timestampPeriod > 0.0f) {
            timestampMs_ = props.limits.timestampPeriod * 1e-6f;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = kStamps * gfx::kFramesInFlight;
            VK_CHECK(vkCreateQueryPool(dev.device(), &qi, nullptr, &queryPool_));
        }
    }
    for (auto& b : frameBuffers_)
        b = dev.createBuffer(sizeof(FrameData) * (1 + kCascades), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_CPU_TO_GPU, true);
    skyLut_ = dev.createImage({kSkyLutW, kSkyLutH}, VK_FORMAT_R16G16B16A16_SFLOAT,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    skyLutIndex_ = bindless.add(skyLut_, bindless.linearClamp());

    VkPipelineLayout layout = bindless.pipelineLayout();
    gfx::GraphicsPipelineDesc d;
    d.colorFormats = {hdrFormat()};
    d.depthFormat = depthFormat();
    d.vs = "terrain.vert";
    d.fs = "terrain.frag";
    d.cull = VK_CULL_MODE_NONE;
    terrainPipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.polygonMode = VK_POLYGON_MODE_LINE;
    terrainWire_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.polygonMode = VK_POLYGON_MODE_FILL;

    d.vs = "water.vert";
    d.fs = "water.frag";
    d.alphaBlend = true;
    waterPipe_ = gfx::createGraphicsPipeline(dev, layout, d);
    d.alphaBlend = false;

    d.vs = "character.vert";
    d.fs = "character.frag";
    d.cull = VK_CULL_MODE_BACK_BIT;
    characterPipe_ = gfx::createGraphicsPipeline(dev, layout, d);

    gfx::GraphicsPipelineDesc sd;
    sd.depthFormat = depthFormat();
    sd.vs = "terrain.vert";
    sd.cull = VK_CULL_MODE_NONE;
    terrainShadowPipe_ = gfx::createGraphicsPipeline(dev, layout, sd);

    gfx::GraphicsPipelineDesc ld;
    ld.vs = "fullscreen.vert";
    ld.fs = "sky_lut.frag";
    ld.cull = VK_CULL_MODE_NONE;
    ld.depthTest = ld.depthWrite = false;
    ld.colorFormats = {VK_FORMAT_R16G16B16A16_SFLOAT};
    skyLutPipe_ = gfx::createGraphicsPipeline(dev, layout, ld);
    exposurePipe_ = gfx::createComputePipeline(dev, layout, "exposure.comp");
    exposureBuf_ = dev.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
    const float initialExposure[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    dev.uploadBuffer(exposureBuf_, 0, initialExposure, sizeof(initialExposure));
    ld.fs = "taa.frag";
    ld.colorFormats = {hdrFormat()};
    taaPipe_ = gfx::createGraphicsPipeline(dev, layout, ld);
    ld.fs = "ssao.frag";
    ld.colorFormats = {VK_FORMAT_R8_UNORM};
    ssaoPipe_ = gfx::createGraphicsPipeline(dev, layout, ld);

    d.vs = "fullscreen.vert";
    d.fs = "sky.frag";
    d.cull = VK_CULL_MODE_NONE;
    d.depthWrite = false;
    skyPipe_ = gfx::createGraphicsPipeline(dev, layout, d);

    gfx::GraphicsPipelineDesc t;
    t.vs = "fullscreen.vert";
    t.fs = "tonemap.frag";
    t.cull = VK_CULL_MODE_NONE;
    t.depthTest = t.depthWrite = false;
    t.colorFormats = {dev.swapFormat()};
    tonemapPipe_ = gfx::createGraphicsPipeline(dev, layout, t);

    // Unit sphere for the third-person mannequin.
    std::vector<TreeVertex> sv;
    std::vector<uint32_t> si;
    const int seg = 24, rings = 16;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= seg; ++s) {
            float th = float(r) / rings * 3.14159265f, ph = float(s) / seg * 6.2831853f;
            glm::vec3 n(std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th));
            sv.push_back({n, n, glm::vec2(float(s) / seg, float(r) / rings)});
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s) {
            uint32_t a = r * (seg + 1) + s, b = a + seg + 1;
            si.insert(si.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    sphereIndexCount_ = uint32_t(si.size());
    sphereVerts_ = dev.createBuffer(sv.size() * sizeof(TreeVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VMA_MEMORY_USAGE_GPU_ONLY);
    dev.uploadBuffer(sphereVerts_, 0, sv.data(), sphereVerts_.size);
    sphereIdx_ = dev.createBuffer(si.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VMA_MEMORY_USAGE_GPU_ONLY, false, false);
    dev.uploadBuffer(sphereIdx_, 0, si.data(), sphereIdx_.size);

    // Dear ImGui
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = 64;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(dev.device(), &pci, nullptr, &imguiPool_));
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = dev.instance();
    ii.PhysicalDevice = dev.physical();
    ii.Device = dev.device();
    ii.QueueFamily = dev.queueFamily();
    ii.Queue = dev.queue();
    ii.DescriptorPool = imguiPool_;
    ii.MinImageCount = 2;
    ii.ImageCount = dev.swapImageCount();
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.UseDynamicRendering = true;
    static VkFormat swapFmt = dev.swapFormat();
    ii.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFmt;
    ImGui_ImplVulkan_Init(&ii);
}

void Renderer::stamp(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t i) {
    if (queryPool_) vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool_, frameIndex * kStamps + i);
}

// The frame slot's fence has been waited for, so the stamps it wrote last time are complete.
void Renderer::readTimestamps(uint32_t frameIndex) {
    if (!queryPool_ || !stampsWritten_[frameIndex]) return;
    uint64_t t[kStamps];
    if (vkGetQueryPoolResults(dev_->device(), queryPool_, frameIndex * kStamps, kStamps, sizeof(t), t, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    auto ms = [&](int a, int b) { return t[b] >= t[a] ? float(t[b] - t[a]) * timestampMs_ : 0.0f; };
    const float k = 0.1f;  // smoothing
    auto blend = [&](float& v, float x) { v += (x - v) * k; };
    blend(gpuTimes_.shadows, ms(0, 1));
    blend(gpuTimes_.scene, ms(1, 2));
    blend(gpuTimes_.ssao, ms(2, 3));
    blend(gpuTimes_.taa, ms(3, 4));
    blend(gpuTimes_.post, ms(4, 5));
    blend(gpuTimes_.total, ms(0, 5));
}

void Renderer::createShadowAtlas(uint32_t tile) {
    if (shadow_.image) {
        dev_->destroyLater(shadow_);
        bindless_->release(shadowIndex_);
    }
    shadowTile_ = tile;
    shadow_ = dev_->createImage({tile * 2, tile * 2}, depthFormat(),
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                                VK_IMAGE_ASPECT_DEPTH_BIT);
    shadowIndex_ = bindless_->add(shadow_, bindless_->linearClamp());
}

// Lowers the 3D resolution in 5 % steps while frames take longer than the target and raises it again once there is
// clear headroom. Steps are rare (hysteresis) because every change recreates the render targets and restarts TAA.
void Renderer::updateDynamicResolution(const SceneParams& params, float dt) {
    if (!params.dynamicResolution || dt <= 0.0f) {
        dynamicScale_ = 1.0f;
        return;
    }
    frameMs_ = frameMs_ > 0.0f ? frameMs_ + (dt * 1000.0f - frameMs_) * 0.1f : dt * 1000.0f;
    sinceScaleChange_ += dt;
    float target = 1000.0f / std::max(params.targetFps, 10.0f);
    if (frameMs_ > target * 1.08f && sinceScaleChange_ > 0.5f && dynamicScale_ > 0.5f) {
        dynamicScale_ = std::max(0.5f, dynamicScale_ - 0.05f);
        sinceScaleChange_ = 0.0f;
    } else if (frameMs_ < target * 0.8f && sinceScaleChange_ > 2.0f && dynamicScale_ < 1.0f) {
        dynamicScale_ = std::min(1.0f, dynamicScale_ + 0.05f);
        sinceScaleChange_ = 0.0f;
    }
}

void Renderer::createTargets() {
    VkExtent2D full = dev_->swapExtent();
    targetExtent_ = {std::max(64u, uint32_t(float(full.width) * scale_ + 0.5f)),
                     std::max(64u, uint32_t(float(full.height) * scale_ + 0.5f))};
    hdr_ = dev_->createImage(targetExtent_, hdrFormat(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    depth_ = dev_->createImage(targetExtent_, depthFormat(),
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1,
                               VK_IMAGE_ASPECT_DEPTH_BIT);
    hdrIndex_ = bindless_->add(hdr_, bindless_->linearClamp());
    depthIndex_ = bindless_->add(depth_, bindless_->linearClamp());
    for (int i = 0; i < 2; ++i) {
        history_[i] = dev_->createImage(targetExtent_, hdrFormat(), VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        historyIndex_[i] = bindless_->add(history_[i], bindless_->linearClamp());
    }
    ao_ = dev_->createImage(targetExtent_, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    aoIndex_ = bindless_->add(ao_, bindless_->linearClamp());
    historyValid_ = false;
}

void Renderer::destroyTargets() {
    for (uint32_t index : {hdrIndex_, depthIndex_, historyIndex_[0], historyIndex_[1], aoIndex_}) bindless_->release(index);
    dev_->destroyLater(hdr_);
    dev_->destroyLater(depth_);
    for (auto& h : history_) dev_->destroyLater(h);
    dev_->destroyLater(ao_);
}

void Renderer::shutdown() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyDescriptorPool(dev_->device(), imguiPool_, nullptr);
    for (VkPipeline p : {terrainPipe_, terrainWire_, waterPipe_, terrainShadowPipe_, skyLutPipe_, taaPipe_, ssaoPipe_, exposurePipe_, skyPipe_, tonemapPipe_,
                         characterPipe_})
        vkDestroyPipeline(dev_->device(), p, nullptr);
    for (auto& b : frameBuffers_) dev_->destroyNow(b);
    if (queryPool_) vkDestroyQueryPool(dev_->device(), queryPool_, nullptr);
    dev_->destroyNow(sphereVerts_);
    dev_->destroyNow(sphereIdx_);
    dev_->destroyNow(captureBuffer_);
    for (auto& t : groundTextures_) dev_->destroyNow(t);
    dev_->destroyNow(hdr_);
    dev_->destroyNow(depth_);
    dev_->destroyNow(shadow_);
    dev_->destroyNow(skyLut_);
    for (auto& h : history_) dev_->destroyNow(h);
    dev_->destroyNow(ao_);
    dev_->destroyNow(exposureBuf_);
}

void Renderer::drawCharacter(VkCommandBuffer cmd, VkDeviceAddress frame, const game::Player& player) {
    VkPipelineLayout layout = bindless_->pipelineLayout();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, characterPipe_);
    vkCmdBindIndexBuffer(cmd, sphereIdx_.buffer, 0, VK_INDEX_TYPE_UINT32);

    // Body frame: x = right, y = forward, z = up (ECEF axes, camera relative origin).
    glm::dvec3 fwd = player.north() * double(std::cos(player.bodyHeading())) +
                     player.east() * double(std::sin(player.bodyHeading()));
    glm::dvec3 right = glm::cross(fwd, player.up());
    glm::mat4 basis(1.0f);
    basis[0] = glm::vec4(glm::vec3(right), 0);
    basis[1] = glm::vec4(glm::vec3(fwd), 0);
    basis[2] = glm::vec4(glm::vec3(player.up()), 0);
    basis[3] = glm::vec4(glm::vec3(player.position() - player.cameraPos()), 1);

    float ph = player.walkPhase();
    float swing = player.speed() > 0.1f ? std::sin(ph) * std::min(0.6f, player.speed() * 0.12f) : 0.0f;
    struct Part { glm::vec3 pos; glm::vec3 scale; float pitch; glm::vec3 pivot; glm::vec4 color; };
    const glm::vec4 shirt(0.15f, 0.25f, 0.45f, 1), pants(0.2f, 0.18f, 0.15f, 1), skin(0.85f, 0.65f, 0.5f, 1);
    Part parts[] = {
        {{0, 0, 1.22f}, {0.20f, 0.13f, 0.32f}, 0, {0, 0, 0}, shirt},                   // torso
        {{0, 0, 1.66f}, {0.11f, 0.12f, 0.13f}, 0, {0, 0, 0}, skin},                    // head
        {{-0.1f, 0, 0.45f}, {0.075f, 0.08f, 0.44f}, swing, {-0.1f, 0, 0.9f}, pants},     // left leg
        {{0.1f, 0, 0.45f}, {0.075f, 0.08f, 0.44f}, -swing, {0.1f, 0, 0.9f}, pants},      // right leg
        {{-0.27f, 0, 1.18f}, {0.055f, 0.06f, 0.33f}, -swing * 0.8f, {-0.27f, 0, 1.48f}, shirt},  // left arm
        {{0.27f, 0, 1.18f}, {0.055f, 0.06f, 0.33f}, swing * 0.8f, {0.27f, 0, 1.48f}, shirt},     // right arm
    };
    for (auto& p : parts) {
        glm::mat4 m = basis;
        m = glm::translate(m, p.pivot);
        m = glm::rotate(m, p.pitch, glm::vec3(1, 0, 0));
        m = glm::translate(m, p.pos - p.pivot);
        m = glm::scale(m, p.scale);
        CharacterPush pc{m, frame, sphereVerts_.address, p.color};
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, sphereIndexCount_, 1, 0, 0, 0);
    }
}

void Renderer::render(gfx::FrameContext& fc, const game::Player& player, world::Terrain& terrain,
                      world::Vegetation& veg, world::Grass& grass, Character& character, const SceneParams& params,
                      float time) {
    float dt = std::clamp(time - lastTime_, 0.0f, 0.25f);
    updateDynamicResolution(params, dt);
    float scale = std::clamp(std::round(params.renderScale * dynamicScale_ * 20.0f) / 20.0f, 0.25f, 1.0f);
    VkExtent2D full = dev_->swapExtent();
    if (scale != scale_ || std::max(64u, uint32_t(float(full.width) * scale + 0.5f)) != targetExtent_.width ||
        std::max(64u, uint32_t(float(full.height) * scale + 0.5f)) != targetExtent_.height) {
        scale_ = scale;
        destroyTargets();
        createTargets();
    }
    const uint32_t wantTile = params.shadowQuality <= 0 ? kShadowTile / 2 : kShadowTile;
    if (wantTile != shadowTile_) createShadowAtlas(wantTile);
    VkCommandBuffer cmd = fc.cmd;
    VkExtent2D ext = targetExtent_;
    readTimestamps(fc.frameIndex);
    if (queryPool_) vkCmdResetQueryPool(cmd, queryPool_, fc.frameIndex * kStamps, kStamps);
    stamp(cmd, fc.frameIndex, 0);

    // ---- frame constants
    glm::dvec3 cam = player.cameraPos();
    double clat, clon, calt;
    geo::fromEcef(cam, clat, clon, calt);
    geo::Enu enu = geo::enuFrame(clat, clon);
    glm::vec3 up = glm::vec3(enu.up);
    glm::vec3 fwd = player.cameraForward();
    glm::vec3 viewUp = std::abs(glm::dot(fwd, up)) > 0.999f ? glm::vec3(enu.north) : up;
    glm::mat4 view = glm::lookAt(glm::vec3(0), fwd, viewUp);
    float aspect = float(ext.width) / float(ext.height);
    float zNear = calt - player.groundHeight() > 500.0 ? 2.0f : 0.05f;
    glm::mat4 proj = reversedInfinitePerspective(glm::radians(params.fovDeg), aspect, zNear);

    FrameData fd{};
    fd.view = view;
    // Sub-pixel jitter for TAA (Halton 2,3): every frame samples a slightly different spot inside each pixel.
    glm::mat4 projJ = proj;
    if (params.taa) {
        uint32_t i = 1 + frameCounter_++ % 8;
        projJ[2][0] -= (halton(i, 2) - 0.5f) * 2.0f / float(ext.width);
        projJ[2][1] -= (halton(i, 3) - 0.5f) * 2.0f / float(ext.height);
    }
    fd.viewProj = projJ * view;
    fd.invViewProj = glm::inverse(fd.viewProj);
    extractFrustum(fd.viewProj, fd.frustum);
    fd.camUp = up;
    fd.time = time;
    // Sun position from local solar time and declination (day of year).
    float hourAngle = (params.timeOfDay - 12.0f) / 24.0f * 6.2831853f;
    float latR = float(clat * geo::kDeg);
    float decl = 23.44f * float(geo::kDeg) * std::sin(6.2831853f * (284.0f + params.dayOfYear) / 365.0f);
    float sinEl = std::sin(latR) * std::sin(decl) + std::cos(latR) * std::cos(decl) * std::cos(hourAngle);
    float el = std::asin(std::clamp(sinEl, -1.0f, 1.0f));
    float az = std::atan2(-std::sin(hourAngle),
                          std::tan(decl) * std::cos(latR) - std::sin(latR) * std::cos(hourAngle));
    glm::vec3 sun = glm::vec3(enu.east) * std::sin(az) * std::cos(el) + glm::vec3(enu.north) * std::cos(az) * std::cos(el) +
                    up * std::sin(el);
    fd.sunDir = glm::normalize(sun);
    float sunUp = glm::dot(fd.sunDir, up);
    // Sunlight and ambient light from the atmosphere model at the camera (km, local frame with z up, sun in xz).
    glm::vec3 sunLocal(std::sqrt(std::max(0.0f, 1.0f - sunUp * sunUp)), 0.0f, sunUp);
    glm::vec3 ro(0.0f, 0.0f, atmo::kRg + float(std::max(calt, 1.0)) * 1e-3f);
    glm::vec3 sunT = atmo::sunTransmittance(ro, sunLocal, 24);
    fd.sunColor = sunT;
    fd.sunIntensity = std::max({sunT.x, sunT.y, sunT.z}) > 1e-4f ? atmo::kSunTOA : 0.0f;
    // Cosine weighted mean sky radiance over the upper hemisphere.
    glm::vec3 skyAmb(0.0f);
    float wsum = 0.0f;
    for (float elDeg : {10.0f, 30.0f, 50.0f, 70.0f})
        for (int a = 0; a < 6; ++a) {
            float el = glm::radians(elDeg), az = float(a) * atmo::kPi / 3.0f;
            glm::vec3 d(std::cos(az) * std::cos(el), std::sin(az) * std::cos(el), std::sin(el));
            float w = std::sin(el) * std::cos(el);
            skyAmb += atmo::skyRadiance(ro, d, sunLocal, 16) * w;
            wsum += w;
        }
    const glm::vec3 night(0.006f, 0.008f, 0.016f);
    fd.skyAmbient = skyAmb / wsum + night;
    // Bounce light from the ground (albedo ~0.15) lit by sun and sky.
    fd.groundAmbient = 0.15f * (sunT * fd.sunIntensity * std::max(sunUp, 0.0f) + fd.skyAmbient) + night * 0.5f;
    fd.cloudCoverage = params.cloudCoverage;
    fd.camMod64k = glm::vec3(glm::mod(cam, glm::dvec3(65536.0)));
    fd.studio = params.studio;
    fd.skyLut = skyLutIndex_;
    fd.fogDensity = params.fogDensity;
    fd.camOriginMod = glm::vec3(glm::mod(cam, glm::dvec3(4096.0)));
    fd.camAltitude = float(calt);
    fd.camForward = fwd;
    fd.exposure = params.exposure;
    fd.viewport = glm::vec2(ext.width, ext.height);
    fd.lodScale = params.treeDetail;
    fd.impostorDistance = params.impostorDistance;
    fd.groundAlbedo = groundAlbedo_;
    fd.groundNormal = groundNormal_;
    fd.shadowMap = shadowIndex_;

    // ---- sun shadow cascades
    // Each cascade is the bounding sphere of a slice of the view frustum (stable under camera rotation), seen from the
    // sun with an orthographic projection. Centres snap to whole texels in an Earth-fixed light space so shadows do
    // not shimmer while walking. Depth is reversed: 1 = towards the sun.
    const float splits[kCascades + 1] = {zNear, 10.0f, 35.0f, 100.0f, 300.0f};
    // Far cascades use a quarter of their tile: dense forest seen from the sun is mostly alpha-tested overdraw.
    const uint32_t shadowRes[kCascades] = {shadowTile_, shadowTile_, shadowTile_ / 2, shadowTile_ / 2};
    // Lower qualities skip the last cascade: shadows end at 100 m.
    const uint32_t cascades = params.shadowQuality >= 2 ? kCascades : kCascades - 1;
    const float casterReach = 2000.0f;  // how far towards the sun casters (mountains) are still captured
    // Trees only throw shadows as long as their height allows at this sun elevation.
    float sunTan = glm::dot(fd.sunDir, up) / std::max(std::sqrt(1.0f - glm::dot(fd.sunDir, up) * glm::dot(fd.sunDir, up)), 1e-3f);
    const float treeReach = std::min(casterReach, 45.0f / std::max(sunTan, 0.03f));
    glm::vec4 terrainPlanes[kCascades][6];
    bool shadowsOn = params.shadows && fd.sunIntensity > 0.0f;
    FrameData shadowFd[kCascades];
    glm::vec3 L = fd.sunDir;
    glm::vec3 lx = glm::normalize(glm::cross(glm::vec3(0, 0, 1), L));  // Earth axis: sun never lines up with it
    glm::vec3 ly = glm::cross(L, lx);
    float tanY = std::tan(glm::radians(params.fovDeg) * 0.5f), tanX = tanY * aspect;
    float t2 = tanX * tanX + tanY * tanY;
    for (uint32_t c = 0; c < kCascades; ++c) {
        float n = splits[c], f = splits[c + 1];
        float d = std::min(f, 0.5f * (n + f) * (1.0f + t2));
        float r = std::ceil(std::max(std::sqrt((f - d) * (f - d) + f * f * t2), std::sqrt((d - n) * (d - n) + n * n * t2)));
        float texel = 2.0f * r / float(shadowRes[c]);
        glm::vec3 center = fwd * d;
        for (glm::vec3 axis : {lx, ly}) {
            double w = glm::dot(cam, glm::dvec3(axis)) + double(glm::dot(center, axis));
            center += axis * float(std::floor(w / texel) * texel - w);
        }
        float range = 2.0f * r + casterReach;
        glm::mat4 m(0.0f);
        auto setRow = [&](int row, glm::vec3 a, float w) {
            m[0][row] = a.x, m[1][row] = a.y, m[2][row] = a.z, m[3][row] = w;
        };
        setRow(0, lx / r, -glm::dot(center, lx) / r);
        setRow(1, ly / r, -glm::dot(center, ly) / r);
        setRow(2, L / range, (r - glm::dot(center, L)) / range);
        setRow(3, glm::vec3(0), 1.0f);
        // Clip space [-1,1] -> this cascade's tile in the 2x2 atlas.
        glm::mat4 tile(1.0f);
        float scale = 0.5f * float(shadowRes[c]) / float(shadowTile_ * 2);
        tile[0][0] = tile[1][1] = scale;
        tile[3] = glm::vec4(scale + 0.5f * float(c & 1), scale + 0.5f * float(c >> 1), 0.0f, 1.0f);
        fd.shadowMat[c] = tile * m;
        fd.cascadeFar[c] = f;
        fd.cascadeTexel[c] = texel;

        FrameData& s = shadowFd[c];
        s = fd;
        s.viewProj = m;
        s.shadowPass = 1.0f;
        s.frustum[0] = glm::vec4(lx, r - glm::dot(center, lx));
        s.frustum[1] = glm::vec4(-lx, r + glm::dot(center, lx));
        s.frustum[2] = glm::vec4(ly, r - glm::dot(center, ly));
        s.frustum[3] = glm::vec4(-ly, r + glm::dot(center, ly));
        s.frustum[4] = glm::vec4(L, r - glm::dot(center, L));
        s.frustum[5] = glm::vec4(-L, r + treeReach + glm::dot(center, L));
        std::copy(s.frustum, s.frustum + 6, terrainPlanes[c]);
        terrainPlanes[c][5] = glm::vec4(-L, r + casterReach + glm::dot(center, L));
    }
    if (!shadowsOn) fd.cascadeFar = glm::vec4(-1.0f);  // every visible point is past the last cascade: fully lit
    else if (cascades < kCascades) fd.cascadeFar.w = fd.cascadeFar.z;  // past the last drawn cascade: lit

    auto* mapped = static_cast<FrameData*>(frameBuffers_[fc.frameIndex].mapped);
    mapped[0] = fd;
    for (uint32_t c = 0; c < kCascades; ++c) mapped[1 + c] = shadowFd[c];
    VkDeviceAddress frame = frameBuffers_[fc.frameIndex].address;
    auto shadowFrame = [&](uint32_t c) { return frame + VkDeviceSize(1 + c) * sizeof(FrameData); };

    // ---- compute: vegetation culling
    // Player character at the feet: model space x right, y forward, z up; camera relative.
    glm::dvec3 bodyFwd = player.north() * double(std::cos(player.bodyHeading())) +
                         player.east() * double(std::sin(player.bodyHeading()));
    glm::mat4 charModel(1.0f);
    charModel[0] = glm::vec4(glm::vec3(glm::cross(bodyFwd, player.up())), 0.0f);
    charModel[1] = glm::vec4(glm::vec3(bodyFwd), 0.0f);
    charModel[2] = glm::vec4(glm::vec3(player.up()), 0.0f);
    charModel[3] = glm::vec4(glm::vec3(player.position() - cam), 1.0f);
    character.prepare(fc.frameIndex);

    veg.beginFrame(fc.frameIndex, cam);
    veg.cull(cmd, frame, 0);
    const uint32_t shadowMinLod[kCascades] = {1, 2, 2, 2};
    if (shadowsOn)
        for (uint32_t c = 0; c < cascades; ++c) veg.cull(cmd, shadowFrame(c), 1 + c, shadowMinLod[c]);

    // ---- shadow atlas
    VkPipelineLayout layout = bindless_->pipelineLayout();
    VkDescriptorSet set = bindless_->set();

    // ---- sky-view LUT for this frame's camera altitude and sun
    gfx::imageBarrier(cmd, skyLut_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo la{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        la.imageView = skyLut_.view;
        la.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        la.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        la.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo lri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        lri.renderArea = {{0, 0}, {kSkyLutW, kSkyLutH}};
        lri.layerCount = 1;
        lri.colorAttachmentCount = 1;
        lri.pColorAttachments = &la;
        vkCmdBeginRendering(cmd, &lri);
        VkViewport lvp{0, 0, float(kSkyLutW), float(kSkyLutH), 0, 1};
        VkRect2D lsc{{0, 0}, {kSkyLutW, kSkyLutH}};
        vkCmdSetViewport(cmd, 0, 1, &lvp);
        vkCmdSetScissor(cmd, 0, 1, &lsc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLutPipe_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(frame), &frame);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    }
    gfx::imageBarrier(cmd, skyLut_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gfx::imageBarrier(cmd, shadow_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
    {
        VkRenderingAttachmentInfo sa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        sa.imageView = shadow_.view;
        sa.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sa.clearValue.depthStencil = {0.0f, 0};
        VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        sri.renderArea = {{0, 0}, {shadowTile_ * 2, shadowTile_ * 2}};
        sri.layerCount = 1;
        sri.pDepthAttachment = &sa;
        vkCmdBeginRendering(cmd, &sri);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        for (uint32_t c = 0; shadowsOn && c < cascades; ++c) {
            int32_t x = int32_t((c & 1) * shadowTile_), y = int32_t((c >> 1) * shadowTile_);
            VkViewport svp{float(x), float(y), float(shadowRes[c]), float(shadowRes[c]), 0, 1};
            VkRect2D ssc{{x, y}, {shadowRes[c], shadowRes[c]}};
            vkCmdSetViewport(cmd, 0, 1, &svp);
            vkCmdSetScissor(cmd, 0, 1, &ssc);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrainShadowPipe_);
            terrain.draw(cmd, layout, shadowFrame(c), cam, terrainPlanes[c]);
            veg.drawShadow(cmd, shadowFrame(c), 1 + c);
            if (c < 3) character.draw(cmd, shadowFrame(c), charModel, true);  // also in first person: own shadow
        }
        vkCmdEndRendering(cmd);
    }
    gfx::imageBarrier(cmd, shadow_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);

    stamp(cmd, fc.frameIndex, 1);

    // ---- main HDR pass
    gfx::imageBarrier(cmd, hdr_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    gfx::imageBarrier(cmd, depth_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = hdr_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = depth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // read by the TAA resolve
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ext};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp{0, 0, float(ext.width), float(ext.height), 0, 1};
    VkRect2D sc{{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, params.wireframe ? terrainWire_ : terrainPipe_);
    terrain.draw(cmd, layout, frame, cam, fd.frustum);
    veg.draw(cmd, frame);
    if (params.grass) grass.draw(cmd, frame, cam, params.grassDensity);
    if (player.view == game::Player::View::ThirdPerson) {
        if (character.loaded()) character.draw(cmd, frame, charModel, false);
        else drawCharacter(cmd, frame, player);  // placeholder mannequin when no character is baked
    }
    // Lakes last among the opaque scene: they blend over the lake bed and the terrain hides them above the shore.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipe_);
    terrain.drawWater(cmd, layout, frame, cam, fd.frustum);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipe_);
    FullscreenPush fp{frame, hdrIndex_, params.exposure, params.menuFade, exposureBuf_.address};
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(fp), &fp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    gfx::imageBarrier(cmd, hdr_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gfx::imageBarrier(cmd, depth_.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT);

    stamp(cmd, fc.frameIndex, 2);

    // ---- screen space ambient occlusion (applied and denoised by the TAA resolve)
    if (params.ssao) {
        gfx::imageBarrier(cmd, ao_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo aa{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        aa.imageView = ao_.view;
        aa.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        aa.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        aa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ari{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ari.renderArea = {{0, 0}, ext};
        ari.layerCount = 1;
        ari.colorAttachmentCount = 1;
        ari.pColorAttachments = &aa;
        vkCmdBeginRendering(cmd, &ari);
        SsaoPush ap{frame, depthIndex_, frameCounter_, 0.85f};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ssaoPipe_);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(ap), &ap);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        gfx::imageBarrier(cmd, ao_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    stamp(cmd, fc.frameIndex, 3);

    // ---- TAA resolve into this frame's history image
    {
        uint32_t cur = historyCur_, prev = historyCur_ ^ 1u;
        gfx::imageBarrier(cmd, history_[cur].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ta{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ta.imageView = history_[cur].view;
        ta.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ta.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ta.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo tri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        tri.renderArea = {{0, 0}, ext};
        tri.layerCount = 1;
        tri.colorAttachmentCount = 1;
        tri.pColorAttachments = &ta;
        vkCmdBeginRendering(cmd, &tri);
        // Teleports and long jumps start a fresh history instead of smearing the old view.
        bool continuous = historyValid_ && glm::length(cam - prevCam_) < 50.0;
        TaaPush tp{prevViewProj_ * glm::translate(glm::mat4(1.0f), glm::vec3(cam - prevCam_)), frame, hdrIndex_,
                   historyIndex_[prev], depthIndex_, params.taa && continuous ? 0.1f : 1.0f,
                   params.ssao ? aoIndex_ : ~0u};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, taaPipe_);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(tp), &tp);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        gfx::imageBarrier(cmd, history_[cur].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        fp.hdrIndex = historyIndex_[cur];
        prevViewProj_ = proj * view;
        prevCam_ = cam;
        historyValid_ = true;
        historyCur_ = prev;
    }

    stamp(cmd, fc.frameIndex, 4);

    // ---- eye adaptation: measure the resolved image, smoothly move the exposure the tonemap uses
    {
        lastTime_ = time;
        gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        ExposurePush ep{exposureBuf_.address, fp.hdrIndex, dt, params.autoExposure ? 1u : 0u};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, exposurePipe_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(ep), &ep);
        vkCmdDispatch(cmd, 1, 1, 1);
        gfx::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    // ---- tonemap + UI into the swapchain image
    gfx::imageBarrier(cmd, fc.swapImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo sw{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    sw.imageView = fc.swapView;
    sw.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sw.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    sw.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri2{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri2.renderArea = {{0, 0}, dev_->swapExtent()};
    ri2.layerCount = 1;
    ri2.colorAttachmentCount = 1;
    ri2.pColorAttachments = &sw;
    vkCmdBeginRendering(cmd, &ri2);
    // The 3D passes ran at the (possibly scaled) render resolution; the tonemap upscales to the whole window.
    VkViewport fullVp{0, 0, float(dev_->swapExtent().width), float(dev_->swapExtent().height), 0, 1};
    VkRect2D fullSc{{0, 0}, dev_->swapExtent()};
    vkCmdSetViewport(cmd, 0, 1, &fullVp);
    vkCmdSetScissor(cmd, 0, 1, &fullSc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipe_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL, 0, sizeof(fp), &fp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
    stamp(cmd, fc.frameIndex, 5);
    stampsWritten_[fc.frameIndex] = queryPool_ != VK_NULL_HANDLE;
    if (captureRequested_) {
        captureRequested_ = false;
        captureExtent_ = dev_->swapExtent();
        dev_->destroyNow(captureBuffer_);
        captureBuffer_ = dev_->createBuffer(VkDeviceSize(captureExtent_.width) * captureExtent_.height * 4,
                                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU, true, false);
        gfx::imageBarrier(cmd, fc.swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {captureExtent_.width, captureExtent_.height, 1};
        vkCmdCopyImageToBuffer(cmd, fc.swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuffer_.buffer, 1, &c);
        gfx::imageBarrier(cmd, fc.swapImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        return;
    }
    gfx::imageBarrier(cmd, fc.swapImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_NONE, 0);
}

bool Renderer::saveCapture(const char* path) {
    if (!captureBuffer_) return false;
    uint32_t w = captureExtent_.width, h = captureExtent_.height;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    std::memcpy(px.data(), captureBuffer_.mapped, px.size());
    if (dev_->swapFormat() == VK_FORMAT_B8G8R8A8_SRGB || dev_->swapFormat() == VK_FORMAT_B8G8R8A8_UNORM)
        for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    bool ok = stbi_write_png(path, int(w), int(h), 4, px.data(), int(w * 4)) != 0;
    dev_->destroyNow(captureBuffer_);
    return ok;
}

}  // namespace render
