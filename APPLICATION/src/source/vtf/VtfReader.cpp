#include "source/vtf/VtfReader.h"

#include "source/Vfs.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace gmod::source::vtf {
namespace {

template <typename T>
T read_value(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("VTF data is truncated.");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::array<std::uint8_t, 4> rgb565(std::uint16_t value) {
    return {
        static_cast<std::uint8_t>(((value >> 11) & 31) * 255 / 31),
        static_cast<std::uint8_t>(((value >> 5) & 63) * 255 / 63),
        static_cast<std::uint8_t>((value & 31) * 255 / 31),
        255
    };
}

std::array<std::array<std::uint8_t, 4>, 4> color_block(
    std::uint16_t c0, std::uint16_t c1, bool dxt1_alpha) {
    std::array<std::array<std::uint8_t, 4>, 4> colors{};
    colors[0] = rgb565(c0);
    colors[1] = rgb565(c1);
    if (dxt1_alpha && c0 <= c1) {
        for (int channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>(
                (colors[0][channel] + colors[1][channel]) / 2);
        }
        colors[3] = {0, 0, 0, 0};
    } else {
        for (int channel = 0; channel < 3; ++channel) {
            colors[2][channel] = static_cast<std::uint8_t>(
                (2 * colors[0][channel] + colors[1][channel]) / 3);
            colors[3][channel] = static_cast<std::uint8_t>(
                (colors[0][channel] + 2 * colors[1][channel]) / 3);
        }
    }
    return colors;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    return vfs::read(path);
}

std::size_t mip_byte_size(int format, int width, int height) {
    if (format >= 13 && format <= 15) {
        return static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4)
            * (format == 13 ? 8 : 16);
    }
    return static_cast<std::size_t>(width) * height * (format == 3 ? 3 : 4);
}

// Offset of frame 0 / face 0 of the given mip. VTF stores the smallest mip
// first, and every mip holds all frames and faces before the next mip starts.
std::size_t mip_offset(
    const std::vector<std::uint8_t>& bytes, const Texture& texture, int target_mip) {
    const auto header_size = read_value<int32_t>(bytes, 12);
    const auto flags = read_value<std::uint32_t>(bytes, 20);
    const auto low_width = read_value<std::uint8_t>(bytes, 61);
    const auto low_height = read_value<std::uint8_t>(bytes, 62);
    const auto low_bytes = texture.low_res_format == 13
        ? ((low_width + 3) / 4) * ((low_height + 3) / 4) * 8
        : low_width * low_height * 4;
    const std::size_t faces = (flags & 0x4000) != 0
        ? (texture.version_major == 7 && texture.version_minor < 5 ? 7 : 6) : 1;
    const std::size_t images = static_cast<std::size_t>(std::max(1, texture.frames)) * faces;
    std::size_t offset = static_cast<std::size_t>(header_size) + low_bytes;
    for (int mip = texture.mip_count - 1; mip > target_mip; --mip) {
        offset += mip_byte_size(texture.high_res_format,
                                std::max(1, texture.width >> mip),
                                std::max(1, texture.height >> mip)) * images;
    }
    return offset;
}

} // namespace

bool Texture::supports_direct_rgba_decode() const noexcept {
    return high_res_format == 0
        || high_res_format == 1
        || high_res_format == 3
        || high_res_format == 12
        || high_res_format == 13
        || high_res_format == 14
        || high_res_format == 15;
}

VtfReader::VtfReader(std::filesystem::path path)
    : path_(std::move(path)) {}

Texture VtfReader::read() const {
    const auto size = vfs::file_size(path_);
    if (!size) {
        throw std::runtime_error("Could not open VTF: " + path_.string());
    }
    const auto bytes = vfs::read_range(path_, 0, static_cast<std::size_t>(std::min<std::uint64_t>(*size, 80)));
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "VTF\0", 4) != 0) {
        throw std::runtime_error("Invalid VTF signature or header.");
    }
    Texture texture{};
    texture.version_major = read_value<int32_t>(bytes, 4);
    texture.version_minor = read_value<int32_t>(bytes, 8);
    const auto header_size = read_value<int32_t>(bytes, 12);
    texture.width = read_value<uint16_t>(bytes, 16);
    texture.height = read_value<uint16_t>(bytes, 18);
    texture.frames = read_value<uint16_t>(bytes, 24);
    texture.high_res_format = read_value<int32_t>(bytes, 52);
    texture.mip_count = read_value<std::uint8_t>(bytes, 56);
    texture.low_res_format = read_value<int32_t>(bytes, 57);
    if (header_size < 64 || texture.width <= 0 || texture.height <= 0) {
        throw std::runtime_error("VTF header contains invalid dimensions.");
    }
    return texture;
}

