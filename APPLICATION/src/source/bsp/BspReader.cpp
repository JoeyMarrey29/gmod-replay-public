#include "source/bsp/BspReader.h"

#include "source/bsp/BspFile.h"
#include "source/bsp/BspModelGeometry.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <set>
#include <unordered_map>

namespace gmod::source::bsp {
namespace {

constexpr std::size_t EntityLump = 0;
constexpr std::size_t VertexLump = 3;
constexpr std::size_t FaceLump = 7;
constexpr std::size_t EdgeLump = 12;
constexpr std::size_t SurfedgeLump = 13;
constexpr std::size_t ModelLump = 14;
constexpr std::size_t TexdataLump = 2;
constexpr std::size_t TexinfoLump = 6;
constexpr std::size_t TextureStringDataLump = 43;
constexpr std::size_t TextureStringTableLump = 44;
constexpr std::size_t DispInfoLump = 26;
constexpr std::size_t DispVertsLump = 33;
constexpr std::size_t DispInfoSize = 176;
constexpr std::size_t DispVertSize = 20;
constexpr std::size_t PlaneLump = 1;
constexpr std::size_t NodeLump = 5;
constexpr std::size_t LeafLump = 10;
constexpr std::size_t VisibilityLump = 4;
constexpr std::size_t LeafFaceLump = 16;
constexpr std::size_t WorldLightLump = 15;
constexpr std::size_t WorldLightSize = 88;
constexpr int32_t EmitSkylight = 3;
constexpr int LeafFlagSky = 0x1;
// SURF_SKY2D | SURF_SKY | SURF_TRIGGER | SURF_NODRAW | SURF_HINT | SURF_SKIP:
// faces the engine never draws.
constexpr int32_t HiddenSurfaceFlags = 0x2 | 0x4 | 0x40 | 0x80 | 0x100 | 0x200;
constexpr std::size_t LightingLump = 8;
// LDR variants: leaf ambient index and leaf ambient lighting samples.
constexpr std::size_t AmbientIndexLump = 52;
constexpr std::size_t AmbientLightingLump = 56;
constexpr int AtlasWidth = 4096;
constexpr int MaxAtlasHeight = 16384;
constexpr int MaxLightmapSide = 256;

struct Edge {
    uint16_t first;
    uint16_t second;
};

struct Face {
    int32_t first_edge;
    int16_t edge_count;
    int16_t texinfo;
    int16_t dispinfo;
    std::array<std::uint8_t, 4> styles;
    int32_t light_offset;
    int32_t lightmap_mins[2];
    int32_t lightmap_size[2];
};

struct LightmapBlock {
    int x = -1;
    int y = -1;
    int width = 0;
    int height = 0;
};

struct DispInfo {
    Vec3 start_position;
    int32_t vert_start;
    int32_t power;
};

struct DispVert {
    Vec3 vector;
    float distance;
    float alpha;
};

template <typename T>
T read_value(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + sizeof(T) > bytes.size()) {
        throw std::runtime_error("BSP structure extends beyond its lump.");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::vector<std::uint8_t> read_lump(const BspFile& file, std::size_t index) {
    return file.lump(index);
}

// Source ColorRGBExp32: three 8-bit mantissas sharing one signed exponent.
// Lightmap luxels are stored in 0..255 light units; ambient samples in 0..1.
float rgbexp_to_linear(std::uint8_t mantissa, std::int8_t exponent) {
    return static_cast<float>(mantissa) * std::ldexp(1.0f, exponent);
}

std::uint32_t encode_rgb9e5(float red, float green, float blue) {
    constexpr int mantissa_bits = 9;
    constexpr int bias = 15;
    constexpr int max_exponent = 31;
    const float max_value = (511.0f / 512.0f) * std::ldexp(1.0f, max_exponent - bias);
    red = std::clamp(red, 0.0f, max_value);
    green = std::clamp(green, 0.0f, max_value);
    blue = std::clamp(blue, 0.0f, max_value);
    const float largest = std::max({red, green, blue});
    if (largest <= 0.0f) {
        return 0;
    }
    int exponent = std::max(-bias - 1, static_cast<int>(std::floor(std::log2(largest)))) + 1 + bias;
    float denominator = std::ldexp(1.0f, exponent - bias - mantissa_bits);
    if (static_cast<int>(std::floor(largest / denominator + 0.5f)) == 512) {
        ++exponent;
        denominator *= 2.0f;
    }
    auto mantissa = [&](float value) {
        return static_cast<std::uint32_t>(std::floor(value / denominator + 0.5f));
    };
    return (static_cast<std::uint32_t>(exponent) << 27) | (mantissa(blue) << 18)
        | (mantissa(green) << 9) | mantissa(red);
}

// Point-to-leaf lookup over the BSP node tree.
struct BspTree {
    static constexpr std::size_t PlaneSize = 20;
    static constexpr std::size_t NodeSize = 32;
    std::vector<std::uint8_t> planes;
    std::vector<std::uint8_t> nodes;
    std::vector<std::uint8_t> leaves;
    std::size_t leaf_size = 32;

    std::size_t leaf_count() const { return leaves.size() / leaf_size; }

    std::size_t leaf_at(const Vec3& point) const {
        const std::size_t node_count = nodes.size() / NodeSize;
        const std::size_t plane_count = planes.size() / PlaneSize;
        int32_t child = 0;
        for (std::size_t steps = 0; child >= 0; ++steps) {
            if (static_cast<std::size_t>(child) >= node_count || steps > node_count) {
                throw std::runtime_error("BSP node tree is invalid.");
            }
            const auto node = static_cast<std::size_t>(child) * NodeSize;
            const auto plane_number = read_value<int32_t>(nodes, node);
            if (plane_number < 0 || static_cast<std::size_t>(plane_number) >= plane_count) {
                throw std::runtime_error("BSP node references an invalid plane.");
            }
            const auto plane = static_cast<std::size_t>(plane_number) * PlaneSize;
            const float distance = point.x * read_value<float>(planes, plane)
                + point.y * read_value<float>(planes, plane + 4)
                + point.z * read_value<float>(planes, plane + 8)
                - read_value<float>(planes, plane + 12);
            child = read_value<int32_t>(nodes, node + (distance < 0.0f ? 8 : 4));
        }
        const auto leaf = static_cast<std::size_t>(-1 - child);
        if (leaf >= leaf_count()) {
            throw std::runtime_error("BSP node references an invalid leaf.");
        }
        return leaf;
    }

    bool clear_segment(const Vec3& a, const Vec3& b, int child=0, std::size_t depth=0) const {
        if (depth > nodes.size()/NodeSize) return false;
        if (child < 0) {
            const auto leaf = static_cast<std::size_t>(-1-child);
            return leaf < leaf_count() && (read_value<int32_t>(leaves,leaf*leaf_size)&1)==0;
        }
        const auto node = static_cast<std::size_t>(child)*NodeSize;
        if (node+NodeSize > nodes.size()) return false;
        const auto plane = static_cast<std::size_t>(read_value<int32_t>(nodes,node))*PlaneSize;
        if (plane+PlaneSize > planes.size()) return false;
        const auto distance = [&](const Vec3& p) {
            return p.x*read_value<float>(planes,plane)+p.y*read_value<float>(planes,plane+4)
                +p.z*read_value<float>(planes,plane+8)-read_value<float>(planes,plane+12);
        };
        const float da=distance(a), db=distance(b);
        const int side=da<0 ? 8 : 4;
        if ((da<0)==(db<0)) return clear_segment(a,b,read_value<int32_t>(nodes,node+side),depth+1);
        const float f=std::clamp(da/(da-db),0.0f,1.0f);
        const Vec3 mid{a.x+(b.x-a.x)*f,a.y+(b.y-a.y)*f,a.z+(b.z-a.z)*f};
        return clear_segment(a,mid,read_value<int32_t>(nodes,node+side),depth+1)
            && clear_segment(mid,b,read_value<int32_t>(nodes,node+(side==4?8:4)),depth+1);
    }

    int cluster(std::size_t leaf) const {
        return read_value<int16_t>(leaves, leaf * leaf_size + 4);
    }
};

BspTree load_tree(const BspFile& file, const std::vector<std::uint8_t>& header) {
    BspTree tree;
    tree.planes = read_lump(file, PlaneLump);
    tree.nodes = read_lump(file, NodeLump);
    tree.leaves = read_lump(file, LeafLump);
    // Lump version 0 leaves embed an ambient cube (56 bytes); later ones don't.
    tree.leaf_size = read_value<int32_t>(header, 8 + LeafLump * 16 + 8) == 0 ? 56 : 32;
    if (tree.planes.size() % BspTree::PlaneSize != 0 || tree.nodes.empty()
        || tree.nodes.size() % BspTree::NodeSize != 0
        || tree.leaves.size() % tree.leaf_size != 0) {
        throw std::runtime_error("BSP tree lumps have unexpected sizes.");
    }
    return tree;
}

std::string entity_value(const std::string& entity, const char* key) {
    const auto token = std::string("\"") + key + "\"";
    const auto key_start = entity.find(token);
    if (key_start == std::string::npos) {
        return {};
    }
    auto value_start = key_start + token.size();
    while (value_start < entity.size()
        && std::isspace(static_cast<unsigned char>(entity[value_start]))) {
        ++value_start;
    }
    if (value_start >= entity.size() || entity[value_start] != '"') {
        return {};
    }
    ++value_start;
    const auto value_end = entity.find('"', value_start);
    return value_end == std::string::npos
        ? std::string{}
        : entity.substr(value_start, value_end - value_start);
}

void read_vector3(const std::string& value, float output[3]) {
    std::istringstream stream(value);
    float parsed[3];
    if (stream >> parsed[0] >> parsed[1] >> parsed[2]) {
        std::copy(parsed, parsed + 3, output);
    }
}

std::optional<SkyCamera> find_sky_camera(const std::string& text) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto begin = text.find('{', cursor);
        if (begin == std::string::npos) {
            break;
        }
        const auto end = text.find('}', begin + 1);
        if (end == std::string::npos) {
            throw std::runtime_error("BSP entity lump has an unterminated entity.");
        }
        cursor = end + 1;
        const auto entity = text.substr(begin + 1, end - begin - 1);
        if (entity_value(entity, "classname") != "sky_camera") {
            continue;
        }
        SkyCamera camera{};
        std::istringstream origin_stream(entity_value(entity, "origin"));
        if (!(origin_stream >> camera.origin.x >> camera.origin.y >> camera.origin.z)) {
            continue;
        }
        const auto scale = entity_value(entity, "scale");
        if (!scale.empty()) {
            camera.scale = std::stof(scale);
        }
        if (camera.scale <= 0.0f) {
            throw std::runtime_error("sky_camera has a non-positive scale.");
        }
        camera.fog_enabled = entity_value(entity, "fogenable") == "1";
        auto number = [&](const char* key, float fallback) {
            const auto text_value = entity_value(entity, key);
            return text_value.empty() ? fallback : std::stof(text_value);
        };
        camera.fog_start = number("fogstart", 0.0f);
        camera.fog_end = number("fogend", 0.0f);
        camera.fog_max_density = number("fogmaxdensity", 1.0f);
        camera.fog_blend = entity_value(entity, "fogblend") == "1";
        // 0..255 colours become the linear-free 0..1 values the engine blends with.
        float color[3]{};
        read_vector3(entity_value(entity, "fogcolor"), color);
        for (int c = 0; c < 3; ++c) camera.fog_color[c] = color[c] / 255.0f;
        float color2[3]{color[0], color[1], color[2]};
        read_vector3(entity_value(entity, "fogcolor2"), color2);
        for (int c = 0; c < 3; ++c) camera.fog_color2[c] = color2[c] / 255.0f;
        read_vector3(entity_value(entity, "fogdir"), camera.fog_direction);
        return camera;
    }
    return std::nullopt;
}

std::optional<SkyPaint> find_sky_paint(const std::string& text) {
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto begin = text.find('{', cursor);
        if (begin == std::string::npos) {
            break;
        }
        const auto end = text.find('}', begin + 1);
        if (end == std::string::npos) {
            throw std::runtime_error("BSP entity lump has an unterminated entity.");
        }
        cursor = end + 1;
        const auto entity = text.substr(begin + 1, end - begin - 1);
        if (entity_value(entity, "classname") != "env_skypaint") {
            continue;
        }
        SkyPaint paint{};
        read_vector3(entity_value(entity, "topcolor"), paint.top);
        read_vector3(entity_value(entity, "bottomcolor"), paint.bottom);
        read_vector3(entity_value(entity, "sunnormal"), paint.sun_normal);
        read_vector3(entity_value(entity, "suncolor"), paint.sun_color);
        const auto size = entity_value(entity, "sunsize");
        if (!size.empty()) {
            paint.sun_size = std::stof(size);
        }
        const auto bias = entity_value(entity, "fadebias");
        if (!bias.empty()) {
            paint.fade_bias = std::stof(bias);
        }
        return paint;
    }
    return std::nullopt;
}

} // namespace

