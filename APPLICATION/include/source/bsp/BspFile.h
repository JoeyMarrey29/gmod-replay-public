#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <vector>

namespace gmod::source::bsp {

// A map read through the VFS (a loose .bsp or one inside a Workshop .gma). Lumps that Valve
// stored LZMA-compressed are all decoded in parallel in the background as soon as the map is
// opened, so readers asking for them later rarely wait.
class BspFile {
public:
    // Readers of the same map share one instance (and its decoded lumps) while any is alive.
    static std::shared_ptr<const BspFile> open(const std::filesystem::path& path);

    const std::filesystem::path& path() const noexcept { return path_; }
    // dheader_t: ident, version, 64 lump records and the map revision.
    const std::vector<std::uint8_t>& header() const noexcept { return header_; }
    // The lump's contents, decompressed.
    std::vector<std::uint8_t> lump(std::size_t index) const;
    // Byte range of an uncompressed lump inside the file.
    std::pair<std::uint64_t, std::uint64_t> lump_location(std::size_t index) const;
    // Raw bytes at an absolute file offset (game lumps), decompressed when they hold an LZMA block.
    std::vector<std::uint8_t> range(std::uint64_t offset, std::size_t length) const;

private:
    explicit BspFile(std::filesystem::path path);

    std::filesystem::path path_;
    std::vector<std::uint8_t> header_;
    std::array<std::shared_future<std::shared_ptr<const std::vector<std::uint8_t>>>, 64> decoded_;
};

} // namespace gmod::source::bsp
