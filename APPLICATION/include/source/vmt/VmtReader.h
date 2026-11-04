#pragma once

#include <filesystem>
#include <string>

namespace gmod::source::vmt {

struct Material {
    std::string shader;
    std::string base_texture;
    std::string base_texture2;
    std::string blend_modulate_texture;
    // Refract/water materials have no $basetexture; they use these instead.
    std::string normal_map;
    std::string dudv_map;
    bool water_above = true; // $abovewater 0 is the reverse side of a water surface
    // Source-relative path of a patch/include parent VMT (e.g. "materials/x.vmt").
    std::string include_path;
    std::string model_material;
    float decal_scale = 1.0f;
    bool translucent = false;
    bool alpha_test = false;
    // 0 when the VMT does not define $alphatestreference.
    float alpha_test_reference = 0.0f;
    bool additive = false;
    bool no_cull = false;
    // $surfaceprop: names the physical surface, which decides footstep sounds.
    std::string surface_prop;
    // The base texture's alpha masks where the entity colour tints the surface.
    bool blend_tint_by_base_alpha = false;
};

class VmtReader {
public:
    explicit VmtReader(std::filesystem::path path);
    Material read() const;

private:
    std::filesystem::path path_;
};

} // namespace gmod::source::vmt
