#include "source/model/VvdReader.h"

#include "source/Vfs.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace gmod::source::model {
namespace {

template <typename T>
T read_value(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("VVD is truncated.");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

} // namespace

VvdReader::VvdReader(std::filesystem::path path)
    : path_(std::move(path)) {}

VvdSummary VvdReader::read() const {
    if (!vfs::exists(path_)) {
        throw std::runtime_error("Could not open VVD: " + path_.string());
    }
    const auto bytes = vfs::read(path_);
    if (bytes.size() < 64) {
        throw std::runtime_error("VVD header is incomplete.");
    }

    VvdSummary summary{};
    summary.version = read_value<int32_t>(bytes, 4);
    summary.checksum = read_value<int32_t>(bytes, 8);
    summary.lod0_vertex_count = read_value<int32_t>(bytes, 16);
    const auto vertex_offset = read_value<int32_t>(bytes, 56);
    constexpr std::size_t vertex_size = 48;
    if (summary.lod0_vertex_count < 0 || vertex_offset < 0) {
        throw std::runtime_error("VVD vertex metadata is invalid.");
    }
    const auto count = static_cast<std::size_t>(summary.lod0_vertex_count);
    if (static_cast<std::size_t>(vertex_offset) + count * vertex_size > bytes.size()) {
        throw std::runtime_error("VVD vertex pool exceeds file.");
    }
    summary.vertices.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto base = static_cast<std::size_t>(vertex_offset) + index * vertex_size;
        summary.vertices.push_back({
            {
                read_value<float>(bytes, base + 16),
                read_value<float>(bytes, base + 20),
                read_value<float>(bytes, base + 24)
            },
            {
                read_value<float>(bytes, base + 28),
                read_value<float>(bytes, base + 32),
                read_value<float>(bytes, base + 36)
            },
            {
                read_value<float>(bytes, base + 40),
                read_value<float>(bytes, base + 44)
            },
            {
                read_value<float>(bytes, base),
                read_value<float>(bytes, base + 4),
                read_value<float>(bytes, base + 8)
            },
            {
                read_value<std::uint8_t>(bytes, base + 12),
                read_value<std::uint8_t>(bytes, base + 13),
                read_value<std::uint8_t>(bytes, base + 14)
            },
            read_value<std::uint8_t>(bytes, base + 15)
        });
    }
    const auto fixup_count = read_value<int32_t>(bytes, 48);
    const auto fixup_offset = read_value<int32_t>(bytes, 52);
    if (fixup_count < 0 || fixup_offset < 0) {
        throw std::runtime_error("VVD fixup metadata is invalid.");
    }
    if (fixup_count > 0) {
        std::vector<ModelVertex> fixed;
        fixed.reserve(count);
        for (int32_t index = 0; index < fixup_count; ++index) {
            const auto base = static_cast<std::size_t>(fixup_offset) +
                              static_cast<std::size_t>(index) * 12;
            const auto lod = read_value<int32_t>(bytes, base);
            const auto source = read_value<int32_t>(bytes, base + 4);
            const auto length = read_value<int32_t>(bytes, base + 8);
            if (lod < 0 || source < 0 || length < 0 ||
                static_cast<std::size_t>(source) + static_cast<std::size_t>(length) >
                    summary.vertices.size()) {
                throw std::runtime_error("VVD fixup range is invalid.");
            }
            if (lod >= 0) {
                fixed.insert(fixed.end(), summary.vertices.begin() + source,
                             summary.vertices.begin() + source + length);
            }
        }
        if (fixed.size() != count) {
            throw std::runtime_error("VVD fixups did not reconstruct LOD 0.");
        }
        summary.vertices = std::move(fixed);
    }
    return summary;
}

} // namespace gmod::source::model
