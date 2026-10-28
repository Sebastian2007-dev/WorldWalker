#pragma once
// Saved games: JSON files in <root>/saves (slot1..slot5.json, autosave.json, written on quit and when returning to
// the main menu). A save holds where the player stands and looks, the time of day and the character.
#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace game {

struct SaveGame {
    double lat = 0, lon = 0;
    float yawDeg = 0, pitchDeg = 0;
    bool thirdPerson = false, flying = false;
    float timeOfDay = 12, dayOfYear = 172;
    nlohmann::json character;  // render::Human::state, null without a custom character
    int64_t savedAt = 0;       // seconds since 1970 (UTC)
};

constexpr int kSaveSlots = 5;
// slot 0: the autosave, 1..kSaveSlots: the player's slots.
std::filesystem::path savePath(const std::filesystem::path& root, int slot);
bool writeSave(const std::filesystem::path& file, const SaveGame& save);
bool readSave(const std::filesystem::path& file, SaveGame& save);
// "28.10.2026 14:05" in local time, for the slot lists.
std::string saveTime(int64_t savedAt);

}  // namespace game
