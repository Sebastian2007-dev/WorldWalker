#pragma once
// Global raster datasets streamed from public cloud-optimized GeoTIFFs:
//  - Elevation: Copernicus DEM GLO-30 (1 deg cells, ~30 m, metres above EGM2008)
//  - LandCover: ESA WorldCover 2021 v200 (3 deg cells, 10 m, class codes 10..100)
// Thread-safe; sampling blocks the calling thread while data is fetched.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "data/CogFile.h"

namespace data {

enum class Dataset { Elevation, LandCover };

// ESA WorldCover class codes.
namespace lc {
constexpr uint8_t Tree = 10, Shrub = 20, Grass = 30, Crop = 40, Built = 50, Bare = 60, Snow = 70,
                  Water = 80, Wetland = 90, Mangrove = 95, Moss = 100;
}

class GeoRaster {
public:
    GeoRaster(Dataset ds, const std::filesystem::path& cacheRoot, size_t maxBlocks);

    // Elevation in metres, bilinear. level 0 = full resolution, higher = overviews.
    float height(double lat, double lon, int level);
    // Land-cover class, nearest neighbour.
    uint8_t landCover(double lat, double lon, int level);

    // Finest overview level whose pixels are not smaller than spacingDeg.
    int levelForSpacing(double spacingDeg) const;

    // Makes every pending and future fetch fail fast (used when the game exits).
    static void beginShutdown() { sShutdown = true; }

    // Fetches the cell header (blocking). False if the server could not be reached.
    bool cellReady(double lat, double lon);

    // While a NoWaitScope is alive on a thread, lookups never block: data that is not resident
    // yields the missing value and marks the scope as missed.
    struct NoWaitScope {
        NoWaitScope() { tNoWait = true; tMissed = false; }
        ~NoWaitScope() { tNoWait = false; }
        bool missed() const { return tMissed; }
    };

private:
    struct Cell {
        CogFile::OpenResult state = CogFile::OpenResult::Error;
        CogFile cog;
    };
    struct Block {
        std::vector<uint8_t> data;
        uint32_t w = 0, h = 0;
        bool ok = false;
    };
    struct BlockSlot {
        std::shared_future<std::shared_ptr<const Block>> fut;
        uint64_t lastUse = 0;
    };

    static inline std::atomic<bool> sShutdown{false};
    static thread_local bool tNoWait;
    static thread_local bool tMissed;
    static std::shared_ptr<Cell> missingCell();
    static std::shared_ptr<const Block> missingBlock();

    std::shared_ptr<Cell> cell(int cy, int cx);
    std::shared_ptr<const Block> block(int cy, int cx, const Cell& c, int level, uint32_t tx, uint32_t ty);
    // Pixel value of cell (cy,cx) at integer pixel (px,py), following into neighbour cells.
    float pixel(int cy, int cx, int level, int64_t px, int64_t py);
    float pixelInCell(const Cell& c, int cy, int cx, int level, int64_t px, int64_t py);
    std::string url(int cy, int cx) const;
    std::string cellName(int cy, int cx) const;
    double cellLat0(int cy) const { return -90.0 + cy * cellDeg_; }
    double cellLon0(int cx) const { return -180.0 + cx * cellDeg_; }

    Dataset ds_;
    std::filesystem::path cacheRoot_;
    double cellDeg_;
    double basePixelDeg_;
    int maxLevel_;
    float missingValue_;
    size_t maxBlocks_;

    std::mutex cellMutex_;
    std::unordered_map<int, std::shared_future<std::shared_ptr<Cell>>> cells_;
    std::mutex blockMutex_;
    std::unordered_map<uint64_t, BlockSlot> blocks_;
    uint64_t useCounter_ = 0;
};

}  // namespace data
