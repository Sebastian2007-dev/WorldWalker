#pragma once
#include "render/Character.h"
#include <map>
#include <memory>
#include <nlohmann/json.hpp>

namespace render {
// CPU morphing and MakeHuman proxy fitting. GPU buffers change only after an edit.
class Human {
public:
    bool load(const std::filesystem::path& directory);
    bool rebuild(Character& character, bool hideClothes = false);
    bool loadPreset(const std::filesystem::path& file);
    bool savePreset(const std::filesystem::path& file) const;
    void reset();
    enum class Template { Male, Female };
    void applyTemplate(Template body);
    // Plausible random adult: body, face details, skin, eyes, hair (style and colour) and an outfit.
    void randomize(uint32_t seed);
    // Hair colour presets (sRGB). The first kNaturalHairShades are natural colours.
    struct HairShade { const char* name; float rgb[3]; };
    static const std::vector<HairShade>& hairShades();
    static constexpr size_t kNaturalHairShades = 11;
    // Clothing slot of a catalog asset ("top", "bottom", "outfit", "underwear", "shoes", "hat", ...; see
    // tools/human_assets.py), empty for other kinds. An outfit covers top and bottom.
    static std::string clothingSlot(const nlohmann::json& asset);
    bool ready() const { return !base_.empty(); }
    const nlohmann::json& catalog() const { return meta_; }
    nlohmann::json state;
private:
    struct RenderVertex { uint32_t base; glm::vec2 uv; };
    struct Target { std::vector<uint32_t> indices; std::vector<glm::vec3> delta; };
    struct ProxyVertex { uint32_t refs[3]; float weights[3]; glm::vec3 offset; glm::vec2 uv; };
    struct Asset {
        std::vector<ProxyVertex> vertices;
        std::vector<uint32_t> indices, deleted;
        uint32_t scaleA[3]{}, scaleB[3]{};
        float reference[3]{1, 1, 1};
        bool scaled = false;
        std::map<std::string, Target> morphs;  // optional (genitals): offset deltas per asset vertex, by name
        // Optional strand rig (hair, "rig" in human.json): bones bound like proxy vertices, per vertex hair bone weights.
        struct RigPoint { uint32_t refs[3]; float weights[3]; glm::vec3 offset; };
        struct RigBone { int32_t parent; RigPoint head, tail; };
        struct RigWeights { uint8_t bones[4], weights[4]; };  // weights of 255; the rest stays with the body's joints
        std::vector<RigBone> rig;
        std::vector<RigWeights> rigWeights;
        bool bound = false;  // rebound to the smoothed body (bindSkinAsset), done when first shown
    };
    // Humans whose sexes have different meshes (tools/blender/bake_mblab.py: <dir>/female, <dir>/male) keep the
    // other sex's data here; activate swaps it in.
    std::unique_ptr<Human> other_;
    bool loadDirectory(const std::filesystem::path& directory);
    void activate(bool female);
    void swapData(Human& other);
    // Selects the template's asset for every body part (eyes, eyelashes, ...) the selection has none of in this
    // catalog, and the template skin for an invalid skin index: presets made for another mesh keep working.
    void completeSelection();
    // MB-Lab's morph model ("modifiers" in human.json): properties from the detail sliders and body macros, mixed
    // into targets by MB-Lab's smart combo.
    void applyModifiers(std::vector<glm::vec3>& positions, const std::map<std::string,float>& factors);
    std::filesystem::path directory_;
    nlohmann::json meta_;
    std::vector<glm::vec3> base_;
    std::vector<RenderVertex> render_;             // subdivided body (see subdivide)
    std::vector<uint32_t> triangles_, joints_, weights_;
    // One Catmull-Clark step of the body, as weights over the base vertices: base vertices keep their index, then
    // one vertex per edge and one per face. Face points list exactly the face's corners.
    std::vector<uint32_t> stencilStart_;
    std::vector<std::pair<uint32_t, float>> stencil_;
    uint32_t facePoints_ = 0;                      // index of the first face point
    std::vector<uint32_t> neighborStart_, neighbors_;  // subdivided vertex adjacency, for skin creases
    void subdivide();
    // Skin assets ("skin") and transferred ones ("smooth") were fitted to the coarse body; rebinds one to the smoothed
    // body, so that it keeps its height over the rendered skin. Done when an asset is first shown: hundreds of them
    // at load took minutes.
    void bindSkinAsset(size_t asset);
    std::vector<glm::vec3> smoothRest_;               // rest pose of the smoothed body (bindSkinAsset)
    std::vector<std::vector<uint32_t>> around_;       // smoothed body triangles around each vertex (bindSkinAsset)
    std::vector<Target> targets_;
    std::vector<Target> realism_;  // sculpted anatomy per sex (tools/blender/bake_realism.py), optional
    std::vector<Asset> assets_;
    std::map<std::string, glm::vec3> meanColors_;  // mean linear colour of opaque texels, by texture path
    glm::vec3 meanColor(const std::string& texture);
};
}
