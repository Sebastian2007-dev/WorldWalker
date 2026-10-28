#pragma once
// The wardrobe: about 300 garments, shoes, underwear and accessories from the MakeHuman community asset packs, baked
// for the bodies (tools/install_asset_packs.py, tools/blender/bake_mblab_proxies.py) and published as one zip
// (assets/wardrobe/package.json). It is not under version control: the game downloads it in the background when the
// player asks for it and unpacks it into the bakes' extra/ directories, which render::Human loads.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace wardrobe {

class Installer {
public:
    enum class Phase { Idle, Downloading, Unpacking, Done, Failed };

    // assets: the assets directory (package.json, baked/); cache: where the download is kept until unpacked.
    Installer(std::filesystem::path assets, std::filesystem::path cache);
    ~Installer();  // stops a running download and waits for it

    bool available() const { return bytes_ > 0; }  // a package is described
    bool installed() const;                        // its version is unpacked
    uint64_t bytes() const { return bytes_; }      // download size

    void start();  // downloads (resuming a partial download) and unpacks in the background
    Phase phase() const { return phase_; }
    bool busy() const { return phase_ == Phase::Downloading || phase_ == Phase::Unpacking; }
    float progress() const;  // 0..1 within the current phase
    std::string error() const;

private:
    void run();
    bool download(const std::filesystem::path& zip);
    bool unpack(const std::filesystem::path& zip);
    void fail(const std::string& message);

    std::filesystem::path assets_, cache_;
    std::string url_;
    uint64_t bytes_ = 0;
    int version_ = 0;
    std::thread thread_;
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<uint64_t> done_{0}, total_{0};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::string error_;
};

}  // namespace wardrobe
