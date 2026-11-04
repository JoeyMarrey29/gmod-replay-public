#include "source/model/StaticPropMeshImporter.h"

#include "source/Vfs.h"

#include "source/model/Animation.h"

#include <cstring>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace gmod::source::model {
namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    if (!vfs::exists(path)) throw std::runtime_error("Could not open model file: " + path.string());
    return vfs::read(path);
}

template <typename T>
T value(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || sizeof(T) > data.size() - offset)
        throw std::runtime_error("Model structure exceeds file bounds.");
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(T));
    return result;
}

std::size_t relative(
    std::size_t base, std::int32_t offset, std::size_t size) {
    // Offsets are signed: a structure may point back at data stored before it.
    const auto target = static_cast<std::int64_t>(base) + offset;
    if (base > size || target < 0 || static_cast<std::uint64_t>(target) > size)
        throw std::runtime_error("Model relative offset is invalid.");
    return static_cast<std::size_t>(target);
}

std::size_t count(std::int32_t value, const char* what) {
    if (value < 0 || value > 1000000)
        throw std::runtime_error(std::string("Invalid ") + what + " count.");
    return static_cast<std::size_t>(value);
}

std::string read_string(
    const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset >= data.size()) {
        throw std::runtime_error("Model string offset is invalid.");
    }
    const auto* begin = reinterpret_cast<const char*>(data.data() + offset);
    const auto remaining = data.size() - offset;
    const auto* end = static_cast<const char*>(std::memchr(begin, '\0', remaining));
    if (!end) {
        throw std::runtime_error("Model string is not null-terminated.");
    }
    return std::string(begin, end);
}

std::string normalize_material_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.rfind("materials/", 0) == 0) {
        path.erase(0, 10);
    }
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return path;
}

struct MdlMesh { std::int32_t material; std::int32_t vertex_count; std::int32_t vertex_offset; };

// Maps each of this model's bones to the matching bone of `skeleton`, walking up
// the parent chain for bones the skeleton lacks (e.g. accessory helper bones).
std::vector<int> map_bones_to_skeleton(
    const std::filesystem::path& mdl_path, const std::vector<std::string>& skeleton,
    const std::vector<std::pair<std::string, std::string>>* aliases = nullptr,
    const std::string& fallback_bone = {}) {
    const auto bones = read_bones(mdl_path);
    std::vector<int> map(bones.size(), 0);
    int fallback = -1;
    if (!fallback_bone.empty()) {
        const auto it = std::find(skeleton.begin(), skeleton.end(), fallback_bone);
        if (it != skeleton.end()) fallback = static_cast<int>(it - skeleton.begin());
    }
    for (std::size_t index = 0; index < bones.size(); ++index) {
        int current = static_cast<int>(index);
        bool matched = false;
        while (current >= 0) {
            auto name = bones[static_cast<std::size_t>(current)].name;
            auto found = std::find(skeleton.begin(), skeleton.end(), name);
            if (found == skeleton.end() && aliases) {
                for (const auto& [from, to] : *aliases) {
                    if (from == name) {
                        found = std::find(skeleton.begin(), skeleton.end(), to);
                        break;
                    }
                }
            }
            if (found != skeleton.end()) {
                map[index] = static_cast<int>(found - skeleton.begin());
                matched = true;
                break;
            }
            current = bones[static_cast<std::size_t>(current)].parent;
        }
        if (!matched && fallback >= 0) map[index] = fallback;
    }
    return map;
}

