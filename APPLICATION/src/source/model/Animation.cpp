#include "source/model/Animation.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace gmod::source::model {
namespace {

constexpr int AnimRawPos = 0x01;
constexpr int AnimRawRot = 0x02;
constexpr int AnimAnimPos = 0x04;
constexpr int AnimAnimRot = 0x08;
constexpr int AnimDelta = 0x10;
constexpr int AnimRawRot2 = 0x20;
constexpr int SeqDelta = 0x04;
constexpr int SeqAutoplay = 0x08;
constexpr float kZero[3]{0.0f, 0.0f, 0.0f};

struct Bytes {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    template <typename T>
    T get(std::size_t offset) const {
        if (offset > size || sizeof(T) > size - offset) {
            throw std::runtime_error("Animation data exceeds its file.");
        }
        T value{};
        std::memcpy(&value, data + offset, sizeof(T));
        return value;
    }
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    if (!vfs::exists(path)) throw std::runtime_error("Could not open animation file: " + path.string());
    return vfs::read(path);
}

std::string read_name(const Bytes& bytes, std::size_t offset) {
    std::string result;
    for (std::size_t at = offset; at < bytes.size; ++at) {
        const char c = static_cast<char>(bytes.data[at]);
        if (c == '\0') return result;
        result.push_back(c);
    }
    throw std::runtime_error("Animation name is not terminated.");
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::vector<Bone> parse_bones(const Bytes& mdl) {
    const auto count = mdl.get<std::int32_t>(156);
    const auto offset = mdl.get<std::int32_t>(160);
    if (count < 0 || count > 4096 || offset < 0) {
        throw std::runtime_error("MDL bone table is invalid.");
    }
    std::vector<Bone> bones(static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < bones.size(); ++index) {
        const auto base = static_cast<std::size_t>(offset) + index * 216;
        auto& bone = bones[index];
        bone.name = lower(read_name(mdl, base + static_cast<std::size_t>(mdl.get<std::int32_t>(base))));
        bone.parent = mdl.get<std::int32_t>(base + 4);
        for (int axis = 0; axis < 3; ++axis) {
            bone.pos[axis] = mdl.get<float>(base + 32 + axis * 4);
            bone.rot[axis] = mdl.get<float>(base + 60 + axis * 4);
            bone.pos_scale[axis] = mdl.get<float>(base + 72 + axis * 4);
            bone.rot_scale[axis] = mdl.get<float>(base + 84 + axis * 4);
        }
        for (int axis = 0; axis < 4; ++axis) {
            bone.quat[axis] = mdl.get<float>(base + 44 + axis * 4);
        }
        for (int cell = 0; cell < 12; ++cell) {
            bone.pose_to_bone[cell] = mdl.get<float>(base + 96 + cell * 4);
        }
        if (bone.parent >= static_cast<int>(index)) {
            throw std::runtime_error("MDL bone parents must precede their children.");
        }
    }
    return bones;
}

float half_to_float(std::uint16_t bits) {
    const int exponent = (bits >> 10) & 31;
    const float fraction = static_cast<float>(bits & 1023);
    float value = exponent == 0 ? std::ldexp(fraction, -24)
        : exponent == 31 ? 65504.0f : std::ldexp(1024.0f + fraction, exponent - 25);
    return (bits & 0x8000) != 0 ? -value : value;
}

void quat_from_euler(const float angles[3], float q[4]) {
    // RadianEuler is (roll, pitch, yaw).
    const float sy = std::sin(angles[2] * 0.5f), cy = std::cos(angles[2] * 0.5f);
    const float sp = std::sin(angles[1] * 0.5f), cp = std::cos(angles[1] * 0.5f);
    const float sr = std::sin(angles[0] * 0.5f), cr = std::cos(angles[0] * 0.5f);
    const float sr_cp = sr * cp, cr_sp = cr * sp;
    q[0] = sr_cp * cy - cr_sp * sy;
    q[1] = cr_sp * cy + sr_cp * sy;
    const float cr_cp = cr * cp, sr_sp = sr * sp;
    q[2] = cr_cp * sy - sr_sp * cy;
    q[3] = cr_cp * cy + sr_sp * sy;
}

void quat_normalize(float q[4]) {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length > 1e-8f) {
        for (int i = 0; i < 4; ++i) q[i] /= length;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

void quat_slerp(const float p[4], const float q_in[4], float t, float out[4]) {
    float q[4]{q_in[0], q_in[1], q_in[2], q_in[3]};
    float dot = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
    if (dot < 0.0f) {
        for (float& value : q) value = -value;
        dot = -dot;
    }
    float weight_p = 1.0f - t;
    float weight_q = t;
    if (1.0f - dot > 1e-6f) {
        const float omega = std::acos(std::min(dot, 1.0f));
        const float sine = std::sin(omega);
        weight_p = std::sin((1.0f - t) * omega) / sine;
        weight_q = std::sin(t * omega) / sine;
    }
    for (int i = 0; i < 4; ++i) out[i] = weight_p * p[i] + weight_q * q[i];
    quat_normalize(out);
}

void blend_into(BonePose& accumulated, const BonePose& next, float t) {
    for (int i = 0; i < 3; ++i) {
        accumulated.pos[i] += (next.pos[i] - accumulated.pos[i]) * t;
    }
    float q[4];
    quat_slerp(accumulated.quat, next.quat, t, q);
    std::copy(q, q + 4, accumulated.quat);
}

Matrix3x4 matrix_from_pose(const BonePose& pose) {
    const float x = pose.quat[0], y = pose.quat[1], z = pose.quat[2], w = pose.quat[3];
    return {1.0f - 2.0f * y * y - 2.0f * z * z, 2.0f * x * y - 2.0f * w * z,
            2.0f * x * z + 2.0f * w * y, pose.pos[0],
            2.0f * x * y + 2.0f * w * z, 1.0f - 2.0f * x * x - 2.0f * z * z,
            2.0f * y * z - 2.0f * w * x, pose.pos[1],
            2.0f * x * z - 2.0f * w * y, 2.0f * y * z + 2.0f * w * x,
            1.0f - 2.0f * x * x - 2.0f * y * y, pose.pos[2]};
}

Matrix3x4 multiply(const Matrix3x4& a, const Matrix3x4& b) {
    Matrix3x4 result{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column]
                + a[row * 4 + 2] * b[8 + column];
        }
        result[row * 4 + 3] = a[row * 4] * b[3] + a[row * 4 + 1] * b[7]
            + a[row * 4 + 2] * b[11] + a[row * 4 + 3];
    }
    return result;
}

Matrix3x4 rigid_inverse(const float* matrix) {
    Matrix3x4 inverse{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column)
            inverse[row * 4 + column] = matrix[column * 4 + row];
        for (int column = 0; column < 3; ++column)
            inverse[row * 4 + 3] -= inverse[row * 4 + column] * matrix[column * 4 + 3];
    }
    return inverse;
}

