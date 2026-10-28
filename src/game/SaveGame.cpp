#include "game/SaveGame.h"

#include <cmath>
#include <ctime>
#include <fstream>

namespace game {

std::filesystem::path savePath(const std::filesystem::path& root, int slot) {
    return root / "saves" / (slot == 0 ? std::string("autosave.json") : "slot" + std::to_string(slot) + ".json");
}

bool writeSave(const std::filesystem::path& file, const SaveGame& s) {
    nlohmann::json j = {{"version", 1},          {"lat", s.lat},           {"lon", s.lon},
                        {"yaw", s.yawDeg},       {"pitch", s.pitchDeg},    {"thirdPerson", s.thirdPerson},
                        {"flying", s.flying},    {"timeOfDay", s.timeOfDay}, {"dayOfYear", s.dayOfYear},
                        {"character", s.character}, {"savedAt", s.savedAt}};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // Written next to the file and renamed, so a crash never leaves half a save.
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!(f << j.dump(1))) return false;
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool readSave(const std::filesystem::path& file, SaveGame& s) {
    std::ifstream f(file);
    if (!f) return false;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 0) != 1) return false;
    try {
        s.lat = j.at("lat").get<double>();
        s.lon = j.at("lon").get<double>();
        s.yawDeg = j.value("yaw", 0.0f);
        s.pitchDeg = j.value("pitch", 0.0f);
        s.thirdPerson = j.value("thirdPerson", false);
        s.flying = j.value("flying", false);
        s.timeOfDay = j.value("timeOfDay", 12.0f);
        s.dayOfYear = j.value("dayOfYear", 172.0f);
        s.character = j.value("character", nlohmann::json());
        s.savedAt = j.value("savedAt", int64_t(0));
    } catch (...) {
        return false;
    }
    return std::isfinite(s.lat) && std::isfinite(s.lon) && std::abs(s.lat) <= 85.0 && std::abs(s.lon) <= 180.0;
}

std::string saveTime(int64_t savedAt) {
    std::time_t t = std::time_t(savedAt);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return {};
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M", &tm);
    return buf;
}

}  // namespace game
