#pragma once
#include "render/Human.h"

namespace ui {
class CharacterCreator {
public:
    enum class Action { None, Apply, Cancel };
    void open(render::Human& human);
    bool isOpen() const { return open_; }
    Action draw(render::Human& human, render::Character& character, const std::filesystem::path& preset);
    Action cancel(render::Human& human);
    // Preview camera around the character: degrees around it (180 = front), pitch in degrees (negative looks down),
    // distance in metres and a manual height offset of the focus point. `view` eases towards the input's target.
    struct View { float orbit = 180, pitch = -5, distance = 2.6f, lift = 0; };
    View view;
    float previewLeft = 0;  // where the free preview area starts, as a fraction of the window width
    void setView(const View& v) { view = target_ = v; }
    void limitPitch(float maxPitch);  // keeps the camera above the ground (see main)
    void showClothes(bool on) { showClothes_ = on; dirty_ = true; }  // the preview with the outfit (checks)
private:
    View target_;
    bool dragging_ = false, panning_ = false;
    void updateCamera();
    nlohmann::json original_;
    bool open_ = false, dirty_ = false, showClothes_ = false;
    int tab_ = 0;
    char filter_[96]{};
    std::string error_;
};
}
