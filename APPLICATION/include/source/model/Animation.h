#pragma once

#include <array>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gmod::source::model {

// One MDL bone. Quaternions are (x, y, z, w); `pose_to_bone` is the inverse of
// the bone's bind-pose world matrix (3x4, row-major: out = R * v + t).
struct Bone {
    std::string name;  // lower case
    int parent = -1;
    float pos[3]{};
    float quat[4]{0.0f, 0.0f, 0.0f, 1.0f};
    float rot[3]{};
    float pos_scale[3]{};
    float rot_scale[3]{};
    float pose_to_bone[12]{};
};

std::vector<Bone> read_bones(const std::filesystem::path& mdl_path);
struct Attachment {
    std::string name;
    int bone = -1;
    std::array<float, 12> local{};
};
std::vector<Attachment> read_attachments(const std::filesystem::path& mdl_path);

// Bone names (lower case) of a ragdoll's physics objects in solid order, read from the
// text section at the end of the model's .phy. Empty when there is none.
std::vector<std::string> read_phy_solid_names(const std::filesystem::path& phy_path);

// Matrix (3x4, out = R * v + t) of a Source origin and (pitch, yaw, roll) angles in degrees.
inline std::array<float, 12> matrix_from_source_angles(const float origin[3], const float angles[3]) {
    constexpr float ToRadians = 0.017453292519943295f;
    const float sp = std::sin(angles[0] * ToRadians), cp = std::cos(angles[0] * ToRadians);
    const float sy = std::sin(angles[1] * ToRadians), cy = std::cos(angles[1] * ToRadians);
    const float sr = std::sin(angles[2] * ToRadians), cr = std::cos(angles[2] * ToRadians);
    // mathlib's AngleMatrix: the columns are the forward, left and up axes.
    return {cp * cy, sr * sp * cy + cr * -sy, cr * sp * cy + -sr * -sy, origin[0],
            cp * sy, sr * sp * sy + cr * cy, cr * sp * sy + -sr * cy, origin[1],
            -sp, sr * cp, cr * cp, origin[2]};
}

// Paths (as stored in the MDL, normalised to lower-case with '/' separators) of
// the models this MDL includes sequences from.
std::vector<std::string> read_include_models(const std::filesystem::path& mdl_path);

// Local (parent-relative) bone transform.
struct BonePose {
    float pos[3]{};
    float quat[4]{0.0f, 0.0f, 0.0f, 1.0f};
};

using Matrix3x4 = std::array<float, 12>;

// Bakes attachment skinning into the parent's bind space, keeping matched bones
// locked to the parent while allowing unmatched descendants to animate.
std::vector<Matrix3x4> bonemerged_attachment_palette(
    const std::vector<Bone>& parent, const std::vector<Bone>& child,
    const std::vector<Matrix3x4>& animation_palette);

// Blends `b` into `a` by `t` per bone (lerped positions, slerped rotations).
void blend_poses(std::vector<BonePose>& a, const std::vector<BonePose>& b, float t);
// Same, with each bone's `t` scaled by its entry of `weights` (all ones when empty).
void blend_poses_weighted(std::vector<BonePose>& a, const std::vector<BonePose>& b, float t,
                          const std::vector<float>& weights);

// A model that carries animation sequences (a player's shared animation MDL
// plus its .ani anim-block file).
class AnimationModel {
public:
    AnimationModel(const std::filesystem::path& mdl, const std::filesystem::path& ani);

    const std::vector<Bone>& bones() const noexcept { return bones_; }
    int find_sequence(std::string_view name) const;
    int find_pose_parameter(std::string_view name) const;
    std::size_t pose_parameter_count() const noexcept { return pose_parameters_.size(); }
    const std::string& pose_parameter_name(std::size_t index) const { return pose_parameters_.at(index).name; }
    // True for parameters that wrap around (wheel spin), where a blend must take the short way.
    bool pose_parameter_loops(std::size_t index) const { return pose_parameters_.at(index).loop > 0.0f; }
    // A networked pose parameter is stored as (value - start) / (end - start); this is the
    // value in the model's own units that the sequences' blend ranges use.
    float pose_parameter_raw(std::size_t index, float normalized) const {
        const auto& parameter = pose_parameters_.at(index);
        return parameter.start + normalized * (parameter.end - parameter.start);
    }
    std::size_t sequence_count() const noexcept { return sequences_.size(); }
    const std::string& sequence_name(int sequence) const { return sequences_.at(static_cast<std::size_t>(sequence)).name; }
    bool is_looping(int sequence) const {
        return sequence >= 0 && static_cast<std::size_t>(sequence) < sequences_.size()
            && (sequences_[static_cast<std::size_t>(sequence)].flags & 0x1) != 0;
    }

    // Samples `sequence` at `cycle` (0..1) with the blend parameters in
    // `parameters` (indexed by pose parameter). `out` is sized to bones().
    void sample(int sequence, const std::vector<float>& parameters, float cycle,
                std::vector<BonePose>& out) const;

