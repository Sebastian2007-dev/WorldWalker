#include "ui/Ui.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include "ui/Theme.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "game/Player.h"
#include "render/Renderer.h"

namespace ui {
namespace {

struct Fonts {
    ImFont* body = nullptr;   // settings, buttons
    ImFont* small = nullptr;  // HUD, hints
    ImFont* bold = nullptr;   // compass cardinals, key caps
    ImFont* item = nullptr;   // pause menu entries
    ImFont* title = nullptr;  // page titles
} g_fonts;
float g_scale = 1.0f;

constexpr uint32_t kAccent = 0xF2C66D;  // warm sunlight
constexpr uint32_t kText = 0xF4F1EA;
constexpr uint32_t kTextDim = 0xA9A59C;
constexpr uint32_t kInk = 0x0E1114;

// Colours are authored in sRGB. The swapchain is an sRGB format, so everything ImGui writes must be linear,
// otherwise the hardware encodes it a second time and the UI looks washed out.
float toLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
ImVec4 srgb(uint32_t rgb, float a = 1.0f) {
    return ImVec4(float((rgb >> 16) & 255) / 255.0f, float((rgb >> 8) & 255) / 255.0f, float(rgb & 255) / 255.0f, a);
}
ImVec4 linear(ImVec4 c) { return ImVec4(toLinear(c.x), toLinear(c.y), toLinear(c.z), c.w); }
ImU32 col(uint32_t rgb, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(linear(srgb(rgb, a * ImGui::GetStyle().Alpha)));
}
ImU32 mixCol(uint32_t a, float aa, uint32_t b, float ba, float t) {
    ImVec4 x = linear(srgb(a, aa)), y = linear(srgb(b, ba));
    ImVec4 m(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, (x.w + (y.w - x.w) * t) * ImGui::GetStyle().Alpha);
    return ImGui::ColorConvertFloat4ToU32(m);
}

float approach(float v, float target, float rate) {
    float step = ImGui::GetIO().DeltaTime * rate;
    return v < target ? std::min(v + step, target) : std::max(v - step, target);
}
float S(float v) { return v * g_scale; }

void shadowText(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 c, const char* text, float alpha = 1.0f) {
    dl->AddText(f, f->FontSize, p + ImVec2(0, S(2)), col(0, 0.25f * alpha), text);
    dl->AddText(f, f->FontSize, p + ImVec2(S(1), S(1)), col(0, 0.7f * alpha), text);
    dl->AddText(f, f->FontSize, p, c, text);
}
ImVec2 textSize(ImFont* f, const char* text) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text); }

void formatLatLon(char* buf, size_t n, double lat, double lon) {
    std::snprintf(buf, n, "%.5f\xC2\xB0 %s   %.5f\xC2\xB0 %s", std::abs(lat), lat >= 0 ? "N" : "S", std::abs(lon),
                  lon >= 0 ? "O" : "W");
}

// ---------------------------------------------------------------- widgets

// Large left-aligned menu entry with an animated accent bar.
bool menuItem(const char* label, float width, ImFont* font = nullptr) {
    ImFont* f = font ? font : g_fonts.item;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = f->FontSize * 1.55f;
    bool pressed = ImGui::InvisibleButton(label, ImVec2(width, h));
    bool hovered = ImGui::IsItemHovered();
    float* t = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), 0.0f);
    *t = approach(*t, hovered ? 1.0f : 0.0f, 9.0f);
    float k = *t * *t * (3.0f - 2.0f * *t);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (k > 0.0f) {
        dl->AddRectFilledMultiColor(pos, pos + ImVec2(width, h), col(kAccent, 0.16f * k), col(kAccent, 0.0f),
                                    col(kAccent, 0.0f), col(kAccent, 0.16f * k));
        float barH = h * (0.35f + 0.4f * k);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + (h - barH) * 0.5f), ImVec2(pos.x + S(4), pos.y + (h + barH) * 0.5f),
                          col(kAccent, k), S(2));
    }
    ImVec2 ts = textSize(f, label);
    shadowText(dl, f, ImVec2(pos.x + S(22) + S(10) * k, pos.y + (h - ts.y) * 0.5f),
               mixCol(kText, 0.78f, kText, 1.0f, k), label);
    return pressed;
}

bool tabButton(const char* label, bool active) {
    ImFont* f = g_fonts.body;
    ImVec2 ts = textSize(f, label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(ts.x + S(28), ts.y + S(16));
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    float* t = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), active ? 1.0f : 0.0f);
    *t = approach(*t, active ? 1.0f : 0.0f, 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    uint32_t c = active || hovered ? kText : kTextDim;
    dl->AddText(f, f->FontSize, pos + ImVec2(S(14), S(6)), col(c), label);
    float uw = (size.x - S(20)) * *t;
    float cx = pos.x + size.x * 0.5f;
    if (uw > S(2))
        dl->AddRectFilled(ImVec2(cx - uw * 0.5f, pos.y + size.y - S(3)), ImVec2(cx + uw * 0.5f, pos.y + size.y),
                      col(kAccent), S(1.5f));
    return pressed;
}

bool toggle(const char* id, bool* v) {
    float h = ImGui::GetFrameHeight() * 0.9f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos() + ImVec2(0, (ImGui::GetFrameHeight() - h) * 0.5f);
    bool pressed = ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    if (pressed) *v = !*v;
    bool hovered = ImGui::IsItemHovered();
    float* t = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *v ? 1.0f : 0.0f);
    *t = approach(*t, *v ? 1.0f : 0.0f, 10.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), mixCol(0xFFFFFF, hovered ? 0.16f : 0.1f, kAccent, 1.0f, *t), h * 0.5f);
    float r = h * 0.5f - S(3);
    dl->AddCircleFilled(ImVec2(p.x + h * 0.5f + (w - h) * *t, p.y + h * 0.5f), r, mixCol(kText, 1, kInk, 1, *t * 0.85f));
    return pressed;
}

