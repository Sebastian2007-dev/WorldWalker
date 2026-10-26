#pragma once
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "core/Jobs.h"

#include "data/GeoRaster.h"

namespace world {

// Facade over the streamed global datasets. Defines THE ground height function so that
// terrain meshes, vegetation placement and player physics agree with each other.
class WorldData {
public:
    explicit WorldData(const std::filesystem::path& cacheRoot);
    void setJobs(core::JobSystem* jobs) { jobs_ = jobs; }

    // Non-blocking variants for the main thread: return false (and start loading in the
    // background) when the data is not resident yet.
    bool tryGroundHeight(double lat, double lon, double& out);
    bool tryMeshHeight(double lat, double lon, double& out);
    bool tryCoarseHeight(double lat, double lon, double& out);
    bool tryLandCover(double lat, double lon, uint8_t& out);

    // Height at the finest detail level (DEM 30 m bilinear + procedural micro relief).
    double groundHeight(double lat, double lon);
    // Height of the finest terrain mesh as drawn: its grid samples interpolated per triangle. Use this for
    // anything that stands on the ground (player, trees, grass); groundHeight() has relief below the mesh
    // resolution and would let things float or sink by a few decimetres. `slope` receives |gradient| (m/m).
    double meshHeight(double lat, double lon, double* slope = nullptr);
    // Grid spacing of the finest terrain level (Terrain::kMaxLevel nodes with Terrain::kGrid quads).
    static constexpr double kMeshSpacingDeg = 1.0 / 16384.0;
    // Height for a sampling grid with the given spacing (uses DEM overviews when coarse).
    double heightForSpacing(double lat, double lon, double spacingDeg);
    uint8_t landCover(double lat, double lon, double spacingDeg = 0.0);
    // Surface level of the lake (connected water cover) at a point: one value per lake, so all terrain chunks and the
    // grass agree on it. NaN where there is no water within one lake grid cell. Cached; safe to call from workers.
    double lakeLevel(double lat, double lon);
    static constexpr double kLakeGridDeg = 1.0 / 5000.0;  // ~22 m

    // Deterministic micro relief in metres (adds detail below the 30 m DEM resolution).
    static double microRelief(double lat, double lon);
    // Spacing below which micro relief is applied.
    static constexpr double kDetailSpacingDeg = 1.0 / 2048.0;

    data::GeoRaster& dem() { return dem_; }
    data::GeoRaster& cover() { return cover_; }

private:
    void prefetch(double lat, double lon);

    core::JobSystem* jobs_ = nullptr;
    std::atomic<bool> prefetching_{false};
    data::GeoRaster dem_;
    data::GeoRaster cover_;
    std::mutex lakeMutex_;
    std::mutex lakeFillMutex_;  // one flood fill at a time: chunks of the same lake then reuse its result
    std::unordered_map<uint64_t, float> lakeCells_;  // lake grid cell -> level of its lake
};

// Small deterministic hash helpers shared by placement code.
inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline uint32_t hash2(int64_t a, int64_t b) {
    return hash32(uint32_t(a) * 0x9E3779B1u ^ hash32(uint32_t(b) + 0x85EBCA6Bu) ^ uint32_t(a >> 32) ^ uint32_t(b >> 32) * 0xC2B2AE35u);
}
inline float hashFloat(uint32_t h) { return float(h & 0xFFFFFF) / float(0x1000000); }

}  // namespace world
