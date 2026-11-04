#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

namespace gmod::source::bsp {

struct ModelFaceRange {
    std::int32_t first = 0;
    std::int32_t count = 0;
};

// A dmodel_t with no faces is not renderable. Do not infer its range from the
// next model's firstface: model face boundaries can be shared or unrelated.
inline std::optional<ModelFaceRange> model_face_range(
    const std::vector<std::uint8_t>& models, std::size_t model_index,
    std::size_t face_count) {
    constexpr std::size_t ModelSize = 48;
    constexpr std::size_t FirstFaceOffset = 40;
    constexpr std::size_t FaceCountOffset = 44;
    if (models.size() % ModelSize != 0 || model_index >= models.size() / ModelSize
        || face_count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::nullopt;
    }

    auto read_i32 = [&](std::size_t offset) {
        std::int32_t value = 0;
        std::memcpy(&value, models.data() + offset, sizeof(value));
        return value;
    };
    const std::size_t record = model_index * ModelSize;
    const auto first = read_i32(record + FirstFaceOffset);
    const auto count = read_i32(record + FaceCountOffset);
    if (first < 0 || count < 0 || static_cast<std::size_t>(first) > face_count) {
        return std::nullopt;
    }
    if (count == 0 || static_cast<std::size_t>(first) + static_cast<std::size_t>(count) > face_count) {
        return std::nullopt;
    }
    return ModelFaceRange{first, count};
}

} // namespace gmod::source::bsp