// Settings rows: label on the left, control in the right column.
float g_labelW = 0, g_fieldW = 0;
void rowLabel(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(g_labelW);
    ImGui::SetNextItemWidth(g_fieldW);
}
bool sliderRow(const char* label, const char* id, float* v, float lo, float hi, const char* fmt,
               ImGuiSliderFlags flags = 0) {
    rowLabel(label);
    // Value text sits right of the track instead of on top of the grab; the filled part is tinted.
    float valueW = S(110);
    ImGui::SetNextItemWidth(g_fieldW - valueW);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    bool changed = ImGui::SliderFloat(id, v, lo, hi, "", flags | ImGuiSliderFlags_AlwaysClamp);
    dl->ChannelsSetCurrent(0);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float t = (flags & ImGuiSliderFlags_Logarithmic) ? std::log(*v / lo) / std::log(hi / lo) : (*v - lo) / (hi - lo);
    t = std::clamp(t, 0.0f, 1.0f);
    dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * t, b.y), col(kAccent, 0.28f), ImGui::GetStyle().FrameRounding);
    dl->ChannelsMerge();
    char buf[64];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    ImGui::SameLine(0, S(16));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(buf);
    return changed;
}
bool toggleRow(const char* label, const char* id, bool* v) {
    rowLabel(label);
    return toggle(id, v);
}
// Combo box in the right column; returns true when the selection changed.
bool choiceRow(const char* label, const char* id, const char* const* items, int count, int* v) {
    rowLabel(label);
    bool changed = false;
    if (ImGui::BeginCombo(id, items[std::clamp(*v, 0, count - 1)])) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(items[i], i == *v)) {
                changed = *v != i;
                *v = i;
            }
        ImGui::EndCombo();
    }
    return changed;
}
void sectionHeader(const char* text) {
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::PushFont(g_fonts.small);
    ImGui::PushStyleColor(ImGuiCol_Text, linear(srgb(kAccent)));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, p + ImVec2(g_labelW + g_fieldW, 0), col(0xFFFFFF, 0.1f));
    ImGui::Dummy(ImVec2(0, S(4)));
}

void keyRow(const char* keys, const char* action) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = g_fonts.bold->FontSize + S(8);
    float x = p.x;
    // Keys separated by '|' get one cap each.
    const char* s = keys;
    while (*s) {
        const char* e = s;
        while (*e && *e != '|') ++e;
        ImVec2 ts = g_fonts.bold->CalcTextSizeA(g_fonts.bold->FontSize, FLT_MAX, 0, s, e);
        float w = std::max(ts.x + S(16), h);
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + h), col(0xFFFFFF, 0.1f), S(5));
        dl->AddRect(ImVec2(x, p.y), ImVec2(x + w, p.y + h), col(0xFFFFFF, 0.16f), S(5));
        dl->AddText(g_fonts.bold, g_fonts.bold->FontSize, ImVec2(x + (w - ts.x) * 0.5f, p.y + S(4)), col(kText), s, e);
        x += w + S(6);
        s = *e ? e + 1 : e;
    }
    ImVec2 as = textSize(g_fonts.body, action);
    dl->AddText(g_fonts.body, g_fonts.body->FontSize, ImVec2(p.x + g_labelW, p.y + (h - as.y) * 0.5f), col(kTextDim),
                action);
    ImGui::Dummy(ImVec2(g_labelW + g_fieldW, h + S(4)));
}

}  // namespace

