#pragma once

#include "source/model/VvdReader.h"

#include <array>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace gmod::source::model {

struct StaticPropMeshVertex {
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    // Packed skinning (see pack_skin in the importer); zero when unskinned.
    std::uint32_t skin_bones = 0;
    std::uint32_t skin_weights = 0;
};

struct StaticPropMeshBatch {
    std::string material;
    std::size_t vertex_offset = 0;
    std::size_t vertex_count = 0;
    std::size_t index_offset = 0;
    std::size_t index_count = 0;
    std::vector<std::string> material_candidates;
};

struct StaticPropMeshResult {
    std::string model_path;
    std::vector<StaticPropMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<StaticPropMeshBatch> batches;
    std::size_t mesh_count = 0;
    std::size_t triangle_count = 0;
};

// Which model each bodygroup shows. `body` is the engine's packed entity body
// value; `overrides` pairs a bodypart index with an explicit model choice.
struct BodySelection {
    int body = 0;
    std::vector<std::pair<std::size_t, int>> overrides;
    std::vector<std::pair<std::string, int>> named_overrides;
    // Bake sequence 0's weighted per-bone idle pose into the mesh.
    bool apply_idle_pose = false;
    // Pushes every vertex out along its normal (source units). Layered clothing
    // uses this so the body mesh cannot poke through between its vertices.
    float normal_offset = 0.0f;
    // Additional clearance for named bodygroups, applied before skinning.
    std::vector<std::pair<std::string, float>> bodygroup_normal_offsets;
    // Lower-case bone names of the player skeleton this mesh is skinned to.
    // Each vertex's bone ids are remapped to indices in this list by bone name.
    const std::vector<std::string>* skeleton = nullptr;
    // Row-major 3x4 skinning matrices (out = R * v + t) indexed by this model's own bone ids.
    // Each vertex is posed by its bone weights, baking an animated pose into the mesh.
    const std::vector<std::array<float, 12>>* bone_matrices = nullptr;
    // Material replacements (original path -> replacement path, both lower-case, no
    // "materials/" prefix or extension), like Entity:SetSubMaterial for clothing skins.
    const std::vector<std::pair<std::string, std::string>>* material_remap = nullptr;
    // Bones that exist on the player model but not in the animation skeleton (hat, shades,
    // earlobes...) follow the listed skeleton bone, as they do on the player model itself.
    const std::vector<std::pair<std::string, std::string>>* bone_aliases = nullptr;
    // Skeleton bone that bones with no counterpart follow (weapon models that carry only
    // their own root bone are held by the hand).
    std::string fallback_bone;
};

struct ModelBodyGroup {
    std::string name;
    int models = 0;
    int base = 1;
};

std::vector<ModelBodyGroup> read_bodygroups(const std::filesystem::path& mdl);

class StaticPropMeshImporter {
public:
    StaticPropMeshResult read(
        const std::string& model_path,
        const std::filesystem::path& mdl,
        const std::filesystem::path& vvd,
        const std::filesystem::path& vtx,
        int skin = 0,
        const BodySelection& selection = {}) const;
};

} // namespace gmod::source::model
