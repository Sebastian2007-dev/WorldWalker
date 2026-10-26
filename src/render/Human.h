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
    };
    std::filesystem::path directory_;
    nlohmann::json meta_;
    std::vector<glm::vec3> base_;
    std::vector<RenderVertex> render_;
    std::vector<uint32_t> triangles_, joints_, weights_;
    std::vector<Target> targets_;
    std::vector<Asset> assets_;
    std::map<std::string, glm::vec3> meanColors_;  // mean linear colour of opaque texels, by texture path
    glm::vec3 meanColor(const std::string& texture);
};
}