namespace theme {
float scale(float v) { return S(v); }
void columns(float width, float fraction) { g_labelW=width*fraction; g_fieldW=width-g_labelW; }
bool tab(const char* label, bool active) { return tabButton(label,active); }
bool slider(const char* label,const char* id,float* value,float lo,float hi,const char* format) {
    return sliderRow(label,id,value,lo,hi,format);
}
void section(const char* text) { sectionHeader(text); }
bool action(const char* label,float width) { return menuItem(label,width,g_fonts.body); }
void muted(const char* text) {
    ImGui::PushFont(g_fonts.small); ImGui::TextDisabled("%s",text); ImGui::PopFont();
}
float row(const char* label) { rowLabel(label); return g_fieldW; }
bool toggle(const char* label,const char* id,bool* value) { return toggleRow(label,id,value); }
bool swatch(const char* id,const float rgb[3],bool active,float size) {
    ImVec2 pos=ImGui::GetCursorScreenPos();
    bool pressed=ImGui::InvisibleButton(id,ImVec2(size,size));
    bool hovered=ImGui::IsItemHovered();
    auto* dl=ImGui::GetWindowDrawList();
    ImVec2 c=pos+ImVec2(size,size)*0.5f;
    ImVec4 fill=linear(ImVec4(rgb[0],rgb[1],rgb[2],ImGui::GetStyle().Alpha));
    if(active || hovered) dl->AddCircle(c,size*0.5f-S(1),col(active?kAccent:0xFFFFFF,active?1.0f:0.35f),0,S(2));
    dl->AddCircleFilled(c,size*0.5f-S(5),ImGui::ColorConvertFloat4ToU32(fill));
    dl->AddCircle(c,size*0.5f-S(5),col(0xFFFFFF,0.18f));
    return pressed;
}
void backdrop(float edge) {
    ImVec2 vs=ImGui::GetIO().DisplaySize;
    auto* dl=ImGui::GetWindowDrawList();
    float solid=std::max(0.0f,edge-S(90));
    dl->AddRectFilled(ImVec2(0,0),ImVec2(solid,vs.y),col(kInk,0.94f));
    dl->AddRectFilledMultiColor(ImVec2(solid,0),ImVec2(edge+S(160),vs.y),
        col(kInk,0.94f),col(kInk,0),col(kInk,0),col(kInk,0.94f));
}
void heading(ImVec2 position,const char* title,const char* subtitle) {
    auto* dl=ImGui::GetWindowDrawList();
    shadowText(dl,g_fonts.title,position,col(kText),title);
    float y=position.y+g_fonts.title->FontSize+S(4);
    dl->AddText(g_fonts.body,g_fonts.body->FontSize,ImVec2(position.x+S(4),y),col(kTextDim),subtitle);
    y+=g_fonts.body->FontSize+S(14);
    dl->AddRectFilled(ImVec2(position.x+S(4),y),ImVec2(position.x+S(52),y+S(3)),col(kAccent),S(1.5f));
    ImGui::SetCursorScreenPos(ImVec2(position.x,y+S(24)));
}
bool choice(const char* id,const char* title,const char* description,bool active,float width) {
    ImVec2 pos=ImGui::GetCursorScreenPos();
    ImVec2 size(width,S(76));
    bool pressed=ImGui::InvisibleButton(id,size);
    bool hovered=ImGui::IsItemHovered();
    auto* dl=ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos,pos+size,col(active?kAccent:0xFFFFFF,active?0.10f:hovered?0.09f:0.035f),S(6));
    dl->AddRect(pos,pos+size,col(active?kAccent:0xFFFFFF,active?0.65f:0.10f),S(6));
    if(active) dl->AddRectFilled(pos+ImVec2(0,S(15)),pos+ImVec2(S(3),size.y-S(15)),col(kAccent),S(1));
    dl->AddText(g_fonts.body,g_fonts.body->FontSize,pos+ImVec2(S(16),S(12)),col(active?kAccent:kText),title);
    dl->AddText(g_fonts.small,g_fonts.small->FontSize,pos+ImVec2(S(16),S(43)),col(kTextDim),description);
    return pressed;
}
} // namespace theme

// ---------------------------------------------------------------- settings file

void applyQuality(int quality, render::SceneParams& p) {
    struct Preset {
        float scale; bool dynamic; float fps; int shadows; float grass, terrain, trees, treeDistance; bool ssao;
    };
    // Low keeps TAA: it smooths the upscaled image and the dithered hair. Low also adapts its resolution to hold 30 FPS.
    static const Preset kPresets[] = {
        {0.7f, true, 30.0f, 0, 0.3f, 0.7f, 0.6f, 900.0f, false},     // Niedrig
        {0.85f, false, 60.0f, 1, 0.6f, 0.85f, 0.8f, 1500.0f, true},  // Mittel
        {1.0f, false, 60.0f, 2, 1.0f, 1.0f, 1.0f, 2000.0f, true},    // Hoch
        {1.0f, false, 60.0f, 2, 1.0f, 1.3f, 1.3f, 2600.0f, true},    // Ultra
    };
    if (quality < kQualityLow || quality > kQualityUltra) return;
    const Preset& q = kPresets[quality];
    p.renderScale = q.scale;
    p.dynamicResolution = q.dynamic;
    p.targetFps = q.fps;
    p.shadows = true;
    p.shadowQuality = q.shadows;
    p.grass = true;
    p.grassDensity = q.grass;
    p.terrainDetail = q.terrain;
    p.treeDetail = q.trees;
    p.impostorDistance = q.treeDistance;
    p.ssao = q.ssao;
    p.taa = true;
}

int detectQuality(bool integratedGpu, uint64_t deviceLocalBytes) {
    const uint64_t gib = 1ull << 30;
    if (integratedGpu || deviceLocalBytes < 3 * gib) return kQualityLow;
    if (deviceLocalBytes < 6 * gib) return kQualityMedium;
    return kQualityHigh;
}

void loadSettings(const std::filesystem::path& file, Settings& s, render::SceneParams& p, game::Player& player) {
    std::ifstream in(file);
    if (!in) return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    auto get = [&](const char* key, auto& v) {
        if (!j.contains(key)) return;
        try {
            j.at(key).get_to(v);
        } catch (...) {
        }
    };
    get("showHud", s.showHud);
    get("showDebug", s.showDebug);
    get("crosshair", s.crosshair);
    get("invertY", s.invertY);
    get("fullscreen", s.fullscreen);
    get("quality", s.quality);
    get("vsync", s.vsync);
    get("fpsLimit", s.fpsLimit);
    get("renderScale", p.renderScale);
    get("dynamicResolution", p.dynamicResolution);
    get("targetFps", p.targetFps);
    get("shadowQuality", p.shadowQuality);
    get("grassDensity", p.grassDensity);
    get("terrainDetail", p.terrainDetail);
    get("treeDetail", p.treeDetail);
    p.renderScale = std::clamp(p.renderScale, 0.5f, 1.0f);
    p.targetFps = std::clamp(p.targetFps, 30.0f, 144.0f);
    p.shadowQuality = std::clamp(p.shadowQuality, 0, 2);
    p.grassDensity = std::clamp(p.grassDensity, 0.0f, 1.0f);
    p.terrainDetail = std::clamp(p.terrainDetail, 0.5f, 1.5f);
    p.treeDetail = std::clamp(p.treeDetail, 0.5f, 1.5f);
    s.quality = std::clamp(s.quality, -1, int(kQualityCustom));
    get("timeOfDay", p.timeOfDay);
    get("dayOfYear", p.dayOfYear);
    get("exposure", p.exposure);
    get("fogDensity", p.fogDensity);
    get("impostorDistance", p.impostorDistance);
    get("fovDeg", p.fovDeg);
    get("shadows", p.shadows);
    get("grass", p.grass);
    get("taa", p.taa);
    get("ssao", p.ssao);
    get("autoExposure", p.autoExposure);
    get("cloudCoverage", p.cloudCoverage);
    get("mouseSensitivity", player.mouseSensitivity);
    get("flySpeed", player.flySpeed);
}