BspReader::BspReader(std::filesystem::path path)
    : path_(std::move(path)) {}

BspSummary BspReader::read() {
    const auto bsp = BspFile::open(path_);
    const BspFile& file = *bsp;
    const auto& header = file.header();

    const auto magic = read_value<std::array<char, 4>>(header, 0);
    if (std::string(magic.data(), magic.size()) != "VBSP") {
        throw std::runtime_error("BSP magic is not VBSP.");
    }
    const auto version = read_value<int32_t>(header, 4);

    const auto vertex_bytes = read_lump(file, VertexLump);
    const auto edge_bytes = read_lump(file, EdgeLump);
    const auto surfedge_bytes = read_lump(file, SurfedgeLump);
    const auto face_bytes = read_lump(file, FaceLump);
    const auto model_bytes = read_lump(file, ModelLump);
    const auto texdata_bytes = read_lump(file, TexdataLump);
    const auto texinfo_bytes = read_lump(file, TexinfoLump);
    const auto texture_string_data = read_lump(file, TextureStringDataLump);
    const auto texture_string_table = read_lump(file, TextureStringTableLump);
    const auto dispinfo_bytes = read_lump(file, DispInfoLump);
    const auto dispvert_bytes = read_lump(file, DispVertsLump);
    if (dispinfo_bytes.size() % DispInfoSize != 0
        || dispvert_bytes.size() % DispVertSize != 0) {
        throw std::runtime_error("BSP displacement lump has an invalid size.");
    }
    std::vector<DispInfo> dispinfos;
    dispinfos.reserve(dispinfo_bytes.size() / DispInfoSize);
    for (std::size_t offset = 0; offset < dispinfo_bytes.size(); offset += DispInfoSize) {
        dispinfos.push_back({
            {read_value<float>(dispinfo_bytes, offset),
             read_value<float>(dispinfo_bytes, offset + 4),
             read_value<float>(dispinfo_bytes, offset + 8)},
            read_value<int32_t>(dispinfo_bytes, offset + 12),
            read_value<int32_t>(dispinfo_bytes, offset + 20)});
    }
    std::vector<DispVert> dispverts;
    dispverts.reserve(dispvert_bytes.size() / DispVertSize);
    for (std::size_t offset = 0; offset < dispvert_bytes.size(); offset += DispVertSize) {
        dispverts.push_back({
            {read_value<float>(dispvert_bytes, offset),
             read_value<float>(dispvert_bytes, offset + 4),
             read_value<float>(dispvert_bytes, offset + 8)},
            read_value<float>(dispvert_bytes, offset + 12),
            read_value<float>(dispvert_bytes, offset + 16)});
    }

    if (vertex_bytes.size() % 12 != 0
        || edge_bytes.size() % 4 != 0
        || surfedge_bytes.size() % 4 != 0
        || face_bytes.size() % 56 != 0
        || model_bytes.size() < 48) {
        throw std::runtime_error("BSP geometry lump has an invalid size.");
    }

    std::vector<Vec3> vertices;
    vertices.reserve(vertex_bytes.size() / 12);
    for (std::size_t offset = 0; offset < vertex_bytes.size(); offset += 12) {
        vertices.push_back({
            read_value<float>(vertex_bytes, offset),
            read_value<float>(vertex_bytes, offset + 4),
            read_value<float>(vertex_bytes, offset + 8)
        });
    }

    std::vector<Edge> edges;
    edges.reserve(edge_bytes.size() / 4);
    for (std::size_t offset = 0; offset < edge_bytes.size(); offset += 4) {
        edges.push_back({
            read_value<uint16_t>(edge_bytes, offset),
            read_value<uint16_t>(edge_bytes, offset + 2)
        });
    }

    std::vector<int32_t> surfedges;
    surfedges.reserve(surfedge_bytes.size() / 4);
    for (std::size_t offset = 0; offset < surfedge_bytes.size(); offset += 4) {
        surfedges.push_back(read_value<int32_t>(surfedge_bytes, offset));
    }

    std::vector<std::string> texture_names;
    for (std::size_t offset = 0; offset + 4 <= texture_string_table.size(); offset += 4) {
        const auto string_offset = read_value<int32_t>(texture_string_table, offset);
        if (string_offset < 0
            || static_cast<std::size_t>(string_offset) >= texture_string_data.size()) {
            texture_names.emplace_back();
            continue;
        }
        const auto* start = reinterpret_cast<const char*>(
            texture_string_data.data() + string_offset);
        const auto* end = static_cast<const char*>(
            std::memchr(start, '\0', texture_string_data.size() - string_offset));
        texture_names.emplace_back(start, end ? end : start);
    }
    std::vector<int32_t> texdata_names;
    std::vector<int32_t> texdata_widths;
    std::vector<int32_t> texdata_heights;
    for (std::size_t offset = 0; offset + 32 <= texdata_bytes.size(); offset += 32) {
        texdata_names.push_back(read_value<int32_t>(texdata_bytes, offset + 12));
        texdata_widths.push_back(read_value<int32_t>(texdata_bytes, offset + 16));
        texdata_heights.push_back(read_value<int32_t>(texdata_bytes, offset + 20));
    }
    std::vector<int32_t> texinfo_texdata;
    std::vector<int32_t> texinfo_flags;
    struct TextureAxes {
        float A[4]{};
        float v[4]{};
        float lightmap_u[4]{};
        float lightmap_v[4]{};
    };
    std::vector<TextureAxes> texture_axes;
    for (std::size_t offset = 0; offset + 72 <= texinfo_bytes.size(); offset += 72) {
        texinfo_texdata.push_back(read_value<int32_t>(texinfo_bytes, offset + 68));
        texinfo_flags.push_back(read_value<int32_t>(texinfo_bytes, offset + 64));
        TextureAxes axes{};
        for (int component = 0; component < 4; ++component) {
            axes.A[component] = read_value<float>(
                texinfo_bytes, offset + component * 4);
            axes.v[component] = read_value<float>(
                texinfo_bytes, offset + 16 + component * 4);
            axes.lightmap_u[component] = read_value<float>(
                texinfo_bytes, offset + 32 + component * 4);
            axes.lightmap_v[component] = read_value<float>(
                texinfo_bytes, offset + 48 + component * 4);
        }
        texture_axes.push_back(axes);
    }

    std::vector<Face> faces;
    faces.reserve(face_bytes.size() / 56);
    for (std::size_t offset = 0; offset < face_bytes.size(); offset += 56) {
        faces.push_back({
            read_value<int32_t>(face_bytes, offset + 4),
            read_value<int16_t>(face_bytes, offset + 8),
            read_value<int16_t>(face_bytes, offset + 10),
            read_value<int16_t>(face_bytes, offset + 12),
            read_value<std::array<std::uint8_t, 4>>(face_bytes, offset + 16),
            read_value<int32_t>(face_bytes, offset + 20),
            {read_value<int32_t>(face_bytes, offset + 28),
             read_value<int32_t>(face_bytes, offset + 32)},
            {read_value<int32_t>(face_bytes, offset + 36),
             read_value<int32_t>(face_bytes, offset + 40)}
        });
    }

    const auto world_first_face = read_value<int32_t>(model_bytes, 40);
    const auto world_face_count = read_value<int32_t>(model_bytes, 44);
    if (world_first_face < 0 || world_face_count < 0
        || static_cast<std::size_t>(world_first_face + world_face_count) > faces.size()) {
        throw std::runtime_error("BSP world model face range is invalid.");
    }

    // The 3D skybox is a miniature map elsewhere in the BSP: exactly the leaves
    // in the PVS of the sky_camera's leaf.
    sky_camera_.reset();
    sky_paint_.reset();
    sky_clusters_.clear();
    std::vector<char> sky_face(faces.size(), 0);
    std::size_t sky_3d_faces = 0;
    std::size_t hidden_faces = 0;
    {
        const auto entity_bytes = read_lump(file, EntityLump);
        const std::string entity_text(
            reinterpret_cast<const char*>(entity_bytes.data()), entity_bytes.size());
        sky_camera_ = find_sky_camera(entity_text);
        sky_paint_ = find_sky_paint(entity_text);
        if (sky_camera_) {
            const auto tree = load_tree(file, header);
            const int cluster = tree.cluster(tree.leaf_at(sky_camera_->origin));
            const auto visibility = read_lump(file, VisibilityLump);
            const auto leaf_faces = read_lump(file, LeafFaceLump);
            if (cluster < 0 || visibility.size() < 4) {
                throw std::runtime_error("sky_camera is not inside a visible leaf cluster.");
            }
            const auto cluster_count = static_cast<std::size_t>(
                read_value<int32_t>(visibility, 0));
            if (static_cast<std::size_t>(cluster) >= cluster_count) {
                throw std::runtime_error("sky_camera cluster exceeds the visibility table.");
            }
            sky_clusters_.assign(cluster_count, false);
            auto at = static_cast<std::size_t>(read_value<int32_t>(
                visibility, 4 + static_cast<std::size_t>(cluster) * 8));
            for (std::size_t visible = 0; visible < cluster_count;) {
                const auto packed = read_value<std::uint8_t>(visibility, at++);
                if (packed == 0) {
                    visible += 8 * static_cast<std::size_t>(
                        read_value<std::uint8_t>(visibility, at++));
                    continue;
                }
                for (int bit = 0; bit < 8 && visible + bit < cluster_count; ++bit) {
                    if ((packed >> bit) & 1) {
                        sky_clusters_[visible + bit] = true;
                    }
                }
                visible += 8;
            }
            for (std::size_t leaf = 0; leaf < tree.leaf_count(); ++leaf) {
                const int leaf_cluster = tree.cluster(leaf);
                if (leaf_cluster < 0 || !sky_clusters_[static_cast<std::size_t>(leaf_cluster)]) {
                    continue;
                }
                const auto base = leaf * tree.leaf_size;
                const std::size_t first = read_value<std::uint16_t>(tree.leaves, base + 20);
                const std::size_t count = read_value<std::uint16_t>(tree.leaves, base + 22);
                for (std::size_t index = first; index < first + count; ++index) {
                    const std::size_t face_number = read_value<std::uint16_t>(leaf_faces, index * 2);
                    if (face_number < sky_face.size()) {
                        sky_face[face_number] = 1;
                    }
                }
            }
        }
    }

    // Faces that sit entirely at or above the lowest visibility-selected
    // skybox vertex also belong to the miniature: its terrain is split into
    // clusters the sky_camera cannot see, but the main map never reaches that high.
    sky_floor_z_ = std::numeric_limits<float>::max();
    auto face_z_range = [&](const Face& face, float& low, float& high) {
        low = std::numeric_limits<float>::max();
        high = std::numeric_limits<float>::lowest();
        if (face.first_edge < 0 || face.edge_count < 3
            || static_cast<std::size_t>(face.first_edge + face.edge_count) > surfedges.size()) {
            return false;
        }
        for (int16_t offset = 0; offset < face.edge_count; ++offset) {
            const auto surfedge = surfedges[static_cast<std::size_t>(face.first_edge + offset)];
            const auto edge_index = static_cast<std::size_t>(surfedge < 0 ? -surfedge : surfedge);
            if (edge_index >= edges.size()) {
                return false;
            }
            const auto vertex_index = surfedge >= 0 ? edges[edge_index].first : edges[edge_index].second;
            if (vertex_index >= vertices.size()) {
                return false;
            }
            low = std::min(low, vertices[vertex_index].z);
            high = std::max(high, vertices[vertex_index].z);
        }
        return true;
    };
    if (sky_camera_) {
        for (int32_t index = world_first_face; index < world_first_face + world_face_count; ++index) {
            float low = 0.0f;
            float high = 0.0f;
            if (sky_face[static_cast<std::size_t>(index)] && face_z_range(faces[static_cast<std::size_t>(index)], low, high)) {
                sky_floor_z_ = std::min(sky_floor_z_, low);
            }
        }
        if (sky_floor_z_ != std::numeric_limits<float>::max()) {
            for (int32_t index = world_first_face; index < world_first_face + world_face_count; ++index) {
                float low = 0.0f;
                float high = 0.0f;
                auto& marked = sky_face[static_cast<std::size_t>(index)];
                if (!marked && face_z_range(faces[static_cast<std::size_t>(index)], low, high)
                    && low >= sky_floor_z_) {
                    marked = 1;
                }
            }
        }
    }

    sun_light_.reset();
    {
        const auto worldlights = read_lump(file, WorldLightLump);
        for (std::size_t offset = 0; offset + WorldLightSize <= worldlights.size();
             offset += WorldLightSize) {
            if (read_value<int32_t>(worldlights, offset + 40) != EmitSkylight) {
                continue;
            }
            SunLight sun{};
            for (int axis = 0; axis < 3; ++axis) {
                sun.color[axis] = read_value<float>(worldlights, offset + 12 + axis * 4);
                sun.direction[axis] = read_value<float>(worldlights, offset + 24 + axis * 4);
            }
            sun_light_ = sun;
        }
    }

    world_triangles_.clear();
    lightmap_atlas_ = {};
    // Indexed by face number: brush entities (doors...) share the face array.
    std::vector<LightmapBlock> blocks(faces.size());
    std::size_t lightmap_faces = 0;
    std::size_t lightmap_failures = 0;
    float lightmap_max = 0.0f;
    double lightmap_sum = 0.0;
    std::uint64_t luxel_total = 0;
    // Each face stores one lightmap per lightstyle slot. Style 0 is constant;
    // 1-31 are animated patterns (their average is used); 32-63 are switchable
    // lights, on at map start unless every light with that style is initially dark.
    std::array<float, 256> style_weight{};
    auto styled = std::make_shared<StyledLightmaps>();
    bank_styles_.fill(-1);
    {
        static const char* const Patterns[12] = {
            "m", "mmnmmommommnonmmonqnmmo", "abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba",
            "mmmmmaaaaammmmmaaaaaabcdefgabcdefg", "mamamamamama", "jklmnopqrstuvwxyzyxwvutsrqponmlkj",
            "nmonqnmomnmomomno", "mmmaaaabcdefgmmmmaaaammmaamm", "mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa",
            "aaaaaaaazzzzzzzz", "mmamammmmammamamaaamammma", "abcdefghijklmnopqrrqponmlkjihgfedcba"};
        for (std::size_t style = 0; style < 12; ++style) {
            float sum = 0.0f;
            const std::string pattern = Patterns[style];
            for (const char c : pattern) {
                sum += static_cast<float>(c - 'a') / 12.0f;
            }
            style_weight[style] = sum / static_cast<float>(pattern.size());
        }
        for (std::size_t style = 12; style < 32; ++style) {
            style_weight[style] = 1.0f;
        }
        std::array<int, 256> lights_on{};
        std::array<int, 256> lights_total{};
        struct StartupLight { int style; bool on; std::string target; };
        std::vector<StartupLight> startup_lights;
        // Startup TurnOn/TurnOff outputs of logic_auto, by lowercase targetname.
        std::map<std::string, bool> startup_state;
        const auto entity_bytes = read_lump(file, EntityLump);
        const std::string text(
            reinterpret_cast<const char*>(entity_bytes.data()), entity_bytes.size());
        std::size_t at = 0;
        while (at < text.size()) {
            const auto begin = text.find('{', at);
            const auto end = begin == std::string::npos ? begin : text.find('}', begin + 1);
            if (end == std::string::npos) {
                break;
            }
            at = end + 1;
            const auto entity = text.substr(begin + 1, end - begin - 1);
            const auto classname = entity_value(entity, "classname");
            if (classname == "logic_auto") {
                // Outputs are "<Output>" "target<ESC>Input<ESC>param<ESC>delay<ESC>once" pairs.
                std::size_t cursor = 0;
                std::vector<std::string> tokens;
                while ((cursor = entity.find('"', cursor)) != std::string::npos) {
                    const auto close = entity.find('"', cursor + 1);
                    if (close == std::string::npos) {
                        break;
                    }
                    tokens.push_back(entity.substr(cursor + 1, close - cursor - 1));
                    cursor = close + 1;
                }
                for (std::size_t i = 0; i + 1 < tokens.size(); i += 2) {
                    if (tokens[i].rfind("On", 0) != 0) {
                        continue;
                    }
                    auto value = tokens[i + 1];
                    std::replace(value.begin(), value.end(), '\x1b', ',');
                    std::vector<std::string> parts;
                    std::stringstream stream(value);
                    for (std::string part; std::getline(stream, part, ',');) {
                        parts.push_back(part);
                    }
                    if (parts.size() < 2) {
                        continue;
                    }
                    std::string target = parts[0];
                    std::transform(target.begin(), target.end(), target.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::string input = parts[1];
                    std::transform(input.begin(), input.end(), input.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (input == "turnoff") {
                        startup_state[target] = false;
                    } else if (input == "turnon") {
                        startup_state[target] = true;
                    }
                }
                continue;
            }
            if (classname.rfind("light", 0) != 0) {
                continue;
            }
            const auto style_text = entity_value(entity, "style");
            const int style = style_text.empty() ? 0 : std::atoi(style_text.c_str());
            if (style < 32 || style > 63) {
                continue;
            }
            const auto flags_text = entity_value(entity, "spawnflags");
            const int flags = flags_text.empty() ? 0 : std::atoi(flags_text.c_str());
            auto target = entity_value(entity, "targetname");
            std::transform(target.begin(), target.end(), target.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            bool initially_on = (flags & 1) == 0;
            const char* bank_names[]{"bank_lights_normal","bank_lights_normal_vault","bank_lights_raid","bank_lights_vault_raid"};
            for (int i=0;i<4;++i) if (target==bank_names[i]) bank_styles_[i]=style;
            startup_lights.push_back({style, initially_on, target});
        }
        for (const auto& light : startup_lights) {
            const int style = light.style;
            bool initially_on = light.on;
            const auto& target = light.target;
            const auto startup = startup_state.find(target);
            if (!target.empty() && startup != startup_state.end()) {
                initially_on = startup->second;
            }
            ++lights_total[static_cast<std::size_t>(style)];
            lights_on[static_cast<std::size_t>(style)] += initially_on ? 1 : 0;
        }
        for (std::size_t style = 32; style < 64; ++style) {
            style_weight[style] = lights_total[style] == 0 || lights_on[style] > 0 ? 1.0f : 0.0f;
        }
    }
    styled->initial = style_weight;
    styled->bank_styles = bank_styles_;
    lightmap_atlas_.styled = styled;
    // SURF_BUMPLIGHT faces store 4 lightmaps per style (flat, then 3 directional);
    // the flat one is first.
    auto bump_multiplier = [&](const Face& face) -> std::size_t {
        return face.texinfo >= 0 && static_cast<std::size_t>(face.texinfo) < texinfo_flags.size()
            && (texinfo_flags[static_cast<std::size_t>(face.texinfo)] & 0x800) != 0 ? 4 : 1;
    };
    auto face_style_count = [](const Face& face) {
        std::size_t count = 0;
        while (count < 4 && face.styles[count] != 255) {
            ++count;
        }
        return count;
    };
    {
        const auto lighting = read_lump(file, LightingLump);
        const auto lighting_length = static_cast<std::uint64_t>(lighting.size());
        std::vector<std::size_t> candidates;
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const auto& face = faces[index];
            if (face.styles[0] == 255 || face.light_offset < 0) {
                continue;
            }
            // Warp (water) surfaces are never lightmapped by the engine; their
            // stored lightmaps are black.
            if (face.texinfo >= 0 && static_cast<std::size_t>(face.texinfo) < texinfo_flags.size()
                && (texinfo_flags[static_cast<std::size_t>(face.texinfo)] & 0x8) != 0) {
                continue;
            }
            const int width = face.lightmap_size[0] + 1;
            const int height = face.lightmap_size[1] + 1;
            if (width < 1 || height < 1 || width > MaxLightmapSide || height > MaxLightmapSide
                || static_cast<std::uint64_t>(face.light_offset)
                    + static_cast<std::uint64_t>(width) * height * 4 * face_style_count(face)
                        * bump_multiplier(face)
                        > lighting_length) {
                ++lightmap_failures;
                continue;
            }
            blocks[index].width = width;
            blocks[index].height = height;
            candidates.push_back(index);
        }
        std::sort(candidates.begin(), candidates.end(), [&](std::size_t a, std::size_t b) {
            return blocks[a].height != blocks[b].height
                ? blocks[a].height > blocks[b].height
                : blocks[a].width > blocks[b].width;
        });
        // Shelf packing with a replicated one-luxel border so bilinear
        // filtering never reads a neighbouring face.
        int cursor_x = 0;
        int shelf_y = 0;
        int shelf_height = 0;
        for (const auto index : candidates) {
            auto& block = blocks[index];
            const int footprint_width = block.width + 2;
            const int footprint_height = block.height + 2;
            if (cursor_x + footprint_width > AtlasWidth) {
                shelf_y += shelf_height;
                shelf_height = 0;
                cursor_x = 0;
            }
            block.x = cursor_x + 1;
            block.y = shelf_y + 1;
            cursor_x += footprint_width;
            shelf_height = std::max(shelf_height, footprint_height);
        }
        const int atlas_height = shelf_y + shelf_height;
        if (atlas_height > MaxAtlasHeight) {
            throw std::runtime_error("BSP lightmaps do not fit in a Direct3D 12 texture.");
        }
        if (!candidates.empty()) {
            lightmap_atlas_.width = AtlasWidth;
            lightmap_atlas_.height = atlas_height;
            lightmap_atlas_.texels.assign(
                static_cast<std::size_t>(AtlasWidth) * atlas_height, 0);
            std::vector<std::uint8_t> raw;
            std::vector<float> accumulated;
            std::vector<std::uint32_t> encoded;
            for (const auto index : candidates) {
                const auto& block = blocks[index];
                const auto& face = faces[index];
                const std::size_t luxels = static_cast<std::size_t>(block.width) * block.height;
                const std::size_t styles = face_style_count(face);
                const std::size_t stride = luxels * 4 * bump_multiplier(face);
                raw.assign(lighting.begin() + face.light_offset,
                           lighting.begin() + face.light_offset + stride * styles);
                accumulated.assign(luxels * 3, 0.0f);
                StyledLightmapBlock* retained = nullptr;
                if (std::any_of(bank_styles_.begin(),bank_styles_.end(),[&](int bank_style) {
                    return bank_style>=0 && std::find(face.styles.begin(),face.styles.begin()+styles,bank_style)!=face.styles.begin()+styles;
                })) {
                    styled->blocks.push_back({block.x,block.y,block.width,block.height});
                    retained = &styled->blocks.back();
                    for (std::size_t slot = 0; slot < styles; ++slot) {
                        retained->styles.push_back(face.styles[slot]);
                        auto& values = retained->samples.emplace_back(luxels * 3);
                        const auto* samples = raw.data() + slot * stride;
                        for (std::size_t luxel = 0; luxel < luxels; ++luxel)
                            for (std::size_t c = 0; c < 3; ++c)
                                values[luxel*3+c] = rgbexp_to_linear(samples[luxel*4+c],
                                    static_cast<std::int8_t>(samples[luxel*4+3])) / 255.0f;
                    }
                }
                for (std::size_t slot = 0; slot < styles; ++slot) {
                    const float weight = style_weight[face.styles[slot]];
                    if (weight <= 0.0f) {
                        continue;
                    }
                    const auto* samples = raw.data() + slot * stride;
                    for (std::size_t luxel = 0; luxel < luxels; ++luxel) {
                        const auto exponent = static_cast<std::int8_t>(samples[luxel * 4 + 3]);
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            accumulated[luxel * 3 + channel] += weight
                                * rgbexp_to_linear(samples[luxel * 4 + channel], exponent) / 255.0f;
                        }
                    }
                }
                encoded.resize(luxels);
                for (std::size_t luxel = 0; luxel < luxels; ++luxel) {
                    const float red = accumulated[luxel * 3];
                    const float green = accumulated[luxel * 3 + 1];
                    const float blue = accumulated[luxel * 3 + 2];
                    encoded[luxel] = encode_rgb9e5(red, green, blue);
                    lightmap_max = std::max({lightmap_max, red, green, blue});
                    lightmap_sum += (red + green + blue) / 3.0f;
                }                luxel_total += encoded.size();
                for (int y = -1; y <= block.height; ++y) {
                    const int source_y = std::clamp(y, 0, block.height - 1);
                    for (int x = -1; x <= block.width; ++x) {
                        const int source_x = std::clamp(x, 0, block.width - 1);
                        lightmap_atlas_.texels[
                            static_cast<std::size_t>(block.y + y) * AtlasWidth + block.x + x] =
                            encoded[static_cast<std::size_t>(source_y) * block.width + source_x];
                    }
                }
                ++lightmap_faces;
            }
        }
    }
    const float atlas_width_f = static_cast<float>(std::max(lightmap_atlas_.width, 1));
    const float atlas_height_f = static_cast<float>(std::max(lightmap_atlas_.height, 1));
    std::size_t displacement_count = 0;
    std::size_t displacement_triangles = 0;
    std::size_t displacement_failures = 0;

    // Entities that draw geometry: .mdl props/doors and brush models ("*N").
    struct FaceJob {
        int32_t face = 0;
        std::size_t job = 0;
    };
    struct JobTransform {
        float origin[3]{};
        float rotation[3][3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    };
    std::vector<JobTransform> job_transforms(1);
    std::vector<FaceJob> jobs;
    jobs.reserve(static_cast<std::size_t>(world_face_count) + 4096);
    for (int32_t face_index = world_first_face; face_index < world_first_face + world_face_count; ++face_index) {
        jobs.push_back({face_index, 0});
    }
    entity_models_.clear();
    brush_models_.clear();
    entity_stats_ = {};
    {
        const auto entity_bytes = read_lump(file, EntityLump);
        const std::string text(
            reinterpret_cast<const char*>(entity_bytes.data()), entity_bytes.size());
        static const std::set<std::string> BrushClasses{
            "func_door", "func_door_rotating", "func_brush", "func_rotating",
            "func_movelinear", "func_illusionary", "func_breakable", "func_button",
            "func_breakable_surf", "func_reflective_glass", "func_wall",
            "func_wall_toggle", "func_physbox"};
        auto number = [](const std::string& value, float fallback) {
            return value.empty() ? fallback : static_cast<float>(std::atof(value.c_str()));
        };
        std::map<std::string, std::string> target_models;
        std::map<std::string, Vec3> target_angles;
        std::size_t cursor = 0;
        std::size_t entity_ordinal = 0;
        while (cursor < text.size()) {
            const auto begin = text.find('{', cursor);
            const auto end = begin == std::string::npos ? begin : text.find('}', begin + 1);
            if (end == std::string::npos) {
                break;
            }
            cursor = end + 1;
            const std::size_t current_entity_ordinal = entity_ordinal++;
            const auto entity = text.substr(begin + 1, end - begin - 1);
            const auto targetname = entity_value(entity, "targetname");
            const auto authored_model = entity_value(entity, "model");
            if (!targetname.empty()) {
                target_models[targetname] = authored_model;
                float rotation[3]{};
                read_vector3(entity_value(entity, "angles"), rotation);
                target_angles[targetname] = {rotation[0],rotation[1],rotation[2]};
            }
            const auto classname = entity_value(entity, "classname");
            const auto& model = authored_model;
            const bool brush = !model.empty() && model[0] == '*';
            const bool prop = classname.rfind("prop_", 0) == 0 && model.size() > 4
                && model.compare(model.size() - 4, 4, ".mdl") == 0
                && classname != "prop_static" && classname != "prop_detail";
            if (!(brush && BrushClasses.contains(classname)) && !prop) {
                continue;
            }
            const bool hidden = entity_value(entity, "rendermode") == "10"
                || entity_value(entity, "StartDisabled") == "1"
                || (static_cast<int>(number(entity_value(entity, "effects"), 0.0f)) & 32) != 0;
            if (hidden) {
                ++entity_stats_.skipped_hidden;
                if (brush) continue;
            }
            Vec3 origin{};
            Vec3 angles{};
            float parsed[3]{};
            read_vector3(entity_value(entity, "origin"), parsed);
            origin = {parsed[0], parsed[1], parsed[2]};
            parsed[0] = parsed[1] = parsed[2] = 0.0f;
            read_vector3(entity_value(entity, "angles"), parsed);
            angles = {parsed[0], parsed[1], parsed[2]};
            if (prop) {
                if (number(entity_value(entity, "modelscale"), 1.0f) != 1.0f) {
                    ++entity_stats_.scaled_models;
                }
                EntityModel result{classname, model, origin, angles, hidden,
                    static_cast<int>(number(entity_value(entity, "skin"), 0.0f)),
                    number(entity_value(entity, "fademaxdist"), 0.0f),
                    static_cast<int>(number(entity_value(entity, "body"), 0.0f)),
                    classname == "prop_door_rotating"
                        ? static_cast<int>(number(entity_value(entity, "hardware"), 0.0f)) : 0,
                    number(entity_value(entity, "distance"), 90.0f),
                    number(entity_value(entity, "speed"), 100.0f),
                    static_cast<int>(number(entity_value(entity, "opendir"), 1.0f))};
                result.map_created_id = map_creation_id(current_entity_ordinal);
                result.targetname = targetname;
                result.parent_name = entity_value(entity, "parentname");
                if (const auto attachment = result.parent_name.find(',');
                    attachment != std::string::npos) {
                    result.parent_name.resize(attachment);
                }
                entity_models_.push_back(std::move(result));
                ++entity_stats_.model_entities;
                continue;
            }
            const auto model_index = static_cast<std::size_t>(std::atoi(model.c_str() + 1));
            if (model_index == 0) {
                continue;
            }
            const auto face_range = model_face_range(model_bytes, model_index, faces.size());
            if (!face_range || face_range->count <= 0) {
                continue;
            }
            const auto first = face_range->first;
            const auto count = face_range->count;
            // Source AngleMatrix (the same one static props use).
            constexpr float degrees_to_radians = 0.01745329251994329577f;
            const float cp = std::cos(angles.x * degrees_to_radians);
            const float sp = std::sin(angles.x * degrees_to_radians);
            const float cy = std::cos(angles.y * degrees_to_radians);
            const float sy = std::sin(angles.y * degrees_to_radians);
            const float cr = std::cos(angles.z * degrees_to_radians);
            const float sr = std::sin(angles.z * degrees_to_radians);
            JobTransform transform;
            transform.origin[0] = origin.x;
            transform.origin[1] = origin.y;
            transform.origin[2] = origin.z;
            const float rotation[3][3] = {
                {cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr},
                {sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr},
                {-sp, cp * sr, cp * cr}};
            std::copy(&rotation[0][0], &rotation[0][0] + 9, &transform.rotation[0][0]);
            job_transforms.push_back(transform);
            BrushModel brush_model;
            brush_model.classname = classname;
            brush_model.targetname = entity_value(entity, "targetname");
            brush_model.model = model;
            brush_model.model_index = model_index;
            brush_model.origin = origin;
            brush_model.angles = angles;
            brush_models_.push_back(std::move(brush_model));
            for (int32_t face = first; face < first + count; ++face) {
                jobs.push_back({face, brush_models_.size()});
            }
            ++entity_stats_.brush_entities;
            entity_stats_.brush_faces += static_cast<std::size_t>(count);
        }
        for (auto& entity_model : entity_models_) {
            const auto parent = target_models.find(entity_model.parent_name);
            if (!entity_model.parent_name.empty() && parent != target_models.end()) {
                entity_model.parent_model = parent->second;
                entity_model.parent_angles = target_angles.at(entity_model.parent_name);
            }
        }
    }

    // Brush triangles are retained in local space for recorded entity poses.
    std::size_t finished_begin = 0;
    std::size_t finished_job = 0;
    auto finish_job = [&]() {
        const auto& transform = job_transforms[finished_job];
        if (finished_job != 0) {
            auto& brush = brush_models_[finished_job - 1];
            for (std::size_t index = finished_begin; index < world_triangles_.size(); ++index) {
                auto& triangle = world_triangles_[index];
                if (sky_floor_z_ != std::numeric_limits<float>::max()) {
                    const float z = transform.rotation[2][0] * triangle.a.x
                        + transform.rotation[2][1] * triangle.a.y
                        + transform.rotation[2][2] * triangle.a.z + transform.origin[2];
                    if (z >= sky_floor_z_) triangle.sky_3d = true;
                }
                brush.triangles.push_back(std::move(triangle));
            }
            world_triangles_.resize(finished_begin);
            entity_stats_.brush_triangles += brush.triangles.size();
        }
        finished_begin = world_triangles_.size();
    };
    for (const auto& job : jobs) {
        if (job.job != finished_job) {
            finish_job();
            finished_job = job.job;
        }
        const int32_t face_index = job.face;
        const auto face = faces[static_cast<std::size_t>(face_index)];
        if (face.first_edge < 0 || face.edge_count < 3
            || static_cast<std::size_t>(face.first_edge + face.edge_count) > surfedges.size()) {
            continue;
        }

        if (face.texinfo >= 0 && static_cast<std::size_t>(face.texinfo) < texinfo_flags.size()
            && (texinfo_flags[static_cast<std::size_t>(face.texinfo)] & HiddenSurfaceFlags) != 0) {
            ++hidden_faces;
            continue;
        }
        const bool sky_3d = sky_face[static_cast<std::size_t>(face_index)] != 0;
        sky_3d_faces += sky_3d ? 1 : 0;
        std::vector<Vec3> polygon;
        polygon.reserve(static_cast<std::size_t>(face.edge_count));
        for (int16_t edge_offset = 0; edge_offset < face.edge_count; ++edge_offset) {
            const auto surfedge = surfedges[
                static_cast<std::size_t>(face.first_edge + edge_offset)];
            const auto edge_index = static_cast<std::size_t>(
                surfedge < 0 ? -surfedge : surfedge);
            if (edge_index >= edges.size()) {
                polygon.clear();
                break;
            }
            const auto edge = edges[edge_index];
            const auto vertex_index = surfedge >= 0 ? edge.first : edge.second;
            if (vertex_index >= vertices.size()) {
                polygon.clear();
                break;
            }
            polygon.push_back(vertices[vertex_index]);
        }
        std::string material;
        TextureAxes axes{};
        float texture_width = 1.0f;
        float texture_height = 1.0f;
        if (face.texinfo >= 0
            && static_cast<std::size_t>(face.texinfo) < texinfo_texdata.size()) {
            const auto texdata_index = texinfo_texdata[face.texinfo];
            if (texdata_index >= 0
                && static_cast<std::size_t>(texdata_index) < texdata_names.size()) {
                const auto string_index = texdata_names[texdata_index];
                if (string_index >= 0
                    && static_cast<std::size_t>(string_index) < texture_names.size()) {
                    material = texture_names[string_index];
                }
                axes = texture_axes[face.texinfo];
                texture_width = static_cast<float>(
                    std::max(texdata_widths[texdata_index], 1));
                texture_height = static_cast<float>(
                    std::max(texdata_heights[texdata_index], 1));
            }
        }
        auto texture_Av = [&](const Vec3& point) {
            const float A = point.x * axes.A[0]
                + point.y * axes.A[1]
                + point.z * axes.A[2]
                + axes.A[3];
            const float v = point.x * axes.v[0]
                + point.y * axes.v[1]
                + point.z * axes.v[2]
                + axes.v[3];
            return std::array<float, 2>{A / texture_width, v / texture_height};
        };
        const auto& block = blocks[static_cast<std::size_t>(face_index)];
        const bool has_block = block.x >= 0;
        auto lightmap_coordinate = [&](const Vec3& point) {
            const float s = point.x * axes.lightmap_u[0]
                + point.y * axes.lightmap_u[1]
                + point.z * axes.lightmap_u[2]
                + axes.lightmap_u[3] - static_cast<float>(face.lightmap_mins[0]);
            const float t = point.x * axes.lightmap_v[0]
                + point.y * axes.lightmap_v[1]
                + point.z * axes.lightmap_v[2]
                + axes.lightmap_v[3] - static_cast<float>(face.lightmap_mins[1]);
            return std::array<float, 2>{
                (static_cast<float>(block.x) + 0.5f + s) / atlas_width_f,
                (static_cast<float>(block.y) + 0.5f + t) / atlas_height_f};
        };

        // Displacements replace their flat base face with the displaced grid
        // (same construction as gmod-replay/src/bsp/displacement.rs).
        if (face.dispinfo >= 0) {
            const auto disp_index = static_cast<std::size_t>(face.dispinfo);
            const bool valid_info = disp_index < dispinfos.size() && polygon.size() == 4;
            const auto& info = valid_info ? dispinfos[disp_index] : DispInfo{};
            const int side = valid_info && info.power >= 2 && info.power <= 4
                ? (1 << info.power) + 1 : 0;
            if (side == 0 || info.vert_start < 0
                || static_cast<std::size_t>(info.vert_start) + side * side
                    > dispverts.size()) {
                ++displacement_failures;
            } else {
                std::size_t start = 0;
                float best = std::numeric_limits<float>::max();
                for (std::size_t corner = 0; corner < 4; ++corner) {
                    const float dx = polygon[corner].x - info.start_position.x;
                    const float dy = polygon[corner].y - info.start_position.y;
                    const float dz = polygon[corner].z - info.start_position.z;
                    const float distance = dx * dx + dy * dy + dz * dz;
                    if (distance < best) {
                        best = distance;
                        start = corner;
                    }
                }
                const Vec3 corners[4]{
                    polygon[start],
                    polygon[(start + 3) % 4],
                    polygon[(start + 2) % 4],
                    polygon[(start + 1) % 4]};
                std::array<float, 2> corner_Av[4];
                for (int corner = 0; corner < 4; ++corner) {
                    corner_Av[corner] = texture_Av(corners[corner]);
                }
                struct GridVertex {
                    Vec3 position;
                    float uv[2];
                    float alpha;
                    float lightmap[2];
                };
                // Displacements reuse the base face's lightmap block; its
                // coordinates are interpolated across the grid like the texture UVs.
                const bool displacement_lightmap = has_block;
                std::array<float, 2> corner_lightmap[4]{};
                if (has_block) {
                    for (int corner = 0; corner < 4; ++corner) {
                        corner_lightmap[corner] = lightmap_coordinate(corners[corner]);
                    }
                }
                std::vector<GridVertex> grid(static_cast<std::size_t>(side * side));
                for (int y = 0; y < side; ++y) {
                    const float v = static_cast<float>(y) / (side - 1);
                    for (int x = 0; x < side; ++x) {
                        const float A = static_cast<float>(x) / (side - 1);
                        auto bilerp = [&](float p00, float p10, float p11, float p01) {
                            const float a = p00 * (1.0f - A) + p10 * A;
                            const float b = p01 * (1.0f - A) + p11 * A;
                            return a * (1.0f - v) + b * v;
                        };
                        const auto& disp = dispverts[
                            static_cast<std::size_t>(info.vert_start + y * side + x)];
                        auto& out = grid[static_cast<std::size_t>(y * side + x)];
                        out.position = {
                            bilerp(corners[0].x, corners[1].x, corners[2].x, corners[3].x)
                                + disp.vector.x * disp.distance,
                            bilerp(corners[0].y, corners[1].y, corners[2].y, corners[3].y)
                                + disp.vector.y * disp.distance,
                            bilerp(corners[0].z, corners[1].z, corners[2].z, corners[3].z)
                                + disp.vector.z * disp.distance};
                        out.uv[0] = bilerp(corner_Av[0][0], corner_Av[1][0],
                                           corner_Av[2][0], corner_Av[3][0]);
                        out.uv[1] = bilerp(corner_Av[0][1], corner_Av[1][1],
                                           corner_Av[2][1], corner_Av[3][1]);
                        out.alpha = disp.alpha;
                        // vrad lays displacement luxels out as a plain grid over the
                        // block, starting at the displacement's start corner.
                        out.lightmap[0] = (static_cast<float>(block.x) + 0.5f
                            + A * static_cast<float>(block.width - 1)) / atlas_width_f;
                        out.lightmap[1] = (static_cast<float>(block.y) + 0.5f
                            + v * static_cast<float>(block.height - 1)) / atlas_height_f;
                    }
                }
                auto emit = [&](int i0, int i1, int i2) {
                    const auto& a = grid[static_cast<std::size_t>(i0)];
                    const auto& b = grid[static_cast<std::size_t>(i1)];
                    const auto& c = grid[static_cast<std::size_t>(i2)];
                    Triangle triangle{a.position, b.position, c.position, material,
                        {{a.uv[0], a.uv[1]}, {b.uv[0], b.uv[1]}, {c.uv[0], c.uv[1]}},
                        {texture_width, texture_height},
                        {a.alpha, b.alpha, c.alpha}};
                    triangle.sky_3d = sky_3d;
                    if (displacement_lightmap) {
                        const GridVertex* corners_lm[3]{&a, &b, &c};
                        for (int corner = 0; corner < 3; ++corner) {
                            triangle.lightmap_uv[corner][0] = corners_lm[corner]->lightmap[0];
                            triangle.lightmap_uv[corner][1] = corners_lm[corner]->lightmap[1];
                        }
                    }
                    world_triangles_.push_back(std::move(triangle));
                    ++displacement_triangles;
                };
                for (int y = 0; y + 1 < side; ++y) {
                    for (int x = 0; x + 1 < side; ++x) {
                        const int i00 = y * side + x;
                        const int i10 = i00 + 1;
                        const int i01 = (y + 1) * side + x;
                        const int i11 = i01 + 1;
                        if ((x + y) % 2 == 0) {
                            emit(i00, i10, i11);
                            emit(i00, i11, i01);
                        } else {
                            emit(i00, i10, i01);
                            emit(i10, i11, i01);
                        }
                    }
                }
                ++displacement_count;
                continue;
            }
        }

        for (std::size_t index = 1; index + 1 < polygon.size(); ++index) {
            const auto Av_a = texture_Av(polygon[0]);
            const auto Av_b = texture_Av(polygon[index]);
            const auto Av_c = texture_Av(polygon[index + 1]);
            Triangle triangle{
                polygon[0], polygon[index], polygon[index + 1], material,
                {{Av_a[0], Av_a[1]}, {Av_b[0], Av_b[1]}, {Av_c[0], Av_c[1]}},
                {texture_width, texture_height}};
            triangle.sky_3d = sky_3d;
            if (has_block) {
                const Vec3* points[3]{&polygon[0], &polygon[index], &polygon[index + 1]};
                for (int corner = 0; corner < 3; ++corner) {
                    const auto coordinate = lightmap_coordinate(*points[corner]);
                    triangle.lightmap_uv[corner][0] = coordinate[0];
                    triangle.lightmap_uv[corner][1] = coordinate[1];
                }
            }
            world_triangles_.push_back(std::move(triangle));
        }
    }

    finish_job();

    std::size_t triangle_count = world_triangles_.size();
    std::set<std::string> materials;
    for (const auto& triangle : world_triangles_) {
        if (!triangle.material.empty()) {
            materials.insert(triangle.material);
        }
    }
    for (const auto& brush : brush_models_) {
        triangle_count += brush.triangles.size();
        for (const auto& triangle : brush.triangles) {
            if (!triangle.material.empty()) materials.insert(triangle.material);
        }
    }
    return {
        version,
        vertices.size(),
        edges.size(),
        surfedges.size(),
        faces.size(),
        static_cast<std::size_t>(world_face_count),
        triangle_count,
        materials.size(),
        displacement_count,
        displacement_triangles,
        displacement_failures,
        lightmap_faces,
        lightmap_failures,
        lightmap_max,
        luxel_total > 0 ? static_cast<float>(lightmap_sum / static_cast<double>(luxel_total)) : 0.0f,
        sky_3d_faces,
        hidden_faces
    };
}

const std::optional<SkyCamera>& BspReader::sky_camera() const noexcept {
    return sky_camera_;
}

const std::optional<SkyPaint>& BspReader::sky_paint() const noexcept {
    return sky_paint_;
}

AudioScene BspReader::audio_scene() const {
    const auto bsp = BspFile::open(path_);
    const BspFile& file = *bsp;
    const auto& header = file.header();
    const auto entity_bytes = read_lump(file, EntityLump);
    const auto model_bytes = read_lump(file, ModelLump);
    const std::string text(
        reinterpret_cast<const char*>(entity_bytes.data()), entity_bytes.size());

    auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    auto parse_vector = [](const std::string& value, Vec3& output) {
        float parsed[3]{};
        read_vector3(value, parsed);
        output = {parsed[0], parsed[1], parsed[2]};
    };
    // Every key/value pair of an entity, keys lower-cased.
    std::vector<std::map<std::string, std::string>> entities;
    for (std::size_t cursor = 0; cursor < text.size();) {
        const auto begin = text.find('{', cursor);
        const auto end = begin == std::string::npos ? begin : text.find('}', begin + 1);
        if (end == std::string::npos) {
            break;
        }
        cursor = end + 1;
        std::map<std::string, std::string> pairs;
        std::size_t at = begin + 1;
        while (true) {
            const auto key_open = text.find('"', at);
            if (key_open == std::string::npos || key_open > end) break;
            const auto key_close = text.find('"', key_open + 1);
            const auto value_open = text.find('"', key_close + 1);
            const auto value_close = text.find('"', value_open + 1);
            if (value_close == std::string::npos || value_close > end) break;
            pairs[lower(text.substr(key_open + 1, key_close - key_open - 1))] =
                text.substr(value_open + 1, value_close - value_open - 1);
            at = value_close + 1;
        }
        entities.push_back(std::move(pairs));
    }
    auto get = [](const std::map<std::string, std::string>& pairs, const char* key) {
        const auto found = pairs.find(key);
        return found == pairs.end() ? std::string{} : found->second;
    };
    auto number = [](const std::string& value, float fallback) {
        return value.empty() ? fallback : static_cast<float>(std::atof(value.c_str()));
    };

    std::map<std::string, Vec3> named_origins;
    for (const auto& entity : entities) {
        const auto name = lower(get(entity, "targetname"));
        if (!name.empty() && entity.contains("origin")) {
            Vec3 origin{};
            parse_vector(get(entity, "origin"), origin);
            named_origins.emplace(name, origin);
        }
    }

    AudioScene scene;
    for (const auto& entity : entities) {
        const auto classname = get(entity, "classname");
        if (classname == "ambient_generic" && !get(entity, "message").empty()) {
            AmbientEmitter emitter;
            emitter.sound = get(entity, "message");
            parse_vector(get(entity, "origin"), emitter.origin);
            emitter.radius = number(get(entity, "radius"), 0.0f);
            emitter.flags = static_cast<int>(number(get(entity, "spawnflags"), 0.0f));
            emitter.volume = std::clamp(number(get(entity, "health"), 10.0f) / 10.0f, 0.0f, 1.0f);
            emitter.pitch = number(get(entity, "pitch"), 100.0f);
            scene.ambient.push_back(std::move(emitter));
        } else if (classname == "env_soundscape_triggerable"
                   && get(entity, "startdisabled") != "1") {
            SoundscapeEntity soundscape;
            soundscape.targetname = lower(get(entity, "targetname"));
            soundscape.soundscape = get(entity, "soundscape");
            parse_vector(get(entity, "origin"), soundscape.origin);
            for (int index = 0; index < 8; ++index) {
                const auto target = lower(get(entity, ("position" + std::to_string(index)).c_str()));
                const auto found = named_origins.find(target);
                if (!target.empty() && found != named_origins.end()) {
                    soundscape.positions.emplace_back(index, found->second);
                }
            }
            scene.soundscapes.push_back(std::move(soundscape));
        } else if (classname == "trigger_soundscape") {
            const auto model = get(entity, "model");
            if (model.size() < 2 || model[0] != '*') {
                continue;
            }
            const auto index = static_cast<std::size_t>(std::atoi(model.c_str() + 1));
            if ((index + 1) * 48 > model_bytes.size()) {
                continue;
            }
            SoundscapeVolume volume;
            volume.target = lower(get(entity, "soundscape"));
            // Brush models with an origin key are stored relative to it.
            Vec3 shift{};
            parse_vector(get(entity, "origin"), shift);
            volume.mins = {read_value<float>(model_bytes, index * 48) + shift.x,
                           read_value<float>(model_bytes, index * 48 + 4) + shift.y,
                           read_value<float>(model_bytes, index * 48 + 8) + shift.z};
            volume.maxs = {read_value<float>(model_bytes, index * 48 + 12) + shift.x,
                           read_value<float>(model_bytes, index * 48 + 16) + shift.y,
                           read_value<float>(model_bytes, index * 48 + 20) + shift.z};
            scene.volumes.push_back(std::move(volume));
        }
    }
    return scene;
}

const std::vector<EntityModel>& BspReader::entity_models() const noexcept {
    return entity_models_;
}

const std::vector<BrushModel>& BspReader::brush_models() const noexcept {
    return brush_models_;
}

const EntityStats& BspReader::entity_stats() const noexcept {
    return entity_stats_;
}

const std::optional<SunLight>& BspReader::sun_light() const noexcept {
    return sun_light_;
}

const LightmapAtlas& BspReader::lightmap_atlas() const noexcept {
    return lightmap_atlas_;
}

LightmapAtlas BspReader::release_lightmap_atlas() noexcept {
    return std::move(lightmap_atlas_);
}

std::vector<LightmapPatch> lightmap_patches(const StyledLightmaps& data,
    const LightStyleWeights& before, const LightStyleWeights& after) {
    std::vector<LightmapPatch> result;
    for (const auto& block : data.blocks) {
        if (std::none_of(block.styles.begin(),block.styles.end(),
            [&](int style) { return before[style] != after[style]; })) continue;
        LightmapPatch patch{block.x-1,block.y-1,block.width+2,block.height+2,{}};
        patch.texels.resize(static_cast<std::size_t>(patch.width)*patch.height);
        for (int y=0; y<patch.height; ++y) for (int x=0; x<patch.width; ++x) {
            const auto luxel = (std::clamp(y-1,0,block.height-1)*block.width
                + std::clamp(x-1,0,block.width-1))*3;
            float rgb[3]{};
            for (std::size_t slot=0; slot<block.styles.size(); ++slot)
                for (int c=0; c<3; ++c) rgb[c] += block.samples[slot][luxel+c]*after[block.styles[slot]];
            patch.texels[y*patch.width+x] = encode_rgb9e5(rgb[0],rgb[1],rgb[2]);
        }
        result.push_back(std::move(patch));
    }
    return result;
}

struct BspProbe::Impl {
    BspTree tree_;
    struct BankAmbient { float color[4][6][3]{}; };
    std::unordered_map<std::size_t,BankAmbient> bank_ambient_;

    void bake_bank(const std::vector<std::uint8_t>& lights,const std::array<int,4>& styles) {
        std::vector<std::pair<std::size_t,int>> selected;
        Vec3 low{FLT_MAX,FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX,-FLT_MAX};
        for (std::size_t o=0;o+WorldLightSize<=lights.size();o+=WorldLightSize) {
            const int style=read_value<int32_t>(lights,o+44),type=read_value<int32_t>(lights,o+40);
            const auto found=std::find(styles.begin(),styles.end(),style);
            if (style<0 || found==styles.end() || type<0 || type>2) continue;
            selected.emplace_back(o,int(found-styles.begin()));
            const float x=read_value<float>(lights,o),y=read_value<float>(lights,o+4),z=read_value<float>(lights,o+8);
            low={std::min(low.x,x-512),std::min(low.y,y-512),std::min(low.z,z-512)};
            high={std::max(high.x,x+512),std::max(high.y,y+512),std::max(high.z,z+512)};
        }
        if (selected.empty()) return;
        for (std::size_t leaf=0;leaf<tree_.leaf_count();++leaf) {
            float mins[3],maxs[3];
            for(int axis=0;axis<3;++axis) {
                mins[axis]=read_value<int16_t>(tree_.leaves,leaf*tree_.leaf_size+8+axis*2);
                maxs[axis]=read_value<int16_t>(tree_.leaves,leaf*tree_.leaf_size+14+axis*2);
            }
            if (maxs[0]<low.x || mins[0]>high.x || maxs[1]<low.y || mins[1]>high.y || maxs[2]<low.z || mins[2]>high.z) continue;
            const auto count=read_value<std::uint16_t>(ambient_index_,leaf*4);
            const auto first=read_value<std::uint16_t>(ambient_index_,leaf*4+2);
            for(std::size_t index=first;index<std::size_t(first)+count && (index+1)*28<=ambient_samples_.size();++index) {
                float xyz[3];
                for(int axis=0;axis<3;++axis) xyz[axis]=mins[axis]+(maxs[axis]-mins[axis])*ambient_samples_[index*28+24+axis]/255.0f;
                const Vec3 point{xyz[0],xyz[1],xyz[2]};
                if(point.x<low.x || point.x>high.x || point.y<low.y || point.y>high.y || point.z<low.z || point.z>high.z) continue;
                BankAmbient ambient; bool lit=false;
                for(const auto& [o,slot]:selected) {
                    const int type=read_value<int32_t>(lights,o+40);
                    const Vec3 origin{read_value<float>(lights,o),read_value<float>(lights,o+4),read_value<float>(lights,o+8)};
                    float direction[3]{origin.x-point.x,origin.y-point.y,origin.z-point.z};
                    const float squared=direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2];
                    const float radius=read_value<float>(lights,o+60);
                    if(squared<0.01f || (radius>0 && squared>radius*radius)) continue;
                    const float distance=std::sqrt(squared);
                    for(auto& v:direction) v/=distance;
                    float ratio=type==0 ? 1.0f/std::max(1.0f,squared)
                        :1.0f/std::max(0.0001f,read_value<float>(lights,o+64)+read_value<float>(lights,o+68)*distance+read_value<float>(lights,o+72)*squared);
                    if(type!=1) {
                        float dot=0;
                        for(int c=0;c<3;++c) dot-=direction[c]*read_value<float>(lights,o+24+c*4);
                        if(type==0) ratio*=std::max(0.0f,dot);
                        else {
                            const float inner=read_value<float>(lights,o+48),outer=read_value<float>(lights,o+52);
                            if(dot<=outer) continue;
                            if(dot<inner && inner>outer) ratio*=std::pow((dot-outer)/(inner-outer),std::max(0.0f,read_value<float>(lights,o+56)));
                        }
                    }
                    float peak=0;
                    for(int c=0;c<3;++c) peak=std::max(peak,ratio*read_value<float>(lights,o+12+c*4));
                    if(peak<0.001f || !tree_.clear_segment(point,origin)) continue;
                    lit=true;
                    for(int face=0;face<6;++face) {
                        const float weight=std::max(0.0f,direction[face/2]*(face%2?-1.0f:1.0f));
                        for(int c=0;c<3;++c) ambient.color[slot][face][c]+=weight*ratio*read_value<float>(lights,o+12+c*4);
                    }
                }
                if(lit) bank_ambient_.emplace(index,ambient);
            }
        }
    }

    std::vector<std::uint8_t> ambient_index_;
    std::vector<std::uint8_t> ambient_samples_;
    std::vector<bool> sky_clusters;
    float sky_floor_z = std::numeric_limits<float>::max();

    AmbientCube sample(const Vec3& point, const BankLightWeights* bank=nullptr) const {
        constexpr std::size_t SampleSize = 28;
        const std::size_t sample_count = ambient_samples_.size() / SampleSize;
        // Props whose origin sits inside solid or an unsampled leaf are probed a
        // little around the origin (favouring up) for the nearest sampled leaf.
        constexpr float Probes[][3]{
            {0, 0, 0}, {0, 0, 16}, {0, 0, 48}, {32, 0, 16}, {-32, 0, 16}, {0, 32, 16},
            {0, -32, 16}, {0, 0, 96}, {64, 0, 32}, {-64, 0, 32}, {0, 64, 32}, {0, -64, 32},
            {0, 0, -16}, {0, 0, 160}};
        AmbientCube cube;
        {
            const auto origin_leaf = tree_.leaf_at(point);
            const int origin_cluster = tree_.cluster(origin_leaf);
            cube.in_sky_3d = point.z >= sky_floor_z
                || (origin_cluster >= 0
                    && static_cast<std::size_t>(origin_cluster) < sky_clusters.size()
                    && sky_clusters[static_cast<std::size_t>(origin_cluster)]);
        }
        // Samples placed inside geometry are black; prefer a real one nearby and
        // only fall back to a black sample when nothing else exists.
        for (const bool allow_black : {false, true})
        for (const auto& probe : Probes) {
            if (cube.found) {
                break;
            }
            const Vec3 probed{point.x + probe[0], point.y + probe[1], point.z + probe[2]};
            const auto leaf_number = tree_.leaf_at(probed);
            const auto leaf = leaf_number * tree_.leaf_size;
            const auto sample_total = read_value<std::uint16_t>(ambient_index_, leaf_number * 4);
            const auto first_sample = read_value<std::uint16_t>(ambient_index_, leaf_number * 4 + 2);
            if (sample_total == 0 || static_cast<std::size_t>(first_sample) + sample_total > sample_count) {
                continue;
            }
            float low[3];
            float high[3];
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = read_value<int16_t>(tree_.leaves, leaf + 8 + axis * 2);
                high[axis] = read_value<int16_t>(tree_.leaves, leaf + 14 + axis * 2);
            }
            const float position[3]{probed.x, probed.y, probed.z};
            float best = std::numeric_limits<float>::max();
            std::size_t best_sample = first_sample;
            bool usable = false;
            const bool leaf_sees_sky =
                (read_value<std::uint16_t>(tree_.leaves, leaf + 6) >> 9 & LeafFlagSky) != 0;
            for (std::size_t sample = first_sample; sample < first_sample + sample_total; ++sample) {
                if (!allow_black) {
                    float peak = 0.0f;
                    for (std::size_t face = 0; face < 6; ++face) {
                        const auto color = sample * SampleSize + face * 4;
                        const auto exponent = static_cast<std::int8_t>(ambient_samples_[color + 3]);
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            peak = std::max(peak, rgbexp_to_linear(ambient_samples_[color + channel], exponent));
                        }
                    }
                    // A sky-visible leaf cannot really be near black, so such a
                    // sample is a placement artefact like a black one.
                    if (peak < (leaf_sees_sky ? 0.05f : 0.01f)) {
                        continue;
                    }
                }
                usable = true;
                float squared = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float fraction = ambient_samples_[sample * SampleSize + 24 + axis] / 255.0f;
                    const float delta = low[axis] + (high[axis] - low[axis]) * fraction - position[axis];
                    squared += delta * delta;
                }
                if (squared < best) {
                    best = squared;
                    best_sample = sample;
                }
            }
            if (!usable) {
                continue;
            }
            cube.found = true;
            cube.sun_visible = (read_value<std::uint16_t>(tree_.leaves, leaf + 6) >> 9 & LeafFlagSky) != 0;
            for (std::size_t face = 0; face < 6; ++face) {
                const auto color = best_sample * SampleSize + face * 4;
                const auto exponent = static_cast<std::int8_t>(ambient_samples_[color + 3]);
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    cube.color[face][channel] =
                        rgbexp_to_linear(ambient_samples_[color + channel], exponent);
                }
            }
            if (bank && !bank_ambient_.empty()) {
                const auto found=bank_ambient_.find(best_sample);
                if(found!=bank_ambient_.end())
                    for(int slot=0;slot<4;++slot) if((*bank)[slot]>0)
                        for(int face=0;face<6;++face) for(int c=0;c<3;++c)
                            cube.color[face][c]+=found->second.color[slot][face][c]*(*bank)[slot];
            }
            break;
        }
        return cube;
    }
};

