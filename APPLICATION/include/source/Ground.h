#pragma once

#include "source/bsp/BspReader.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gmod::source {

// The floor under a point, for footstep sounds: every upward-facing world triangle,
// bucketed on a 2D grid, carrying the surface name of its material ($surfaceprop).
class GroundMap {
public:
    // `surface_of` maps a material name to a $surfaceprop name ("" when unknown).
    GroundMap(const std::vector<bsp::Triangle>& triangles,
              const std::function<std::string(const std::string&)>& surface_of);

    // Surface name of the highest floor at or below z + step_height at (x, y); empty when
    // no floor is found within reach.
    const std::string& surface_at(float x, float y, float z, float step_height = 24.0f) const;

private:
    struct Tri {
        float a[3];
        float b[3];
        float c[3];
        std::uint16_t surface;
    };
    static constexpr float Cell = 256.0f;
    static long long key(int cx, int cy) {
        return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned int>(cy);
    }

    std::vector<Tri> triangles_;
    std::vector<std::string> surfaces_;
    std::unordered_map<long long, std::vector<std::uint32_t>> grid_;
};

// scripts/surfaceproperties.txt: which footstep sound set each surface uses.
class SurfaceSounds {
public:
    // Reads the script; an unreadable file leaves every surface on "default".
    explicit SurfaceSounds(const std::filesystem::path& script);

    // The surface's step sound name without its ".StepLeft" suffix, lower-case
    // ("concrete", "metalgrate", ...), following the script's `base` chain.
    std::string step_set(const std::string& surface) const;

private:
    struct Surface {
        std::string base;
        std::string step;
    };
    std::unordered_map<std::string, Surface> surfaces_;
};

} // namespace gmod::source