// Run-length encoded channel values (mstudioanimvalue_t). `values` points at the
// first run header. Produces this frame's value and the next frame's value.
void extract_value(const Bytes& bytes, std::size_t values, int frame, float scale,
                   float& v1, float& v2) {
    const auto valid = [&](std::size_t at) { return static_cast<int>(bytes.get<std::uint8_t>(at)); };
    const auto total = [&](std::size_t at) { return static_cast<int>(bytes.get<std::uint8_t>(at + 1)); };
    const auto value = [&](std::size_t at) { return static_cast<float>(bytes.get<std::int16_t>(at)); };
    if (total(values) == 1 && valid(values) == 1) {
        v1 = v2 = value(values + 2) * scale;
        return;
    }
    int k = frame;
    while (total(values) <= k) {
        k -= total(values);
        values += static_cast<std::size_t>(valid(values) + 1) * 2;
        if (total(values) == 0) {
            v1 = v2 = 0.0f;
            return;
        }
    }
    const int run_valid = valid(values);
    const int run_total = total(values);
    if (run_valid > k) {
        v1 = value(values + static_cast<std::size_t>(k + 1) * 2) * scale;
        if (run_valid > k + 1) {
            v2 = value(values + static_cast<std::size_t>(k + 2) * 2) * scale;
        } else if (run_total > k + 1) {
            v2 = v1;
        } else {
            v2 = value(values + static_cast<std::size_t>(run_valid + 2) * 2) * scale;
        }
    } else {
        v1 = value(values + static_cast<std::size_t>(run_valid) * 2) * scale;
        if (run_total > k + 1) {
            v2 = v1;
        } else {
            v2 = value(values + static_cast<std::size_t>(run_valid + 2) * 2) * scale;
        }
    }
}

} // namespace

std::vector<Bone> read_bones(const std::filesystem::path& mdl_path) {
    const auto bytes = read_file(mdl_path);
    return parse_bones({bytes.data(), bytes.size()});
}

std::vector<Attachment> read_attachments(const std::filesystem::path& mdl_path) {
    const auto file = read_file(mdl_path);
    const Bytes bytes{file.data(),file.size()};
    const auto count = bytes.get<std::int32_t>(240);
    const auto offset = bytes.get<std::int32_t>(244);
    if (count < 0 || count > 4096 || offset < 0) throw std::runtime_error("Invalid MDL attachment table.");
    std::vector<Attachment> result;
    for (int i = 0; i < count; ++i) {
        const auto base = static_cast<std::size_t>(offset) + static_cast<std::size_t>(i)*92;
        const auto name_offset = bytes.get<std::int32_t>(base);
        if (name_offset < 0) throw std::runtime_error("Invalid MDL attachment name.");
        Attachment attachment;
        attachment.name = lower(read_name(bytes,base+static_cast<std::size_t>(name_offset)));
        attachment.bone = bytes.get<std::int32_t>(base+8);
        for (int cell = 0; cell < 12; ++cell) attachment.local[cell] = bytes.get<float>(base+12+cell*4);
        result.push_back(std::move(attachment));
    }
    return result;
}

std::vector<std::string> read_include_models(const std::filesystem::path& mdl_path) {
    const auto file = read_file(mdl_path);
    const Bytes bytes{file.data(), file.size()};
    const auto count = bytes.get<std::int32_t>(336);
    const auto offset = bytes.get<std::int32_t>(340);
    std::vector<std::string> includes;
    if (count <= 0 || count > 64 || offset < 0) return includes;
    for (int index = 0; index < count; ++index) {
        const auto base = static_cast<std::size_t>(offset) + static_cast<std::size_t>(index) * 8;
        auto name = lower(read_name(bytes, base + static_cast<std::size_t>(bytes.get<std::int32_t>(base + 4))));
        std::replace(name.begin(), name.end(), '\\', '/');
        if (!name.empty()) includes.push_back(std::move(name));
    }
    return includes;
}

