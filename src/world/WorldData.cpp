#include "world/WorldData.h"

#include <cmath>

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