Pixels VtfReader::read_top_mip() const {
    const auto bytes = read_file(path_);
    const auto texture = read();
    const std::size_t pixel_offset = mip_offset(bytes, texture, 0);
    const auto pixel_count = static_cast<std::size_t>(texture.width) * texture.height;
    Pixels result{
        texture.width,
        texture.height,
        std::vector<std::uint8_t>(pixel_count * 4)
    };

    if (texture.high_res_format == 0 || texture.high_res_format == 1 || texture.high_res_format == 3
        || texture.high_res_format == 12) {
        const auto bytes_per_pixel = texture.high_res_format == 3 ? 3u : 4u;
        if (pixel_offset + pixel_count * bytes_per_pixel > bytes.size()) {
            throw std::runtime_error("VTF uncompressed pixels are truncated.");
        }
        for (std::size_t index = 0; index < pixel_count; ++index) {
            const auto source = pixel_offset + index * bytes_per_pixel;
            if (texture.high_res_format == 1) {
                result.rgba[index * 4 + 0] = bytes[source + 3];
                result.rgba[index * 4 + 1] = bytes[source + 2];
                result.rgba[index * 4 + 2] = bytes[source + 1];
                result.rgba[index * 4 + 3] = bytes[source + 0];
            } else if (texture.high_res_format == 12) {
                result.rgba[index * 4 + 0] = bytes[source + 2];
                result.rgba[index * 4 + 1] = bytes[source + 1];
                result.rgba[index * 4 + 2] = bytes[source + 0];
                result.rgba[index * 4 + 3] = bytes[source + 3];
            } else {
                result.rgba[index * 4 + 0] = bytes[source + 0];
                result.rgba[index * 4 + 1] = bytes[source + 1];
                result.rgba[index * 4 + 2] = bytes[source + 2];
                result.rgba[index * 4 + 3] = texture.high_res_format == 3 ? 255 : bytes[source + 3];
            }
        }
        return result;
    }
    if (texture.high_res_format < 13 || texture.high_res_format > 15) {
        throw std::runtime_error("VTF format is not implemented by the RGBA decoder.");
    }

    const auto blocks_x = (texture.width + 3) / 4;
    const auto blocks_y = (texture.height + 3) / 4;
    const auto block_size = texture.high_res_format == 13 ? 8 : 16;
    const auto required = static_cast<std::size_t>(blocks_x) * blocks_y * block_size;
    if (pixel_offset + required > bytes.size()) {
        throw std::runtime_error("VTF DXT1 pixels are truncated.");
    }
    for (int block_y = 0; block_y < blocks_y; ++block_y) {
        for (int block_x = 0; block_x < blocks_x; ++block_x) {
            const auto base = pixel_offset +
                (static_cast<std::size_t>(block_y) * blocks_x + block_x) * block_size;
            const auto color_base = texture.high_res_format == 13 ? base : base + 8;
            const auto c0 = read_value<std::uint16_t>(bytes, color_base);
            const auto c1 = read_value<std::uint16_t>(bytes, color_base + 2);
            const auto colors = color_block(
                c0, c1, texture.high_res_format == 13);
            const auto indices = read_value<std::uint32_t>(bytes, color_base + 4);
            std::array<std::uint8_t, 16> alpha{};
            if (texture.high_res_format == 14) {
                for (int i = 0; i < 8; ++i) {
                    const auto packed = bytes[base + i];
                    alpha[i * 2] = (packed & 0x0f) * 17;
                    alpha[i * 2 + 1] = (packed >> 4) * 17;
                }
            } else if (texture.high_res_format == 15) {
                const auto a0 = bytes[base];
                const auto a1 = bytes[base + 1];
                std::array<std::uint8_t, 8> palette{a0, a1, 0, 0, 0, 0, 0, 0};
                if (a0 > a1) {
                    for (int i = 2; i < 8; ++i) {
                        palette[i] = static_cast<std::uint8_t>(
                            ((8 - i) * a0 + (i - 1) * a1) / 7);
                    }
                } else {
                    for (int i = 2; i < 7; ++i) {
                        palette[i] = static_cast<std::uint8_t>(
                            ((6 - i) * a0 + (i - 1) * a1) / 5);
                    }
                    palette[6] = 0;
                    palette[7] = 255;
                }
                std::uint64_t alpha_indices = 0;
                for (int i = 0; i < 6; ++i) {
                    alpha_indices |= static_cast<std::uint64_t>(bytes[base + 2 + i])
                        << (8 * i);
                }
                for (int i = 0; i < 16; ++i) {
                    alpha[i] = palette[(alpha_indices >> (3 * i)) & 7];
                }
            }
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const auto px = block_x * 4 + x;
                    const auto py = block_y * 4 + y;
                    if (px >= texture.width || py >= texture.height) {
                        continue;
                    }
                    auto color = colors[(indices >> (2 * (y * 4 + x))) & 3];
                    if (texture.high_res_format == 14
                        || texture.high_res_format == 15) {
                        color[3] = alpha[y * 4 + x];
                    }
                    const auto destination =
                        (static_cast<std::size_t>(py) * texture.width + px) * 4;
                    std::copy(color.begin(), color.end(), result.rgba.begin() + destination);
                }
            }
        }
    }
    return result;
}

Pixels VtfReader::read_native() const {
    const auto texture = read();
    const int format = texture.high_res_format;
    if (format < 13 || format > 15 || texture.width % 4 != 0 || texture.height % 4 != 0) {
        return read_top_mip();
    }
    const auto bytes = read_file(path_);
    int largest = std::max(texture.width, texture.height);
    int max_mips = 1;
    while (largest > 1) {
        largest >>= 1;
        ++max_mips;
    }
    const int mips = std::clamp(texture.mip_count, 1, max_mips);
    Pixels result;
    result.width = texture.width;
    result.height = texture.height;
    result.mip_levels = mips;
    result.format = format == 13 ? PixelFormat::Bc1
        : format == 14 ? PixelFormat::Bc2 : PixelFormat::Bc3;
    for (int mip = 0; mip < mips; ++mip) {
        const auto size = mip_byte_size(
            format, std::max(1, texture.width >> mip), std::max(1, texture.height >> mip));
        const auto offset = mip_offset(bytes, texture, mip);
        if (offset + size > bytes.size()) {
            throw std::runtime_error("VTF DXT mip data is truncated.");
        }
        result.rgba.insert(result.rgba.end(), bytes.begin() + offset,
                           bytes.begin() + offset + size);
    }
    return result;
}

} // namespace gmod::source::vtf