AnimationModel::AnimationModel(const std::filesystem::path& mdl, const std::filesystem::path& ani)
    : mdl_(read_file(mdl)) {
    if (!ani.empty() && vfs::exists(ani)) {
        ani_ = read_file(ani);
    }
    const Bytes bytes{mdl_.data(), mdl_.size()};
    bones_ = parse_bones(bytes);

    const auto parameter_count = bytes.get<std::int32_t>(300);
    const auto parameter_offset = bytes.get<std::int32_t>(304);
    if (parameter_count < 0 || parameter_count > 64 || parameter_offset < 0) {
        throw std::runtime_error("MDL pose parameter table is invalid.");
    }
    for (int index = 0; index < parameter_count; ++index) {
        const auto base = static_cast<std::size_t>(parameter_offset) + static_cast<std::size_t>(index) * 20;
        PoseParameter parameter;
        parameter.name = lower(read_name(
            bytes, base + static_cast<std::size_t>(bytes.get<std::int32_t>(base))));
        parameter.start = bytes.get<float>(base + 8);
        parameter.end = bytes.get<float>(base + 12);
        parameter.loop = bytes.get<float>(base + 16);
        pose_parameters_.push_back(std::move(parameter));
    }

    const auto anim_count = bytes.get<std::int32_t>(180);
    const auto anim_offset = bytes.get<std::int32_t>(184);
    if (anim_count < 0 || anim_count > 100000 || anim_offset < 0) {
        throw std::runtime_error("MDL animation table is invalid.");
    }
    anims_.resize(static_cast<std::size_t>(anim_count));
    for (std::size_t index = 0; index < anims_.size(); ++index) {
        const auto base = static_cast<std::size_t>(anim_offset) + index * 100;
        auto& desc = anims_[index];
        desc.offset = base;
        desc.fps = bytes.get<float>(base + 8);
        desc.frames = bytes.get<std::int32_t>(base + 16);
        desc.block = bytes.get<std::int32_t>(base + 52);
        desc.index = bytes.get<std::int32_t>(base + 56);
        desc.section_index = bytes.get<std::int32_t>(base + 80);
        desc.section_frames = bytes.get<std::int32_t>(base + 84);
        const auto movements = bytes.get<std::int32_t>(base + 20);
        if (movements > 0 && desc.fps > 0.0f && desc.frames > 1) {
            const auto movement_offset = bytes.get<std::int32_t>(base + 24);
            const auto last = base + static_cast<std::size_t>(movement_offset)
                + static_cast<std::size_t>(movements - 1) * 44;
            const float px = bytes.get<float>(last + 32);
            const float py = bytes.get<float>(last + 36);
            const float seconds = static_cast<float>(desc.frames - 1) / desc.fps;
            desc.ground_speed = std::sqrt(px * px + py * py) / seconds;
        }
    }

    const auto sequence_count = bytes.get<std::int32_t>(188);
    const auto sequence_offset = bytes.get<std::int32_t>(192);
    if (sequence_count < 0 || sequence_count > 100000 || sequence_offset < 0) {
        throw std::runtime_error("MDL sequence table is invalid.");
    }
    for (int index = 0; index < sequence_count; ++index) {
        const auto base = static_cast<std::size_t>(sequence_offset) + static_cast<std::size_t>(index) * 212;
        Sequence sequence;
        sequence.name = lower(read_name(
            bytes, base + static_cast<std::size_t>(bytes.get<std::int32_t>(base + 4))));
        sequence.flags = bytes.get<std::int32_t>(base + 12);
        // Optional extras (activity name, autolayers, weightlist): a model that stores them
        // oddly must not stop its plain sequences from loading.
        try {
            if (const auto activity_offset = bytes.get<std::int32_t>(base + 8); activity_offset > 0) {
                sequence.activity = read_name(bytes, base + static_cast<std::size_t>(activity_offset));
            }
            const auto layer_count = bytes.get<std::int32_t>(base + 148);
            const auto layer_offset = bytes.get<std::int32_t>(base + 152);
            if (layer_count > 0 && layer_count <= 16 && layer_offset > 0) {
                for (int layer = 0; layer < layer_count; ++layer) {
                    const auto at = base + static_cast<std::size_t>(layer_offset) + static_cast<std::size_t>(layer) * 24;
                    AutoLayer entry;
                    entry.sequence = bytes.get<std::int16_t>(at);
                    entry.flags = bytes.get<std::int32_t>(at + 4);
                    entry.start = bytes.get<float>(at + 8);
                    entry.peak = bytes.get<float>(at + 12);
                    entry.tail = bytes.get<float>(at + 16);
                    entry.end = bytes.get<float>(at + 20);
                    sequence.layers.push_back(entry);
                }
            }
            if (const auto weight_offset = bytes.get<std::int32_t>(base + 156); weight_offset > 0) {
                std::vector<float> weights;
                for (std::size_t bone = 0; bone < bones_.size(); ++bone) {
                    weights.push_back(bytes.get<float>(
                        base + static_cast<std::size_t>(weight_offset) + bone * 4));
                }
                sequence.weights = std::move(weights);
            }
        } catch (const std::exception&) {
        }
        const auto blends = bytes.get<std::int32_t>(base + 56);
        const auto table = bytes.get<std::int32_t>(base + 60);
        for (int axis = 0; axis < 2; ++axis) {
            sequence.group[axis] = std::max(bytes.get<std::int32_t>(base + 68 + axis * 4), 1);
            sequence.param[axis] = bytes.get<std::int32_t>(base + 76 + axis * 4);
            sequence.start[axis] = bytes.get<float>(base + 84 + axis * 4);
            sequence.end[axis] = bytes.get<float>(base + 92 + axis * 4);
        }
        if (blends < 0 || blends > 64 || sequence.group[0] * sequence.group[1] != std::max(blends, 1)) {
            // Sequences without a coherent blend grid are never sampled.
            sequences_.push_back(std::move(sequence));
            continue;
        }
        for (int blend = 0; blend < blends; ++blend) {
            // Blend tables hold 16-bit anim indices.
            sequence.anims.push_back(bytes.get<std::int16_t>(
                base + static_cast<std::size_t>(table) + static_cast<std::size_t>(blend) * 2));
        }
        sequences_.push_back(std::move(sequence));
    }

    const auto block_count = bytes.get<std::int32_t>(352);
    const auto block_offset = bytes.get<std::int32_t>(356);
    if (block_count > 0 && block_offset > 0 && block_count < 100000) {
        for (int index = 0; index < block_count; ++index) {
            const auto base = static_cast<std::size_t>(block_offset) + static_cast<std::size_t>(index) * 8;
            blocks_.emplace_back(bytes.get<std::int32_t>(base), bytes.get<std::int32_t>(base + 4));
        }
    }
}