void saveSettings(const std::filesystem::path& file, const Settings& s, const render::SceneParams& p,
                  const game::Player& player) {
    nlohmann::json j = {
        {"showHud", s.showHud},
        {"showDebug", s.showDebug},
        {"crosshair", s.crosshair},
        {"invertY", s.invertY},
        {"fullscreen", s.fullscreen},
        {"quality", s.quality},
        {"vsync", s.vsync},
        {"fpsLimit", s.fpsLimit},
        {"renderScale", p.renderScale},
        {"dynamicResolution", p.dynamicResolution},
        {"targetFps", p.targetFps},
        {"shadowQuality", p.shadowQuality},
        {"grassDensity", p.grassDensity},
        {"terrainDetail", p.terrainDetail},
        {"treeDetail", p.treeDetail},
        {"timeOfDay", p.timeOfDay},
        {"dayOfYear", p.dayOfYear},
        {"exposure", p.exposure},
        {"fogDensity", p.fogDensity},
        {"impostorDistance", p.impostorDistance},
        {"fovDeg", p.fovDeg},
        {"shadows", p.shadows},
        {"grass", p.grass},
        {"taa", p.taa},
        {"ssao", p.ssao},
        {"autoExposure", p.autoExposure},
        {"cloudCoverage", p.cloudCoverage},
        {"mouseSensitivity", player.mouseSensitivity},
        {"flySpeed", player.flySpeed},
    };
    std::ofstream out(file);
    if (out) out << j.dump(2) << "\n";
}

// ---------------------------------------------------------------- style

void setupStyle(GLFWwindow* window) {
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    g_scale = std::max(sx, 1.0f);

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    std::filesystem::path fontDir = "C:\\Windows\\Fonts";
    char* windir = nullptr;
    size_t len = 0;
    if (_dupenv_s(&windir, &len, "WINDIR") == 0 && windir) fontDir = std::filesystem::path(windir) / "Fonts";
    std::free(windir);
    auto load = [&](const char* file, float size) -> ImFont* {
        std::filesystem::path p = fontDir / file;
        if (!std::filesystem::exists(p)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromFileTTF(p.string().c_str(), size * g_scale, &cfg, io.Fonts->GetGlyphRangesDefault());
    };
    g_fonts.body = load("segoeui.ttf", 19.0f);  // first font is ImGui's default
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    g_fonts.small = load("segoeui.ttf", 15.0f);
    g_fonts.bold = load("segoeuib.ttf", 15.0f);
    g_fonts.item = load("segoeuisl.ttf", 30.0f);
    g_fonts.title = load("segoeuil.ttf", 64.0f);
    for (ImFont** f : {&g_fonts.small, &g_fonts.bold, &g_fonts.item, &g_fonts.title})
        if (!*f) *f = g_fonts.body;

    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 10;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.GrabRounding = 6;
    st.ScrollbarRounding = 8;
    st.WindowBorderSize = 0;
    st.FrameBorderSize = 0;
    st.PopupBorderSize = 0;
    st.WindowPadding = ImVec2(16, 14);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 10);
    st.GrabMinSize = 14;
    st.ScrollbarSize = 10;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = srgb(kText);
    c[ImGuiCol_TextDisabled] = srgb(kTextDim);
    c[ImGuiCol_WindowBg] = srgb(0x14181C, 0.82f);
    c[ImGuiCol_ChildBg] = srgb(0, 0);
    c[ImGuiCol_PopupBg] = srgb(0x14181C, 0.96f);
    c[ImGuiCol_Border] = srgb(0xFFFFFF, 0.08f);
    c[ImGuiCol_FrameBg] = srgb(0xFFFFFF, 0.07f);
    c[ImGuiCol_FrameBgHovered] = srgb(0xFFFFFF, 0.12f);
    c[ImGuiCol_FrameBgActive] = srgb(0xFFFFFF, 0.16f);
    c[ImGuiCol_SliderGrab] = srgb(kAccent);
    c[ImGuiCol_SliderGrabActive] = srgb(0xFFDE96);
    c[ImGuiCol_CheckMark] = srgb(kAccent);
    c[ImGuiCol_Button] = srgb(0xFFFFFF, 0.08f);
    c[ImGuiCol_ButtonHovered] = srgb(0xFFFFFF, 0.14f);
    c[ImGuiCol_ButtonActive] = srgb(kAccent, 0.5f);
    c[ImGuiCol_Header] = srgb(0xFFFFFF, 0.06f);
    c[ImGuiCol_HeaderHovered] = srgb(0xFFFFFF, 0.10f);
    c[ImGuiCol_HeaderActive] = srgb(0xFFFFFF, 0.14f);
    c[ImGuiCol_Separator] = srgb(0xFFFFFF, 0.12f);
    c[ImGuiCol_TextSelectedBg] = srgb(kAccent, 0.35f);
    c[ImGuiCol_ScrollbarBg] = srgb(0, 0);
    c[ImGuiCol_ScrollbarGrab] = srgb(0xFFFFFF, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered] = srgb(0xFFFFFF, 0.25f);
    c[ImGuiCol_ScrollbarGrabActive] = srgb(kAccent, 0.6f);
    c[ImGuiCol_NavHighlight] = srgb(kAccent);
    for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = linear(c[i]);
    st.ScaleAllSizes(g_scale);
}

