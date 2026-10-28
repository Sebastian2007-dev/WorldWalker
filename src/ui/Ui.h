#pragma once
// Game UI: theme and fonts, in-game HUD and the pause menu (Dear ImGui).
#include <cstdint>
#include <filesystem>
#include <string>

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
    int quality = -1;      // Quality preset; -1 until the first start picked one for this GPU
    bool vsync = false;
    int fpsLimit = 0;      // frames per second, 0 = unlimited
    std::string language = "de";  // interface language: "de" or "en"
    bool wardrobeDeclined = false;  // the player chose "Später" on the start screen's wardrobe offer
    std::string gpu;       // adapter name, shown next to the presets (not saved)
    // Wardrobe (data/Wardrobe): shown under Options > Display (not saved). The menu sets wardrobeRequest.
    bool wardrobeAvailable = false, wardrobeInstalled = false, wardrobeRequest = false;
    double wardrobeMegabytes = 0;
};

// Graphics presets. Custom means the player changed individual settings after choosing one.
enum Quality { kQualityLow, kQualityMedium, kQualityHigh, kQualityUltra, kQualityCustom };
void applyQuality(int quality, render::SceneParams& p);
// Starting preset for a GPU: integrated or little video memory -> Low, mid-range -> Medium, else High.
int detectQuality(bool integratedGpu, uint64_t deviceLocalBytes);

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

// Full screen wardrobe set-up: the offer (with the download size), then progress, then an error with a retry.
struct WardrobeScreen {
    enum class Stage { Offer, Downloading, Unpacking, Failed } stage = Stage::Offer;
    double megabytes = 0;
    float progress = 0;   // 0..1 within the stage
    std::string error;
};
enum class WardrobeChoice { None, Install, Later };
WardrobeChoice drawWardrobeScreen(const WardrobeScreen& screen);

// Full screen while the world around the player loads: progress 0..1, time drives the spinner.
void drawLoadingScreen(float progress, float time);

// A save slot as the menu lists it.
struct SaveSlot {
    bool used = false;
    std::string when, where;  // "28.10.2026 14:05", "47.87° N 8.00° O"
};

class PauseMenu {
public:
    enum class Action { None, Resume, Quit, Teleport, CharacterCreator, NewGame, Continue, Save, Load, MainMenu };

    bool isOpen() const { return open_; }
    float fade() const { return fade_; }
    void open(double lat, double lon);
    // The main menu: before the game starts (and after "Hauptmenü"); Esc does not close it.
    void openTitle(bool canContinue);
    bool isTitle() const { return title_; }
    void close() { open_ = false; }
    void back();  // Esc: leave a sub page, or close from the main page

    // Draws the menu while it is open or fading out. Returns what the player picked this frame.
    Action draw(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player);

    double teleportLat = 0, teleportLon = 0;
    int slot = 0;                                  // Save / Load: the chosen slot (0 = autosave, Load only)
    SaveSlot slots[6];                             // index 0: autosave; set by the caller before draw

private:
    enum class Page { Main, Options, Teleport, Save, Load };
    Action drawMain(const game::Player& player);
    Action drawSlots(bool saving);
    void drawOptions(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player);
    Action drawTeleport();

    bool open_ = false, title_ = false, canContinue_ = false;
    Page page_ = Page::Main;
    int tab_ = 0;
    float fade_ = 0;
    char latBuf_[32] = {}, lonBuf_[32] = {};
};

}  // namespace ui
