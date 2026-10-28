#include "data/Wardrobe.h"

#include <libdeflate.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "data/Http.h"

namespace wardrobe {
namespace fs = std::filesystem;
namespace {

constexpr uint64_t kChunk = 8ull << 20;  // download request size
// The package unpacks into these directories of assets/baked, nowhere else.
const char* const kTargets[] = {"human/extra/", "mblab/female/extra/", "mblab/male/extra/"};

uint32_t u16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t u32(const uint8_t* p) { return u16(p) | u16(p + 2) << 16; }

bool safeName(const std::string& name) {
    bool target = false;
    for (const char* t : kTargets) target |= name.rfind(t, 0) == 0;
    return target && name.find("..") == std::string::npos && name.find(':') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

}  // namespace

Installer::Installer(fs::path assets, fs::path cache) : assets_(std::move(assets)), cache_(std::move(cache)) {
    std::ifstream f(assets_ / "wardrobe" / "package.json");
    if (!f) return;
    auto j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    url_ = j.value("url", std::string());
    bytes_ = j.value("bytes", uint64_t(0));
    version_ = j.value("version", 0);
    if (url_.rfind("https://", 0) != 0) bytes_ = 0;
}

Installer::~Installer() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

bool Installer::installed() const {
    std::ifstream f(assets_ / "baked" / "human" / "extra" / "wardrobe.json");
    if (!f) return false;
    auto j = nlohmann::json::parse(f, nullptr, false);
    return !j.is_discarded() && j.value("version", 0) == version_ &&
           fs::exists(assets_ / "baked" / "mblab" / "female" / "extra" / "human.json") &&
           fs::exists(assets_ / "baked" / "mblab" / "male" / "extra" / "human.json");
}

void Installer::start() {
    if (busy() || !available()) return;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard lock(mutex_);
        error_.clear();
    }
    done_ = 0;
    total_ = bytes_;
    phase_ = Phase::Downloading;
    thread_ = std::thread([this] { run(); });
}

float Installer::progress() const {
    uint64_t t = total_;
    return t ? float(double(done_) / double(t)) : 0.0f;
}

std::string Installer::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void Installer::fail(const std::string& message) {
    std::fprintf(stderr, "Wardrobe: %s\n", message.c_str());
    {
        std::lock_guard lock(mutex_);
        error_ = message;
    }
    phase_ = Phase::Failed;
}

void Installer::run() {
    fs::path zip = cache_ / ("wardrobe-v" + std::to_string(version_) + ".zip");
    std::error_code ec;
    fs::create_directories(cache_, ec);
    if (!fs::exists(zip) && !download(zip)) return;
    phase_ = Phase::Unpacking;
    if (!unpack(zip)) return;
    fs::remove(zip, ec);  // unpacked; a new version downloads again
    phase_ = Phase::Done;
}

bool Installer::download(const fs::path& zip) {
    // Requests of 8 MB into <zip>.part, which keeps what an interrupted run got.
    fs::path part = zip;
    part += ".part";
    std::error_code ec;
    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    if (have > bytes_) {
        fs::remove(part, ec);
        have = 0;
    }
    std::ofstream out(part, std::ios::binary | std::ios::app);
    if (!out) return fail("Kann " + part.string() + " nicht schreiben."), false;
    done_ = have;
    while (have < bytes_) {
        if (stop_) return false;
        uint64_t end = std::min(have + kChunk, bytes_) - 1;
        net::HttpResult r = net::get(url_, int64_t(have), int64_t(end));
        if (!r.ok()) return fail("Download fehlgeschlagen (HTTP " + std::to_string(r.status) + ")."), false;
        if (r.status == 200) {  // the server sent everything instead of the range
            if (have != 0 || r.body.size() != bytes_) return fail("Unerwartete Antwort des Servers."), false;
        } else if (r.body.size() != end - have + 1) {
            return fail("Unvollständige Antwort des Servers."), false;
        }
        out.write(reinterpret_cast<const char*>(r.body.data()), std::streamsize(r.body.size()));
        if (!out) return fail("Kann " + part.string() + " nicht schreiben (Speicherplatz?)."), false;
        have += r.body.size();
        done_ = have;
    }
    out.close();
    fs::rename(part, zip, ec);
    if (ec) return fail("Kann " + zip.string() + " nicht anlegen."), false;
    return true;
}

bool Installer::unpack(const fs::path& zip) {
    std::ifstream in(zip, std::ios::binary);
    if (!in) return fail("Kann " + zip.string() + " nicht lesen."), false;
    auto read = [&](uint64_t offset, size_t n, std::vector<uint8_t>& buf) {
        buf.resize(n);
        in.seekg(std::streamoff(offset));
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(n));
        return bool(in);
    };
    std::error_code ec;
    uint64_t size = fs::file_size(zip, ec);
    // End of central directory: in the last 64 KB (it ends with a comment of up to 65535 bytes).
    std::vector<uint8_t> tail;
    size_t n = size_t(std::min<uint64_t>(size, 65557));
    if (n < 22 || !read(size - n, n, tail)) return fail("Das Paket ist beschädigt."), false;
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0;)
        if (u32(&tail[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return fail("Das Paket ist beschädigt."), false;
    uint32_t entries = u16(&tail[eocd + 10]), cdSize = u32(&tail[eocd + 12]), cdOffset = u32(&tail[eocd + 16]);
    std::vector<uint8_t> cd;
    if (uint64_t(cdOffset) + cdSize > size || !read(cdOffset, cdSize, cd)) return fail("Das Paket ist beschädigt."), false;

    struct Entry { std::string name; uint32_t method, crc, packed, size, local; };
    std::vector<Entry> list;
    uint64_t total = 0;
    for (size_t p = 0, k = 0; k < entries; ++k) {
        if (p + 46 > cd.size() || u32(&cd[p]) != 0x02014b50) return fail("Das Paket ist beschädigt."), false;
        Entry e{std::string(reinterpret_cast<const char*>(&cd[p + 46]), u16(&cd[p + 28])), u16(&cd[p + 10]),
                u32(&cd[p + 16]), u32(&cd[p + 20]), u32(&cd[p + 24]), u32(&cd[p + 42])};
        p += 46 + u16(&cd[p + 28]) + u16(&cd[p + 30]) + u16(&cd[p + 32]);
        if (e.name.empty() || e.name.back() == '/') continue;
        if (!safeName(e.name) || (e.method != 0 && e.method != 8)) return fail("Unerwarteter Inhalt: " + e.name), false;
        total += e.size;
        list.push_back(std::move(e));
    }
    done_ = 0;
    total_ = total;

    // A new version replaces the old one completely; the marker comes last, so an interrupted unpack starts over.
    fs::path baked = assets_ / "baked";
    for (const char* t : kTargets) fs::remove_all(baked / t, ec);
    libdeflate_decompressor* dec = libdeflate_alloc_decompressor();
    std::vector<uint8_t> local, packed, data;
    bool ok = true;
    for (const Entry& e : list) {
        if (stop_) { ok = false; break; }
        if (!read(e.local, 30, local) || u32(local.data()) != 0x04034b50) { ok = false; break; }
        uint64_t start = uint64_t(e.local) + 30 + u16(&local[26]) + u16(&local[28]);
        if (!read(start, e.packed, packed)) { ok = false; break; }
        data.resize(e.size);
        if (e.method == 0) {
            if (e.packed != e.size) { ok = false; break; }
            data = packed;
        } else if (libdeflate_deflate_decompress(dec, packed.data(), packed.size(), data.data(), data.size(), nullptr) !=
                   LIBDEFLATE_SUCCESS) {
            ok = false;
            break;
        }
        if (libdeflate_crc32(0, data.data(), data.size()) != e.crc) { ok = false; break; }
        fs::path target = baked / fs::path(reinterpret_cast<const char8_t*>(e.name.c_str()));
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) {
            libdeflate_free_decompressor(dec);
            return fail("Kann " + target.string() + " nicht schreiben (Speicherplatz?)."), false;
        }
        done_ += e.size;
    }
    libdeflate_free_decompressor(dec);
    if (!ok) {
        for (const char* t : kTargets) fs::remove_all(baked / t, ec);
        in.close();
        if (!stop_) fs::remove(zip, ec);  // damaged: the next attempt downloads it again
        return stop_ ? false : (fail("Das Paket ist beschädigt und wird beim nächsten Versuch neu geladen."), false);
    }
    std::ofstream(baked / "human" / "extra" / "wardrobe.json") << nlohmann::json{{"version", version_}}.dump();
    return true;
}

}  // namespace wardrobe
