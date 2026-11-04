#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <vector>

namespace gmod::ui {

struct ScopeVertex {
    std::array<float, 4> clip{};
    std::array<float, 2> uv{};
    bool operator==(const ScopeVertex&) const = default;
};

struct ScopeProjection {
    std::vector<std::array<ScopeVertex, 3>> triangles;
    bool operator==(const ScopeProjection&) const = default;

    static std::optional<std::array<float, 3>> weights(
        const std::array<std::array<float, 2>, 3>& triangle, float x, float y) {
        const auto& a = triangle[0];
        const auto& b = triangle[1];
        const auto& c = triangle[2];
        const float determinant = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
        if (std::fabs(determinant) < 1e-8f) return std::nullopt;
        const float u = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1])) / determinant;
        const float v = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1])) / determinant;
        if (u < -1e-5f || v < -1e-5f || u+v > 1.00001f) return std::nullopt;
        return std::array<float, 3>{u, v, 1-u-v};
    }

    std::optional<std::array<float, 2>> screen(float u, float v, int width, int height) const {
        for (const auto& triangle : triangles) {
            const auto barycentric = weights({triangle[0].uv, triangle[1].uv, triangle[2].uv}, u, v);
            if (!barycentric) continue;
            std::array<float, 4> clip{};
            for (int i = 0; i < 3; ++i)
                for (int axis = 0; axis < 4; ++axis) clip[axis] += (*barycentric)[i]*triangle[i].clip[axis];
            if (clip[3] <= 0) continue;
            return std::array<float, 2>{(clip[0]/clip[3]+1)*0.5f*width, (1-clip[1]/clip[3])*0.5f*height};
        }
        return std::nullopt;
    }

    std::optional<std::array<float, 2>> texture(float x, float y, int width, int height) const {
        if (width <= 0 || height <= 0) return std::nullopt;
        const float nx = x*2/width-1, ny = 1-y*2/height;
        for (const auto& triangle : triangles) {
            std::array<std::array<float, 2>, 3> projected{};
            bool valid = true;
            for (int i = 0; i < 3; ++i) {
                if (triangle[i].clip[3] <= 0) { valid = false; break; }
                projected[i] = {triangle[i].clip[0]/triangle[i].clip[3], triangle[i].clip[1]/triangle[i].clip[3]};
            }
            if (!valid) continue;
            const auto barycentric = weights(projected, nx, ny);
            if (!barycentric) continue;
            float total = 0;
            std::array<float, 2> uv{};
            for (int i = 0; i < 3; ++i) {
                const float weight = (*barycentric)[i]/triangle[i].clip[3];
                total += weight;
                for (int axis = 0; axis < 2; ++axis) uv[axis] += weight*triangle[i].uv[axis];
            }
            if (total > 0) return std::array<float, 2>{uv[0]/total, uv[1]/total};
        }
        return std::nullopt;
    }
};

} // namespace gmod::ui