int AnimationModel::find_sequence(std::string_view name) const {
    for (std::size_t index = 0; index < sequences_.size(); ++index) {
        if (sequences_[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

int AnimationModel::find_pose_parameter(std::string_view name) const {
    for (std::size_t index = 0; index < pose_parameters_.size(); ++index) {
        if (pose_parameters_[index].name == name) return static_cast<int>(index);
    }
    return -1;
}

AnimationModel::BlendCell AnimationModel::blend_cell(
    const Sequence& sequence, const std::vector<float>& parameters) const {
    int low[2]{0, 0};
    float fraction[2]{0.0f, 0.0f};
    for (int axis = 0; axis < 2; ++axis) {
        if (sequence.group[axis] <= 1) continue;
        const int parameter = sequence.param[axis];
        const float span = sequence.end[axis] - sequence.start[axis];
        if (parameter < 0 || static_cast<std::size_t>(parameter) >= parameters.size()
            || std::abs(span) < 1e-6f) {
            continue;
        }
        const float value = std::clamp(parameters[static_cast<std::size_t>(parameter)],
                                       std::min(sequence.start[axis], sequence.end[axis]),
                                       std::max(sequence.start[axis], sequence.end[axis]));
        const float position = (value - sequence.start[axis]) / span
            * static_cast<float>(sequence.group[axis] - 1);
        low[axis] = std::clamp(static_cast<int>(std::floor(position)), 0, sequence.group[axis] - 2);
        fraction[axis] = std::clamp(position - static_cast<float>(low[axis]), 0.0f, 1.0f);
    }
    const auto at = [&](int x, int y) {
        const auto index = static_cast<std::size_t>(x + y * sequence.group[0]);
        return index < sequence.anims.size() ? sequence.anims[index] : -1;
    };
    const int x1 = std::min(low[0] + 1, sequence.group[0] - 1);
    const int y1 = std::min(low[1] + 1, sequence.group[1] - 1);
    BlendCell cell;
    cell.anim[0] = at(low[0], low[1]);
    cell.anim[1] = at(x1, low[1]);
    cell.anim[2] = at(low[0], y1);
    cell.anim[3] = at(x1, y1);
    cell.weight[0] = (1.0f - fraction[0]) * (1.0f - fraction[1]);
    cell.weight[1] = fraction[0] * (1.0f - fraction[1]);
    cell.weight[2] = (1.0f - fraction[0]) * fraction[1];
    cell.weight[3] = fraction[0] * fraction[1];
    return cell;
}

float AnimationModel::duration(int sequence, const std::vector<float>& parameters) const {
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size()) return 1.0f;
    const auto cell = blend_cell(sequences_[static_cast<std::size_t>(sequence)], parameters);
    float total = 0.0f;
    float weight_sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (cell.weight[i] <= 0.0f || cell.anim[i] < 0
            || static_cast<std::size_t>(cell.anim[i]) >= anims_.size()) {
            continue;
        }
        const auto& desc = anims_[static_cast<std::size_t>(cell.anim[i])];
        if (desc.fps <= 0.0f || desc.frames < 2) continue;
        total += cell.weight[i] * static_cast<float>(desc.frames - 1) / desc.fps;
        weight_sum += cell.weight[i];
    }
    return weight_sum > 0.0f ? total / weight_sum : 1.0f;
}

float AnimationModel::ground_speed(int sequence, const std::vector<float>& parameters) const {
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size()) return 0.0f;
    const auto cell = blend_cell(sequences_[static_cast<std::size_t>(sequence)], parameters);
    float total = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (cell.weight[i] <= 0.0f || cell.anim[i] < 0
            || static_cast<std::size_t>(cell.anim[i]) >= anims_.size()) {
            continue;
        }
        total += cell.weight[i] * anims_[static_cast<std::size_t>(cell.anim[i])].ground_speed;
    }
    return total;
}

