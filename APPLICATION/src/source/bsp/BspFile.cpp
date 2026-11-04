#include "source/bsp/BspFile.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace gmod::source::bsp {
namespace {

constexpr std::size_t HeaderBytes = 8 + 64 * 16 + 4;

std::int32_t field(const std::vector<std::uint8_t>& header, std::size_t index, std::size_t slot) {
    std::int32_t value = 0;
    std::memcpy(&value, header.data() + 8 + index * 16 + slot * 4, 4);
    return value;
}

std::mutex g_open_mutex;
std::map<std::wstring, std::weak_ptr<const BspFile>> g_open;

} // namespace

std::shared_ptr<const BspFile> BspFile::open(const std::filesystem::path& path) {
    std::lock_guard lock(g_open_mutex);
    auto& slot = g_open[path.wstring()];
    if (auto existing = slot.lock()) return existing;
    std::shared_ptr<const BspFile> file(new BspFile(path));
    slot = file;
    return file;
}

BspFile::BspFile(std::filesystem::path path) : path_(std::move(path)) {
    if (!vfs::exists(path_)) throw std::runtime_error("Could not open BSP: " + path_.string());
    header_ = vfs::read_range(path_, 0, HeaderBytes);
    if (std::memcmp(header_.data(), "VBSP", 4) != 0) throw std::runtime_error("BSP magic is not VBSP.");
    for (std::size_t index = 0; index < decoded_.size(); ++index) {
        const auto offset = field(header_, index, 0);
        const auto length = field(header_, index, 1);
        const auto unpacked = field(header_, index, 3);  // fourCC: the size before compression
        if (unpacked <= 0 || length < 17 || offset < 0) continue;
        auto file_path = path_;
        decoded_[index] = std::async(std::launch::async, [file_path, offset, length] {
            const auto packed = vfs::read_range(file_path, static_cast<std::uint64_t>(offset),
                                                static_cast<std::size_t>(length));
            if (!vfs::is_valve_lzma(packed)) return std::make_shared<const std::vector<std::uint8_t>>(packed);
            return std::make_shared<const std::vector<std::uint8_t>>(
                vfs::valve_lzma_decode(packed.data(), packed.size()));
        }).share();
    }
}

std::vector<std::uint8_t> BspFile::lump(std::size_t index) const {
    if (index >= decoded_.size()) throw std::runtime_error("BSP lump index is out of range.");
    if (decoded_[index].valid()) return *decoded_[index].get();
    const auto offset = field(header_, index, 0);
    const auto length = field(header_, index, 1);
    if (offset < 0 || length < 0) throw std::runtime_error("BSP contains a negative lump range.");
    if (length == 0) return {};
    auto bytes = vfs::read_range(path_, static_cast<std::uint64_t>(offset), static_cast<std::size_t>(length));
    if (vfs::is_valve_lzma(bytes)) return vfs::valve_lzma_decode(bytes.data(), bytes.size());
    return bytes;
}

std::pair<std::uint64_t, std::uint64_t> BspFile::lump_location(std::size_t index) const {
    if (index >= decoded_.size()) throw std::runtime_error("BSP lump index is out of range.");
    return {static_cast<std::uint64_t>(std::max(field(header_, index, 0), 0)),
            static_cast<std::uint64_t>(std::max(field(header_, index, 1), 0))};
}

std::vector<std::uint8_t> BspFile::range(std::uint64_t offset, std::size_t length) const {
    auto bytes = vfs::read_range(path_, offset, length);
    if (vfs::is_valve_lzma(bytes)) return vfs::valve_lzma_decode(bytes.data(), bytes.size());
    return bytes;
}

} // namespace gmod::source::bsp
