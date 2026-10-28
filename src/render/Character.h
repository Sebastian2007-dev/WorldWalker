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

// Simulated hair strands (tools/blender/bake_hair.py rigs): bones in chains hanging from one body bone, bind pose,
// model space. Their joints follow the skeleton's in the palette.
struct HairBone {
    int parent = -1;  // hair bone, -1: hangs from the anchor bone
    glm::vec3 head{0}, tail{0};
};
// Capsules the strands are kept out of (head, neck, trunk, shoulders), bind pose, model space.
struct HairCollider {
    int bone = 0;
    glm::vec3 a{0}, b{0};
    float radius = 0;
};

struct MeshPart {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t albedo = 0, normal = 0;  // bindless texture indices
    bool alphaTest = false;
    enum class Shading : uint32_t { Cloth, Skin, Hair, Eyes, Underwear };  // mirrors kSkin* in shaders/skin.glsl
    Shading shading = Shading::Cloth;
    glm::vec3 tint{1.0f};  // linear albedo multiplier
    bool recolor = false;  // tint replaces the texture's colour and keeps its luminance (hair colours)
    float normalScale = 1.0f;  // strength of the normal map's relief
};

class Character {
public:
    static constexpr uint32_t kMaxBones = 256;  // joint indices are bytes

    // Pipelines and animation clips (from <animDir>/character.json).
    bool init(gfx::Device& dev, gfx::Bindless& bindless, const std::filesystem::path& animDir, VkFormat colorFormat,
              VkFormat depthFormat);
    void shutdown();
    // Fixed character baked from an FBX (mesh + skeleton + materials).
    bool loadBakedMesh(const std::filesystem::path& dir);

    void setSkeleton(const std::vector<SkeletonBone>& bones);
    // After setSkeleton (which clears it): strands whose joints are skeleton().size() + hair bone index.
    void setHair(int anchor, const std::vector<HairBone>& bones, const std::vector<HairCollider>& colliders);
    void setMesh(const std::vector<SkinVertex>& vertices, const std::vector<uint32_t>& indices,
                 const std::vector<MeshPart>& parts);
    uint32_t loadTexture(const std::filesystem::path& file, bool srgb);  // cached by path

    bool loaded() const { return !bones_.empty() && vertices_; }
    const std::vector<SkeletonBone>& skeleton() const { return bones_; }

    // Advances and blends the animation; call once per frame.
    void update(float dt, const AnimInput& in);
    // Off: the mesh stays in its bind pose (checks of the mesh and skinning without animation, --no-anim).
    void setAnimated(bool on) { animated_ = on; }
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
    // Strand physics on the sampled pose: verlet tails pulled towards the rest shape, follow-the-leader lengths, kept
    // out of the colliders; the frame's own motion (in) is inertia.
    void simulateHair(float dt, const AnimInput& in);

    gfx::Device* dev_ = nullptr;
    gfx::Bindless* bindless_ = nullptr;
    std::vector<SkeletonBone> bones_;
    std::vector<glm::mat4> inverseBind_;
    std::vector<glm::quat> align_;  // per bone: rotates the bind pose onto the clips' source rest pose
    float hipScale_ = 1.0f;         // target hip height / source hip height
    std::vector<Clip> clips_;
    std::vector<Layer> layers_;     // one per clip
    bool animated_ = true;
    std::vector<MeshPart> parts_;
    std::vector<gfx::Image> textures_;
    std::unordered_map<std::string, uint32_t> textureCache_;
    std::vector<glm::mat4> palette_;  // skeleton, then hair bones
    int hairAnchor_ = -1;
    std::vector<HairBone> hair_;
    std::vector<HairCollider> colliders_;
    std::vector<glm::vec3> hairTail_, hairPrev_;  // simulated tails, model space (current and previous step)
    std::vector<float> hairStiffness_;            // pull towards the rest shape per 1/60 s, stiff at the roots
    std::vector<float> hairRigidity_;             // how far the rest shape turns with the head, 1 at the roots
    float hairTime_ = 0;                          // fixed step accumulator
    bool hairReset_ = true;
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
