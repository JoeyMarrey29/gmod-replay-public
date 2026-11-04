#pragma once

#include "demo/DemoData.h"

#include <cstdint>
#include <optional>

namespace gmod::demo {

// Disposable, content-addressed parser snapshots. Playback never owns files in this cache.
class DemoCache {
public:
    struct Usage {
        std::uintmax_t bytes = 0;
        std::size_t entries = 0;
    };

    explicit DemoCache(std::filesystem::path root = default_root());
    static std::filesystem::path default_root();
    static std::string fingerprint(const std::filesystem::path& file,
                                   const std::atomic<bool>& cancel, const ProgressFn& progress = {});
    static std::string key(const std::string& demo_hash, const std::string& parser_hash);

    std::optional<DemoData> load(const std::string& key, const std::filesystem::path& demo,
                                 const std::atomic<bool>& cancel, const ProgressFn& progress);
    bool touch(const std::string& key);
    // Returns false if this recording alone exceeds the limit; it is not written.
    bool store(const std::string& key, const DemoData& data, std::uintmax_t limit,
               const std::atomic<bool>& cancel, const ProgressFn& progress);
    Usage usage() const;
    void trim(std::uintmax_t limit);
    void clear();

private:
    std::filesystem::path root_;
};

} // namespace gmod::demo
