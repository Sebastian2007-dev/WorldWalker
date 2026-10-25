#pragma once
#include <glm/glm.hpp>

#include <filesystem>
#include <vector>

#include "game/Player.h"
#include "gfx/Pipeline.h"
#include "render/GpuTypes.h"
#include "render/Character.h"
#include "world/Grass.h"
#include "world/Terrain.h"
#include "world/Vegetation.h"

struct GLFWwindow;

namespace render {

struct SceneParams {
    float timeOfDay = 10.5f;    // local solar hours
    float dayOfYear = 172.0f;   // 1..365, drives solar declination
    float exposure = 1.0f;
    float fogDensity = 1.0f / 18000.0f;
    float impostorDistance = 2000.0f;
    float fovDeg = 70.0f;
    bool wireframe = false;
    bool shadows = true;
    bool grass = true;
    bool taa = true;
    bool ssao = true;
    bool autoExposure = true;  // eye adaptation; `exposure` stays as a manual correction on top
    float cloudCoverage = 0.45f;  // 0 = clear sky, 1 = overcast
    float menuFade = 0.0f;      // 0..1, blurs and dims the scene behind the pause menu
};

class Renderer {
public:
    void init(gfx::Device& dev, gfx::Bindless& bindless, GLFWwindow* window, const std::filesystem::path& assetDir);
    void shutdown();

    void render(gfx::FrameContext& fc, const game::Player& player, world::Terrain& terrain,
                world::Vegetation& veg, world::Grass& grass, Character& character, const SceneParams& params,
                float time);

    // Copies the next rendered frame to host memory; call saveCapture() after the GPU finished it.
    void requestCapture() { captureRequested_ = true; }
    bool saveCapture(const char* path);

    static VkFormat hdrFormat() { return VK_FORMAT_R16G16B16A16_SFLOAT; }
    static VkFormat depthFormat() { return VK_FORMAT_D32_SFLOAT; }
    static constexpr uint32_t kCascades = 4;
    static constexpr uint32_t kShadowTile = 2048;  // per cascade; the atlas is 2x2 tiles
    static constexpr uint32_t kSkyLutW = 256, kSkyLutH = 128;

private:
    void createTargets();
    void destroyTargets();
    void drawCharacter(VkCommandBuffer cmd, VkDeviceAddress frame, const game::Player& player);

    gfx::Device* dev_ = nullptr;
    gfx::Bindless* bindless_ = nullptr;
    gfx::Image hdr_, depth_, shadow_, skyLut_;
    uint32_t hdrIndex_ = 0, depthIndex_ = 0, shadowIndex_ = 0, skyLutIndex_ = 0;
    // Temporal anti-aliasing: ping-pong history and last frame's unjittered camera.
    gfx::Image history_[2], ao_;
    uint32_t aoIndex_ = 0;
    uint32_t historyIndex_[2] = {}, historyCur_ = 0, frameCounter_ = 0;
    bool historyValid_ = false;
    glm::mat4 prevViewProj_{1.0f};
    glm::dvec3 prevCam_{0.0};
    gfx::Buffer exposureBuf_;
    float lastTime_ = 0.0f;
    uint32_t groundAlbedo_ = 0, groundNormal_ = 0;
    std::vector<gfx::Image> groundTextures_;
    VkExtent2D targetExtent_{};

    gfx::Buffer frameBuffers_[gfx::kFramesInFlight];  // FrameData for the camera, then one per shadow cascade
    gfx::Buffer sphereVerts_, sphereIdx_;
    uint32_t sphereIndexCount_ = 0;

    VkPipeline terrainPipe_ = VK_NULL_HANDLE, terrainWire_ = VK_NULL_HANDLE, terrainShadowPipe_ = VK_NULL_HANDLE;
    VkPipeline skyLutPipe_ = VK_NULL_HANDLE, taaPipe_ = VK_NULL_HANDLE, ssaoPipe_ = VK_NULL_HANDLE, exposurePipe_ = VK_NULL_HANDLE, skyPipe_ = VK_NULL_HANDLE, tonemapPipe_ = VK_NULL_HANDLE, characterPipe_ = VK_NULL_HANDLE;
    VkDescriptorPool imguiPool_ = VK_NULL_HANDLE;
    bool captureRequested_ = false;
    gfx::Buffer captureBuffer_;
    VkExtent2D captureExtent_{};
};

}  // namespace render