    // Same, but also applies the model's autoplay delta layers (wheel suspension, steering
    // and spin on vehicles) with the given pose parameters.
    void sample_with_layers(int sequence, const std::vector<float>& parameters, float cycle,
                            std::vector<BonePose>& out) const;

    // Gesture sequences (range_pistol...) are additive: they store offsets from the bind
    // pose that are layered over another pose rather than a pose of their own.
    bool is_delta(int sequence) const;
    // Samples a delta sequence's offsets (identity where it does not touch a bone).
    void sample_delta(int sequence, const std::vector<float>& parameters, float cycle,
                      std::vector<BonePose>& out) const;
    // Layers `delta` over `pose` scaled by `weight` (per bone by `bone_weights` when given),
    // the way Source's SlerpBones applies a delta sequence.
    void apply_delta(std::vector<BonePose>& pose, const std::vector<BonePose>& delta, float weight,
                     const std::vector<float>& bone_weights, int sequence) const;

    // Playback length in seconds and ground speed (units/second) of a sequence
    // at the given blend parameters; both are blended like the poses.
    float duration(int sequence, const std::vector<float>& parameters) const;
    float ground_speed(int sequence, const std::vector<float>& parameters) const;

    // First sequence whose activity name matches (e.g. "ACT_VM_DEPLOY_7"), -1 when none.
    int find_activity(std::string_view activity) const;
    bool sequence_has_activity(int sequence, std::string_view activity) const;
    // The sequence's own per-bone weights (its weightlist); all ones when it has none.
    const std::vector<float>& sequence_weights(int sequence) const;
    // A full-body sequence played in a gesture slot, laid over `pose` at `weight`: the
    // sequence itself through its weightlist, then each autolayer (the pieces an act is
    // built from, delta or not) at its own weight for `cycle`.
    void apply_gesture(std::vector<BonePose>& pose, int sequence, const std::vector<float>& parameters,
                       float cycle, float weight) const;

private:
    struct AutoLayer {
        int sequence = -1;
        int flags = 0;
        float start = 0.0f, peak = 0.0f, tail = 0.0f, end = 0.0f;
    };
    struct AnimDesc {
        std::size_t offset = 0;  // position of the descriptor inside the MDL
        float fps = 30.0f;
        int frames = 1;
        int block = 0;
        int index = 0;
        int section_index = 0;
        int section_frames = 0;
        float ground_speed = 0.0f;
    };
    struct Sequence {
        std::string name;
        std::string activity;
        int flags = 0;
        int group[2]{1, 1};
        int param[2]{-1, -1};
        float start[2]{};
        float end[2]{};
        std::vector<int> anims;
        std::vector<float> weights;  // per bone; empty = all ones
        std::vector<AutoLayer> layers;
    };
    struct PoseParameter {
        std::string name;
        float start = 0.0f;
        float end = 1.0f;
        float loop = 0.0f;
    };
    struct BlendCell {
        int anim[4]{};
        float weight[4]{};
    };

    BlendCell blend_cell(const Sequence& sequence, const std::vector<float>& parameters) const;
    void sample_anim(const AnimDesc& desc, float cycle, std::vector<BonePose>& pose,
                     bool delta_layer = false) const;

    std::vector<std::uint8_t> mdl_;
    std::vector<std::uint8_t> ani_;
    std::vector<Bone> bones_;
    std::vector<AnimDesc> anims_;
    std::vector<Sequence> sequences_;
    std::vector<PoseParameter> pose_parameters_;
    std::vector<std::pair<std::int32_t, std::int32_t>> blocks_;
};

// Turns animation poses into per-bone skinning matrices for a player model.
// Bones are matched to the animation model by name; bones the animation does
// not drive keep the player's own bind pose.
class SkeletonPoser {
public:
    SkeletonPoser(std::vector<Bone> skeleton, const AnimationModel& animation);

    std::size_t bone_count() const noexcept { return skeleton_.size(); }
    const std::vector<Bone>& skeleton() const noexcept { return skeleton_; }

    // Writes bone_count() 3x4 matrices: world_pose * pose_to_bone.
    void palette(const std::vector<BonePose>& animation_pose, std::vector<Matrix3x4>& out) const;
    // Same, with a second pose blended in by `t` (0 = first, 1 = second).
    void palette(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float t,
                 std::vector<Matrix3x4>& out) const;
    // Blends `b` over `a` per skeleton bone: `weights[bone]` (0..1) scales `t`.
    void palette(const std::vector<BonePose>& a, const std::vector<BonePose>& b, float t,
                 const std::vector<float>& weights, std::vector<Matrix3x4>& out) const;
    // Ragdoll pose: `world[bone]` holds an absolute bone matrix for the skeleton bones the
    // physics drives (see `driven`); every other bone follows its parent with its bind pose.
    void palette_from_world(const std::vector<Matrix3x4>& world, const std::vector<bool>& driven,
                            std::vector<Matrix3x4>& out) const;

private:
    std::vector<Bone> skeleton_;
    std::vector<int> animation_bone_;  // player bone -> animation bone or -1
};

} // namespace gmod::source::model
