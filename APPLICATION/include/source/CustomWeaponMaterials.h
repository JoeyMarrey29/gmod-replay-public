#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gmod::source {

enum class WeaponMaterialVariant { Standard, Recorder, OtherPlayers };

bool weapon_material_visible(WeaponMaterialVariant variant, int owner, int recorder);

class CustomWeaponMaterials {
public:
    explicit CustomWeaponMaterials(const std::filesystem::path& gmod);

    bool enabled() const noexcept { return !material_root_.empty(); }
    std::filesystem::path find_override(const std::string& material) const;
    std::filesystem::path find_file(const std::string& source_relative) const;

    static std::string material_key(const std::string& material);
    static bool is_custom(std::string_view material);
    static std::string original_name(const std::string& material);

private:
    std::filesystem::path material_root_;
    std::vector<std::filesystem::path> texture_roots_;
};

} // namespace gmod::source
