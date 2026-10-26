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
    float orbit = 180, distance = 3.5f;
private:
    nlohmann::json original_;
    bool open_ = false, dirty_ = false, showClothes_ = false;
    int tab_ = 0;
    char filter_[96]{};
    std::string error_;
};
}
