#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gmod::source::vtf {

struct Texture {
    int version_major = 0;
    int version_minor = 0;
    int width = 0;
    int height = 0;
    int frames = 0;
    int mip_count = 0;
    int high_res_format = 0;
    int low_res_format = 0;

    bool supports_direct_rgba_decode() const noexcept;
};

enum class PixelFormat { Rgba8, Bc1, Bc2, Bc3, Rgb9E5 };

// For Rgba8 and Rgb9E5, `rgba` is one tightly packed top mip. For Bc1/Bc2/Bc3 it
// holds the original DXT blocks of every mip, largest first, each mip tightly packed.
struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    PixelFormat format = PixelFormat::Rgba8;
    int mip_levels = 1;
};

class VtfReader {
public:
    explicit VtfReader(std::filesystem::path path);
    Texture read() const;
    Pixels read_top_mip() const;
    // DXT1/3/5 textures keep their stored blocks and full mip chain; every
    // other format falls back to read_top_mip().
    Pixels read_native() const;

private:
    std::filesystem::path path_;
};

} // namespace gmod::source::vtf