void pack_skin(const ModelVertex& vertex, const std::vector<int>& map,
               std::uint32_t& bones, std::uint32_t& weights) {
    const int count = std::min<int>(vertex.bone_count, 3);
    if (count <= 0) {
        bones = weights = 0;
        return;
    }
    std::uint32_t ids[3]{};
    float w[3]{};
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        const auto id = static_cast<std::size_t>(vertex.bone_id[i]);
        const int mapped = id < map.size() ? map[id] : 0;
        ids[i] = static_cast<std::uint32_t>(std::clamp(mapped, 0, 255));
        w[i] = std::max(vertex.bone_weight[i], 0.0f);
        sum += w[i];
    }
    if (sum <= 1e-6f) {
        w[0] = 1.0f;
        sum = 1.0f;
    }
    const auto w0 = static_cast<std::uint32_t>(std::lround(std::clamp(w[0] / sum, 0.0f, 1.0f) * 65535.0f));
    const auto w1 = count > 1
        ? static_cast<std::uint32_t>(std::lround(std::clamp(w[1] / sum, 0.0f, 1.0f) * 65535.0f))
        : 0u;
    bones = ids[0] | (ids[1] << 8) | (ids[2] << 16) | (static_cast<std::uint32_t>(count) << 24);
    weights = w0 | (std::min(w1, 65535u - w0) << 16);
}

// Net transform of sequence 0's root-bone pose applied to the reference mesh
// (animated pose * the bone's pose-to-bone). Door models are authored turned
// 90 degrees and only the idle animation puts the closed door in place; models
// without a root-bone animation keep the identity. Row-major 3x4: out = R*v + t.
std::array<float, 12> idle_pose_transform(const std::vector<std::uint8_t>& mdl) {
    std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const auto bone_count = value<std::int32_t>(mdl, 156);
    const auto bone_offset = value<std::int32_t>(mdl, 160);
    const auto anim_count = value<std::int32_t>(mdl, 180);
    const auto anim_offset = value<std::int32_t>(mdl, 184);
    const auto sequence_count = value<std::int32_t>(mdl, 188);
    const auto sequence_offset = value<std::int32_t>(mdl, 192);
    if (bone_count <= 0 || sequence_count <= 0 || anim_count <= 0 || bone_offset < 0
        || anim_offset < 0 || sequence_offset < 0
        || static_cast<std::size_t>(bone_offset) + 216 > mdl.size()
        || static_cast<std::size_t>(sequence_offset) + 212 > mdl.size()) {
        return identity;
    }
    const std::size_t bone = static_cast<std::size_t>(bone_offset);
    if (value<std::int32_t>(mdl, bone + 4) != -1) {
        return identity;
    }
    const std::size_t sequence = static_cast<std::size_t>(sequence_offset);
    const auto blend_table = sequence + static_cast<std::size_t>(
        value<std::int32_t>(mdl, sequence + 60));
    if (blend_table + 4 > mdl.size()) {
        return identity;
    }
    const auto anim_index = value<std::int32_t>(mdl, blend_table);
    if (anim_index < 0 || anim_index >= anim_count) {
        return identity;
    }
    const std::size_t desc = static_cast<std::size_t>(anim_offset) + anim_index * 100u;
    if (desc + 100 > mdl.size()) {
        return identity;
    }
    const std::size_t first = desc + static_cast<std::size_t>(
        std::max<std::int32_t>(value<std::int32_t>(mdl, desc + 56), 0));
    if (first + 4 > mdl.size() || mdl[first] != 0) {
        return identity;
    }
    const auto flags = mdl[first + 1];
    // Only raw (non-curve) root-bone channels are supported.
    if ((flags & (0x04 | 0x08 | 0x10)) != 0) {
        return identity;
    }
    float quat[4] = {value<float>(mdl, bone + 44), value<float>(mdl, bone + 48),
                     value<float>(mdl, bone + 52), value<float>(mdl, bone + 56)};
    float pos[3] = {value<float>(mdl, bone + 32), value<float>(mdl, bone + 36),
                    value<float>(mdl, bone + 40)};
    std::size_t cursor = first + 4;
    auto half = [&](std::size_t at) {
        const std::uint16_t bits = value<std::uint16_t>(mdl, at);
        const int exponent = (bits >> 10) & 31;
        const float fraction = static_cast<float>(bits & 1023);
        float result = exponent == 0 ? std::ldexp(fraction, -24)
            : exponent == 31 ? 65504.0f : std::ldexp(1024.0f + fraction, exponent - 25);
        return (bits & 0x8000) != 0 ? -result : result;
    };
    if ((flags & 0x20) != 0) {
        const auto packed = value<std::uint64_t>(mdl, cursor);
        cursor += 8;
        const auto unpack = [](std::uint64_t bits) {
            return (static_cast<float>(bits & 0x1FFFFF) - 1048576.0f) / 1048576.5f;
        };
        quat[0] = unpack(packed);
        quat[1] = unpack(packed >> 21);
        quat[2] = unpack(packed >> 42);
        const float w = std::sqrt(std::max(0.0f,
            1.0f - quat[0] * quat[0] - quat[1] * quat[1] - quat[2] * quat[2]));
        quat[3] = (packed >> 63) != 0 ? -w : w;
    } else if ((flags & 0x02) != 0) {
        // Quaternion48: x and y are 16-bit, z is 15-bit with w's sign in the top bit.
        quat[0] = (static_cast<float>(value<std::uint16_t>(mdl, cursor)) - 32768.0f) / 32768.0f;
        quat[1] = (static_cast<float>(value<std::uint16_t>(mdl, cursor + 2)) - 32768.0f) / 32768.0f;
        quat[2] = (static_cast<float>(value<std::uint16_t>(mdl, cursor + 4) & 0x7FFF) - 16384.0f)
            / 16384.0f;
        const float w = std::sqrt(std::max(0.0f,
            1.0f - quat[0] * quat[0] - quat[1] * quat[1] - quat[2] * quat[2]));
        quat[3] = (value<std::uint16_t>(mdl, cursor + 4) & 0x8000) != 0 ? -w : w;
        cursor += 6;
    }
    if ((flags & 0x01) != 0) {
        pos[0] = half(cursor);
        pos[1] = half(cursor + 2);
        pos[2] = half(cursor + 4);
    }
    const float x = quat[0];
    const float y = quat[1];
    const float z = quat[2];
    const float w = quat[3];
    const float rotation[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float pose_to_bone[3][4];
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            pose_to_bone[row][column] = value<float>(mdl, bone + 96 + (row * 4 + column) * 4);
        }
    }
    std::array<float, 12> net{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = column == 3 ? pos[row] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += rotation[row][k] * pose_to_bone[k][column];
            }
            net[row * 4 + column] = sum;
        }
    }
    return net;
}

} // namespace