void AnimationModel::sample(int sequence, const std::vector<float>& parameters, float cycle,
                            std::vector<BonePose>& out) const {
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size()) {
        throw std::runtime_error("Animation sequence index is invalid.");
    }
    const auto cell = blend_cell(sequences_[static_cast<std::size_t>(sequence)], parameters);
    std::vector<BonePose> rest(bones_.size());
    for (std::size_t i = 0; i < bones_.size(); ++i) {
        std::copy(bones_[i].pos, bones_[i].pos + 3, rest[i].pos);
        std::copy(bones_[i].quat, bones_[i].quat + 4, rest[i].quat);
    }
    std::vector<BonePose> scratch;
    float accumulated = 0.0f;
    out = rest;
    for (int i = 0; i < 4; ++i) {
        if (cell.weight[i] <= 1e-4f || cell.anim[i] < 0
            || static_cast<std::size_t>(cell.anim[i]) >= anims_.size()) {
            continue;
        }
        scratch = rest;
        sample_anim(anims_[static_cast<std::size_t>(cell.anim[i])], cycle, scratch);
        if (accumulated == 0.0f) {
            out = scratch;
        } else {
            const float t = cell.weight[i] / (accumulated + cell.weight[i]);
            for (std::size_t bone = 0; bone < out.size(); ++bone) {
                blend_into(out[bone], scratch[bone], t);
            }
        }
        accumulated += cell.weight[i];
    }
}

bool AnimationModel::is_delta(int sequence) const {
    return sequence >= 0 && static_cast<std::size_t>(sequence) < sequences_.size()
        && (sequences_[static_cast<std::size_t>(sequence)].flags & SeqDelta) != 0;
}

void AnimationModel::sample_delta(int sequence, const std::vector<float>& parameters, float cycle,
                                  std::vector<BonePose>& out) const {
    out.assign(bones_.size(), BonePose{});
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size()) return;
    const auto cell = blend_cell(sequences_[static_cast<std::size_t>(sequence)], parameters);
    std::vector<BonePose> scratch;
    float accumulated = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (cell.weight[i] <= 1e-4f || cell.anim[i] < 0
            || static_cast<std::size_t>(cell.anim[i]) >= anims_.size()) {
            continue;
        }
        scratch.assign(bones_.size(), BonePose{});
        sample_anim(anims_[static_cast<std::size_t>(cell.anim[i])], cycle, scratch, true);
        if (accumulated == 0.0f) {
            out = scratch;
        } else {
            const float t = cell.weight[i] / (accumulated + cell.weight[i]);
            for (std::size_t bone = 0; bone < out.size(); ++bone) blend_into(out[bone], scratch[bone], t);
        }
        accumulated += cell.weight[i];
    }
}

void AnimationModel::apply_delta(std::vector<BonePose>& pose, const std::vector<BonePose>& delta,
                                 float weight, const std::vector<float>& bone_weights,
                                 int sequence) const {
    constexpr int SeqPost = 0x10;
    const bool post = sequence >= 0 && static_cast<std::size_t>(sequence) < sequences_.size()
        && (sequences_[static_cast<std::size_t>(sequence)].flags & SeqPost) != 0;
    const std::size_t count = std::min(pose.size(), delta.size());
    for (std::size_t bone = 0; bone < count; ++bone) {
        const float s = weight * (bone < bone_weights.size() ? bone_weights[bone] : 1.0f);
        if (s <= 0.0f) continue;
        for (int axis = 0; axis < 3; ++axis) pose[bone].pos[axis] += delta[bone].pos[axis] * s;
        // mathlib's QuaternionScale: the delta rotation scaled by `s` from identity.
        const float* d = delta[bone].quat;
        float sinom = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        sinom = std::min(sinom, 1.0f);
        const float sinsom = std::sin(std::asin(sinom) * s);
        const float k = sinsom / (sinom + 1e-12f);
        float scaled[4]{d[0] * k, d[1] * k, d[2] * k, 0.0f};
        const float r = std::sqrt(std::max(0.0f, 1.0f - sinsom * sinsom));
        scaled[3] = d[3] < 0.0f ? -r : r;
        // SlerpBones: pre-deltas rotate before the pose (QuaternionSM), post-deltas after.
        const float* a = post ? pose[bone].quat : scaled;
        const float* b = post ? scaled : pose[bone].quat;
        float product[4]{
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
        quat_normalize(product);
        std::copy(product, product + 4, pose[bone].quat);
    }
}

void blend_poses(std::vector<BonePose>& a, const std::vector<BonePose>& b, float t) {
    const std::size_t count = std::min(a.size(), b.size());
    for (std::size_t bone = 0; bone < count; ++bone) blend_into(a[bone], b[bone], t);
}

int AnimationModel::find_activity(std::string_view activity) const {
    for (std::size_t index = 0; index < sequences_.size(); ++index) {
        if (sequences_[index].activity == activity) return static_cast<int>(index);
    }

    return -1;
}

bool AnimationModel::sequence_has_activity(int sequence, std::string_view activity) const {
    return sequence >= 0 && static_cast<std::size_t>(sequence) < sequences_.size()
        && sequences_[static_cast<std::size_t>(sequence)].activity == activity;
}

const std::vector<float>& AnimationModel::sequence_weights(int sequence) const {
    static const std::vector<float> none;
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size()) return none;
    return sequences_[static_cast<std::size_t>(sequence)].weights;
}