void setFullscreen(GLFWwindow* window, bool on) {
    static int wx = 100, wy = 100, ww = 1600, wh = 900;
    bool isFull = glfwGetWindowMonitor(window) != nullptr;
    if (on == isFull) return;
    if (on) {
        glfwGetWindowPos(window, &wx, &wy);
        glfwGetWindowSize(window, &ww, &wh);
        GLFWmonitor* m = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(m);
        glfwSetWindowMonitor(window, m, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(window, nullptr, wx, wy, ww, wh, 0);
    }
}

const char* landCoverName(uint8_t cls) {
    switch (cls) {
        case 10: return "Wald";
        case 20: return "Buschland";
        case 30: return "Grasland";
        case 40: return "Ackerland";
        case 50: return "Bebaut";
        case 60: return "Kahl / spärlich";
        case 70: return "Schnee / Eis";
        case 80: return "Wasser";
        case 90: return "Feuchtgebiet";
        case 95: return "Mangroven";
        case 100: return "Moos / Flechten";
    }
    return "Unbekannt";
}

// ---------------------------------------------------------------- HUD

void drawHud(const game::Player& player, const Settings& s, const HudInfo& info) {
    if (info.alpha <= 0.001f) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 vs = ImGui::GetIO().DisplaySize;
    float a = info.alpha;
    char buf[128];

    if (s.showHud) {
        // Compass strip, top centre, on a translucent pill.
        float half = S(240), cy = S(34), cx = vs.x * 0.5f;
        float headingDeg = player.heading() * 57.29578f;
        static const char* kCardinal[] = {"N", "NO", "O", "SO", "S", "SW", "W", "NW"};
        dl->AddRectFilled(ImVec2(cx - half - S(18), cy - S(17)), ImVec2(cx + half + S(18), cy + S(13)), col(kInk, 0.4f * a),
                          S(15));
        for (int deg = 0; deg < 360; deg += 15) {
            float d = std::fmod(float(deg) - headingDeg + 540.0f, 360.0f) - 180.0f;
            if (std::abs(d) > 90.0f) continue;
            float x = cx + d / 90.0f * half;
            float fa = a * (1.0f - std::pow(std::abs(d) / 90.0f, 3.0f));
            if (deg % 45 == 0) {
                const char* label = kCardinal[deg / 45];
                ImFont* f = deg % 90 == 0 ? g_fonts.bold : g_fonts.small;
                fa *= deg % 90 == 0 ? 1.0f : 0.8f;
                ImVec2 ts = textSize(f, label);
                uint32_t c = deg == 0 ? kAccent : kText;
                shadowText(dl, f, ImVec2(x - ts.x * 0.5f, cy - ts.y * 0.5f - S(2)), col(c, fa), label, fa);
            } else {
                dl->AddLine(ImVec2(x, cy - S(5)), ImVec2(x, cy + S(3)), col(kText, 0.6f * fa), S(1.5f));
            }
        }
        dl->AddTriangleFilled(ImVec2(cx - S(5), cy + S(19)), ImVec2(cx + S(5), cy + S(19)), ImVec2(cx, cy + S(13)),
                              col(kAccent, a));
        std::snprintf(buf, sizeof(buf), "%03d\xC2\xB0", int(std::lround(headingDeg)) % 360);
        ImVec2 ts = textSize(g_fonts.small, buf);
        shadowText(dl, g_fonts.small, ImVec2(cx - ts.x * 0.5f, cy + S(20)), col(kText, 0.9f * a), buf, a);

        // Location card, bottom left.
        struct Line { ImFont* font; uint32_t color; float alpha; char text[96]; };
        Line lines[3];
        int n = 0;
        formatLatLon(lines[n].text, sizeof(lines[n].text), player.lat(), player.lon());
        lines[n].font = g_fonts.small, lines[n].color = kText, lines[n++].alpha = 0.7f;
        std::snprintf(lines[n].text, sizeof(lines[n].text), "%.0f m \xC3\xBC. NN   \xC2\xB7   %s", player.altitude(),
                      landCoverName(info.landCover));
        lines[n].font = g_fonts.body, lines[n].color = kText, lines[n++].alpha = 1.0f;
        if (player.flying) {
            std::snprintf(lines[n].text, sizeof(lines[n].text), "FLUGMODUS  \xC2\xB7  %.0f m/s", player.speed());
            lines[n].font = g_fonts.bold, lines[n].color = kAccent, lines[n++].alpha = 1.0f;
        }
        float cardW = 0, cardH = 0;
        for (int i = 0; i < n; ++i) {
            ImVec2 ls = textSize(lines[i].font, lines[i].text);
            cardW = std::max(cardW, ls.x);
            cardH += ls.y + S(2);
        }
        ImVec2 pad(S(14), S(10));
        ImVec2 c1(S(20), vs.y - S(20)), c0(c1.x, c1.y - cardH - pad.y * 2.0f);
        dl->AddRectFilled(c0, ImVec2(c1.x + cardW + pad.x * 2.0f, c1.y), col(kInk, 0.4f * a), S(8));
        dl->AddRectFilled(c0 + ImVec2(0, S(8)), ImVec2(c0.x + S(3), c1.y - S(8)), col(kAccent, 0.9f * a), S(1.5f));
        float y = c0.y + pad.y;
        for (int i = 0; i < n; ++i) {
            shadowText(dl, lines[i].font, ImVec2(c0.x + pad.x, y), col(lines[i].color, lines[i].alpha * a), lines[i].text, a);
            y += textSize(lines[i].font, lines[i].text).y + S(2);
        }

        if (player.waitingForGround()) {
            const char* text = "Lade Gel\xC3\xA4nde ...";
            ImVec2 ts = textSize(g_fonts.body, text);
            ImVec2 c(vs.x * 0.5f - ts.x * 0.5f - S(16), vs.y - S(60));
            dl->PathArcTo(c, S(8), info.time * 6.0f, info.time * 6.0f + 4.5f, 24);
            dl->PathStroke(col(kAccent, a), 0, S(2.5f));
            shadowText(dl, g_fonts.body, ImVec2(c.x + S(18), c.y - ts.y * 0.5f), col(kText, a), text, a);
        }
    }

    if (s.crosshair && player.view == game::Player::View::FirstPerson) {
        ImVec2 c = vs * 0.5f;
        dl->AddCircleFilled(c, S(3.2f), col(0, 0.35f * a));
        dl->AddCircleFilled(c, S(2.0f), col(kText, 0.85f * a));
    }

    // Short hint after start.
    float hint = std::clamp(7.0f - info.time, 0.0f, 1.0f) * std::clamp(info.time - 0.5f, 0.0f, 1.0f) * a;
    if (hint > 0.0f) {
        const char* text = "Esc  Men\xC3\xBC";
        ImVec2 ts = textSize(g_fonts.small, text);
        shadowText(dl, g_fonts.small, ImVec2(vs.x - ts.x - S(28), vs.y - ts.y - S(28)), col(kText, 0.8f * hint), text, hint);
    }
}

// ---------------------------------------------------------------- pause menu

void PauseMenu::open(double lat, double lon) {
    open_ = true;
    page_ = Page::Main;
    std::snprintf(latBuf_, sizeof(latBuf_), "%.5f", lat);
    std::snprintf(lonBuf_, sizeof(lonBuf_), "%.5f", lon);
}

void PauseMenu::back() {
    if (page_ != Page::Main) page_ = Page::Main;
    else open_ = false;
}

PauseMenu::Action PauseMenu::draw(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player) {
    fade_ = approach(fade_, open_ ? 1.0f : 0.0f, open_ ? 6.0f : 8.0f);
    if (fade_ <= 0.0f) return Action::None;
    float ease = 1.0f - (1.0f - fade_) * (1.0f - fade_);

    ImVec2 vs = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(vs);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ease);
    ImGui::Begin("##pause", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    // Backdrop: the renderer blurs and darkens the scene, this adds a soft panel on the left.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float panelW = vs.x * (page_ == Page::Main ? 0.5f : 0.66f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(vs.x * 0.04f, vs.y), col(kInk, 0.8f));
    dl->AddRectFilledMultiColor(ImVec2(vs.x * 0.04f, 0), ImVec2(panelW, vs.y), col(kInk, 0.8f), col(kInk, 0.0f),
                                col(kInk, 0.0f), col(kInk, 0.8f));

    float x0 = std::max(vs.x * 0.07f, S(48)) - (1.0f - ease) * S(40);
    float top = vs.y * 0.16f;
    Action action = Action::None;

    const char* title = page_ == Page::Main ? "WorldWalker" : page_ == Page::Options ? "Optionen" : "Teleport";
    ImVec2 ts = textSize(g_fonts.title, title);
    shadowText(dl, g_fonts.title, ImVec2(x0, top), col(kText), title);
    float y = top + ts.y + S(2);
    if (page_ == Page::Main) {
        char buf[128];
        formatLatLon(buf, sizeof(buf), player.lat(), player.lon());
        dl->AddText(g_fonts.body, g_fonts.body->FontSize, ImVec2(x0 + S(4), y), col(kTextDim), buf);
        y += g_fonts.body->FontSize;
    }
    dl->AddRectFilled(ImVec2(x0 + S(4), y + S(14)), ImVec2(x0 + S(52), y + S(17)), col(kAccent), S(1.5f));
    ImGui::SetCursorScreenPos(ImVec2(x0, y + S(40)));

    switch (page_) {
        case Page::Main: action = drawMain(player); break;
        case Page::Options: drawOptions(window, s, p, player); break;
        case Page::Teleport: action = drawTeleport(); break;
    }

    const char* hint = page_ == Page::Main ? "Esc  Zur\xC3\xBC" "ck zum Spiel" : "Esc  Zur\xC3\xBC" "ck";
    dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(x0 + S(4), vs.y - S(48)), col(kTextDim, 0.8f), hint);

    ImGui::End();
    ImGui::PopStyleVar();
    return open_ ? action : Action::None;
}

