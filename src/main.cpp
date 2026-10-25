// WorldWalker: walk the whole Earth at 1:1 scale.
//   WorldWalker.exe [--lat <deg>] [--lon <deg>] [--validation] [--noshadows] [--nograss] [--notaa] [--nossao] [--clouds <0..1>]
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/Jobs.h"
#include "game/Player.h"
#include "geo/Geodesy.h"
#include "gfx/Device.h"
#include "gfx/Pipeline.h"
#include "render/Renderer.h"
#include "ui/Ui.h"
#include "ui/CharacterCreator.h"
#include "world/Grass.h"
#include "world/Terrain.h"
#include "world/Vegetation.h"
#include "world/WorldData.h"
#include "data/Http.h"
#include "data/CogFile.h"

namespace fs = std::filesystem;

namespace {

struct App {
    GLFWwindow* window = nullptr;
    bool mouseCaptured = true;
    float scroll = 0;
    bool toggleView = false, toggleFly = false, toggleDebug = false, kneel = false, escape = false, focusLost = false;
    double lastX = 0, lastY = 0;
    bool firstMouse = true;
};

void captureMouse(App& app, bool on) {
    app.mouseCaptured = on;
    glfwSetInputMode(app.window, GLFW_CURSOR, on ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (on && glfwRawMouseMotionSupported()) glfwSetInputMode(app.window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    app.firstMouse = true;
}

}  // namespace

int main(int argc, char** argv) {
    // Default start: Feldberg, Black Forest (mixed conifer forest on a 1493 m summit).
    double startLat = 47.8740, startLon = 8.0040;
    bool validation = false;
    const char* shotPath = nullptr;   // --shot: render until the world is loaded, save PNG, exit
    float yaw = 0, pitch = 0, shotTime = 10.5f;
    bool creatorRequested = false;
    const char* creatorTemplate = nullptr;
    bool thirdPerson = false, timeGiven = false, noShadows = false, noGrass = false, noTaa = false, noSsao = false;
    float clouds = -1.0f;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--lat") && i + 1 < argc) startLat = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!std::strcmp(argv[i], "--yaw") && i + 1 < argc) yaw = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--pitch") && i + 1 < argc) pitch = float(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--time") && i + 1 < argc) shotTime = float(std::atof(argv[++i])), timeGiven = true;
        else if (!std::strcmp(argv[i], "--tp")) thirdPerson = true;
        else if (!std::strcmp(argv[i], "--creator")) creatorRequested = true;
        else if (!std::strcmp(argv[i], "--creator-template") && i + 1 < argc) {
            creatorTemplate=argv[++i]; creatorRequested=true;
        }
        else if (!std::strcmp(argv[i], "--lon") && i + 1 < argc) startLon = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--validation")) validation = true;
        else if (!std::strcmp(argv[i], "--noshadows")) noShadows = true;
        else if (!std::strcmp(argv[i], "--nograss")) noGrass = true;
        else if (!std::strcmp(argv[i], "--notaa")) noTaa = true;
        else if (!std::strcmp(argv[i], "--nossao")) noSsao = true;
        else if (!std::strcmp(argv[i], "--clouds") && i + 1 < argc) clouds = float(std::atof(argv[++i]));
    }

    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--selftest")) {
            // Network / data pipeline check without a window.
            const char* url =
                "https://copernicus-dem-30m.s3.amazonaws.com/Copernicus_DSM_COG_10_N47_00_E008_00_DEM/"
                "Copernicus_DSM_COG_10_N47_00_E008_00_DEM.tif";
            auto r = net::get(url, 0, 1023);
            std::printf("http status %d, %zu bytes\n", r.status, r.body.size());
            data::CogFile cog;
            auto res = cog.open(url, fs::temp_directory_path() / "ww_selftest");
            std::printf("cog open: %d, levels %zu\n", int(res), cog.levels().size());
            world::WorldData wd(fs::path(WW_ASSET_DIR).parent_path() / "cache");
            std::printf("height Feldberg: %.1f m, cover %d\n", wd.groundHeight(startLat, startLon),
                        int(wd.landCover(startLat, startLon)));
            return 0;
        }

    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (shotPath) {
        // Screenshot runs stay invisible and never take focus, so they do not get in the way of other work.
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }
    App app;
    app.window = glfwCreateWindow(1600, 900, "WorldWalker", nullptr, nullptr);
    glfwSetWindowUserPointer(app.window, &app);

    gfx::Device dev;
    dev.init(app.window, validation);
    glfwSetFramebufferSizeCallback(app.window, [](GLFWwindow* w, int, int) {
        (void)w;
    });

    gfx::Bindless bindless;
    bindless.init(dev);

    fs::path assetDir = WW_ASSET_DIR;
    fs::path cacheDir = assetDir.parent_path() / "cache";
    world::WorldData data(cacheDir);
    core::JobSystem jobs(16);
    data.setJobs(&jobs);

    render::Renderer renderer;
    renderer.init(dev, bindless, app.window, assetDir);
    // ImGui installs its own callbacks (chained), ours come after so they are not overwritten.
    glfwSetScrollCallback(app.window, [](GLFWwindow* w, double, double y) {
        ImGui_ImplGlfw_ScrollCallback(w, 0, y);
        static_cast<App*>(glfwGetWindowUserPointer(w))->scroll += float(y);
    });
    glfwSetKeyCallback(app.window, [](GLFWwindow* w, int key, int sc, int action, int mods) {
        ImGui_ImplGlfw_KeyCallback(w, key, sc, action, mods);
        auto* a = static_cast<App*>(glfwGetWindowUserPointer(w));
        if (action != GLFW_PRESS) return;
        if (key == GLFW_KEY_V) a->toggleView = true;
        if (key == GLFW_KEY_F) a->toggleFly = true;
        if (key == GLFW_KEY_F3) a->toggleDebug = true;
        if (key == GLFW_KEY_C) a->kneel = true;
        if (key == GLFW_KEY_ESCAPE) a->escape = true;
    });
    glfwSetWindowFocusCallback(app.window, [](GLFWwindow* w, int focused) {
        ImGui_ImplGlfw_WindowFocusCallback(w, focused);
        if (!focused) static_cast<App*>(glfwGetWindowUserPointer(w))->focusLost = true;
    });
    ui::setupStyle(app.window);

    world::Terrain terrain;
    terrain.init(dev, data, jobs);
    world::Vegetation veg;
    veg.init(dev, bindless, data, jobs, assetDir / "baked" / "trees", render::Renderer::hdrFormat(),
             render::Renderer::depthFormat());
    world::Grass grass;
    grass.init(dev, bindless, data, jobs, render::Renderer::hdrFormat(), render::Renderer::depthFormat());
    render::Character character;
    character.init(dev, bindless, assetDir / "baked" / "character", render::Renderer::hdrFormat(),
                   render::Renderer::depthFormat());
    character.loadBakedMesh(assetDir / "baked" / "character");
    render::Human human;
    human.load(assetDir / "baked" / "human");
    fs::path characterFile = assetDir.parent_path() / "character.json";
    if (human.ready()) {
        if (human.loadPreset(characterFile)) {
            if (human.state.value("enabled",true)) human.rebuild(character);
        } else human.state["enabled"] = false;
    }

    std::printf("Loading start location %.4f, %.4f ...\n", startLat, startLon);
    game::Player player;
    player.spawn(startLat, startLon, data);
    player.setLook(yaw, pitch);
    if (thirdPerson) player.view = game::Player::View::ThirdPerson;
    captureMouse(app, !shotPath);

    render::SceneParams params;
    ui::Settings settings;
    ui::PauseMenu menu;
    ui::CharacterCreator creator;
    game::Player::View previousView = player.view;
    float previousDistance = player.thirdPersonDistance, previousYaw = yaw, previousPitch = pitch;
    auto openCreator = [&] {
        if (!human.ready()) { std::fprintf(stderr,"Character creator data unavailable\n"); return; }
        previousView = player.view; previousDistance = player.thirdPersonDistance;
        previousYaw = player.heading() * 57.29578f;
        previousPitch = player.lookPitch() * 57.29578f;
        creator.open(human); menu.close(); player.view = game::Player::View::ThirdPerson;
        captureMouse(app,false);
    };
    auto finishCreator = [&](ui::CharacterCreator::Action action) {
        if (action == ui::CharacterCreator::Action::None) return;
        if (action == ui::CharacterCreator::Action::Apply) human.rebuild(character);
        if (action == ui::CharacterCreator::Action::Cancel) {
            if (human.state.value("enabled",false)) human.rebuild(character);
            else character.loadBakedMesh(assetDir / "baked" / "character");
        }
        player.view = previousView; player.thirdPersonDistance = previousDistance;
        player.setLook(previousYaw,previousPitch);
        if (!shotPath) menu.open(player.lat(),player.lon());
    };
    if (creatorRequested) openCreator();
    if (creator.isOpen() && creatorTemplate)
        human.applyTemplate(!std::strcmp(creatorTemplate,"female") ? render::Human::Template::Female : render::Human::Template::Male);
    fs::path settingsFile = assetDir.parent_path() / "settings.json";
    if (!shotPath) {
        ui::loadSettings(settingsFile, settings, params, player);
        if (settings.fullscreen) ui::setFullscreen(app.window, true);
    }
    if (shotPath || timeGiven) params.timeOfDay = shotTime;
    if (noShadows) params.shadows = false;
    if (noGrass) params.grass = false;
    if (noTaa) params.taa = false;
    if (noSsao) params.ssao = false;
    if (clouds >= 0.0f) params.cloudCoverage = clouds;
    int idleFrames = 0;
    bool captureSubmitted = false;

    auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    float fpsAccum = 0, fps = 0;
    float tPlayer = 0, tTerrain = 0, tVeg = 0, tRender = 0, tWait = 0;
    int fpsFrames = 0;

    while (!glfwWindowShouldClose(app.window)) {
        glfwPollEvents();
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        float time = std::chrono::duration<float>(now - t0).count();
        fpsAccum += dt;
        if (++fpsFrames >= 30) {
            fps = fpsFrames / fpsAccum;
            fpsAccum = 0;
            fpsFrames = 0;
        }

        if (!shotPath) {
            if (app.escape && !ImGui::GetIO().WantTextInput) {
                if (creator.isOpen()) finishCreator(creator.cancel(human));
                else if (menu.isOpen()) menu.back();
                else menu.open(player.lat(), player.lon());
            }
            if (app.focusLost && !menu.isOpen() && !creator.isOpen()) menu.open(player.lat(), player.lon());
            bool uiOpen = menu.isOpen() || creator.isOpen();
            if (uiOpen == app.mouseCaptured) captureMouse(app, !uiOpen);
        }
        bool paused = menu.isOpen() || creator.isOpen();
        if (creator.isOpen()) {
            player.view = game::Player::View::ThirdPerson;
            player.setLook(player.bodyHeading()*57.29578f + creator.orbit,-5.0f);
            player.thirdPersonDistance = creator.distance;
        }
        if (!paused) {
            if (app.toggleView)
                player.view = player.view == game::Player::View::FirstPerson ? game::Player::View::ThirdPerson
                                                                              : game::Player::View::FirstPerson;
            if (app.toggleFly) player.flying = !player.flying;
        }
        if (app.toggleDebug) settings.showDebug = !settings.showDebug;
        app.escape = app.focusLost = app.toggleView = app.toggleFly = app.toggleDebug = false;

        game::InputState in;
        if (app.mouseCaptured && !shotPath) {
            double x, y;
            glfwGetCursorPos(app.window, &x, &y);
            if (!app.firstMouse)
                in.mouseDelta = glm::vec2(float(x - app.lastX), float(y - app.lastY) * (settings.invertY ? -1.0f : 1.0f));
            app.lastX = x;
            app.lastY = y;
            app.firstMouse = false;
            auto key = [&](int k) { return glfwGetKey(app.window, k) == GLFW_PRESS; };
            in.forward = key(GLFW_KEY_W);
            in.back = key(GLFW_KEY_S);
            in.left = key(GLFW_KEY_A);
            in.right = key(GLFW_KEY_D);
            in.run = key(GLFW_KEY_LEFT_SHIFT);
            in.jump = key(GLFW_KEY_SPACE);
            in.up = key(GLFW_KEY_E);
            in.down = key(GLFW_KEY_Q) || key(GLFW_KEY_LEFT_CONTROL);
            in.scroll = app.scroll;
            in.kneel = app.kneel && !paused;
        }
        app.scroll = 0;
        app.kneel = false;

        auto ms = [](auto a, auto b) { return std::chrono::duration<float, std::milli>(b - a).count(); };
        auto tA = std::chrono::steady_clock::now();
        // Paused: dt = 0 freezes movement but still lets the player settle onto freshly streamed ground.
        player.update(paused ? 0.0f : dt, in, data, veg);
        if (creator.isOpen()) player.previewCamera(creator.distance,1.0f,creator.distance*0.5f);
        render::AnimInput anim;
        anim.velocity = player.localVelocity();
        anim.grounded = player.onGround();
        anim.flying = player.flying;
        anim.jumped = player.jumped();
        anim.verticalSpeed = player.verticalSpeed();
        anim.turnRate = player.bodyTurnRate();
        anim.kneeling = player.kneeling();
        if (const char* tv = std::getenv("WW_TESTVEL")) { float x = 0, y = 0; std::sscanf(tv, "%f,%f", &x, &y); anim.velocity = glm::vec2(x, y); }  // TEMP
        if (creator.isOpen()) anim = render::AnimInput{};
        character.update(creator.isOpen() ? std::min(dt,0.1f) : paused ? 0.0f : std::min(dt, 0.1f), anim);
        auto tB = std::chrono::steady_clock::now();
        terrain.update(player.cameraPos(), player.lat(), player.lon());
        auto tC = std::chrono::steady_clock::now();
        veg.update(player.cameraPos(), player.lat(), player.lon(), params.impostorDistance + 200.0f);
        grass.update(player.cameraPos());
        auto tD = std::chrono::steady_clock::now();
        auto smooth = [](float& v, float x) { v = v * 0.9f + x * 0.1f; };
        smooth(tPlayer, ms(tA, tB));
        smooth(tTerrain, ms(tB, tC));
        smooth(tVeg, ms(tC, tD));

        // ---- UI
        if (app.mouseCaptured) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (!shotPath) {
            ui::HudInfo hud;
            data.tryLandCover(player.lat(), player.lon(), hud.landCover);
            hud.time = time;
            hud.alpha = creator.isOpen() ? 0.0f : 1.0f - menu.fade();
            ui::drawHud(player, settings, hud);
        }
        if (settings.showDebug) {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.6f);
            ImGui::Begin("##debug", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::Text("%.0f FPS (%.2f ms)", fps, fps > 0 ? 1000.0f / fps : 0.0f);
            ImGui::TextDisabled("CPU ms  Spieler %.2f  Terrain %.2f  BÃ¤ume %.2f  Render %.2f  GPU-Wait %.2f", tPlayer,
                                tTerrain, tVeg, tRender, tWait);
            ImGui::Text("HÃ¶he %.1f m (Boden %.1f m)   Tempo %.1f m/s", player.altitude(), player.groundHeight(),
                        player.speed());
            ImGui::Text("Terrain: %zu Chunks (%zu sichtbar), %zu Jobs", terrain.chunkCount(), terrain.drawnCount(),
                        jobs.pending());
            ImGui::Text("BÃ¤ume: %zu in %zu Zellen", veg.instanceCount(), veg.activeCells());
            ImGui::End();
        }
        if (!shotPath) {
            switch (menu.draw(app.window, settings, params, player)) {
                case ui::PauseMenu::Action::Resume:
                    menu.close();
                    ui::saveSettings(settingsFile, settings, params, player);
                    break;
                case ui::PauseMenu::Action::Teleport:
                    player.spawn(menu.teleportLat, menu.teleportLon, data);
                    menu.close();
                    break;
                case ui::PauseMenu::Action::Quit: glfwSetWindowShouldClose(app.window, GLFW_TRUE); break;
                case ui::PauseMenu::Action::CharacterCreator: openCreator(); break;
                case ui::PauseMenu::Action::None: break;
            }
        }
        finishCreator(creator.draw(human,character,characterFile));
        params.menuFade = creator.isOpen() ? 0.0f : menu.fade();
        ImGui::Render();

        if (shotPath) {
            // Wait until streaming has settled for a while, then capture one frame.
            bool busy = jobs.pending() > 0 || terrain.inFlight() > 0;
            idleFrames = busy ? 0 : idleFrames + 1;
            if (!captureSubmitted && idleFrames > 60) renderer.requestCapture();
        }

        gfx::FrameContext fc;
        auto tE = std::chrono::steady_clock::now();
        if (!dev.beginFrame(fc)) continue;
        auto tF = std::chrono::steady_clock::now();
        smooth(tWait, ms(tE, tF));
        bool capturing = shotPath && !captureSubmitted && idleFrames > 60;
        renderer.render(fc, player, terrain, veg, grass, character, params, time);
        dev.endFrame(fc);
        smooth(tRender, ms(tF, std::chrono::steady_clock::now()));
        if (capturing) {
            dev.waitIdle();
            std::printf("screenshot %s: %s (%.0f fps, %.2f ms)\n", shotPath, renderer.saveCapture(shotPath) ? "ok" : "failed",
                        fps, fps > 0 ? 1000.0f / fps : 0.0f);
            break;
        }
    }

    if (!shotPath) ui::saveSettings(settingsFile, settings, params, player);
    data::GeoRaster::beginShutdown();
    glfwHideWindow(app.window);
    dev.waitIdle();
    character.shutdown();
    grass.shutdown();
    veg.shutdown();
    terrain.shutdown();
    renderer.shutdown();
    bindless.shutdown();
    dev.shutdown();
    glfwDestroyWindow(app.window);
    glfwTerminate();
    return 0;
}
