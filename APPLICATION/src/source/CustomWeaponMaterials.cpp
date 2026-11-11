#include "source/CustomWeaponMaterials.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace gmod::source {
namespace {

constexpr std::string_view prefix = "__recorder_weapon__/";

std::string normalize(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::filesystem::path path(name);
    if (path.is_absolute() || path.has_root_name()
        || name.find(':') != std::string::npos) {
        throw std::runtime_error("Custom weapon material path must be Source-relative: " + name);
    }
    for (const auto& part : path)
        if (part == "..")
            throw std::runtime_error("Custom weapon material path escapes its content root: " + name);
    return name;
}

} // namespace

bool weapon_material_visible(WeaponMaterialVariant variant, int owner, int recorder) {
    if (variant == WeaponMaterialVariant::Standard) return true;
    if (owner < 0 || recorder < 0) return false;
    return (owner == recorder) == (variant == WeaponMaterialVariant::Recorder);
}

CustomWeaponMaterials::CustomWeaponMaterials(const std::filesystem::path& gmod) {
    if (gmod.empty()) return;
    const auto root = gmod / L"garrysmod" / L"data" / L"phcustomtextures";
    if (!std::filesystem::is_directory(root)) return;
    for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
        if (file.is_regular_file() && normalize(file.path().extension().string()) == ".vmt") {
            material_root_ = root;
            break;
        }
    }
    if (!enabled()) return;
    const auto addons = gmod / L"garrysmod" / L"addons";
    if (!std::filesystem::is_directory(addons)) return;
    for (const auto& addon : std::filesystem::directory_iterator(addons)) {
        if (addon.is_directory() && std::filesystem::is_directory(addon.path() / L"materials"))
            texture_roots_.push_back(addon.path());
    }
    std::sort(texture_roots_.begin(), texture_roots_.end());
}

std::filesystem::path CustomWeaponMaterials::find_override(const std::string& material) const {
    if (!enabled()) return {};
    auto name = normalize(material);
    if (name.starts_with("materials/")) name.erase(0, 10);
    if (!name.ends_with(".vmt")) name += ".vmt";
    const auto path = material_root_ / std::filesystem::path(name);
    return vfs::exists(path) ? path : std::filesystem::path{};
}

std::filesystem::path CustomWeaponMaterials::find_file(const std::string& source_relative) const {
    if (!enabled()) return {};
    const auto name = normalize(source_relative);
    if (!name.starts_with("materials/")) return {};
    if (name.ends_with(".vmt")) {
        const auto override = find_override(name);
        if (!override.empty()) return override;
    }
    for (const auto& root : texture_roots_) {
        const auto path = root / std::filesystem::path(name);
        if (vfs::exists(path)) return path;
    }
    return {};
}

std::string CustomWeaponMaterials::material_key(const std::string& material) {
    return std::string(prefix) + material;
}

bool CustomWeaponMaterials::is_custom(std::string_view material) {
    return material.starts_with(prefix);
}

std::string CustomWeaponMaterials::original_name(const std::string& material) {
    return is_custom(material) ? material.substr(prefix.size()) : material;
}

} // namespace gmod::source
