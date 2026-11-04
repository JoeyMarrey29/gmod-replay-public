#pragma once

#include "source/bsp/BspReader.h"

#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>

namespace gmod::source {

// Ray and line-of-sight queries against the map's world triangles (Source space), bucketed
// on a 2D grid. Used to hide name tags behind walls and to place bullet holes where a shot
// would have landed. Visibility ignores glass/fences by default; camera/decal
// queries can include them. Tool textures and 3D sky geometry are excluded.
class WorldTrace {
public:
    explicit WorldTrace(const std::vector<bsp::Triangle>& triangles, bool transparent_surfaces = false);

    struct Hit {
        float point[3]{};
        float normal[3]{0, 0, 1};
        float distance = 0.0f;
    };

    // True when a solid triangle crosses the segment from `a` to `b`.
    bool blocked(const float a[3], const float b[3]) const;
    // The first solid triangle along the ray (`direction` need not be normalised).
    bool trace(const float origin[3], const float direction[3], float max_distance, Hit& hit) const;
    // Fraction of the segment a sphere can travel before touching a surface.
    // Unlike visibility rays this includes edges, vertices and initial overlap.
    float sweep_sphere(const float start[3], const float end[3], float radius) const;
    struct DecalVertex {
        std::array<float, 3> position;
        std::array<float, 2> uv;
    };
    std::vector<DecalVertex> project_decal(const float point[3], const float normal[3],
                                          float width, float height) const;

private:
    struct Tri {
        float a[3];
        float edge1[3];
        float edge2[3];
    };
    static constexpr float Cell = 256.0f;
    static long long key(int cx, int cy) {
        return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned int>(cy);
    }
    // Calls `visit(triangle index)` for the triangles in the cells along the 2D segment, until
    // it returns true.
    template <typename Visit>
    bool walk(const float a[3], const float b[3], Visit&& visit) const;

    std::vector<Tri> triangles_;
    std::vector<std::uint32_t> large_triangles_;
    std::unordered_map<long long, std::vector<std::uint32_t>> grid_;
};

} // namespace gmod::source
