#include "source/WorldTrace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace gmod::source {
namespace {

bool see_through(std::string_view material) {
    if (material.rfind("tools/", 0) == 0 || material.rfind("TOOLS/", 0) == 0) return true;
    for (const char* word : {"glass", "window", "fence", "grate", "chainlink", "wire", "grill"}) {
        for (std::size_t i = 0; i + std::char_traits<char>::length(word) <= material.size(); ++i) {
            bool same = true;
            for (std::size_t j = 0; word[j] != '\0'; ++j) {
                if (std::tolower(static_cast<unsigned char>(material[i + j])) != word[j]) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
    }
    return false;
}

// Moller-Trumbore for a segment: distance along `direction` (parametric t in [0, 1]).
bool hit_triangle(const float a[3], const float edge1[3], const float edge2[3], const float origin[3],
                  const float direction[3], float& t) {
    const float px = direction[1] * edge2[2] - direction[2] * edge2[1];
    const float py = direction[2] * edge2[0] - direction[0] * edge2[2];
    const float pz = direction[0] * edge2[1] - direction[1] * edge2[0];
    const float det = edge1[0] * px + edge1[1] * py + edge1[2] * pz;
    if (std::fabs(det) < 1e-7f) return false;
    const float inverse = 1.0f / det;
    const float tx = origin[0] - a[0];
    const float ty = origin[1] - a[1];
    const float tz = origin[2] - a[2];
    const float u = (tx * px + ty * py + tz * pz) * inverse;
    if (u < 0.0f || u > 1.0f) return false;
    const float qx = ty * edge1[2] - tz * edge1[1];
    const float qy = tz * edge1[0] - tx * edge1[2];
    const float qz = tx * edge1[1] - ty * edge1[0];
    const float v = (direction[0] * qx + direction[1] * qy + direction[2] * qz) * inverse;
    if (v < 0.0f || u + v > 1.0f) return false;
    t = (edge2[0] * qx + edge2[1] * qy + edge2[2] * qz) * inverse;
    return true;
}

} // namespace

WorldTrace::WorldTrace(const std::vector<bsp::Triangle>& triangles, bool transparent_surfaces) {
    triangles_.reserve(triangles.size());
    for (const auto& triangle : triangles) {
        if (triangle.sky_3d || triangle.material.rfind("tools/",0) == 0
            || triangle.material.rfind("TOOLS/",0) == 0
            || (!transparent_surfaces && see_through(triangle.material))) continue;
        Tri tri;
        tri.a[0] = triangle.a.x;
        tri.a[1] = triangle.a.y;
        tri.a[2] = triangle.a.z;
        tri.edge1[0] = triangle.b.x - triangle.a.x;
        tri.edge1[1] = triangle.b.y - triangle.a.y;
        tri.edge1[2] = triangle.b.z - triangle.a.z;
        tri.edge2[0] = triangle.c.x - triangle.a.x;
        tri.edge2[1] = triangle.c.y - triangle.a.y;
        tri.edge2[2] = triangle.c.z - triangle.a.z;
        const auto index = static_cast<std::uint32_t>(triangles_.size());
        triangles_.push_back(tri);
        const float min_x = std::min({triangle.a.x, triangle.b.x, triangle.c.x});
        const float max_x = std::max({triangle.a.x, triangle.b.x, triangle.c.x});
        const float min_y = std::min({triangle.a.y, triangle.b.y, triangle.c.y});
        const float max_y = std::max({triangle.a.y, triangle.b.y, triangle.c.y});
        const int x0 = static_cast<int>(std::floor(min_x / Cell));
        const int x1 = static_cast<int>(std::floor(max_x / Cell));
        const int y0 = static_cast<int>(std::floor(min_y / Cell));
        const int y1 = static_cast<int>(std::floor(max_y / Cell));
        // Keep oversized surfaces outside the grid rather than dropping collision.
        if ((x1 - x0 + 1) * (y1 - y0 + 1) > 400) {
            large_triangles_.push_back(index);
            continue;
        }
        for (int cx = x0; cx <= x1; ++cx) {
            for (int cy = y0; cy <= y1; ++cy) grid_[key(cx, cy)].push_back(index);
        }
    }
}

template <typename Visit>
bool WorldTrace::walk(const float a[3], const float b[3], Visit&& visit) const {
    for (const auto index : large_triangles_) if (visit(index)) return true;
    int cx = static_cast<int>(std::floor(a[0] / Cell));
    int cy = static_cast<int>(std::floor(a[1] / Cell));
    const int end_x = static_cast<int>(std::floor(b[0] / Cell));
    const int end_y = static_cast<int>(std::floor(b[1] / Cell));
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const int step_x = dx > 0 ? 1 : -1;
    const int step_y = dy > 0 ? 1 : -1;
    const float inf = 1e30f;
    float t_max_x = dx == 0 ? inf : ((dx > 0 ? (cx + 1) * Cell : cx * Cell) - a[0]) / dx;
    float t_max_y = dy == 0 ? inf : ((dy > 0 ? (cy + 1) * Cell : cy * Cell) - a[1]) / dy;
    const float t_delta_x = dx == 0 ? inf : std::fabs(Cell / dx);
    const float t_delta_y = dy == 0 ? inf : std::fabs(Cell / dy);
    // A cell count bound keeps a malformed segment from looping.
    const int limit = std::abs(end_x - cx) + std::abs(end_y - cy) + 2;
    for (int n = 0; n < limit; ++n) {
        const auto found = grid_.find(key(cx, cy));
        if (found != grid_.end()) {
            for (const auto index : found->second) {
                if (visit(index)) return true;
            }
        }
        if (cx == end_x && cy == end_y) break;
        if (t_max_x < t_max_y) {
            cx += step_x;
            t_max_x += t_delta_x;
        } else {
            cy += step_y;
            t_max_y += t_delta_y;
        }
    }
    return false;
}

float WorldTrace::sweep_sphere(const float start[3], const float end[3], float radius) const {
    using V = std::array<float,3>;
    const auto sub = [](const V& a, const V& b) { return V{a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
    const auto add = [](const V& a, const V& b) { return V{a[0]+b[0],a[1]+b[1],a[2]+b[2]}; };
    const auto mul = [](const V& a, float s) { return V{a[0]*s,a[1]*s,a[2]*s}; };
    const auto dot = [](const V& a, const V& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    const auto cross = [](const V& a, const V& b) {
        return V{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    };
    const V origin{start[0],start[1],start[2]}, delta{end[0]-start[0],end[1]-start[1],end[2]-start[2]};
    radius = std::max(radius,0.0f);
    const float radius2 = radius*radius;
    float best = 1.0f;
    const auto quadratic = [&](float a, float b, float c, const auto& accept) {
        if (a < 1e-12f) return;
        const float discriminant = b*b-a*c;
        if (discriminant < 0) return;
        const float t = (-b-std::sqrt(discriminant))/a;
        if (t >= 0 && t < best && accept(t)) best = t;
    };
    const auto visit = [&](std::uint32_t index) {
        const auto& tri = triangles_[index];
        const V a{tri.a[0],tri.a[1],tri.a[2]};
        const V e1{tri.edge1[0],tri.edge1[1],tri.edge1[2]};
        const V e2{tri.edge2[0],tri.edge2[1],tri.edge2[2]};
        const V b = add(a,e1), c = add(a,e2);
        V normal = cross(e1,e2);
        const float length2 = dot(normal,normal);
        if (length2 < 1e-12f) return;
        normal = mul(normal,1.0f/std::sqrt(length2));
        const auto inside = [&](const V& p) {
            return dot(cross(e1,sub(p,a)),normal) >= -1e-5f
                && dot(cross(sub(c,b),sub(p,b)),normal) >= -1e-5f
                && dot(cross(sub(a,c),sub(p,c)),normal) >= -1e-5f;
        };
        const float distance = dot(sub(origin,a),normal);
        if (std::fabs(distance) <= radius && inside(sub(origin,mul(normal,distance)))) {
            best = 0;
            return;
        }
        const float speed = dot(delta,normal);
        if (std::fabs(speed) > 1e-8f) {
            for (const float side : {-radius,radius}) {
                const float t = (side-distance)/speed;
                if (t >= 0 && t < best && inside(sub(add(origin,mul(delta,t)),mul(normal,side))))
                    best = t;
            }
        }
        for (const auto& edge : {std::pair{a,b},std::pair{b,c},std::pair{c,a}}) {
            const V axis = sub(edge.second,edge.first), offset = sub(origin,edge.first);
            const float axis2 = dot(axis,axis);
            if (axis2 < 1e-12f) continue;
            const float along = dot(offset,axis)/axis2, travel = dot(delta,axis)/axis2;
            const V perpendicular = sub(offset,mul(axis,along));
            const V velocity = sub(delta,mul(axis,travel));
            const V closest = sub(offset,mul(axis,std::clamp(along,0.0f,1.0f)));
            if (dot(closest,closest) <= radius2) { best = 0; return; }
            quadratic(dot(velocity,velocity),dot(perpendicular,velocity),
                dot(perpendicular,perpendicular)-radius2,[&](float t) {
                    const float position = along+t*travel;
                    return position >= 0 && position <= 1;
                });
            quadratic(dot(delta,delta),dot(offset,delta),dot(offset,offset)-radius2,
                [](float) { return true; });
        }
    };
    for (const auto index : large_triangles_) visit(index);
    const int x0 = static_cast<int>(std::floor((std::min(start[0],end[0])-radius)/Cell));
    const int x1 = static_cast<int>(std::floor((std::max(start[0],end[0])+radius)/Cell));
    const int y0 = static_cast<int>(std::floor((std::min(start[1],end[1])-radius)/Cell));
    const int y1 = static_cast<int>(std::floor((std::max(start[1],end[1])+radius)/Cell));
    std::unordered_set<std::uint32_t> visited;
    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            const auto cell = grid_.find(key(x,y));
            if (cell == grid_.end()) continue;
            for (const auto index : cell->second) if (visited.insert(index).second) visit(index);
        }
    }
    return best;
}

bool WorldTrace::blocked(const float a[3], const float b[3]) const {
    const float direction[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    return walk(a, b, [&](std::uint32_t index) {
        const auto& tri = triangles_[index];
        float t = 0.0f;
        // Stay off the very ends so a tag standing against a wall is not hidden by it.
        return hit_triangle(tri.a, tri.edge1, tri.edge2, a, direction, t) && t > 0.02f && t < 0.98f;
    });
}

bool WorldTrace::trace(const float origin[3], const float direction[3], float max_distance, Hit& hit) const {
    const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]
                                   + direction[2] * direction[2]);
    if (length < 1e-6f) return false;
    const float unit[3]{direction[0] / length * max_distance, direction[1] / length * max_distance,
                        direction[2] / length * max_distance};
    const float end[3]{origin[0] + unit[0], origin[1] + unit[1], origin[2] + unit[2]};
    float best = 2.0f;
    const Tri* best_tri = nullptr;
    walk(origin, end, [&](std::uint32_t index) {
        const auto& tri = triangles_[index];
        float t = 0.0f;
        if (hit_triangle(tri.a, tri.edge1, tri.edge2, origin, unit, t) && t > 0.0005f && t < best) {
            best = t;
            best_tri = &tri;
        }
        return false;
    });
    if (!best_tri) return false;
    hit.distance = best * max_distance;
    for (int axis = 0; axis < 3; ++axis) hit.point[axis] = origin[axis] + unit[axis] * best;
    const auto& e1 = best_tri->edge1;
    const auto& e2 = best_tri->edge2;
    float nx = e1[1] * e2[2] - e1[2] * e2[1];
    float ny = e1[2] * e2[0] - e1[0] * e2[2];
    float nz = e1[0] * e2[1] - e1[1] * e2[0];
    const float n_length = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (n_length > 1e-6f) {
        nx /= n_length;
        ny /= n_length;
        nz /= n_length;
    }
    // Face the normal back toward the shooter.
    if (nx * unit[0] + ny * unit[1] + nz * unit[2] > 0.0f) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    hit.normal[0] = nx;
    hit.normal[1] = ny;
    hit.normal[2] = nz;
    return true;
}

std::vector<WorldTrace::DecalVertex> WorldTrace::project_decal(
    const float point[3], const float normal[3], float width, float height) const {
    using V = std::array<float, 3>;
    auto dot = [](const V& a, const V& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
    auto cross = [](const V& a, const V& b) {
        return V{a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
    };
    V n{normal[0], normal[1], normal[2]};
    const float length = std::sqrt(dot(n,n));
    if (length < 0.001f || width <= 0 || height <= 0) return {};
    for (auto& v : n) v /= length;
    auto u = cross(std::fabs(n[2]) > 0.9f ? V{0,1,0} : V{0,0,1}, n);
    const float u_length = std::sqrt(dot(u,u));
    for (auto& v : u) v /= u_length;
    const auto v = cross(n,u);
    const float radius = std::max(width,height);
    std::unordered_set<std::uint32_t> candidates;
    for (int x = static_cast<int>(std::floor((point[0]-radius)/Cell));
         x <= static_cast<int>(std::floor((point[0]+radius)/Cell)); ++x) {
        for (int y = static_cast<int>(std::floor((point[1]-radius)/Cell));
             y <= static_cast<int>(std::floor((point[1]+radius)/Cell)); ++y) {
            const auto found = grid_.find(key(x,y));
            if (found != grid_.end()) candidates.insert(found->second.begin(), found->second.end());
        }
    }
    std::vector<DecalVertex> result;
    for (const auto index : candidates) {
        const auto& tri = triangles_[index];
        V a{tri.a[0],tri.a[1],tri.a[2]};
        V e1{tri.edge1[0],tri.edge1[1],tri.edge1[2]};
        V e2{tri.edge2[0],tri.edge2[1],tri.edge2[2]};
        auto face = cross(e1,e2);
        const float face_length = std::sqrt(dot(face,face));
        if (face_length < 1e-6f || std::fabs(dot(face,n)) / face_length < 0.95f) continue;
        V relative{a[0]-point[0],a[1]-point[1],a[2]-point[2]};
        if (std::fabs(dot(relative,n)) > 1.0f) continue;
        std::vector<V> polygon{a, {a[0]+e1[0],a[1]+e1[1],a[2]+e1[2]},
                                 {a[0]+e2[0],a[1]+e2[1],a[2]+e2[2]}};
        for (int plane = 0; plane < 4 && !polygon.empty(); ++plane) {
            const auto& axis = plane < 2 ? u : v;
            const float extent = (plane < 2 ? width : height) * 0.5f;
            const float sign = plane % 2 == 0 ? 1.0f : -1.0f;
            auto distance = [&](const V& p) {
                return extent - sign*((p[0]-point[0])*axis[0]+(p[1]-point[1])*axis[1]+(p[2]-point[2])*axis[2]);
            };
            std::vector<V> clipped;
            auto previous = polygon.back();
            float previous_d = distance(previous);
            for (const auto& next : polygon) {
                const float next_d = distance(next);
                if ((previous_d >= 0) != (next_d >= 0)) {
                    const float t = previous_d / (previous_d-next_d);
                    clipped.push_back({previous[0]+(next[0]-previous[0])*t,
                                       previous[1]+(next[1]-previous[1])*t,
                                       previous[2]+(next[2]-previous[2])*t});
                }
                if (next_d >= 0) clipped.push_back(next);
                previous = next;
                previous_d = next_d;
            }
            polygon = std::move(clipped);
        }
        auto vertex = [&](const V& p) {
            const V delta{p[0]-point[0],p[1]-point[1],p[2]-point[2]};
            return DecalVertex{{p[0]+n[0]*0.08f,p[1]+n[1]*0.08f,p[2]+n[2]*0.08f},
                                {dot(delta,u)/width+0.5f,0.5f-dot(delta,v)/height}};
        };
        for (std::size_t i = 1; i+1 < polygon.size(); ++i) {
            result.push_back(vertex(polygon[0]));
            result.push_back(vertex(polygon[i]));
            result.push_back(vertex(polygon[i+1]));
        }
    }
    return result;
}

} // namespace gmod::source
