#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace gmod::dx12 {

// App-session ownership of immutable resources only. Accessed on the render/UI thread.
class GpuAssetCache {
public:
    using Resource = Microsoft::WRL::ComPtr<ID3D12Resource>;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t buffer_hits = 0;

    static std::string content_key(const void* data, std::size_t size, const std::string& description) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Could not create GPU cache content hash.");
        const auto* bytes = static_cast<const unsigned char*>(data);
        while (size > 0) {
            const auto count = static_cast<ULONG>(std::min<std::size_t>(size, std::numeric_limits<ULONG>::max()));
            const auto result = BCryptHashData(hash, const_cast<PUCHAR>(bytes), count, 0);
            if (result < 0) {
                BCryptDestroyHash(hash);
                throw std::runtime_error("Could not hash GPU cache content.");
            }
            bytes += count;
            size -= count;
        }
        std::array<unsigned char, 32> digest{};
        const auto result = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        BCryptDestroyHash(hash);
        if (result < 0) throw std::runtime_error("Could not finish GPU cache content hash.");
        return description + ":" + std::string(reinterpret_cast<const char*>(digest.data()), digest.size());
    }

    // Virtual archive paths have a real container file somewhere above their entry path.
    static std::string texture_source_key(const std::filesystem::path& path, bool png) {
        auto container = path;
        std::error_code error;
        while (!container.empty() && !std::filesystem::is_regular_file(container, error)) {
            error.clear();
            const auto parent = container.parent_path();
            if (parent == container) break;
            container = parent;
        }
        if (container.empty() || !std::filesystem::is_regular_file(container))
            throw std::runtime_error("GPU cache texture container is unavailable: " + path.string());
        return "file:" + path.lexically_normal().string() + ":" + std::to_string(png)
            + ":" + std::to_string(std::filesystem::file_size(container))
            + ":" + std::to_string(std::filesystem::last_write_time(container).time_since_epoch().count());
    }

    Resource find(const std::string& key) {
        const auto entry = resources_.find(key);
        if (entry == resources_.end()) {
            ++misses;
            return {};
        }
        ++hits;
        if (key.starts_with("buffer:")) ++buffer_hits;
        return entry->second;
    }

    void store(const std::string& key, const Resource& resource) {
        if (!resource) throw std::runtime_error("Cannot cache an empty GPU asset.");
        resources_.insert_or_assign(key, resource);
    }

    std::size_t size() const { return resources_.size(); }

    void clear() {
        resources_.clear();
        device.Reset();
        adapter.Reset();
        hits = misses = buffer_hits = 0;
    }

private:
    std::map<std::string, Resource> resources_;
};

} // namespace gmod::dx12
