#pragma once

#include "source/model/Animation.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gmod::source::model {

// Drives a ValveBiped player skeleton from recorded movement: idle/walk/run
// blends (or the crouch set) selected by speed, with the playback rate matched
// to ground speed so feet do not slide.
class PlayerAnimator {
public:
    PlayerAnimator(const std::filesystem::path& animation_mdl,
                   const std::filesystem::path& animation_ani);

    // Lower-case names of the skeleton every skinned mesh is remapped to.
    const std::vector<std::string>& skeleton_names() const noexcept { return names_; }
    const std::vector<Bone>& skeleton() const noexcept { return poser_.skeleton(); }
    std::size_t bone_count() const noexcept { return poser_.bone_count(); }

    struct State {
        float cycle = 0.0f;
        float forward = 0.0f;  // smoothed local velocity, units/second
        float right = 0.0f;
        bool valid = false;
    };
    struct Input {
        float forward = 0.0f;  // local velocity components, units/second
        float right = 0.0f;
        bool crouching = false;
        bool grounded = true;
        bool seated = false;
        bool driver = false;  // seated at the wheel rather than as a passenger
        // Weapon hold type ("pistol", "smg1", "ar2", "shotgun", "melee", ...); empty = unarmed.
        std::string hold;
        float pitch = 0.0f;   // view pitch in degrees (positive looks down)
        // Seconds since the last shot (drives the hold type's additive firing gesture);
        // <0 when not firing.
        float fire_elapsed = -1.0f;
        // Seconds since the reload began; <0 when not reloading. Held on its last frame.
        float reload_elapsed = -1.0f;
        bool hold_reload = true;  // false for a one-shot recorded gesture without a live reload flag
        // An act the server started on the player (an emote such as surrendering): a sequence
        // of the animation model, the seconds since it began, and whether it loops.
        int act = -1;
        float act_elapsed = 0.0f;
        bool act_loop = false;
        // A gesture held while a networked state is true (handcuffed, on the phone), at its
        // blend-in weight. The server restarts it every frame, so it stays on its first frame.
        int hold_gesture = -1;
        float hold_weight = 0.0f;
    };

    // Sequence of the animation model for a server activity name, -1 when it has none.
    int activity_sequence(std::string_view activity) const { return animation_.find_activity(activity); }
    std::size_t sequence_count() const noexcept { return animation_.sequence_count(); }
    const std::string& sequence_name(int sequence) const { return animation_.sequence_name(sequence); }
    float sequence_seconds(int sequence) const;

    // Advances `state` by `dt` seconds of demo time and writes the skinning palette.
    void update(State& state, const Input& input, float dt, std::vector<Matrix3x4>& palette) const;

    // Ragdoll pose. Physics object i drives skeleton bone `solid_to_bone[i]` (-1 = unknown)
    // with the world `pos`/`ang` (three floats each); everything is shifted by `-origin`
    // so the caller can place the model at `origin`.
    void ragdoll_palette(const std::vector<int>& solid_to_bone, const std::vector<float>& pos,
                         const std::vector<float>& ang, const float origin[3],
                         std::vector<Matrix3x4>& palette) const;

private:
    struct LocomotionSet {
        int walk = -1;
        int run = -1;
        int crouch_walk = -1;
        int range = -1;  // the firing gesture of this hold type
        int reload = -1; // the reloading gesture of this hold type
    };

    std::vector<float> parameters(float forward, float right, float magnitude, float pitch) const;

    AnimationModel animation_;
    SkeletonPoser poser_;
    std::vector<std::string> names_;
    std::unordered_map<std::string, LocomotionSet> holds_;
    std::vector<float> upper_body_;  // per skeleton bone: 1 above the pelvis, 0 for the legs
    LocomotionSet unarmed_;
    int idle_ = -1;
    int walk_ = -1;
    int run_ = -1;
    int fast_run_ = -1;  // run_all_02: the server's ACT_HL2MP_RUN_FAST, used above 200 units/s
    int crouch_walk_ = -1;
    int sit_ = -1;
    int drive_ = -1;
    int move_x_ = -1;
    int move_y_ = -1;
    int aim_pitch_ = -1;
    int head_pitch_ = -1;
};

} // namespace gmod::source::model
