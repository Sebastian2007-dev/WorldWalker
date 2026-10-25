#include "data/GeoRaster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace data {

thread_local bool GeoRaster::tNoWait = false;
thread_local bool GeoRaster::tMissed = false;

std::shared_ptr<GeoRaster::Cell> GeoRaster::missingCell() {
    static auto c = std::make_shared<Cell>();  // state Error -> treated as "no data yet"
    return c;
}

std::shared_ptr<const GeoRaster::Block> GeoRaster::missingBlock() {
    static auto b = std::make_shared<const Block>();
    return b;
}

GeoRaster::GeoRaster(Dataset ds, const std::filesystem::path& cacheRoot, size_t maxBlocks)
    : ds_(ds), maxBlocks_(maxBlocks) {
    if (ds == Dataset::Elevation) {
        cacheRoot_ = cacheRoot / "copernicus_dem30";
        cellDeg_ = 1.0;
        basePixelDeg_ = 1.0 / 3600.0;
        maxLevel_ = 3;
        missingValue_ = 0.0f;
    } else {
        cacheRoot_ = cacheRoot / "esa_worldcover2021";
        cellDeg_ = 3.0;
        basePixelDeg_ = 1.0 / 12000.0;
        maxLevel_ = 6;
        missingValue_ = float(lc::Water);
    }
}

std::string GeoRaster::cellName(int cy, int cx) const {
    int lat = int(std::lround(cellLat0(cy)));
    int lon = int(std::lround(cellLon0(cx)));
    char buf[160];
    if (ds_ == Dataset::Elevation) {
        std::snprintf(buf, sizeof(buf), "Copernicus_DSM_COG_10_%c%02d_00_%c%03d_00_DEM", lat >= 0 ? 'N' : 'S',
                      std::abs(lat), lon >= 0 ? 'E' : 'W', std::abs(lon));
    } else {
        std::snprintf(buf, sizeof(buf), "ESA_WorldCover_10m_2021_v200_%c%02d%c%03d_Map", lat >= 0 ? 'N' : 'S',
                      std::abs(lat), lon >= 0 ? 'E' : 'W', std::abs(lon));
    }
    return buf;
}

std::string GeoRaster::url(int cy, int cx) const {
    std::string n = cellName(cy, cx);
    if (ds_ == Dataset::Elevation) return "https://copernicus-dem-30m.s3.amazonaws.com/" + n + "/" + n + ".tif";
    return "https://esa-worldcover.s3.eu-central-1.amazonaws.com/v200/2021/map/" + n + ".tif";
}

std::shared_ptr<GeoRaster::Cell> GeoRaster::cell(int cy, int cx) {
    int key = cy * 1000 + cx;
    std::promise<std::shared_ptr<Cell>> prom;
    std::shared_future<std::shared_ptr<Cell>> fut;
    bool owner = false;
    {
        std::lock_guard lk(cellMutex_);
        auto it = cells_.find(key);
        if (it != cells_.end()) {
            fut = it->second;
            if (tNoWait && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                tMissed = true;
                return missingCell();
            }
        } else if (tNoWait) {
            tMissed = true;
            return missingCell();
        } else {
            fut = prom.get_future().share();
            cells_[key] = fut;
            owner = true;
        }
    }
    if (owner) {
        auto c = std::make_shared<Cell>();
        if (!sShutdown) c->state = c->cog.open(url(cy, cx), cacheRoot_ / cellName(cy, cx));
        prom.set_value(c);
        if (c->state == CogFile::OpenResult::Error) {
            // Transient failure (offline?): forget it so a later request retries.
            std::lock_guard lk(cellMutex_);
            cells_.erase(key);
        }
        return c;
    }
    return fut.get();
}

bool GeoRaster::cellReady(double lat, double lon) {
    int cy = std::clamp(int(std::floor((lat + 90.0) / cellDeg_)), 0, int(180.0 / cellDeg_) - 1);
    int cx = std::clamp(int(std::floor((lon + 180.0) / cellDeg_)), 0, int(360.0 / cellDeg_) - 1);
    return cell(cy, cx)->state != CogFile::OpenResult::Error;
}