PauseMenu::Action PauseMenu::drawMain(const game::Player&) {
    float w = S(420);
    float x = ImGui::GetCursorScreenPos().x - S(22);
    auto item = [&](const char* label) {
        ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
        return menuItem(label, w);
    };
    Action a = Action::None;
    if (item("Fortsetzen")) a = Action::Resume;
    if (item("Charakter erstellen")) a = Action::CharacterCreator;
    if (item("Teleport")) page_ = Page::Teleport;
    if (item("Optionen")) page_ = Page::Options;
    ImGui::Dummy(ImVec2(0, S(10)));
    if (item("Beenden")) a = Action::Quit;
    return a;
}

void PauseMenu::drawOptions(GLFWwindow* window, Settings& s, render::SceneParams& p, game::Player& player) {
    float width = std::min(S(680), ImGui::GetIO().DisplaySize.x * 0.6f);
    g_labelW = width * 0.42f;
    g_fieldW = width - g_labelW;

    static const char* kTabs[] = {"Grafik", "Welt", "Steuerung", "Anzeige"};
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(start - ImVec2(S(14), S(24)));
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, S(2));
        if (tabButton(kTabs[i], tab_ == i)) tab_ = i;
    }
    ImVec2 lp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(start.x, lp.y - S(10)), ImVec2(start.x + width, lp.y - S(10)),
                                        col(0xFFFFFF, 0.12f));

    ImGui::SetCursorScreenPos(ImVec2(start.x, lp.y));
    float height = ImGui::GetIO().DisplaySize.y - lp.y - S(150);
    ImGui::BeginChild("##options", ImVec2(width + S(12), height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    char fmt[64];
    switch (tab_) {
        case 0: {
            sectionHeader("QUALIT\xC3\x84T");
            static const char* kQualities[] = {"Niedrig", "Mittel", "Hoch", "Ultra", "Benutzerdefiniert"};
            int quality = std::clamp(s.quality, 0, int(kQualityCustom));
            if (choiceRow("Voreinstellung", "##quality", kQualities, 5, &quality) && quality != kQualityCustom) {
                applyQuality(quality, p);
                s.quality = quality;
            }
            if (!s.gpu.empty()) {
                ImGui::PushFont(g_fonts.small);
                ImGui::TextDisabled("Grafikkarte: %s", s.gpu.c_str());
                ImGui::PopFont();
            }
            // Any individual change turns the preset into "Benutzerdefiniert".
            bool custom = false;
            float percent = p.renderScale * 100.0f;
            if (sliderRow("Renderaufl\xC3\xB6sung", "##scale", &percent, 50.0f, 100.0f, "%.0f %%")) {
                p.renderScale = std::round(percent / 5.0f) * 0.05f;
                custom = true;
            }
            custom |= toggleRow("Dynamische Aufl\xC3\xB6sung", "##dynres", &p.dynamicResolution);
            if (p.dynamicResolution) custom |= sliderRow("Ziel-Bildrate", "##target", &p.targetFps, 30.0f, 144.0f, "%.0f FPS");
            static const char* kShadows[] = {"Aus", "Niedrig", "Mittel", "Hoch"};
            int shadows = p.shadows ? p.shadowQuality + 1 : 0;
            if (choiceRow("Schatten", "##shadowq", kShadows, 4, &shadows)) {
                p.shadows = shadows > 0;
                if (shadows > 0) p.shadowQuality = shadows - 1;
                custom = true;
            }
            custom |= toggleRow("Gras", "##grass", &p.grass);
            if (p.grass) {
                float grass = p.grassDensity * 100.0f;
                if (sliderRow("Grasdichte", "##grassd", &grass, 0.0f, 100.0f, "%.0f %%")) {
                    p.grassDensity = grass / 100.0f;
                    custom = true;
                }
            }
            float terrainDetail = p.terrainDetail * 100.0f, treeDetail = p.treeDetail * 100.0f;
            if (sliderRow("Gel\xC3\xA4ndedetails", "##terrain", &terrainDetail, 50.0f, 150.0f, "%.0f %%")) {
                p.terrainDetail = terrainDetail / 100.0f;
                custom = true;
            }
            if (sliderRow("Baumdetails", "##treed", &treeDetail, 50.0f, 150.0f, "%.0f %%")) {
                p.treeDetail = treeDetail / 100.0f;
                custom = true;
            }
            custom |= toggleRow("Kantengl\xC3\xA4ttung (TAA)", "##taa", &p.taa);
            custom |= toggleRow("Umgebungsverdeckung (SSAO)", "##ssao", &p.ssao);
            sectionHeader("DARSTELLUNG");
            sliderRow("Sichtfeld", "##fov", &p.fovDeg, 50.0f, 100.0f, "%.0f\xC2\xB0");
            toggleRow("Automatische Belichtung", "##autoexp", &p.autoExposure);
            sliderRow("Belichtung", "##exp", &p.exposure, 0.2f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            if (toggleRow("Vollbild", "##full", &s.fullscreen)) setFullscreen(window, s.fullscreen);
            toggleRow("VSync", "##vsync", &s.vsync);
            static const char* kLimits[] = {"Aus", "30 FPS", "60 FPS", "120 FPS", "144 FPS"};
            static const int kLimitValues[] = {0, 30, 60, 120, 144};
            int limit = 0;
            for (int i = 0; i < 5; ++i) if (kLimitValues[i] == s.fpsLimit) limit = i;
            if (choiceRow("Bildraten-Limit", "##fpslimit", kLimits, 5, &limit)) s.fpsLimit = kLimitValues[limit];
            sectionHeader("SICHTWEITE");
            custom |= sliderRow("B\xC3\xA4ume", "##trees", &p.impostorDistance, 300.0f, 3000.0f, "%.0f m");
            if (custom) s.quality = kQualityCustom;
            float vis = 1.0f / p.fogDensity / 1000.0f;
            if (sliderRow("Dunst", "##fog", &vis, 2.0f, 200.0f, "%.0f km", ImGuiSliderFlags_Logarithmic))
                p.fogDensity = 1.0f / (vis * 1000.0f);
            sectionHeader("DEBUG");
            toggleRow("Drahtgitter (Terrain)", "##wire", &p.wireframe);
            break;
        }
        case 1: {
            static const int kLen[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
            static const char* kMonth[] = {"Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli",
                                           "August", "September", "Oktober", "November", "Dezember"};
            sectionHeader("SONNENSTAND");
            int minutes = int(p.timeOfDay * 60.0f + 0.5f);
            std::snprintf(fmt, sizeof(fmt), "%02d:%02d Uhr", (minutes / 60) % 24, minutes % 60);
            sliderRow("Uhrzeit", "##time", &p.timeOfDay, 0.0f, 24.0f, fmt);
            int doy = int(p.dayOfYear + 0.5f), m = 0;
            while (m < 11 && doy > kLen[m]) doy -= kLen[m++];
            std::snprintf(fmt, sizeof(fmt), "%d. %s", doy, kMonth[m]);
            sliderRow("Datum", "##day", &p.dayOfYear, 1.0f, 365.0f, fmt);
            sectionHeader("WETTER");
            std::snprintf(fmt, sizeof(fmt), "%d %%%%", int(p.cloudCoverage * 100.0f + 0.5f));
            sliderRow("Bew\xC3\xB6lkung", "##clouds", &p.cloudCoverage, 0.0f, 1.0f, fmt);
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::PushFont(g_fonts.small);
            ImGui::TextDisabled("Uhrzeit ist lokale Sonnenzeit am aktuellen Standort.");
            ImGui::PopFont();
            break;
        }
        case 2: {
            sectionHeader("MAUS");
            float sens = player.mouseSensitivity / 0.0025f;
            if (sliderRow("Empfindlichkeit", "##sens", &sens, 0.2f, 3.0f, "%.2f\xC3\x97"))
                player.mouseSensitivity = sens * 0.0025f;
            toggleRow("Y-Achse invertieren", "##inv", &s.invertY);
            sectionHeader("FLIEGEN");
            sliderRow("Fluggeschwindigkeit", "##fly", &player.flySpeed, 2.0f, 50000.0f, "%.0f m/s",
                      ImGuiSliderFlags_Logarithmic);
            sectionHeader("TASTENBELEGUNG");
            keyRow("W|A|S|D", "Laufen");
            keyRow("Shift", "Rennen / schneller fliegen");
            keyRow("Leertaste", "Springen");
            keyRow("C", "Knien / Aufstehen");
            keyRow("F", "Flugmodus an / aus");
            keyRow("E|Q", "Steigen / Sinken (Flug)");
            keyRow("V", "Ich- / Verfolgeransicht");
            keyRow("Mausrad", "Fluggeschwindigkeit / Kamera-Abstand");
            keyRow("F3", "Debug-Info");
            keyRow("Esc", "Men\xC3\xBC");
            break;
        }
        case 3: {
            sectionHeader("HUD");
            toggleRow("HUD anzeigen", "##hud", &s.showHud);
            toggleRow("Fadenkreuz", "##cross", &s.crosshair);
            toggleRow("Debug-Info (F3)", "##dbg", &s.showDebug);
            break;
        }
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(start.x - S(22), ImGui::GetCursorScreenPos().y + S(8)));
    if (menuItem("Zur\xC3\xBC" "ck", S(300), g_fonts.body)) page_ = Page::Main;
}

PauseMenu::Action PauseMenu::drawTeleport() {
    struct Place { const char* name; const char* region; double lat, lon; };
    static const Place kPlaces[] = {
        {"Feldberg", "Schwarzwald", 47.8740, 8.0040},
        {"Zugspitze", "Wettersteingebirge", 47.4211, 10.9853},
        {"Kleine Scheidegg", "Berner Oberland", 46.5850, 7.9610},
        {"Reine", "Lofoten, Norwegen", 67.9325, 13.0890},
        {"Yosemite Valley", "Kalifornien, USA", 37.7456, -119.5936},
        {"Grand Canyon", "South Rim, Arizona", 36.0570, -112.1401},
    };
    float width = std::min(S(680), ImGui::GetIO().DisplaySize.x * 0.6f);
    g_labelW = width * 0.42f;
    g_fieldW = width - g_labelW;
    Action a = Action::None;
    ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::BeginChild("##teleport", ImVec2(width + S(12), ImGui::GetIO().DisplaySize.y - start.y - S(100)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

    sectionHeader("KOORDINATEN");
    rowLabel("Breite");
    ImGui::InputText("##lat", latBuf_, sizeof(latBuf_), ImGuiInputTextFlags_CharsScientific);
    rowLabel("L\xC3\xA4nge");
    ImGui::InputText("##lon", lonBuf_, sizeof(lonBuf_), ImGuiInputTextFlags_CharsScientific);
    ImGui::SetCursorPosX(g_labelW);
    ImGui::PushStyleColor(ImGuiCol_Button, linear(srgb(kAccent, 0.9f)));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, linear(srgb(0xFFDE96)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, linear(srgb(0xD9AC55)));
    ImGui::PushStyleColor(ImGuiCol_Text, linear(srgb(kInk)));
    if (ImGui::Button("Hinspringen", ImVec2(g_fieldW, 0))) {
        teleportLat = std::clamp(std::atof(latBuf_), -85.0, 85.0);
        teleportLon = std::clamp(std::atof(lonBuf_), -180.0, 180.0);
        a = Action::Teleport;
    }
    ImGui::PopStyleColor(4);

    sectionHeader("ORTE");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = ImGui::GetCursorScreenPos().x - S(22);
    for (const Place& pl : kPlaces) {
        ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (menuItem(pl.name, width + S(22), g_fonts.body)) {
            teleportLat = pl.lat;
            teleportLon = pl.lon;
            a = Action::Teleport;
        }
        ImVec2 rs = textSize(g_fonts.small, pl.region);
        float h = g_fonts.body->FontSize * 1.55f;
        dl->AddText(g_fonts.small, g_fonts.small->FontSize, ImVec2(x + width + S(22) - rs.x - S(12), pos.y + (h - rs.y) * 0.5f),
                    col(kTextDim), pl.region);
    }
    ImGui::EndChild();
    return a;
}

}  // namespace ui
