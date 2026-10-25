#pragma once
// Minimal Cloud-Optimized GeoTIFF reader: fetches the header once, then individual
// compressed tiles via HTTP range requests. Everything fetched is mirrored on disk.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace data {

class CogFile {
public:
    struct Level {
        uint32_t width = 0, height = 0, tileW = 0, tileH = 0, tilesX = 0, tilesY = 0;
        std::vector<uint64_t> offsets, counts;
    };

    enum class OpenResult { Ok, Missing, Error };

    // url of the .tif, cacheDir is a per-file directory.
    OpenResult open(const std::string& url, const std::filesystem::path& cacheDir);

    // Decodes one tile into raw samples (tileW*tileH*bytesPerSample). Returns false on failure.
    bool readTile(int level, uint32_t tx, uint32_t ty, std::vector<uint8_t>& out) const;

    const std::vector<Level>& levels() const { return levels_; }
    int bytesPerSample() const { return bits_ / 8; }
    bool isFloat() const { return sampleFormat_ == 3; }
    // Geo-referencing of level 0 (top-left corner in degrees, pixel size in degrees).
    double originLon() const { return originLon_; }
    double originLat() const { return originLat_; }
    double pixelLon() const { return pixelLon_; }
    double pixelLat() const { return pixelLat_; }

private:
    bool parse(const std::vector<uint8_t>& d, bool& needMore, uint64_t& wanted);

    std::string url_;
    std::filesystem::path cacheDir_;
    std::vector<Level> levels_;
    int bits_ = 0, sampleFormat_ = 1, predictor_ = 1, compression_ = 1;
    double originLon_ = 0, originLat_ = 0, pixelLon_ = 0, pixelLat_ = 0;
};

}  // namespace data
