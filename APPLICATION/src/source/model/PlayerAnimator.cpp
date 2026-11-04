#include "source/model/PlayerAnimator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace gmod::source::model {
namespace {

float smoothstep(float low, float high, float value) {
    const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

PlayerAnimator::PlayerAnimator(const std::filesystem::path& animation_mdl,
                               const std::filesystem::path& animation_ani)
    : animation_(animation_mdl, animation_ani), poser_(animation_.bones(), animation_) {
    for (const auto& bone : poser_.skeleton()) names_.push_back(bone.name);
    if (poser_.bone_count() > 128) {
        throw std::runtime_error("Animation skeleton has more bones than the renderer supports.");
    }
    idle_ = animation_.find_sequence("idle_all_01");
    walk_ = animation_.find_sequence("walk_all");
    run_ = animation_.find_sequence("run_all_01");
    fast_run_ = animation_.find_sequence("run_all_02");
    crouch_walk_ = animation_.find_sequence("cwalk_all");
    sit_ = animation_.find_sequence("sit");
    drive_ = animation_.find_sequence("drive_jeep");
    move_x_ = animation_.find_pose_parameter("move_x");
    move_y_ = animation_.find_pose_parameter("move_y");
    aim_pitch_ = animation_.find_pose_parameter("aim_pitch");
    head_pitch_ = animation_.find_pose_parameter("head_pitch");
    unarmed_ = {walk_, run_, crouch_walk_};
    for (const char* hold : {"pistol", "revolver", "smg1", "ar2", "shotgun", "melee", "melee2",
                             "knife", "fist", "camera", "physgun", "passive", "crossbow", "rpg",
                             "grenade", "slam", "magic"}) {
        const std::string suffix = std::string("_") + hold;
        LocomotionSet set;
        set.walk = animation_.find_sequence("walk" + suffix);
        set.run = animation_.find_sequence("run" + suffix);
        set.crouch_walk = animation_.find_sequence("cwalk" + suffix);
        set.range = animation_.find_sequence("range" + suffix);
        if (set.range < 0) {
            auto activity = "ACT_HL2MP_GESTURE_RANGE_ATTACK" + suffix;
            std::transform(activity.begin(),activity.end(),activity.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            set.range = animation_.find_activity(activity);
        }
        set.reload = animation_.find_sequence("reload" + suffix);
        if (set.reload < 0) {
            auto activity = "ACT_HL2MP_GESTURE_RELOAD" + suffix;
            std::transform(activity.begin(),activity.end(),activity.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            set.reload = animation_.find_activity(activity);
        }
        if (set.walk >= 0) {
            if (set.run < 0) set.run = run_;
            if (set.crouch_walk < 0) set.crouch_walk = crouch_walk_;
            holds_.emplace(hold, set);
        }
    }
    for (const auto& name : names_) {
        const bool leg = name.find("thigh") != std::string::npos || name.find("calf") != std::string::npos
            || name.find("foot") != std::string::npos || name.find("toe") != std::string::npos
            || name.find("pelvis") != std::string::npos;
        upper_body_.push_back(leg ? 0.0f : 1.0f);
    }
    if (walk_ < 0 || run_ < 0 || crouch_walk_ < 0 || move_x_ < 0 || move_y_ < 0) {
        throw std::runtime_error(
            "The animation model lacks the walk/run/crouch sequences or move_x/move_y parameters.");
    }
}

std::vector<float> PlayerAnimator::parameters(float forward, float right, float magnitude,
                                              float pitch) const {
    std::vector<float> values(animation_.pose_parameter_count(), 0.0f);
    values[static_cast<std::size_t>(move_x_)] = forward * magnitude;
    values[static_cast<std::size_t>(move_y_)] = right * magnitude;
    // Eye pitch arrives as 0..360; fold it to -180..180 (positive looks down).
    float folded = std::fmod(pitch, 360.0f);
    if (folded > 180.0f) folded -= 360.0f;
    if (folded < -180.0f) folded += 360.0f;
    if (aim_pitch_ >= 0) values[static_cast<std::size_t>(aim_pitch_)] = std::clamp(folded, -80.0f, 80.0f);
    if (head_pitch_ >= 0) values[static_cast<std::size_t>(head_pitch_)] = std::clamp(folded * 0.4f, -55.0f, 55.0f);
    return values;
}

void PlayerAnimator::update(State& state, const Input& input, float dt,
                            std::vector<Matrix3x4>& palette) const {
    if (!state.valid) {
        state.forward = input.forward;
        state.right = input.right;
        state.valid = true;
    } else if (dt != 0.0f) {
        // Smoothing follows elapsed time in either direction; the cycle below
        // runs backwards for negative dt, so reversed playback walks backwards.
        const float follow = 1.0f - std::exp(-std::fabs(dt) / 0.08f);
        state.forward += (input.forward - state.forward) * follow;
        state.right += (input.right - state.right) * follow;
    }

    std::vector<BonePose> pose_a;
    std::vector<BonePose> pose_b;
    if (input.seated && (sit_ >= 0 || drive_ >= 0)) {
        const int pose = input.driver && drive_ >= 0 ? drive_ : sit_ >= 0 ? sit_ : drive_;
        animation_.sample(pose, parameters(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, pose_a);
        poser_.palette(pose_a, palette);
        return;
    }

    const float speed = std::hypot(state.forward, state.right);
    // Locomotion fades in from the standing pose (the centre of each blend).
    const float magnitude = std::clamp(speed / 12.0f, 0.0f, 1.0f);
    const float direction_forward = speed > 1e-3f ? state.forward / speed : 0.0f;
    const float direction_right = speed > 1e-3f ? state.right / speed : 0.0f;
    const auto actual = parameters(direction_forward, direction_right, magnitude, input.pitch);
    const auto full = parameters(direction_forward, direction_right, 1.0f, input.pitch);

    const auto hold = input.hold.empty() ? holds_.end() : holds_.find(input.hold);
    const LocomotionSet& set = hold != holds_.end() ? hold->second : unarmed_;
    const int first = input.crouching ? set.crouch_walk : set.walk;
    const int second = input.crouching ? -1 : set.run;
    // The server picks the cycle by speed (sh_animations.lua CalcMainActivity): walk up to
    // 100 units/s, the run cycle above that, and ACT_HL2MP_RUN_FAST (run_all_02) above 200.
    // The hand-over is eased over a few units so the pose does not snap.
    float run_weight = 0.0f;
    float fast_weight = 0.0f;
    if (second >= 0) {
        run_weight = smoothstep(95.0f, 105.0f, speed);
        if (fast_run_ >= 0) fast_weight = smoothstep(195.0f, 205.0f, speed);
    }

    float ground = animation_.ground_speed(first, full);
    float duration = animation_.duration(first, actual);
    if (second >= 0) {
        ground += (animation_.ground_speed(second, full) - ground) * run_weight;
        duration += (animation_.duration(second, actual) - duration) * run_weight;
    }
    if (fast_weight > 0.0f) {
        ground += (animation_.ground_speed(fast_run_, full) - ground) * fast_weight;
        duration += (animation_.duration(fast_run_, actual) - duration) * fast_weight;
    }
    const float rate = (speed >= 12.0f && ground > 1.0f)
        ? std::clamp(speed / ground, 0.35f, 3.0f) : 1.0f;
    if (input.grounded && duration > 1e-3f) {
        state.cycle += dt * rate / duration;
        state.cycle -= std::floor(state.cycle);
    }

    animation_.sample(first, actual, state.cycle, pose_a);
    if (run_weight > 0.001f) {
        animation_.sample(second, actual, state.cycle, pose_b);
        blend_poses(pose_a, pose_b, run_weight);
    }
    if (fast_weight > 0.001f) {
        std::vector<BonePose> pose_fast;
        animation_.sample(fast_run_, actual, state.cycle, pose_fast);
        blend_poses(pose_a, pose_fast, fast_weight);
    }
    // Gesture-slot sequences play over the body at their own per-bone weights.
    const auto layer_gesture = [&](int sequence, float cycle, float weight) {
        animation_.apply_gesture(pose_a, sequence, actual, cycle, std::min(weight, 1.0f));
    };
    layer_gesture(input.hold_gesture, 0.0f, input.hold_weight);
    if (input.act >= 0) {
        const float length = std::max(animation_.duration(input.act, actual), 0.05f);
        float cycle = input.act_elapsed / length;
        float weight = std::clamp(input.act_elapsed / 0.2f, 0.0f, 1.0f);
        if (input.act_loop) {
            cycle -= std::floor(cycle);
        } else if (cycle < 1.0f) {
            weight = std::min(weight, std::clamp((1.0f - cycle) * length / 0.1f, 0.0f, 1.0f));
        } else {
            weight = 0.0f;
        }
        if (input.act_elapsed >= 0.0f) layer_gesture(input.act, cycle, weight);
    }
    // The reload gesture is layered the same way and holds its last frame while reloading.
    if (input.reload_elapsed >= 0.0f && hold != holds_.end() && set.reload >= 0) {
        const float length = std::max(animation_.duration(set.reload, actual), 0.05f);
        const float cycle = std::clamp(input.reload_elapsed / length, 0.0f, 1.0f);
        if (input.hold_reload || input.reload_elapsed < length) {
            if (animation_.is_delta(set.reload)) {
                std::vector<BonePose> gesture;
                animation_.sample_delta(set.reload, actual, cycle, gesture);
                animation_.apply_delta(pose_a, gesture, 1.0f, upper_body_, set.reload);
            } else {
                layer_gesture(set.reload, cycle, 1.0f);
            }
        }
    }
    // The firing gesture is an additive layer over the upper body (Source plays it in the
    // attack gesture slot), easing out over its last fifth.
    if (input.fire_elapsed >= 0.0f && hold != holds_.end() && set.range >= 0) {
        const float length = std::max(animation_.duration(set.range, actual), 0.05f);
        const float cycle = input.fire_elapsed / length;
        if (cycle < 1.0f) {
            const float fade = std::clamp((1.0f - cycle) / 0.2f, 0.0f, 1.0f);
            if (animation_.is_delta(set.range)) {
                std::vector<BonePose> gesture;
                animation_.sample_delta(set.range, actual, cycle, gesture);
                animation_.apply_delta(pose_a, gesture, fade, upper_body_, set.range);
            } else {
                std::vector<BonePose> gesture;
                animation_.sample(set.range, actual, cycle, gesture);
                poser_.palette(pose_a, gesture, fade, upper_body_, palette);
                return;
            }
        }
    }
    poser_.palette(pose_a, palette);
}

float PlayerAnimator::sequence_seconds(int sequence) const {
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= animation_.sequence_count()) return 0.0f;
    return animation_.duration(sequence, std::vector<float>(animation_.pose_parameter_count(), 0.0f));
}

void PlayerAnimator::ragdoll_palette(const std::vector<int>& solid_to_bone,
                                     const std::vector<float>& pos, const std::vector<float>& ang,
                                     const float origin[3], std::vector<Matrix3x4>& palette) const {
    std::vector<Matrix3x4> world(poser_.bone_count());
    std::vector<bool> driven(poser_.bone_count(), false);
    for (std::size_t solid = 0; solid < solid_to_bone.size(); ++solid) {
        const int bone = solid_to_bone[solid];
        if (bone < 0 || static_cast<std::size_t>(bone) >= world.size()
            || solid * 3 + 2 >= pos.size() || solid * 3 + 2 >= ang.size()) {
            continue;
        }
        const float at[3]{pos[solid * 3] - origin[0], pos[solid * 3 + 1] - origin[1],
                          pos[solid * 3 + 2] - origin[2]};
        const float angles[3]{ang[solid * 3], ang[solid * 3 + 1], ang[solid * 3 + 2]};
        world[static_cast<std::size_t>(bone)] = matrix_from_source_angles(at, angles);
        driven[static_cast<std::size_t>(bone)] = true;
    }
    poser_.palette_from_world(world, driven, palette);
}

} // namespace gmod::source::model
