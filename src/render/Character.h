#pragma once
// Skinned player character. The mesh and skeleton come either from a fixed baked character (.wwc, see
// tools/blender/bake_character.py) or from the morphable human (render/Human). Animation clips (.wwa) are stored in
// model space and retargeted at load time to whatever skeleton is set, so the same Mixamo clips drive any body.
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "gfx/Pipeline.h"

namespace render {

struct AnimInput {
    glm::vec2 velocity{0};   // ground velocity in the body frame: x right, y forward (m/s)
    bool grounded = true;
    bool flying = false;
    bool jumped = false;     // a jump started this frame
    float verticalSpeed = 0;
    float turnRate = 0;      // body yaw rate while standing (rad/s, positive = right)
    bool kneeling = false;
};

struct SkinVertex {  // mirrors SkinVertex in shaders/skin.glsl
    glm::vec3 pos, normal;
    glm::vec2 uv;
    uint32_t joints, weights;  // 4 x u8 each
};

struct SkeletonBone {
    std::string name;  // bare Mixamo name (no "mixamorig:" prefix)
    int parent = -1;   // parents come before children
    glm::quat rot{1, 0, 0, 0};  // bind pose, model space (z up, +y forward, metres)
    glm::vec3 pos{0};
};

struct MeshPart {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t albedo = 0, normal = 0;  // bindless texture indices
    bool alphaTest = false;
};

class Character {
public:
    static constexpr uint32_t kMaxBones = 128;

    // Pipelines and animation clips (from <animDir>/character.json).
    bool init(gfx::Device& dev, gfx::Bindless& bindless, const std::filesystem::path& animDir, VkFormat colorFormat,
              VkFormat depthFormat);
    void shutdown();
    // Fixed character baked from an FBX (mesh + skeleton + materials).
    bool loadBakedMesh(const std::filesystem::path& dir);

    void setSkeleton(const std::vector<SkeletonBone>& bones);
    void setMesh(const std::vector<SkinVertex>& vertices, const std::vector<uint32_t>& indices,
                 const std::vector<MeshPart>& parts);
    uint32_t loadTexture(const std::filesystem::path& file, bool srgb);  // cached by path

    bool loaded() const { return !bones_.empty() && vertices_; }
    const std::vector<SkeletonBone>& skeleton() const { return bones_; }

    // Advances and blends the animation; call once per frame.
    void update(float dt, const AnimInput& in);
    // Uploads this frame's joint palette. Call before any draw of the frame.
    void prepare(uint32_t frameIndex);
    // model: model space (z up, +y forward) -> camera relative world.
    void draw(VkCommandBuffer cmd, VkDeviceAddress frameData, const glm::mat4& model, bool shadow);

private:
    struct Clip {
        std::string name;
        uint32_t frames = 0;
        float fps = 30, duration = 1;
        glm::vec2 rootMotion{0};    // horizontal hip travel over the clip (source metres), removed at playback
        float rootYaw = 0;          // hip rotation about z over the clip (turn clips), removed at playback
        float speed = 0;            // root motion speed (source m/s), drives the playback rate
        std::vector<std::string> boneNames;
        std::vector<int> boneParent;
        std::vector<glm::vec3> restPos;    // source rest pose, model space
        std::vector<glm::quat> restRot;
        std::vector<glm::vec3> hips;       // per frame
        std::vector<glm::quat> delta;      // per frame per clip bone: pose * rest^-1 (model space)
        std::vector<int> map;              // skeleton bone -> clip bone (-1: not animated)
    };
    struct Layer {                  // one clip's contribution to the blended pose
        int clip = -1;
        float time = 0, weight = 0, target = 0;
        float fadeRate = 10;        // cross-fade speed (1/s)
        bool loop = true;
    };

    bool loadClips(const std::filesystem::path& dir);
    void retarget();
    int findClip(const char* name) const;
    void setTarget(int clip, float w);
    void samplePose();

    gfx::Device* dev_ = nullptr;
    gfx::Bindless* bindless_ = nullptr;
    std::vector<SkeletonBone> bones_;
    std::vector<glm::mat4> inverseBind_;
    std::vector<glm::quat> align_;  // per bone: rotates the bind pose onto the clips' source rest pose
    float hipScale_ = 1.0f;         // target hip height / source hip height
    std::vector<Clip> clips_;
    std::vector<Layer> layers_;     // one per clip
    std::vector<MeshPart> parts_;
    std::vector<gfx::Image> textures_;
    std::unordered_map<std::string, uint32_t> textureCache_;
    std::vector<glm::mat4> palette_;
    gfx::Buffer vertices_, indices_;
    std::vector<gfx::Buffer> retiredMeshes_; // enqueue deletion after beginFrame has submitted pending uploads
    gfx::Buffer joints_[gfx::kFramesInFlight];
    uint32_t frameIndex_ = 0;
    VkPipeline pipe_ = VK_NULL_HANDLE, hairPipe_ = VK_NULL_HANDLE, shadowPipe_ = VK_NULL_HANDLE;

    // Locomotion state.
    float locoPhase_ = 0;           // shared normalised cycle phase of all locomotion clips
    float airTime_ = 0;
    float landTimer_ = 0;
    int idle_ = -1, walk_ = -1, run_ = -1, back_ = -1, strafeL_ = -1, strafeR_ = -1, strafeRunL_ = -1,
        strafeRunR_ = -1, jump_ = -1, fall_ = -1, land_ = -1, turnL_ = -1, turnR_ = -1, kneel_ = -1, backRun_ = -1;
};

}  // namespace render
