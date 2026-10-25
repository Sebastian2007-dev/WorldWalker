#pragma once
// Game UI: theme and fonts, in-game HUD and the pause menu (Dear ImGui).
#include <cstdint>
#include <filesystem>

struct GLFWwindow;
namespace game { class Player; }
namespace render { struct SceneParams; }

namespace ui {

struct Settings {
    bool showHud = true;
    bool showDebug = false;
    bool crosshair = true;
    bool invertY = false;
    bool fullscreen = false;
};

// Settings, scene parameters and player tuning persisted as JSON.
void loadSettings(const std::filesystem::path& file, Settings& s, render::SceneParams& p, game::Player& player);
void saveSettings(const std::filesystem::path& file, const Settings& s, const render::SceneParams& p,
                  const game::Player& player);

// Loads fonts and installs the theme. Call once after ImGui is initialised, before the first frame.
void setupStyle(GLFWwindow* window);
void setFullscreen(GLFWwindow* window, bool on);

const char* landCoverName(uint8_t cls);

struct HudInfo {
    uint8_t landCover = 0;
    float time = 0;      // seconds since start, drives hints and spinners
    float alpha = 1;     // fades out behind the pause menu
};
void drawHud(const game::Player& player, const Settings& s, const HudInfo& info);

class PauseMenu {
public:
    enum class Action { None, Resume, Quit, Teleport, CharacterCreator };

    bool isOpen() const { return open_; }
    float fade() const { return fade_; }
    void open(double lat, double lon);
    void close() { open_ = false; }
    void back();  // Esc: leave a sub page, or close from the main page

    // Draws the menu while it is open or fading out. Returns what the player picked this frame.
    Action draw(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player);

    double teleportLat = 0, teleportLon = 0;

private:
    enum class Page { Main, Options, Teleport };
    Action drawMain(const game::Player& player);
    void drawOptions(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player);
    Action drawTeleport();

    bool open_ = false;
    Page page_ = Page::Main;
    int tab_ = 0;
    float fade_ = 0;
    char latBuf_[32] = {}, lonBuf_[32] = {};
};

}  // namespace ui