namespace {

bool is_identity_transform(const std::array<float, 12>& m) {
    constexpr std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (std::size_t i = 0; i < 12; ++i) {
        if (std::fabs(m[i] - identity[i]) > 1e-4f) return false;
    }
    return true;
}

// The root bone's idle pose through the full animation reader (compressed channels too),
// times the bone's pose-to-bone. Identity when the model has no usable animation.
std::array<float, 12> idle_pose_via_animation(const std::filesystem::path& mdl_path) {
    std::array<float, 12> net{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    try {
        const AnimationModel animation(mdl_path, {});
        if (animation.sequence_count() == 0 || animation.bones().empty()) return net;
        const auto& bone = animation.bones().front();
        if (bone.parent != -1) return net;
        std::vector<BonePose> pose;
        animation.sample(0, std::vector<float>(animation.pose_parameter_count(), 0.0f), 0.0f, pose);
        const float x = pose[0].quat[0], y = pose[0].quat[1], z = pose[0].quat[2], w = pose[0].quat[3];
        const float rotation[3][3] = {
            {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                float sum = column == 3 ? pose[0].pos[row] : 0.0f;
                for (int k = 0; k < 3; ++k) sum += rotation[row][k] * bone.pose_to_bone[k * 4 + column];
                net[static_cast<std::size_t>(row * 4 + column)] = sum;
            }
        }
    } catch (const std::exception&) {
    }
    return net;
}

} // namespace

StaticPropMeshResult StaticPropMeshImporter::read(
    const std::string& model_path,
    const std::filesystem::path& mdl_path,
    const std::filesystem::path& vvd_path,
    const std::filesystem::path& vtx_path,
    int skin,
    const BodySelection& selection) const {
    const auto mdl = read_file(mdl_path);
    const auto vtx = read_file(vtx_path);
    const auto vvd = VvdReader(vvd_path).read();
    std::vector<int> bone_map;
    if (selection.skeleton) {
        bone_map = map_bones_to_skeleton(
            mdl_path, *selection.skeleton, selection.bone_aliases, selection.fallback_bone);
    }
    if (mdl.size() < 240 || vtx.size() < 36)
        throw std::runtime_error("Static-prop MDL/VTX header is incomplete.");

    const auto texture_count = count(value<std::int32_t>(mdl, 204), "MDL texture");
    const auto texture_offset = value<std::int32_t>(mdl, 208);
    const auto texture_array = relative(0, texture_offset, mdl.size());
    std::vector<std::string> texture_names;
    texture_names.reserve(texture_count);
    for (std::size_t index = 0; index < texture_count; ++index) {
        const auto texture = texture_array + index * 64;
        if (texture > mdl.size() || 64 > mdl.size() - texture) {
            throw std::runtime_error("MDL texture table exceeds file.");
        }
        const auto name_offset = value<std::int32_t>(mdl, texture);
        texture_names.push_back(read_string(mdl, relative(texture, name_offset, mdl.size())));
    }
    const auto directory_count = count(value<std::int32_t>(mdl, 212), "MDL material directory");
    const auto directory_offset = value<std::int32_t>(mdl, 216);
    const auto directory_array = relative(0, directory_offset, mdl.size());
    std::vector<std::string> material_directories;
    material_directories.reserve(directory_count);
    for (std::size_t index = 0; index < directory_count; ++index) {
        const auto string_offset = value<std::int32_t>(mdl, directory_array + index * 4);
        material_directories.push_back(normalize_material_path(
            read_string(mdl, relative(0, string_offset, mdl.size()))));
    }
    const auto skin_reference_count =
        count(value<std::int32_t>(mdl, 220), "MDL skin reference");
    const auto skin_family_count =
        count(value<std::int32_t>(mdl, 224), "MDL skin family");
    const auto skin_offset = value<std::int32_t>(mdl, 228);
    const auto skin_array = relative(0, skin_offset, mdl.size());
    const auto selected_skin = skin_family_count == 0 || skin < 0
        ? 0
        : std::min(static_cast<std::size_t>(skin), skin_family_count - 1);
    auto material_candidates_for_mesh = [&](std::int32_t mesh_material) {
        if (mesh_material < 0) {
            throw std::runtime_error("MDL mesh material index is invalid.");
        }
        auto texture_index = static_cast<std::size_t>(mesh_material);
        if (skin_reference_count > 0 && skin_family_count > 0
            && texture_index < skin_reference_count) {
            const auto skin_entry = skin_array
                + (selected_skin * skin_reference_count + texture_index) * sizeof(std::uint16_t);
            texture_index = value<std::uint16_t>(mdl, skin_entry);
        }
        if (texture_index >= texture_names.size()) {
            return std::vector<std::string>{};
        }
        auto texture_name = normalize_material_path(texture_names[texture_index]);
        if (texture_name.empty()) {
            return std::vector<std::string>{};
        }
        std::vector<std::string> candidates;
        for (const auto& directory : material_directories) {
            const auto candidate = normalize_material_path(
                directory.empty() ? texture_name : directory + "/" + texture_name);
            if (!candidate.empty()
                && std::find(candidates.begin(), candidates.end(), candidate)
                    == candidates.end()) {
                candidates.push_back(candidate);
            }
        }
        if (candidates.empty()) {
            candidates.push_back(texture_name);
        }
        if (selection.material_remap) {
            for (const auto& [original, replacement] : *selection.material_remap) {
                const auto key = normalize_material_path(original);
                if (std::find(candidates.begin(), candidates.end(), key) != candidates.end()) {
                    return std::vector<std::string>{normalize_material_path(replacement)};
                }
            }
        }
        return candidates;
    };

    const auto body_count = count(value<std::int32_t>(mdl, 232), "MDL bodypart");
    if (body_count == 0) throw std::runtime_error("Static-prop MDL has no bodyparts.");
    const auto vtx_body_count = count(value<std::int32_t>(vtx, 28), "VTX bodypart");
    if (vtx_body_count == 0) throw std::runtime_error("Static-prop VTX has no bodyparts.");
    const auto body_array = relative(
        0, value<std::int32_t>(mdl, 236), mdl.size());
    const auto vtx_body_array = relative(
        0, value<std::int32_t>(vtx, 32), vtx.size());

    StaticPropMeshResult result{};
    std::vector<Matrix3x4> idle_matrices;
    const auto* bone_matrices = selection.bone_matrices;
    if (selection.apply_idle_pose && !selection.skeleton && !bone_matrices) {
        const AnimationModel animation(mdl_path, {});
        if (animation.sequence_count() > 0 && !animation.bones().empty()) {
            std::vector<BonePose> idle;
            animation.sample(0, std::vector<float>(animation.pose_parameter_count(), 0.0f), 0.0f, idle);
            SkeletonPoser(animation.bones(), animation).palette(idle, idle_matrices);
            bone_matrices = &idle_matrices;
        }
    }
    // Plain props retain the root-pose shortcut; doors use weighted bone matrices above.
    const bool plain = !selection.skeleton && !bone_matrices;
    auto pose = std::array<float, 12>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (!bone_matrices && (selection.apply_idle_pose || plain)) {
        pose = idle_pose_transform(mdl);
        if (is_identity_transform(pose)) pose = idle_pose_via_animation(mdl_path);
    }
    const bool use_pose = !bone_matrices && (selection.apply_idle_pose || plain) && !is_identity_transform(pose);
    result.model_path = model_path;
    const auto bodies_to_read = std::min(body_count, vtx_body_count);
    for (std::size_t body_index = 0; body_index < bodies_to_read; ++body_index) {
        const auto body = body_array + body_index * 16;
        const auto vtx_body = vtx_body_array + body_index * 8;
        if (body > mdl.size() || 16 > mdl.size() - body
            || vtx_body > vtx.size() || 8 > vtx.size() - vtx_body) {
            throw std::runtime_error("MDL/VTX bodypart exceeds file.");
        }
        const auto body_model_count =
            count(value<std::int32_t>(mdl, body + 4), "MDL model");
        const auto vtx_model_count =
            count(value<std::int32_t>(vtx, vtx_body), "VTX model");
        if (body_model_count == 0 || vtx_model_count == 0) {
            continue;
        }
        const auto model_array = relative(
            body, value<std::int32_t>(mdl, body + 12), mdl.size());
        const auto body_name = normalize_material_path(read_string(mdl,
            relative(body, value<std::int32_t>(mdl, body), mdl.size())));
        float normal_offset = selection.normal_offset;
        for (const auto& [name, offset] : selection.bodygroup_normal_offsets)
            if (name == body_name) normal_offset += offset;
        // Engine rule: choice = (body / base) % models; overrides win.
        const auto body_base = std::max<std::int32_t>(value<std::int32_t>(mdl, body + 8), 1);
        std::size_t choice = static_cast<std::size_t>(
            std::max(selection.body, 0) / body_base) % body_model_count;
        for (const auto& [part, wanted] : selection.overrides) {
            if (part == body_index && wanted >= 0
                && static_cast<std::size_t>(wanted) < body_model_count) {
                choice = static_cast<std::size_t>(wanted);
            }
        }
        for (const auto& [name, wanted] : selection.named_overrides) {
            if (name == normalize_material_path(read_string(mdl,
                    relative(body, value<std::int32_t>(mdl, body), mdl.size())))
                && wanted >= 0 && static_cast<std::size_t>(wanted) < body_model_count)
                choice = static_cast<std::size_t>(wanted);
        }
        if (choice >= vtx_model_count) {
            choice = 0;
        }
        const auto model = model_array + choice * 148;
        if (model > mdl.size() || 148 > mdl.size() - model) {
            throw std::runtime_error("MDL model exceeds file.");
        }
        const auto mesh_count = count(
            value<std::int32_t>(mdl, model + 72), "MDL mesh");
        const auto vertex_base_bytes = value<std::int32_t>(mdl, model + 84);
        if (vertex_base_bytes < 0 || vertex_base_bytes % 48 != 0) {
            throw std::runtime_error("MDL vertex index is not a VVD vertex offset.");
        }
        const auto mesh_array = relative(
            model, value<std::int32_t>(mdl, model + 76), mdl.size());
        const auto vtx_model_array = relative(
            vtx_body, value<std::int32_t>(vtx, vtx_body + 4), vtx.size());
        const auto vtx_model = vtx_model_array + choice * 8;
        if (vtx_model > vtx.size() || 8 > vtx.size() - vtx_model) {
            throw std::runtime_error("VTX model exceeds file.");
        }
        const auto lod_count =
            count(value<std::int32_t>(vtx, vtx_model), "VTX LOD");
        if (lod_count == 0) {
            continue;
        }
        const auto lod_array = relative(
            vtx_model, value<std::int32_t>(vtx, vtx_model + 4), vtx.size());
        const auto lod = lod_array;
        if (lod > vtx.size() || 8 > vtx.size() - lod) {
            throw std::runtime_error("VTX LOD exceeds file.");
        }
        const auto vtx_mesh_count =
            count(value<std::int32_t>(vtx, lod), "VTX mesh");
        const auto vtx_mesh_array = relative(
            lod, value<std::int32_t>(vtx, lod + 4), vtx.size());
        const auto meshes_to_read = std::min(mesh_count, vtx_mesh_count);
        result.mesh_count += meshes_to_read;
        for (std::size_t mesh_index = 0; mesh_index < meshes_to_read; ++mesh_index) {
            const auto md = mesh_array + mesh_index * 116;
            const auto vm = vtx_mesh_array + mesh_index * 9;
            if (md > mdl.size() || 116 > mdl.size() - md
                || vm > vtx.size() || 9 > vtx.size() - vm) {
                throw std::runtime_error("Static-prop mesh exceeds file.");
            }
            const MdlMesh source{
                value<std::int32_t>(mdl, md),
                value<std::int32_t>(mdl, md + 8),
                value<std::int32_t>(mdl, md + 12)};
            const auto vertex_start = result.vertices.size();
            const auto index_start = result.indices.size();
            const auto material_candidates =
                material_candidates_for_mesh(source.material);
            const auto material_name = material_candidates.empty()
                ? std::string{}
                : material_candidates.front();
            // Shares one output vertex per referenced VVD vertex within this mesh.
            std::unordered_map<std::size_t, std::uint32_t> vertex_remap;
            const auto groups = count(
                value<std::int32_t>(vtx, vm), "VTX stripgroup");
            const auto group_array = relative(
                vm, value<std::int32_t>(vtx, vm + 4), vtx.size());
            for (std::size_t group_index = 0; group_index < groups; ++group_index) {
                const auto group = group_array + group_index * 25;
                if (group > vtx.size() || 25 > vtx.size() - group) {
                    throw std::runtime_error("VTX stripgroup exceeds file.");
                }
                const auto group_vertex_count = count(
                    value<std::int32_t>(vtx, group), "VTX vertex");
                const auto vertex_array = relative(
                    group, value<std::int32_t>(vtx, group + 4), vtx.size());
                const auto index_count = count(
                    value<std::int32_t>(vtx, group + 8), "VTX index");
                const auto index_array = relative(
                    group, value<std::int32_t>(vtx, group + 12), vtx.size());
                const auto strip_count = count(
                    value<std::int32_t>(vtx, group + 16), "VTX strip");
                const auto strip_array = relative(
                    group, value<std::int32_t>(vtx, group + 20), vtx.size());
                std::vector<std::uint16_t> indices;
                indices.reserve(index_count);
                for (std::size_t index = 0; index < index_count; ++index) {
                    indices.push_back(value<std::uint16_t>(vtx, index_array + index * 2));
                }
                for (std::size_t strip_index = 0; strip_index < strip_count; ++strip_index) {
                    const auto strip = strip_array + strip_index * 27;
                    if (strip > vtx.size() || 27 > vtx.size() - strip) {
                        throw std::runtime_error("VTX strip exceeds file.");
                    }
                    const auto strip_indices = value<std::int32_t>(vtx, strip);
                    const auto strip_offset = value<std::int32_t>(vtx, strip + 4);
                    const auto flags = value<std::uint8_t>(vtx, strip + 18);
                    if (strip_indices < 3 || strip_offset < 0
                        || static_cast<std::size_t>(strip_offset)
                            + static_cast<std::size_t>(strip_indices) > indices.size()) {
                        continue;
                    }
                    const auto add_triangle = [&](std::uint16_t a, std::uint16_t b,
                                                  std::uint16_t c) {
                        if (a >= group_vertex_count || b >= group_vertex_count
                            || c >= group_vertex_count) {
                            return;
                        }
                        const auto original = [&](std::uint16_t index) {
                            return value<std::uint16_t>(
                                vtx, vertex_array + static_cast<std::size_t>(index) * 9 + 4);
                        };
                        const auto source_index = [&](std::uint16_t index) {
                            const auto id = original(index);
                            const auto local =
                                static_cast<std::int64_t>(source.vertex_offset) + id;
                            const auto absolute =
                                static_cast<std::int64_t>(vertex_base_bytes / 48) + local;
                            if (local < 0 || absolute < 0
                                || static_cast<std::size_t>(absolute) >= vvd.vertices.size()) {
                                throw std::runtime_error(
                                    "VTX references a vertex outside VVD.");
                            }
                            const auto key = static_cast<std::size_t>(absolute);
                            const auto existing = vertex_remap.find(key);
                            if (existing != vertex_remap.end()) {
                                return existing->second;
                            }
                            const auto& input = vvd.vertices[key];
                            StaticPropMeshVertex vertex{
                                {input.position[0], input.position[1], input.position[2]},
                                {input.normal[0], input.normal[1], input.normal[2]},
                                {input.uv[0], input.uv[1]}};
                            for (int axis = 0; axis < 3; ++axis) {
                                vertex.position[axis] +=
                                    vertex.normal[axis] * normal_offset;
                            }
                            if (selection.skeleton) {
                                pack_skin(input, bone_map, vertex.skin_bones, vertex.skin_weights);
                            }
                            if (bone_matrices) {
                                const auto& matrices = *bone_matrices;
                                const int count = std::max<int>(input.bone_count, 1);
                                float weights[3]{1.0f, 0.0f, 0.0f};
                                float total = 0.0f;
                                for (int i = 0; i < std::min(count, 3); ++i) {
                                    weights[i] = input.bone_count > 0 ? input.bone_weight[i] : 1.0f;
                                    total += weights[i];
                                }
                                if (total <= 1e-6f) {
                                    weights[0] = total = 1.0f;
                                }
                                float moved[3]{};
                                float turned[3]{};
                                for (int i = 0; i < std::min(count, 3); ++i) {
                                    const auto id = static_cast<std::size_t>(input.bone_id[i]);
                                    if (id >= matrices.size() || weights[i] <= 0.0f) continue;
                                    const auto& m = matrices[id];
                                    const float w = weights[i] / total;
                                    for (int row = 0; row < 3; ++row) {
                                        moved[row] += w * (m[row * 4] * vertex.position[0]
                                            + m[row * 4 + 1] * vertex.position[1]
                                            + m[row * 4 + 2] * vertex.position[2]
                                            + m[row * 4 + 3]);
                                        turned[row] += w * (m[row * 4] * vertex.normal[0]
                                            + m[row * 4 + 1] * vertex.normal[1]
                                            + m[row * 4 + 2] * vertex.normal[2]);
                                    }
                                }
                                std::copy(moved, moved + 3, vertex.position);
                                std::copy(turned, turned + 3, vertex.normal);
                            }
                            if (use_pose) {
                                float moved[3];
                                float turned[3];
                                for (int row = 0; row < 3; ++row) {
                                    moved[row] = pose[row * 4] * vertex.position[0]
                                        + pose[row * 4 + 1] * vertex.position[1]
                                        + pose[row * 4 + 2] * vertex.position[2]
                                        + pose[row * 4 + 3];
                                    turned[row] = pose[row * 4] * vertex.normal[0]
                                        + pose[row * 4 + 1] * vertex.normal[1]
                                        + pose[row * 4 + 2] * vertex.normal[2];
                                }
                                std::copy(moved, moved + 3, vertex.position);
                                std::copy(turned, turned + 3, vertex.normal);
                            }
                            result.vertices.push_back(vertex);
                            const auto output =
                                static_cast<std::uint32_t>(result.vertices.size() - 1);
                            vertex_remap.emplace(key, output);
                            return output;
                        };
                        result.indices.push_back(source_index(a));
                        result.indices.push_back(source_index(b));
                        result.indices.push_back(source_index(c));
                        ++result.triangle_count;
                    };
                    const auto start = static_cast<std::size_t>(strip_offset);
                    if ((flags & 1) != 0) {
                        for (std::size_t index = 0;
                             index + 2 < static_cast<std::size_t>(strip_indices); index += 3) {
                            add_triangle(indices[start + index],
                                indices[start + index + 1], indices[start + index + 2]);
                        }
                    } else if ((flags & 2) != 0) {
                        for (std::size_t index = 0;
                             index + 2 < static_cast<std::size_t>(strip_indices); ++index) {
                            add_triangle(indices[start + index],
                                indices[start + index + 1], indices[start + index + 2]);
                        }
                    }
                }
            }
            const auto vertex_count = result.vertices.size() - vertex_start;
            if (vertex_count > 0) {
                result.batches.push_back({
                    material_name, vertex_start, vertex_count, index_start,
                    result.indices.size() - index_start, material_candidates});
            }
        }
    }
    return result;
}
std::vector<ModelBodyGroup> read_bodygroups(const std::filesystem::path& path) {
    const auto mdl = read_file(path);
    const auto size = count(value<std::int32_t>(mdl, 232), "MDL bodypart");
    const auto offset = relative(0, value<std::int32_t>(mdl, 236), mdl.size());
    std::vector<ModelBodyGroup> result;
    for (std::size_t i = 0; i < size; ++i) {
        const auto body = offset + i * 16;
        result.push_back({normalize_material_path(read_string(mdl,
            relative(body, value<std::int32_t>(mdl, body), mdl.size()))),
            static_cast<int>(count(value<std::int32_t>(mdl, body + 4), "MDL bodygroup models")),
            std::max(value<std::int32_t>(mdl, body + 8), 1)});
    }
    return result;
}
} // namespace gmod::source::model
