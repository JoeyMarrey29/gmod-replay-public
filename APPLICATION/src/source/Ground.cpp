#include "source/Ground.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

namespace gmod::source {
namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const std::string EmptySurface;

} // namespace

GroundMap::GroundMap(const std::vector<bsp::Triangle>& triangles,
                     const std::function<std::string(const std::string&)>& surface_of) {
    std::unordered_map<std::string, std::uint16_t> surface_ids;
    std::unordered_map<std::string, std::uint16_t> by_material;
    surfaces_.emplace_back();
    surface_ids.emplace("", 0);
    for (const auto& triangle : triangles) {
        if (triangle.sky_3d) continue;
        const float ux = triangle.b.x - triangle.a.x, uy = triangle.b.y - triangle.a.y,
                    uz = triangle.b.z - triangle.a.z;
        const float vx = triangle.c.x - triangle.a.x, vy = triangle.c.y - triangle.a.y,
                    vz = triangle.c.z - triangle.a.z;
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        // Floors only: the normal's z is positive (either winding) and steep faces are walls.
        if (length < 1e-3f || std::fabs(nz) / length < 0.6f) continue;

        auto id = by_material.find(triangle.material);
        if (id == by_material.end()) {
            const auto surface = lower(surface_of(triangle.material));
            auto [slot, added] = surface_ids.emplace(
                surface, static_cast<std::uint16_t>(surfaces_.size()));
            if (added) surfaces_.push_back(surface);
            id = by_material.emplace(triangle.material, slot->second).first;
        }
        Tri tri{{triangle.a.x, triangle.a.y, triangle.a.z},
                {triangle.b.x, triangle.b.y, triangle.b.z},
                {triangle.c.x, triangle.c.y, triangle.c.z},
                id->second};
        const auto index = static_cast<std::uint32_t>(triangles_.size());
        triangles_.push_back(tri);
        const float low_x = std::min({tri.a[0], tri.b[0], tri.c[0]});
        const float high_x = std::max({tri.a[0], tri.b[0], tri.c[0]});
        const float low_y = std::min({tri.a[1], tri.b[1], tri.c[1]});
        const float high_y = std::max({tri.a[1], tri.b[1], tri.c[1]});
        for (int cx = static_cast<int>(std::floor(low_x / Cell)); cx <= static_cast<int>(std::floor(high_x / Cell)); ++cx) {
            for (int cy = static_cast<int>(std::floor(low_y / Cell)); cy <= static_cast<int>(std::floor(high_y / Cell)); ++cy) {
                grid_[key(cx, cy)].push_back(index);
            }
        }
    }
}

const std::string& GroundMap::surface_at(float x, float y, float z, float step_height) const {
    const auto bucket = grid_.find(key(static_cast<int>(std::floor(x / Cell)),
                                       static_cast<int>(std::floor(y / Cell))));
    if (bucket == grid_.end()) return EmptySurface;
    float best_height = -1e30f;
    std::uint16_t best = 0;
    bool found = false;
    for (const auto index : bucket->second) {
        const auto& t = triangles_[index];
        // Barycentric test in the XY plane.
        const float d = (t.b[1] - t.c[1]) * (t.a[0] - t.c[0]) + (t.c[0] - t.b[0]) * (t.a[1] - t.c[1]);
        if (std::fabs(d) < 1e-6f) continue;
        const float l1 = ((t.b[1] - t.c[1]) * (x - t.c[0]) + (t.c[0] - t.b[0]) * (y - t.c[1])) / d;
        const float l2 = ((t.c[1] - t.a[1]) * (x - t.c[0]) + (t.a[0] - t.c[0]) * (y - t.c[1])) / d;
        const float l3 = 1.0f - l1 - l2;
        constexpr float Edge = -0.01f;
        if (l1 < Edge || l2 < Edge || l3 < Edge) continue;
        const float height = l1 * t.a[2] + l2 * t.b[2] + l3 * t.c[2];
        if (height > z + step_height || height <= best_height) continue;
        best_height = height;
        best = t.surface;
        found = true;
    }
    return found ? surfaces_[best] : EmptySurface;
}

SurfaceSounds::SurfaceSounds(const std::filesystem::path& script) {
    if (script.empty() || !vfs::exists(script)) return;
    const auto bytes = vfs::read(script);
    const std::string text(bytes.begin(), bytes.end());

    // Tokens are quoted strings and braces; "//" comments run to the end of the line.
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '"') {
            const auto end = text.find('"', i + 1);
            if (end == std::string::npos) break;
            tokens.push_back(text.substr(i + 1, end - i - 1));
            i = end + 1;
        } else if (c == '{' || c == '}') {
            tokens.push_back(std::string(1, c));
            ++i;
        } else {
            ++i;
        }
    }
    // Top level: "name" { "key" "value" ... }.
    for (std::size_t i = 0; i + 1 < tokens.size();) {
        if (tokens[i + 1] != "{") {
            ++i;
            continue;
        }
        const auto name = lower(tokens[i]);
        Surface surface;
        std::size_t at = i + 2;
        while (at < tokens.size() && tokens[at] != "}") {
            if (at + 1 < tokens.size() && tokens[at + 1] != "{" && tokens[at + 1] != "}") {
                const auto key = lower(tokens[at]);
                if (key == "base") surface.base = lower(tokens[at + 1]);
                else if (key == "stepleft") surface.step = lower(tokens[at + 1]);
                at += 2;
            } else {
                ++at;
            }
        }
        surfaces_[name] = std::move(surface);
        i = at + 1;
    }
}

std::string SurfaceSounds::step_set(const std::string& surface) const {
    std::string name = surface.empty() ? "default" : lower(surface);
    for (int depth = 0; depth < 8; ++depth) {
        const auto found = surfaces_.find(name);
        if (found == surfaces_.end()) break;
        if (!found->second.step.empty()) {
            auto step = found->second.step;
            const auto dot = step.find('.');
            return dot == std::string::npos ? step : step.substr(0, dot);
        }
        if (found->second.base.empty()) break;
        name = found->second.base;
    }
    return "default";
}

} // namespace gmod::source
