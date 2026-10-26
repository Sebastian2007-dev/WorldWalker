#include "world/WorldData.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

#include "geo/Geodesy.h"

namespace world {

WorldData::WorldData(const std::filesystem::path& cacheRoot)
    : dem_(data::Dataset::Elevation, cacheRoot, 160),       // 160 x 4 MB float blocks
      cover_(data::Dataset::LandCover, cacheRoot, 400) {}   // 400 x 1 MB byte blocks

static double valueNoise(double x, double y) {
    double fx = std::floor(x), fy = std::floor(y);
    int64_t ix = int64_t(fx), iy = int64_t(fy);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    double a = hashFloat(hash2(ix, iy)), b = hashFloat(hash2(ix + 1, iy));
    double c = hashFloat(hash2(ix, iy + 1)), d = hashFloat(hash2(ix + 1, iy + 1));
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;  // 0..1
}

double WorldData::microRelief(double lat, double lon) {
    // Local metric coordinates; the scale drift with latitude is irrelevant for noise.
    double y = lat * 111320.0;
    double x = lon * 111320.0 * std::cos(lat * geo::kDeg);
    double n = (valueNoise(x / 23.0, y / 23.0) - 0.5) * 1.6 +
               (valueNoise(x / 8.7 + 17.3, y / 8.7 - 4.1) - 0.5) * 0.55 +
               (valueNoise(x / 3.1 - 9.7, y / 3.1 + 2.9) - 0.5) * 0.16;
    return n;
}

double WorldData::groundHeight(double lat, double lon) {
    double h = dem_.height(lat, lon, 0);
    if (cover_.landCover(lat, lon, 0) != data::lc::Water) h += microRelief(lat, lon);
    return h;
}

double WorldData::meshHeight(double lat, double lon, double* slope) {
    const double d = kMeshSpacingDeg;
    double gy = (lat + 90.0) / d, gx = (lon + 180.0) / d;
    double fy = std::floor(gy), fx = std::floor(gx);
    double ty = gy - fy, tx = gx - fx;
    double la = -90.0 + fy * d, lo = -180.0 + fx * d;
    double h00 = groundHeight(la, lo), h10 = groundHeight(la, lo + d);
    double h01 = groundHeight(la + d, lo), h11 = groundHeight(la + d, lo + d);
    // Same split as Terrain's index buffer: triangles (00, 10, 11) and (00, 11, 01).
    double dx, dy, h;
    if (tx >= ty) {
        dx = h10 - h00;
        dy = h11 - h10;
    } else {
        dx = h11 - h01;
        dy = h01 - h00;
    }
    h = h00 + tx * dx + ty * dy;
    if (slope) {
        double gxm = dx / (d * geo::metersPerDegLon(lat)), gym = dy / (d * geo::metersPerDegLat(lat));
        *slope = std::sqrt(gxm * gxm + gym * gym);
    }
    return h;
}

bool WorldData::tryMeshHeight(double lat, double lon, double& out) {
    data::GeoRaster::NoWaitScope scope;
    double h = meshHeight(lat, lon);
    if (scope.missed()) {
        prefetch(lat, lon);
        return false;
    }
    out = h;
    return true;
}

double WorldData::heightForSpacing(double lat, double lon, double spacingDeg) {
    if (spacingDeg <= kDetailSpacingDeg) return groundHeight(lat, lon);
    return dem_.height(lat, lon, dem_.levelForSpacing(spacingDeg * 0.5));
}

double WorldData::lakeLevel(double lat, double lon) {
    const double step = kLakeGridDeg;
    auto key = [](int64_t y, int64_t x) { return (uint64_t(y) << 32) | uint64_t(uint32_t(x)); };
    auto water = [&](int64_t y, int64_t x) {
        return landCover(-90.0 + (double(y) + 0.5) * step, -180.0 + (double(x) + 0.5) * step) == data::lc::Water;
    };
    int64_t gy = int64_t(std::floor((lat + 90.0) / step)), gx = int64_t(std::floor((lon + 180.0) / step));
    {
        std::lock_guard<std::mutex> lock(lakeMutex_);
        if (auto it = lakeCells_.find(key(gy, gx)); it != lakeCells_.end()) return it->second;
    }
    // The point may sit just beside the water at this grid's resolution: start from a water cell next to it.
    int64_t sy = gy, sx = gx;
    bool found = water(gy, gx);
    for (int dy = -1; dy <= 1 && !found; ++dy)
        for (int dx = -1; dx <= 1 && !found; ++dx)
            if (water(gy + dy, gx + dx)) sy = gy + dy, sx = gx + dx, found = true;
    if (!found) return std::numeric_limits<double>::quiet_NaN();
    // Many chunks of one lake are built in parallel; without this they would all flood fill the same lake.
    std::lock_guard<std::mutex> fill(lakeFillMutex_);
    {
        std::lock_guard<std::mutex> lock(lakeMutex_);
        if (auto it = lakeCells_.find(key(sy, sx)); it != lakeCells_.end()) return it->second;
    }
    // Flood fill the connected water and take a low percentile of the DEM over it: lake surfaces in the DEM are not
    // flat, and the cover map reaches a little onto the banks. Huge lakes and long rivers stop at a cell budget.
    const size_t kMaxCells = 60000;
    std::vector<std::pair<int64_t, int64_t>> queue{{sy, sx}};
    std::unordered_set<uint64_t> seen{key(sy, sx)};
    std::vector<double> heights;
    for (size_t i = 0; i < queue.size() && queue.size() < kMaxCells; ++i) {
        auto [y, x] = queue[i];
        heights.push_back(dem_.height(-90.0 + (double(y) + 0.5) * step, -180.0 + (double(x) + 0.5) * step, 0));
        const int64_t ny[4] = {y + 1, y - 1, y, y}, nx[4] = {x, x, x + 1, x - 1};
        for (int k = 0; k < 4; ++k)
            if (seen.insert(key(ny[k], nx[k])).second && water(ny[k], nx[k])) queue.push_back({ny[k], nx[k]});
    }
    size_t q = heights.size() / 4;
    std::nth_element(heights.begin(), heights.begin() + q, heights.end());
    float level = float(heights[q]);
    std::lock_guard<std::mutex> lock(lakeMutex_);
    if (lakeCells_.size() > 4000000) lakeCells_.clear();  // a long journey: forget lakes far behind
    for (auto [y, x] : queue) lakeCells_.emplace(key(y, x), level);
    return lakeCells_.at(key(sy, sx));
}

uint8_t WorldData::landCover(double lat, double lon, double spacingDeg) {
    return cover_.landCover(lat, lon, spacingDeg > 0 ? cover_.levelForSpacing(spacingDeg) : 0);
}

void WorldData::prefetch(double lat, double lon) {
    if (!jobs_ || prefetching_.exchange(true)) return;
    jobs_->submit(-1000.0f, [this, lat, lon] {
        groundHeight(lat, lon);
        dem_.height(lat, lon, 3);
        prefetching_ = false;
    });
}

bool WorldData::tryGroundHeight(double lat, double lon, double& out) {
    data::GeoRaster::NoWaitScope scope;
    double h = groundHeight(lat, lon);
    if (scope.missed()) {
        prefetch(lat, lon);
        return false;
    }
    out = h;
    return true;
}

bool WorldData::tryCoarseHeight(double lat, double lon, double& out) {
    data::GeoRaster::NoWaitScope scope;
    double h = dem_.height(lat, lon, 3);
    if (scope.missed()) {
        prefetch(lat, lon);
        return false;
    }
    out = h;
    return true;
}

bool WorldData::tryLandCover(double lat, double lon, uint8_t& out) {
    data::GeoRaster::NoWaitScope scope;
    uint8_t c = cover_.landCover(lat, lon, 0);
    if (scope.missed()) return false;
    out = c;
    return true;
}

}  // namespace world
