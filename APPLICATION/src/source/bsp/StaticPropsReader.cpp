#include "source/bsp/StaticPropsReader.h"

#include "source/bsp/BspFile.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace gmod::source::bsp {
namespace {

constexpr std::size_t GameLump = 35;

template <typename T>
T read_value(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("Static-prop lump is truncated.");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

} // namespace

StaticPropsReader::StaticPropsReader(std::filesystem::path path)
    : path_(std::move(path)) {}

StaticPropsSummary StaticPropsReader::read() {
    const auto file = BspFile::open(path_);
    const auto& header = file->header();

    const auto game_offset = read_value<int32_t>(header, 8 + GameLump * 16);
    const auto game_length = read_value<int32_t>(header, 12 + GameLump * 16);
    if (game_offset < 0 || game_length < 0) {
        throw std::runtime_error("Static-prop lump has invalid bounds.");
    }
    const auto game = file->lump(GameLump);
    const auto count = read_value<int32_t>(game, 0);
    if (count < 0) {
        throw std::runtime_error("BSP game-lump count is invalid.");
    }

    int32_t prop_offset = -1;
    int32_t prop_length = -1;
    uint16_t prop_version = 0;
    for (int32_t index = 0; index < count; ++index) {
        const auto base = 4 + static_cast<std::size_t>(index) * 16;
        if (base + 16 > game.size()) {
            throw std::runtime_error("BSP game-lump directory is truncated.");
        }
        const auto id = read_value<std::array<char, 4>>(game, base);
        const auto id_text = std::string(id.data(), id.size());
        const auto version = read_value<uint16_t>(game, base + 6);
        if (id_text == "prps" || id_text == "sprp") {
            prop_version = version;
            prop_offset = read_value<int32_t>(game, base + 8);
            prop_length = read_value<int32_t>(game, base + 12);
            break;
        }
    }

    if (prop_offset < 0) {
        return {};
    }
    const auto prop_lump = file->range(static_cast<std::uint64_t>(prop_offset), static_cast<std::size_t>(std::max(prop_length, 0)));
    std::size_t cursor = 0;
    const auto model_count = read_value<int32_t>(prop_lump, cursor);
    cursor += 4;
    if (model_count < 0) {
        throw std::runtime_error("Static-prop model count is invalid.");
    }
    std::vector<std::string> models;
    models.reserve(static_cast<std::size_t>(model_count));
    for (int32_t index = 0; index < model_count; ++index) {
        if (cursor + 128 > prop_lump.size()) {
            throw std::runtime_error("Static-prop model dictionary is truncated.");
        }
        const auto* start = reinterpret_cast<const char*>(prop_lump.data() + cursor);
        const auto* end = static_cast<const char*>(std::memchr(start, '\0', 128));
        models.emplace_back(start, end ? end : start + 128);
        cursor += 128;
    }

    const auto leaf_count = read_value<int32_t>(prop_lump, cursor);
    cursor += 4;
    if (leaf_count < 0 || cursor + static_cast<std::size_t>(leaf_count) * 2
        > prop_lump.size()) {
        throw std::runtime_error("Static-prop leaf array is invalid.");
    }
    cursor += static_cast<std::size_t>(leaf_count) * 2;
    const auto prop_count = read_value<int32_t>(prop_lump, cursor);
    cursor += 4;
    if (prop_count < 0 || prop_count == 0) {
        props_.clear();
        return {models.size(), 0, 0};
    }
    const auto entry_size = (prop_lump.size() - cursor)
        / static_cast<std::size_t>(prop_count);
    if (entry_size < 56) {
        throw std::runtime_error("Static-prop entry size is invalid.");
    }

    props_.clear();
    props_.reserve(static_cast<std::size_t>(prop_count));
    std::size_t invalid = 0;
    for (int32_t index = 0; index < prop_count; ++index) {
        const auto base = cursor + static_cast<std::size_t>(index) * entry_size;
        StaticProp prop{};
        for (int component = 0; component < 3; ++component) {
            prop.origin[component] = read_value<float>(prop_lump, base + component * 4);
            prop.angles[component] =
                read_value<float>(prop_lump, base + 12 + component * 4);
        }
        const auto model_index = read_value<uint16_t>(prop_lump, base + 24);
        if (model_index >= models.size()) {
            ++invalid;
        } else {
            prop.model = models[model_index];
        }
        prop.skin = read_value<int32_t>(prop_lump, base + 32);
        if (entry_size >= 44) {
            prop.fade_min = read_value<float>(prop_lump, base + 36);
            prop.fade_max = read_value<float>(prop_lump, base + 40);
        }
        props_.push_back(std::move(prop));
    }
    (void)prop_version;
    return {models.size(), props_.size(), invalid};
}

const std::vector<StaticProp>& StaticPropsReader::props() const noexcept {
    return props_;
}

} // namespace gmod::source::bsp