std::shared_ptr<const GeoRaster::Block> GeoRaster::block(int cy, int cx, const Cell& c, int level, uint32_t tx,
                                                         uint32_t ty) {
    uint64_t key = ((((uint64_t(cy) * 512 + uint64_t(cx)) * 16 + uint64_t(level)) * 128 + ty) * 128) + tx;
    std::promise<std::shared_ptr<const Block>> prom;
    std::shared_future<std::shared_ptr<const Block>> fut;
    bool owner = false;
    {
        std::lock_guard lk(blockMutex_);
        auto it = blocks_.find(key);
        if (it != blocks_.end()) {
            it->second.lastUse = ++useCounter_;
            fut = it->second.fut;
            if (tNoWait && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                tMissed = true;
                return missingBlock();
            }
        } else if (tNoWait) {
            tMissed = true;
            return missingBlock();
        } else {
            if (blocks_.size() >= maxBlocks_) {
                // Evict the least recently used quarter of ready blocks.
                std::vector<std::pair<uint64_t, uint64_t>> ages;
                for (auto& [k, s] : blocks_)
                    if (s.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                        ages.push_back({s.lastUse, k});
                std::sort(ages.begin(), ages.end());
                for (size_t i = 0; i < ages.size() / 4; ++i) blocks_.erase(ages[i].second);
            }
            fut = prom.get_future().share();
            blocks_[key] = {fut, ++useCounter_};
            owner = true;
        }
    }
    if (owner) {
        auto b = std::make_shared<Block>();
        const auto& lv = c.cog.levels()[level];
        b->w = lv.tileW;
        b->h = lv.tileH;
        b->ok = !sShutdown && c.cog.readTile(level, tx, ty, b->data);
        prom.set_value(b);
        if (!b->ok) {
            std::lock_guard lk(blockMutex_);
            blocks_.erase(key);
        }
        return b;
    }
    return fut.get();
}

float GeoRaster::pixelInCell(const Cell& c, int cy, int cx, int level, int64_t px, int64_t py) {
    const auto& lv = c.cog.levels()[level];
    px = std::clamp<int64_t>(px, 0, lv.width - 1);
    py = std::clamp<int64_t>(py, 0, lv.height - 1);
    uint32_t tx = uint32_t(px / lv.tileW), ty = uint32_t(py / lv.tileH);
    auto b = block(cy, cx, c, level, tx, ty);
    if (!b->ok) return missingValue_;
    size_t i = size_t(py % lv.tileH) * b->w + size_t(px % lv.tileW);
    if (c.cog.isFloat()) {
        float v;
        std::memcpy(&v, &b->data[i * 4], 4);
        return (v < -1000.0f || std::isnan(v)) ? missingValue_ : v;
    }
    uint8_t v = b->data[i];
    return (ds_ == Dataset::LandCover && v == 0) ? missingValue_ : float(v);
}

float GeoRaster::pixel(int cy, int cx, int level, int64_t px, int64_t py) {
    int cyMax = int(180.0 / cellDeg_) - 1, cxMax = int(360.0 / cellDeg_) - 1;
    cy = std::clamp(cy, 0, cyMax);
    if (cx < 0) cx += cxMax + 1;
    if (cx > cxMax) cx -= cxMax + 1;
    auto c = cell(cy, cx);
    if (c->state != CogFile::OpenResult::Ok) return missingValue_;
    int lvl = std::min(level, int(c->cog.levels().size()) - 1);
    const auto& lv = c->cog.levels()[lvl];
    if (px >= 0 && py >= 0 && px < lv.width && py < lv.height) return pixelInCell(*c, cy, cx, lvl, px, py);

    // Outside this cell: convert the pixel centre to degrees and look it up in the neighbour.
    double dLon = cellDeg_ / lv.width, dLat = cellDeg_ / lv.height;
    double lon = cellLon0(cx) + (double(px) + 0.5) * dLon;
    double lat = cellLat0(cy) + cellDeg_ - (double(py) + 0.5) * dLat;
    int ncy = int(std::floor((lat + 90.0) / cellDeg_));
    int ncx = int(std::floor((lon + 180.0) / cellDeg_));
    if (ncy < 0 || ncy > cyMax) return pixelInCell(*c, cy, cx, lvl, px, py);
    if (ncx < 0) { ncx += cxMax + 1; lon += 360.0; }
    if (ncx > cxMax) { ncx -= cxMax + 1; lon -= 360.0; }
    auto n = cell(ncy, ncx);
    if (n->state != CogFile::OpenResult::Ok) return missingValue_;
    int nl = std::min(level, int(n->cog.levels().size()) - 1);
    const auto& nlv = n->cog.levels()[nl];
    int64_t npx = int64_t(std::floor((lon - cellLon0(ncx)) / (cellDeg_ / nlv.width)));
    int64_t npy = int64_t(std::floor((cellLat0(ncy) + cellDeg_ - lat) / (cellDeg_ / nlv.height)));
    return pixelInCell(*n, ncy, ncx, nl, npx, npy);
}

float GeoRaster::height(double lat, double lon, int level) {
    lat = std::clamp(lat, -89.9999, 89.9999);
    if (lon >= 180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
    int cy = int(std::floor((lat + 90.0) / cellDeg_));
    int cx = int(std::floor((lon + 180.0) / cellDeg_));
    auto c = cell(cy, cx);
    if (c->state != CogFile::OpenResult::Ok) {
        // Neighbouring land cells can still matter close to the border; sampling grid handles that.
        return missingValue_;
    }
    int lvl = std::min(level, int(c->cog.levels().size()) - 1);
    const auto& lv = c->cog.levels()[lvl];
    double fx = (lon - cellLon0(cx)) / (cellDeg_ / lv.width) - 0.5;
    double fy = (cellLat0(cy) + cellDeg_ - lat) / (cellDeg_ / lv.height) - 0.5;
    int64_t ix = int64_t(std::floor(fx)), iy = int64_t(std::floor(fy));
    float tx = float(fx - double(ix)), ty = float(fy - double(iy));
    float h00 = pixel(cy, cx, lvl, ix, iy), h10 = pixel(cy, cx, lvl, ix + 1, iy);
    float h01 = pixel(cy, cx, lvl, ix, iy + 1), h11 = pixel(cy, cx, lvl, ix + 1, iy + 1);
    return (h00 * (1 - tx) + h10 * tx) * (1 - ty) + (h01 * (1 - tx) + h11 * tx) * ty;
}

uint8_t GeoRaster::landCover(double lat, double lon, int level) {
    lat = std::clamp(lat, -89.9999, 89.9999);
    if (lon >= 180.0) lon -= 360.0;
    if (lon < -180.0) lon += 360.0;
    int cy = int(std::floor((lat + 90.0) / cellDeg_));
    int cx = int(std::floor((lon + 180.0) / cellDeg_));
    auto c = cell(cy, cx);
    if (c->state != CogFile::OpenResult::Ok) return uint8_t(missingValue_);
    int lvl = std::min(level, int(c->cog.levels().size()) - 1);
    const auto& lv = c->cog.levels()[lvl];
    int64_t px = int64_t(std::floor((lon - cellLon0(cx)) / (cellDeg_ / lv.width)));
    int64_t py = int64_t(std::floor((cellLat0(cy) + cellDeg_ - lat) / (cellDeg_ / lv.height)));
    return uint8_t(pixelInCell(*c, cy, cx, lvl, px, py));
}

int GeoRaster::levelForSpacing(double spacingDeg) const {
    int l = 0;
    while (l < maxLevel_ && basePixelDeg_ * double(1 << (l + 1)) <= spacingDeg) ++l;
    return l;
}

}  // namespace data