BspProbe::BspProbe(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

AmbientCube BspProbe::sample(const Vec3& point) const {
    return impl_->sample(point);
}

AmbientCube BspProbe::sample(const Vec3& point, const BankLightWeights& weights) const {
    return impl_->sample(point,&weights);
}

bool BspProbe::is_solid(const Vec3& point) const {
    const auto leaf = impl_->tree_.leaf_at(point);
    return (read_value<std::int32_t>(impl_->tree_.leaves, leaf * impl_->tree_.leaf_size) & 1) != 0;
}

bool BspProbe::is_water(const Vec3& point) const {
    const auto leaf = impl_->tree_.leaf_at(point);
    const auto contents = read_value<std::int32_t>(impl_->tree_.leaves, leaf * impl_->tree_.leaf_size);
    return (contents & 0x30) != 0; // Source CONTENTS_WATER | CONTENTS_SLIME
}

BspProbe BspReader::make_probe(bool bake_bank_lighting) const {
    const auto bsp = BspFile::open(path_);
    const BspFile& file = *bsp;
    const auto& header = file.header();
    auto impl = std::make_shared<BspProbe::Impl>();
    impl->tree_ = load_tree(file, header);
    impl->ambient_index_ = read_lump(file, AmbientIndexLump);
    impl->ambient_samples_ = read_lump(file, AmbientLightingLump);
    constexpr std::size_t SampleSize = 28;
    if (impl->ambient_index_.size() % 4 != 0
        || impl->ambient_index_.size() / 4 != impl->tree_.leaf_count()
        || impl->ambient_samples_.size() % SampleSize != 0) {
        throw std::runtime_error("BSP ambient lighting lumps have unexpected sizes.");
    }
    if (bake_bank_lighting) impl->bake_bank(read_lump(file,WorldLightLump),bank_styles_);
    impl->sky_clusters = sky_clusters_;
    impl->sky_floor_z = sky_floor_z_;
    return BspProbe(std::move(impl));
}

std::vector<AmbientCube> BspReader::ambient_cubes(const std::vector<Vec3>& points) const {
    const auto probe = make_probe();
    std::vector<AmbientCube> result;
    result.reserve(points.size());
    for (const auto& point : points) {
        result.push_back(probe.sample(point));
    }
    return result;
}

const std::vector<Triangle>& BspReader::world_triangles() const noexcept {
    return world_triangles_;
}

std::vector<Triangle> BspReader::release_world_triangles() noexcept {
    return std::move(world_triangles_);
}

std::optional<PlayerSpawnPoint> BspReader::first_player_spawn() const {
    const auto bsp = BspFile::open(path_);
    const BspFile& file = *bsp;
    const auto& header = file.header();
    const auto entities = read_lump(file, EntityLump);
    const std::string text(
        reinterpret_cast<const char*>(entities.data()), entities.size());
    std::optional<PlayerSpawnPoint> fallback;
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const auto begin = text.find('{', cursor);
        if (begin == std::string::npos) {
            break;
        }
        const auto end = text.find('}', begin + 1);
        if (end == std::string::npos) {
            throw std::runtime_error("BSP entity lump has an unterminated entity.");
        }
        cursor = end + 1;
        const auto entity = text.substr(begin + 1, end - begin - 1);
        const auto classname = entity_value(entity, "classname");
        const bool primary = classname == "info_player_start";
        const bool supported = primary
            || classname == "info_player_deathmatch"
            || classname == "info_player_counterterrorist"
            || classname == "info_player_terrorist"
            || classname == "info_player_combine"
            || classname == "info_player_rebel";
        if (!supported) {
            continue;
        }
        std::istringstream origin_stream(entity_value(entity, "origin"));
        PlayerSpawnPoint spawn{};
        if (!(origin_stream >> spawn.origin.x >> spawn.origin.y >> spawn.origin.z)) {
            continue;
        }
        std::istringstream angles_stream(entity_value(entity, "angles"));
        float pitch = 0.0f;
        float roll = 0.0f;
        if (angles_stream >> pitch >> spawn.yaw_degrees >> roll) {
            (void)pitch;
            (void)roll;
        } else {
            const auto angle = entity_value(entity, "angle");
            if (!angle.empty()) {
                std::istringstream angle_stream(angle);
                angle_stream >> spawn.yaw_degrees;
            }
        }
        if (primary) {
            return spawn;
        }
        if (!fallback) {
            fallback = spawn;
        }
    }
    return fallback;
}

} // namespace gmod::source::bsp