void AnimationModel::apply_gesture(std::vector<BonePose>& pose, int sequence,
                                   const std::vector<float>& parameters, float cycle,
                                   float weight) const {
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= sequences_.size() || weight <= 0.0f) return;
    const auto& entry = sequences_[static_cast<std::size_t>(sequence)];
    std::vector<BonePose> own;
    sample(sequence, parameters, cycle, own);
    blend_poses_weighted(pose, own, weight, entry.weights);
    for (const auto& layer : entry.layers) {
        if (layer.sequence < 0 || static_cast<std::size_t>(layer.sequence) >= sequences_.size()) continue;
        if (cycle < layer.start || cycle >= layer.end) continue;
        float amount = 1.0f;
        if (cycle < layer.peak && layer.start != layer.peak) {
            amount = (cycle - layer.start) / (layer.peak - layer.start);
        } else if (cycle > layer.tail && layer.end != layer.tail) {
            amount = (layer.end - cycle) / (layer.end - layer.tail);
        }
        if ((layer.flags & 0x40) != 0) amount = amount * amount * (3.0f - 2.0f * amount);
        amount *= weight;
        if (amount <= 0.0f) continue;
        const auto& layer_weights = sequences_[static_cast<std::size_t>(layer.sequence)].weights;
        std::vector<BonePose> layer_pose;
        if (is_delta(layer.sequence)) {
            sample_delta(layer.sequence, parameters, cycle, layer_pose);
            apply_delta(pose, layer_pose, amount, layer_weights, layer.sequence);
        } else {
            sample(layer.sequence, parameters, cycle, layer_pose);
            blend_poses_weighted(pose, layer_pose, amount, layer_weights);
        }
    }
}

void blend_poses_weighted(std::vector<BonePose>& a, const std::vector<BonePose>& b, float t,
                          const std::vector<float>& weights) {
    const std::size_t count = std::min(a.size(), b.size());
    for (std::size_t bone = 0; bone < count; ++bone) {
        const float s = t * (bone < weights.size() ? weights[bone] : 1.0f);
        if (s > 0.0f) blend_into(a[bone], b[bone], s);
    }
}

