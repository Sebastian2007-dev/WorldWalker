#pragma once
#include "render/Character.h"
#include <map>
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
    };
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
    // Skin assets ("skin") were fitted to the coarse body; rebinds them to the smoothed one, so that their edge lies on
    // the rendered skin.
    void bindSkinAssets();
    std::vector<Target> targets_;
    std::vector<Target> realism_;  // sculpted anatomy per sex (tools/blender/bake_realism.py), optional
    std::vector<Asset> assets_;
    std::map<std::string, glm::vec3> meanColors_;  // mean linear colour of opaque texels, by texture path
    glm::vec3 meanColor(const std::string& texture);
};
}
