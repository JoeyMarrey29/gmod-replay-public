#include "source/model/PlayerAnimator.h"
#include "source/Vfs.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <utility>

int run_tests(int argc, char** argv) {
    if (argc == 5 && std::string_view(argv[1]) == "--attachment") {
        if (!gmod::source::vfs::mount(argv[4]))
            throw std::runtime_error("Could not mount attachment fixture archive.");
        using namespace gmod::source::model;
        const AnimationModel animation(argv[2], {});
        const auto parent = read_bones(argv[3]);
        const auto& bones = animation.bones();
        if (bones.size() != 2 || bones[0].parent != -1 || bones[1].parent != 0
            || animation.find_sequence("idle_down") != 1)
            throw std::runtime_error("Expected a two-bone folding sight fixture.");
        const auto anchor = std::find_if(parent.begin(), parent.end(),
            [&](const auto& bone) { return bone.name == bones[0].name; });
        if (anchor == parent.end()) throw std::runtime_error("Weapon has no sight mounting bone.");
        const auto inverse_point = [](const float* matrix, const std::array<float, 3>& point) {
            std::array<float, 3> result{};
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    result[row] += matrix[column * 4 + row] * (point[column] - matrix[column * 4 + 3]);
            return result;
        };
        const auto transform_point = [](const Matrix3x4& matrix, const std::array<float, 3>& point) {
            std::array<float, 3> result{};
            for (int row = 0; row < 3; ++row) {
                result[row] = matrix[row * 4 + 3];
                for (int column = 0; column < 3; ++column)
                    result[row] += matrix[row * 4 + column] * point[column];
            }
            return result;
        };
        const auto baseline = bonemerged_attachment_palette(parent, bones, {});
        for (int sequence : {0, 1}) {
            std::vector<BonePose> pose;
            animation.sample(sequence,
                std::vector<float>(animation.pose_parameter_count(), 0.0f), 0.0f, pose);
            std::vector<Matrix3x4> palette;
            SkeletonPoser(bones, animation).palette(pose, palette);
            const auto merged = bonemerged_attachment_palette(parent, bones, palette);
            for (std::size_t i = 0; i < 12; ++i) {
                if (std::abs(merged[0][i] - baseline[0][i]) > 0.0001f)
                    throw std::runtime_error("Sight animation moved its bonemerged mounting bone.");
            }
            for (bool tip : {false, true}) {
                const std::array<float, 3> local{0.0f, tip ? 1.0f : 0.0f, 0.0f};
                const auto point = inverse_point(bones[1].pose_to_bone, local);
                const auto actual = transform_point(merged[1], point);
                std::array<float, 3> expected_local{pose[1].pos[0], pose[1].pos[1], pose[1].pos[2]};
                if (tip) expected_local[sequence == 0 ? 1 : 2] += 1.0f;
                const auto expected = inverse_point(anchor->pose_to_bone, expected_local);
                for (int axis = 0; axis < 3; ++axis) {
                    if (!std::isfinite(actual[axis]) || std::abs(actual[axis] - expected[axis]) > 0.0001f)
                        throw std::runtime_error("Sight must fold 90 degrees around its fixed mounting hinge.");
                }
            }
            pose[0].pos[0] += 10.0f;
            pose[0].quat[0] = pose[0].quat[1] = pose[0].quat[2] = 0.0f;
            pose[0].quat[3] = 1.0f;
            SkeletonPoser(bones, animation).palette(pose, palette);
            const auto moved_root = bonemerged_attachment_palette(parent, bones, palette);
            for (std::size_t bone = 0; bone < merged.size(); ++bone) {
                for (std::size_t i = 0; i < 12; ++i) {
                    if (std::abs(merged[bone][i] - moved_root[bone][i]) > 0.0001f)
                        throw std::runtime_error("Bonemerged geometry inherited the attachment root animation.");
                }
            }
        }
        std::cout << "PASS: fixed sight mount, 90-degree hinge fold, and independent root animation\n";
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--doll") {
        if (!gmod::source::vfs::mount(argv[3]))
            throw std::runtime_error("Could not mount doll fixture archive.");
        const gmod::source::model::AnimationModel doll(argv[2], {});
        for (int sequence = 3; sequence <= 7; ++sequence) {
            if (!doll.sequence_has_activity(sequence, "ACT_VM_FIDGET")
                || doll.sequence_has_activity(sequence, "ACT_VM_PRIMARYATTACK"))
                throw std::runtime_error("Every elf fidget variant must match its recorded activity.");
        }
        if (!doll.sequence_has_activity(2, "ACT_VM_PRIMARYATTACK")
            || doll.sequence_has_activity(-1, "ACT_VM_FIDGET")
            || doll.sequence_has_activity(999999, "ACT_VM_FIDGET"))
            throw std::runtime_error("Doll shake and invalid sequence activity checks failed.");
        std::cout << "PASS: elf shake and all five fidget activities, including recorded sequence 4\n";
        return 0;
    }
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: gmod-replay-animation-tests <vanilla_anims.mdl> <vanilla_anims.ani> [archive.gma]\n";
        return 1;
    }
    if (argc == 4 && !gmod::source::vfs::mount(argv[3]))
        throw std::runtime_error("Could not mount animation fixture archive.");
    using namespace gmod::source::model;
    PlayerAnimator animator(argv[1],argv[2]);
    const AnimationModel animation(argv[1],argv[2]);
    const SkeletonPoser poser(animation.bones(),animation);
    {
        PlayerAnimator::Input input;
        input.hold = "pistol";
        input.hold_reload = false;
        PlayerAnimator::State state;
        std::vector<Matrix3x4> idle, reloading, expired;
        animator.update(state,input,0,idle);
        input.reload_elapsed = 0.3f;
        animator.update(state,input,0,reloading);
        input.reload_elapsed = 30;
        animator.update(state,input,0,expired);
        if (reloading == idle || expired != idle)
            throw std::runtime_error("Recorded one-shot reload must animate then restore idle.");
        std::cout << "PASS: recorded pistol reload gesture animates and expires without a live reload flag\n";
    }
    for (const auto& [driver,sequence] : {
            std::pair{false,"sit"}, std::pair{true,"drive_jeep"}}) {
        const int index = animation.find_sequence(sequence);
        if (index < 0) throw std::runtime_error(std::string("Missing seating sequence: ") + sequence);
        std::vector<BonePose> pose;
        animation.sample(index,std::vector<float>(animation.pose_parameter_count(),0.0f),0.0f,pose);
        std::vector<Matrix3x4> expected,actual;
        poser.palette(pose,expected);
        PlayerAnimator::State state;
        PlayerAnimator::Input input;
        input.seated = true;
        input.driver = driver;
        animator.update(state,input,0,actual);
        if (actual != expected) throw std::runtime_error(std::string("Wrong seating pose: ") + sequence);
        std::cout << "PASS: seating uses " << sequence << "\n";
    }
    for (const char* hold : {"fist","melee","melee2","knife","grenade"}) {
        PlayerAnimator::State state;
        PlayerAnimator::Input input;
        input.hold = hold;
        std::vector<Matrix3x4> idle,attack,repeated,expired;
        animator.update(state,input,0,idle);
        input.fire_elapsed = 0.12f;
        animator.update(state,input,0,attack);
        animator.update(state,input,0,repeated);
        input.fire_elapsed = 5;
        animator.update(state,input,0,expired);
        if (idle.size() != attack.size() || idle.empty() || repeated != attack)
            throw std::runtime_error("Attack palette is missing or nondeterministic.");
        float difference = 0;
        for (std::size_t bone = 0; bone < idle.size(); ++bone) {
            for (std::size_t value = 0; value < 12; ++value) {
                if (!std::isfinite(attack[bone][value]))
                    throw std::runtime_error("Attack produced a nonfinite bone transform.");
                difference = std::max(difference,std::abs(attack[bone][value]-idle[bone][value]));
                if (std::abs(expired[bone][value]-idle[bone][value]) > 0.0001f)
                    throw std::runtime_error("Expired attack did not restore the idle pose.");
            }
        }
        if (difference < 0.01f) throw std::runtime_error(std::string("Missing attack motion for ") + hold);
        std::cout << "PASS: " << hold << " attack moves real model bones, max_delta=" << difference
            << ", deterministic pause and expiry\n";
    }
    return 0;
}

int main(int argc, char** argv) {
    try {
        return run_tests(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