void AnimationModel::sample_with_layers(int sequence, const std::vector<float>& parameters,
                                        float cycle, std::vector<BonePose>& out) const {
    sample(sequence, parameters, cycle, out);
    for (const auto& layer : sequences_) {
        if ((layer.flags & SeqAutoplay) == 0 || (layer.flags & SeqDelta) == 0) continue;
        const auto cell = blend_cell(layer, parameters);
        std::vector<BonePose> delta(bones_.size());
        std::vector<BonePose> scratch;
        float accumulated = 0.0f;
        for (int i = 0; i < 4; ++i) {
            if (cell.weight[i] <= 1e-4f || cell.anim[i] < 0
                || static_cast<std::size_t>(cell.anim[i]) >= anims_.size()) {
                continue;
            }
            scratch.assign(bones_.size(), BonePose{});
            sample_anim(anims_[static_cast<std::size_t>(cell.anim[i])], cycle, scratch, true);
            if (accumulated == 0.0f) {
                delta = scratch;
            } else {
                const float t = cell.weight[i] / (accumulated + cell.weight[i]);
                for (std::size_t bone = 0; bone < delta.size(); ++bone) {
                    blend_into(delta[bone], scratch[bone], t);
                }
            }
            accumulated += cell.weight[i];
        }
        if (accumulated == 0.0f) continue;
        for (std::size_t bone = 0; bone < out.size(); ++bone) {
            for (int axis = 0; axis < 3; ++axis) out[bone].pos[axis] += delta[bone].pos[axis];
            const float* a = out[bone].quat;
            const float* b = delta[bone].quat;
            float product[4]{
                a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
            quat_normalize(product);
            std::copy(product, product + 4, out[bone].quat);
        }
    }
}

void AnimationModel::sample_anim(const AnimDesc& desc, float cycle,
                                 std::vector<BonePose>& pose, bool delta_layer) const {
    const int frames = std::max(desc.frames, 1);
    const float position = frames > 1 ? std::clamp(cycle, 0.0f, 1.0f) * static_cast<float>(frames - 1) : 0.0f;
    int frame = static_cast<int>(std::floor(position));
    float s = position - static_cast<float>(frame);
    if (frame >= frames - 1) {
        frame = frames - 1;
        s = 0.0f;
    }

    const Bytes mdl{mdl_.data(), mdl_.size()};
    const Bytes ani{ani_.data(), ani_.size()};
    int block = desc.block;
    std::int64_t index = desc.index;
    if (desc.section_frames != 0) {
        int section = 0;
        if (frames > desc.section_frames && frame == frames - 1) {
            // The final frame of a long animation is stored as its own section.
            frame = 0;
            section = frames / desc.section_frames + 1;
        } else {
            section = frame / desc.section_frames;
            frame -= section * desc.section_frames;
        }
        const auto entry = desc.offset + static_cast<std::size_t>(desc.section_index)
            + static_cast<std::size_t>(section) * 8;
        block = mdl.get<std::int32_t>(entry);
        index = mdl.get<std::int32_t>(entry + 4);
    }
    if (block < 0) return;
    const Bytes* source = &mdl;
    std::int64_t base = static_cast<std::int64_t>(desc.offset) + index;
    if (block != 0) {
        if (static_cast<std::size_t>(block) >= blocks_.size()) return;
        source = &ani;
        base = static_cast<std::int64_t>(blocks_[static_cast<std::size_t>(block)].first) + index;
    }
    if (base < 0) return;

    std::size_t record = static_cast<std::size_t>(base);
    for (;;) {
        const auto bone_index = source->get<std::uint8_t>(record);
        const auto flags = source->get<std::uint8_t>(record + 1);
        const auto next = source->get<std::int16_t>(record + 2);
        if (bone_index < bones_.size() && ((flags & AnimDelta) != 0) == delta_layer) {
            const auto& rest = bones_[bone_index];
            auto& out = pose[bone_index];
            std::size_t cursor = record + 4;
            // Delta layers store offsets from the bind pose, not absolute values.
            const float* base_rot = delta_layer ? kZero : rest.rot;
            const float* base_pos = delta_layer ? kZero : rest.pos;

            if ((flags & AnimRawRot) != 0) {
                const auto x = source->get<std::uint16_t>(cursor);
                const auto y = source->get<std::uint16_t>(cursor + 2);
                const auto zw = source->get<std::uint16_t>(cursor + 4);
                out.quat[0] = (static_cast<float>(x) - 32768.0f) / 32768.0f;
                out.quat[1] = (static_cast<float>(y) - 32768.0f) / 32768.0f;
                out.quat[2] = (static_cast<float>(zw & 0x7FFF) - 16384.0f) / 16384.0f;
                const float w = std::sqrt(std::max(0.0f, 1.0f - out.quat[0] * out.quat[0]
                    - out.quat[1] * out.quat[1] - out.quat[2] * out.quat[2]));
                out.quat[3] = (zw & 0x8000) != 0 ? -w : w;
                cursor += 6;
            } else if ((flags & AnimRawRot2) != 0) {
                const auto packed = source->get<std::uint64_t>(cursor);
                const auto unpack = [](std::uint64_t bits) {
                    return (static_cast<float>(bits & 0x1FFFFF) - 1048576.0f) / 1048576.5f;
                };
                out.quat[0] = unpack(packed);
                out.quat[1] = unpack(packed >> 21);
                out.quat[2] = unpack(packed >> 42);
                const float w = std::sqrt(std::max(0.0f, 1.0f - out.quat[0] * out.quat[0]
                    - out.quat[1] * out.quat[1] - out.quat[2] * out.quat[2]));
                out.quat[3] = (packed >> 63) != 0 ? -w : w;
                cursor += 8;
            } else if ((flags & AnimAnimRot) != 0) {
                float first[3];
                float second[3];
                bool changes = false;
                for (int axis = 0; axis < 3; ++axis) {
                    const auto offset = source->get<std::int16_t>(cursor + static_cast<std::size_t>(axis) * 2);
                    if (offset == 0) {
                        first[axis] = second[axis] = base_rot[axis];
                        continue;
                    }
                    float v1 = 0.0f;
                    float v2 = 0.0f;
                    extract_value(*source, cursor + static_cast<std::size_t>(offset), frame,
                                  rest.rot_scale[axis], v1, v2);
                    first[axis] = base_rot[axis] + v1;
                    second[axis] = base_rot[axis] + v2;
                    changes = changes || v1 != v2;
                }
                float q1[4];
                quat_from_euler(first, q1);
                if (changes && s > 0.001f) {
                    float q2[4];
                    quat_from_euler(second, q2);
                    quat_slerp(q1, q2, s, out.quat);
                } else {
                    std::copy(q1, q1 + 4, out.quat);
                }
                cursor += 6;
            }

            if ((flags & AnimRawPos) != 0) {
                for (int axis = 0; axis < 3; ++axis) {
                    out.pos[axis] = half_to_float(
                        source->get<std::uint16_t>(cursor + static_cast<std::size_t>(axis) * 2));
                }
            } else if ((flags & AnimAnimPos) != 0) {
                for (int axis = 0; axis < 3; ++axis) {
                    const auto offset = source->get<std::int16_t>(cursor + static_cast<std::size_t>(axis) * 2);
                    if (offset == 0) continue;
                    float v1 = 0.0f;
                    float v2 = 0.0f;
                    extract_value(*source, cursor + static_cast<std::size_t>(offset), frame,
                                  rest.pos_scale[axis], v1, v2);
                    out.pos[axis] = base_pos[axis] + v1 * (1.0f - s) + v2 * s;
                }
            }
        }
        if (next <= 0) break;
        record += static_cast<std::size_t>(next);
    }
}

SkeletonPoser::SkeletonPoser(std::vector<Bone> skeleton, const AnimationModel& animation)
    : skeleton_(std::move(skeleton)), animation_bone_(skeleton_.size(), -1) {
    const auto& source = animation.bones();
    for (std::size_t bone = 0; bone < skeleton_.size(); ++bone) {
        for (std::size_t other = 0; other < source.size(); ++other) {
            if (source[other].name == skeleton_[bone].name) {
                animation_bone_[bone] = static_cast<int>(other);
                break;
            }
        }
    }
}

std::vector<Matrix3x4> bonemerged_attachment_palette(
    const std::vector<Bone>& parent, const std::vector<Bone>& child,
    const std::vector<Matrix3x4>& animation_palette) {
    if (!animation_palette.empty() && animation_palette.size() != child.size())
        throw std::runtime_error("Attachment animation palette does not match its skeleton.");
    std::vector<Matrix3x4> result;
    result.reserve(child.size());
    for (std::size_t i = 0; i < child.size(); ++i) {
        int anchor = static_cast<int>(i);
        auto match = parent.end();
        while (anchor >= 0) {
            match = std::find_if(parent.begin(), parent.end(),
                [&](const auto& bone) { return bone.name == child.at(anchor).name; });
            if (match != parent.end()) break;
            anchor = child.at(anchor).parent;
        }
        if (match == parent.end())
            throw std::runtime_error("Attachment bone has no parent match: " + child[i].name);
        Matrix3x4 inverse_bind{};
        std::copy_n(child.at(anchor).pose_to_bone, 12, inverse_bind.begin());
        auto transform = multiply(rigid_inverse(match->pose_to_bone), inverse_bind);
        if (!animation_palette.empty()) {
            transform = multiply(transform, rigid_inverse(animation_palette.at(anchor).data()));
            transform = multiply(transform, animation_palette[i]);
        }
        result.push_back(transform);
    }
    return result;
}

void SkeletonPoser::palette(const std::vector<BonePose>& animation_pose,
                            std::vector<Matrix3x4>& out) const {
    palette(animation_pose, animation_pose, 0.0f, out);
}

void SkeletonPoser::palette(const std::vector<BonePose>& a, const std::vector<BonePose>& b,
                            float t, std::vector<Matrix3x4>& out) const {
    palette(a, b, t, {}, out);
}

void SkeletonPoser::palette(const std::vector<BonePose>& a, const std::vector<BonePose>& b,
                            float t, const std::vector<float>& weights,
                            std::vector<Matrix3x4>& out) const {
    out.resize(skeleton_.size());
    std::vector<Matrix3x4> world(skeleton_.size());
    for (std::size_t bone = 0; bone < skeleton_.size(); ++bone) {
        BonePose local;
        std::copy(skeleton_[bone].pos, skeleton_[bone].pos + 3, local.pos);
        std::copy(skeleton_[bone].quat, skeleton_[bone].quat + 4, local.quat);
        const int source = animation_bone_[bone];
        if (source >= 0 && static_cast<std::size_t>(source) < a.size()) {
            local = a[static_cast<std::size_t>(source)];
            const float amount = weights.empty() ? t : t * weights[bone];
            if (amount > 0.0f && static_cast<std::size_t>(source) < b.size()) {
                blend_into(local, b[static_cast<std::size_t>(source)], amount);
            }
        }
        const auto local_matrix = matrix_from_pose(local);
        const int parent = skeleton_[bone].parent;
        world[bone] = parent < 0 ? local_matrix
            : multiply(world[static_cast<std::size_t>(parent)], local_matrix);
        Matrix3x4 inverse_bind;
        std::copy(skeleton_[bone].pose_to_bone, skeleton_[bone].pose_to_bone + 12, inverse_bind.begin());
        out[bone] = multiply(world[bone], inverse_bind);
    }
}

void SkeletonPoser::palette_from_world(const std::vector<Matrix3x4>& world_in,
                                       const std::vector<bool>& driven,
                                       std::vector<Matrix3x4>& out) const {
    out.resize(skeleton_.size());
    std::vector<Matrix3x4> world(skeleton_.size());
    for (std::size_t bone = 0; bone < skeleton_.size(); ++bone) {
        if (bone < driven.size() && driven[bone] && bone < world_in.size()) {
            world[bone] = world_in[bone];
        } else {
            BonePose local;
            std::copy(skeleton_[bone].pos, skeleton_[bone].pos + 3, local.pos);
            std::copy(skeleton_[bone].quat, skeleton_[bone].quat + 4, local.quat);
            const auto local_matrix = matrix_from_pose(local);
            const int parent = skeleton_[bone].parent;
            world[bone] = parent < 0 ? local_matrix
                : multiply(world[static_cast<std::size_t>(parent)], local_matrix);
        }
        Matrix3x4 inverse_bind;
        std::copy(skeleton_[bone].pose_to_bone, skeleton_[bone].pose_to_bone + 12, inverse_bind.begin());
        out[bone] = multiply(world[bone], inverse_bind);
    }
}

std::vector<std::string> read_phy_solid_names(const std::filesystem::path& phy_path) {
    if (!vfs::exists(phy_path)) return {};
    const auto bytes = vfs::read(phy_path);
    const std::string text(bytes.begin(), bytes.end());
    std::vector<std::string> names;
    std::size_t at = 0;
    while ((at = text.find("solid {", at)) != std::string::npos) {
        const auto end = text.find('}', at);
        if (end == std::string::npos) break;
        const auto block = text.substr(at, end - at);
        at = end;
        const auto key = block.find("\"name\"");
        if (key == std::string::npos) continue;
        const auto open = block.find('"', key + 6);
        const auto close = open == std::string::npos ? open : block.find('"', open + 1);
        if (open == std::string::npos || close == std::string::npos) continue;
        auto name = block.substr(open + 1, close - open - 1);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.push_back(std::move(name));
    }
    return names;
}

} // namespace gmod::source::model
