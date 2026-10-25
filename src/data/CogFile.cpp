#include "data/CogFile.h"

#include <libdeflate.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include "data/Http.h"

namespace fs = std::filesystem;

namespace data {
namespace {

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    out.resize(size_t(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

void writeFileAtomic(const fs::path& p, const void* data, size_t n) {
    fs::path tmp = p;
    tmp += ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(static_cast<const char*>(data), std::streamsize(n));
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

struct Reader {
    const std::vector<uint8_t>& d;
    bool le;
    bool inRange(uint64_t off, uint64_t n) const { return off + n <= d.size(); }
    uint16_t u16(uint64_t o) const {
        uint16_t v;
        std::memcpy(&v, &d[o], 2);
        return le ? v : uint16_t((v >> 8) | (v << 8));
    }
    uint32_t u32(uint64_t o) const {
        uint32_t v;
        std::memcpy(&v, &d[o], 4);
        return le ? v : _byteswap_ulong(v);
    }
    uint64_t u64(uint64_t o) const {
        uint64_t v;
        std::memcpy(&v, &d[o], 8);
        return le ? v : _byteswap_uint64(v);
    }
    double f64(uint64_t o) const {
        uint64_t v = u64(o);
        double r;
        std::memcpy(&r, &v, 8);
        return r;
    }
};

int typeSize(int t) {
    switch (t) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: return 4;
        case 5: case 10: case 12: case 16: case 17: case 18: return 8;
        default: return 1;
    }
}

}  // namespace

CogFile::OpenResult CogFile::open(const std::string& url, const fs::path& cacheDir) {
    url_ = url;
    cacheDir_ = cacheDir;
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    if (fs::exists(cacheDir / "missing")) return OpenResult::Missing;

    std::vector<uint8_t> hdr;
    fs::path hdrPath = cacheDir / "header.bin";
    bool needMore = false;
    uint64_t wanted = 0;
    if (readFile(hdrPath, hdr) && parse(hdr, needMore, wanted) && !needMore) return OpenResult::Ok;

    uint64_t size = 64 * 1024;
    for (int i = 0; i < 8; ++i) {
        auto r = net::get(url, 0, int64_t(size) - 1);
        if (r.notFound()) {
            writeFileAtomic(cacheDir / "missing", "", 0);
            return OpenResult::Missing;
        }
        if (!r.ok()) return OpenResult::Error;
        hdr = std::move(r.body);
        levels_.clear();
        if (!parse(hdr, needMore, wanted)) {
            if (!needMore) return OpenResult::Error;
        } else if (!needMore) {
            writeFileAtomic(hdrPath, hdr.data(), hdr.size());
            return OpenResult::Ok;
        }
        size = std::max<uint64_t>(size * 2, wanted + 4096);
    }
    return OpenResult::Error;
}

bool CogFile::parse(const std::vector<uint8_t>& d, bool& needMore, uint64_t& wanted) {
    needMore = false;
    levels_.clear();
    if (d.size() < 16) return false;
    Reader r{d, d[0] == 'I'};
    uint16_t magic = r.u16(2);
    bool big = magic == 43;
    if (magic != 42 && !big) return false;
    uint64_t ifd = big ? r.u64(8) : r.u32(4);
    auto more = [&](uint64_t end) {
        needMore = true;
        wanted = std::max(wanted, end);
        return false;
    };

    while (ifd) {
        uint64_t cntSize = big ? 8 : 2, entSize = big ? 20 : 12;
        if (!r.inRange(ifd, cntSize)) return more(ifd + 64 * 1024);
        uint64_t cnt = big ? r.u64(ifd) : r.u16(ifd);
        uint64_t entries = ifd + cntSize;
        if (!r.inRange(entries, cnt * entSize + 8)) return more(entries + cnt * entSize + 8);

        Level lv;
        bool reduced = false;
        for (uint64_t i = 0; i < cnt; ++i) {
            uint64_t e = entries + i * entSize;
            uint16_t tag = r.u16(e), type = r.u16(e + 2);
            uint64_t count = big ? r.u64(e + 4) : r.u32(e + 4);
            uint64_t valOff = big ? e + 12 : e + 8;
            uint64_t bytes = count * typeSize(type);
            uint64_t dataOff = bytes <= (big ? 8u : 4u) ? valOff : (big ? r.u64(valOff) : r.u32(valOff));
            if (!r.inRange(dataOff, bytes)) return more(dataOff + bytes);
            auto num = [&](uint64_t k) -> uint64_t {
                uint64_t o = dataOff + k * typeSize(type);
                switch (type) {
                    case 3: return r.u16(o);
                    case 4: return r.u32(o);
                    case 16: return r.u64(o);
                    default: return d[o];
                }
            };
            switch (tag) {
                case 254: reduced = num(0) & 1; break;
                case 256: lv.width = uint32_t(num(0)); break;
                case 257: lv.height = uint32_t(num(0)); break;
                case 258: bits_ = int(num(0)); break;
                case 259: compression_ = int(num(0)); break;
                case 317: predictor_ = int(num(0)); break;
                case 322: lv.tileW = uint32_t(num(0)); break;
                case 323: lv.tileH = uint32_t(num(0)); break;
                case 339: sampleFormat_ = int(num(0)); break;
                case 324:
                    lv.offsets.resize(count);
                    for (uint64_t k = 0; k < count; ++k) lv.offsets[k] = num(k);
                    break;
                case 325:
                    lv.counts.resize(count);
                    for (uint64_t k = 0; k < count; ++k) lv.counts[k] = num(k);
                    break;
                case 33550:
                    if (!reduced && levels_.empty()) {
                        pixelLon_ = r.f64(dataOff);
                        pixelLat_ = r.f64(dataOff + 8);
                    }
                    break;
                case 33922:
                    if (!reduced && levels_.empty()) {
                        originLon_ = r.f64(dataOff + 24);
                        originLat_ = r.f64(dataOff + 32);
                    }
                    break;
            }
        }
        (void)reduced;
        if (lv.tileW == 0 || lv.offsets.empty()) return false;  // stripped TIFFs unsupported
        lv.tilesX = (lv.width + lv.tileW - 1) / lv.tileW;
        lv.tilesY = (lv.height + lv.tileH - 1) / lv.tileH;
        levels_.push_back(std::move(lv));
        uint64_t next = entries + cnt * entSize;
        ifd = big ? r.u64(next) : r.u32(next);
    }
    return !levels_.empty() && pixelLon_ > 0 && compression_ == 8;
}

bool CogFile::readTile(int level, uint32_t tx, uint32_t ty, std::vector<uint8_t>& out) const {
    const Level& lv = levels_[level];
    size_t idx = size_t(ty) * lv.tilesX + tx;
    size_t bps = size_t(bits_ / 8);
    size_t rowBytes = size_t(lv.tileW) * bps;
    out.assign(rowBytes * lv.tileH, 0);
    if (idx >= lv.offsets.size() || lv.counts[idx] == 0) return true;  // sparse tile

    fs::path tilePath = cacheDir_ / ("L" + std::to_string(level) + "_" + std::to_string(tx) + "_" +
                                     std::to_string(ty) + ".z");
    std::vector<uint8_t> comp;
    if (!readFile(tilePath, comp)) {
        auto r = net::get(url_, int64_t(lv.offsets[idx]), int64_t(lv.offsets[idx] + lv.counts[idx] - 1));
        if (!r.ok() || r.body.size() != lv.counts[idx]) return false;
        comp = std::move(r.body);
        writeFileAtomic(tilePath, comp.data(), comp.size());
    }

    thread_local libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    size_t actual = 0;
    if (libdeflate_zlib_decompress(dec, comp.data(), comp.size(), out.data(), out.size(), &actual) !=
        LIBDEFLATE_SUCCESS)
        return false;

    if (predictor_ == 2) {
        for (uint32_t y = 0; y < lv.tileH; ++y) {
            uint8_t* row = &out[y * rowBytes];
            if (bps == 1)
                for (size_t x = 1; x < rowBytes; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
        }
    } else if (predictor_ == 3) {
        // Floating-point predictor: byte-wise differencing over byte planes (MSB plane first).
        std::vector<uint8_t> tmp(rowBytes);
        for (uint32_t y = 0; y < lv.tileH; ++y) {
            uint8_t* row = &out[y * rowBytes];
            for (size_t x = 1; x < rowBytes; ++x) row[x] = uint8_t(row[x] + row[x - 1]);
            for (uint32_t x = 0; x < lv.tileW; ++x)
                for (size_t b = 0; b < bps; ++b) tmp[x * bps + b] = row[(bps - 1 - b) * lv.tileW + x];
            std::memcpy(row, tmp.data(), rowBytes);
        }
    }
    return true;
}

}  // namespace data
