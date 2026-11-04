#include "source/model/Animation.h"
#include "source/model/StaticPropMeshImporter.h"
#include "demo/ModelIdentifiers.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using namespace gmod::source::model;
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--identifiers") {
            const std::filesystem::path content(argv[2]);
            const StaticPropMeshImporter importer;
            const auto read = [&](const std::filesystem::path& path, const BodySelection& selection) {
                auto vvd = path, vtx = path;
                vvd.replace_extension(".vvd"); vtx.replace_extension(".dx90.vtx");
                return importer.read(path.string(), path, vvd, vtx, 0, selection);
            };
            const std::vector<std::pair<std::string,std::string>> digits{
                {"models/jarheads/government/police/badge/n1", "models/jarheads/government/police/badge/n0"},
                {"models/jarheads/government/police/badge/n2", "models/jarheads/government/police/badge/n7"},
                {"models/jarheads/government/police/badge/n3", "models/jarheads/government/police/badge/n0"},
                {"models/jarheads/government/police/badge/n4", "models/jarheads/government/police/badge/n9"}};
            for (const auto* sex : {"male", "female"}) {
                const auto mdl = content / "models" / "perpheads" / "government" / sex / "Police_Kevlar.mdl";
                BodySelection visible, hidden;
                visible.named_overrides = {{"badge_num",0}};
                visible.material_remap = &digits;
                hidden.named_overrides = {{"badge_num",1}};
                const auto shown = read(mdl, visible), blank = read(mdl, hidden);
                visible.bodygroup_normal_offsets = {{"badge_num",0.25f}};
                const auto raised = read(mdl, visible);
                if (raised.vertices.size() != shown.vertices.size() || raised.indices != shown.indices)
                    throw std::runtime_error("Badge clearance changed uniform topology.");
                for (std::size_t i = 0; i < shown.vertices.size(); ++i) {
                    const auto& original = shown.vertices[i];
                    const auto& adjusted = raised.vertices[i];
                    const float offset = i >= blank.vertices.size() ? 0.25f : 0.0f;
                    for (int axis = 0; axis < 3; ++axis)
                        if (std::fabs(adjusted.position[axis] - original.position[axis]
                                - original.normal[axis]*offset) > 0.0001f)
                            throw std::runtime_error("Badge clearance moved the sleeve or missed badge geometry.");
                }
                for (const auto& batch : shown.batches) {
                    if (!batch.material.starts_with("models/jarheads/government/police/badge/n")) continue;
                    const auto& vertex = shown.vertices.at(batch.vertex_offset);
                    if (vertex.position[1] >= -7 || vertex.position[2] < 55 || vertex.position[2] > 65)
                        throw std::runtime_error("Original badge digits are not on the uniform shoulder.");
                }
                if (shown.indices.size() <= blank.indices.size())
                    throw std::runtime_error("Badge bodygroup did not hide number geometry.");
                for (const auto& digit : {"n0", "n7", "n9"})
                    if (std::none_of(shown.batches.begin(),shown.batches.end(),[&](const auto& batch) {
                        return batch.material == "models/jarheads/government/police/badge/" + std::string(digit);
                    })) throw std::runtime_error("Real uniform badge digit was not replaced.");
                const auto groups = read_bodygroups(mdl);
                if (std::none_of(groups.begin(),groups.end(),[](const auto& group) {
                    return group.name == "badge_num" && group.models == 2;
                })) throw std::runtime_error("Original badge bodygroup missing.");
            }
            for (const auto& registration : {"PD-1234", "AB12"}) {
                const auto appearance = gmod::demo::plate_appearance({}, {"b",registration,""});
                BodySelection selection; selection.material_remap = &appearance.materials;
                const auto mesh = read(content / (appearance.model + ".mdl"), selection);
                if (mesh.indices.empty()) throw std::runtime_error("Original plate geometry missing.");
                for (const auto& [from,to] : appearance.materials)
                    if (std::none_of(mesh.batches.begin(),mesh.batches.end(),[&](const auto& batch) {
                        return batch.material == to;
                    })) throw std::runtime_error("Original plate glyph material not replaced.");
            }
            std::cout << "PASS: actual male/female uniform badge bodygroups, digit materials and odd/even plate meshes\n";
            return 0;
        }
        if (argc != 2) throw std::runtime_error("Usage: gmod-replay-model-pose-tests <door.mdl>");
        const std::filesystem::path mdl(argv[1]);
        auto vvd = mdl;
        auto vtx = mdl;
        vvd.replace_extension(L".vvd");
        vtx.replace_extension(L".dx90.vtx");
        const AnimationModel animation(mdl, {});
        if (animation.sequence_count() == 0) throw std::runtime_error("Fixture has no idle sequence.");
        std::vector<BonePose> idle;
        animation.sample(0, std::vector<float>(animation.pose_parameter_count(), 0.0f), 0, idle);
        std::vector<Matrix3x4> matrices;
        SkeletonPoser(animation.bones(), animation).palette(idle, matrices);
        BodySelection explicit_pose;
        explicit_pose.bone_matrices = &matrices;
        BodySelection automatic_pose;
        automatic_pose.apply_idle_pose = true;
        const StaticPropMeshImporter importer;
        const auto expected = importer.read(mdl.string(), mdl, vvd, vtx, 0, explicit_pose);
        const auto actual = importer.read(mdl.string(), mdl, vvd, vtx, 0, automatic_pose);
        if (actual.vertices.empty() || actual.vertices.size() != expected.vertices.size())
            throw std::runtime_error("Door idle geometry missing.");
        float min_x = actual.vertices.front().position[0], max_x = min_x;
        for (std::size_t i = 0; i < actual.vertices.size(); ++i) {
            const auto& vertex = actual.vertices[i];
            min_x = std::min(min_x, vertex.position[0]);
            max_x = std::max(max_x, vertex.position[0]);
            for (int axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis])
                    || std::abs(vertex.position[axis] - expected.vertices[i].position[axis]) > 0.0001f
                    || std::abs(vertex.normal[axis] - expected.vertices[i].normal[axis]) > 0.0001f)
                    throw std::runtime_error("Door mesh was not posed by each vertex's own bone.");
            }
        }
        if (mdl.filename() == L"cabin_door.mdl" && max_x - min_x < 47)
            throw std::runtime_error("Cabin door panel is sideways instead of spanning the closed doorway.");
        std::cout << "PASS: weighted per-bone door idle pose, width=" << max_x - min_x << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
