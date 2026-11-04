#pragma once

#include "demo/DemoData.h"
#include "dx12/GpuAssetCache.h"

#include <map>

namespace gmod::app {

// Store pristine parser output; scene binding and playback receive independent copies.
class ParsedDemoCache {
public:
    const demo::DemoData* find(const std::filesystem::path& path) {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        const auto found = entries_.find(normalized);
        if (found == entries_.end()) return nullptr;
        if (found->second.version != dx12::GpuAssetCache::texture_source_key(normalized, false)) {
            entries_.erase(found);
            return nullptr;
        }
        return &found->second.data;
    }

    void store(const std::filesystem::path& path, const std::string& version, const demo::DemoData& data) {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        if (version != dx12::GpuAssetCache::texture_source_key(normalized, false))
            throw std::runtime_error("The recording changed while it was being parsed: " + path.string());
        entries_.insert_or_assign(normalized, Entry{version, data});
    }

    void clear() { entries_.clear(); }
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string version;
        demo::DemoData data;
    };
    std::map<std::filesystem::path, Entry> entries_;
};

} // namespace gmod::app
