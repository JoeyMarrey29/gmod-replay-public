#pragma once

#include "demo/Perpheads.h"
#include "source/model/Animation.h"

#include <set>
#include <cctype>
#include <stdexcept>
#include <tuple>

namespace gmod::demo {

struct VehiclePlateState {
    std::string id, registration, skin;
    bool operator==(const VehiclePlateState&) const = default;
    bool operator<(const VehiclePlateState& other) const {
        return std::tie(id, registration, skin) < std::tie(other.id, other.registration, other.skin);
    }
};

inline VehiclePlateState vehicle_plate_at(const DemoProp& prop, float time) {
    return {timed_text_at(prop.vehicle_id, time), timed_text_at(prop.registration, time),
            timed_text_at(prop.plate_skin, time)};
}

inline std::set<VehiclePlateState> vehicle_plate_states(const DemoProp& prop) {
    std::set<float> times;
    for (const auto* keys : {&prop.vehicle_id, &prop.registration, &prop.plate_skin})
        for (const auto& key : *keys) times.insert(key.time);
    std::set<VehiclePlateState> states;
    for (float time : times) {
        auto state = vehicle_plate_at(prop, time);
        if (!state.id.empty() && !state.registration.empty()) states.insert(std::move(state));
    }
    return states;
}

struct PlateAppearance {
    std::string model;
    std::vector<std::pair<std::string, std::string>> materials;
    std::array<float, 3> color{29.0f / 255, 36.0f / 255, 71.0f / 255};
};

inline PlateAppearance plate_appearance(const PerpheadsRegistry& registry,
                                       const VehiclePlateState& state) {
    if (state.registration.empty() || state.registration.size() > 7)
        throw std::runtime_error("Invalid recorded vehicle registration: " + state.registration);
    const bool even = state.registration.size() % 2 == 0;
    const std::size_t letters = even ? 6 : 7;
    std::string padded((7 - state.registration.size()) / 2, ' ');
    padded += state.registration;
    padded.resize(letters, ' ');
    PlateAppearance result;
    result.model = std::string("models/jarheads/vehicles/license_plate_skinned") + (even ? "_even" : "");
    const std::string base = "models/jarheads/vehicles/licenseplatesskinned/";
    for (std::size_t i = 0; i < letters; ++i) {
        const char c = padded[i];
        if (c != ' ' && c != '-' && !(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'Z'))
            throw std::runtime_error("Unsupported recorded registration character: " + state.registration);
        const std::string glyph = c == ' ' ? "space" : c == '-' ? "dash"
            : std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        result.materials.emplace_back(base + "char" + std::to_string(i + 1), base + glyph);
    }
    if (!state.skin.empty()) {
        const auto skin = registry.plate_skins.find(state.skin);
        if (skin == registry.plate_skins.end())
            throw std::runtime_error("Unknown recorded vehicle plate skin: " + state.skin);
        result.materials.emplace_back(base + "plate", skin->second.material);
        result.color = skin->second.color;
    }
    return result;
}

inline source::model::Matrix3x4 plate_transform(const PropPose& vehicle, const PlatePlacement& plate) {
    const float origin[]{vehicle.x, vehicle.y, vehicle.z};
    const float angles[]{vehicle.pitch, vehicle.yaw, vehicle.roll};
    const auto parent = source::model::matrix_from_source_angles(origin, angles);
    const auto local = source::model::matrix_from_source_angles(plate.pos.data(), plate.ang.data());
    source::model::Matrix3x4 world{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) {
            for (int k = 0; k < 3; ++k)
                world[row * 4 + col] += parent[row * 4 + k] * local[k * 4 + col];
            if (col == 3) world[row * 4 + col] += parent[row * 4 + 3];
        }
    return world;
}

} // namespace gmod::demo
