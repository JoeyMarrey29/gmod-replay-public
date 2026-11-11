#include "source/CustomWeaponMaterials.h"
#include "source/Vfs.h"
#include "source/vmt/VmtReader.h"
#include "source/vtf/VtfReader.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("gmod-replay-custom-weapons-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Fixture() { std::filesystem::remove_all(root); }

    void write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        require(static_cast<bool>(out), "Could not write custom weapon fixture.");
    }
};

} // namespace

int main() {
    try {
        using gmod::source::CustomWeaponMaterials;
        using gmod::source::WeaponMaterialVariant;
        using gmod::source::weapon_material_visible;
        Fixture fixture;
        require(!CustomWeaponMaterials({}).enabled(), "Missing game install must not enable overrides.");
        require(!CustomWeaponMaterials(fixture.root).enabled(), "Missing skin folder must leave materials unchanged.");
        const auto data = std::filesystem::path(L"garrysmod\\data\\phcustomtextures");
        const auto addon = std::filesystem::path(L"garrysmod\\addons\\any-skin-addon\\materials");
        const std::string gun = "models/weapons/v_models/as50/mag";
        const std::string attachment = "models/attachments/suppressor";
        fixture.write(data / (gun + ".vmt"),
            R"("VertexLitGeneric" { "$basetexture" "models/weapons/v_models/as50/mag_custom" "$alphatest" "1" })");
        fixture.write(data / (attachment + ".vmt"),
            R"("Patch" { "include" "materials/models/attachments/parent.vmt" "replace" { "$basetexture" "skins/suppressor" } })");
        fixture.write(addon / L"models\\attachments\\parent.vmt",
            R"("VertexLitGeneric" { "$basetexture" "skins/parent" })");
        // A real one-pixel RGBA VTF, under the renamed texture path from the guide.
        std::string vtf(84, '\0');
        std::memcpy(vtf.data(), "VTF\0", 4);
        const auto put = [&]<typename T>(std::size_t offset, T value) {
            std::memcpy(vtf.data() + offset, &value, sizeof(value));
        };
        put(4, std::int32_t{7});
        put(8, std::int32_t{2});
        put(12, std::int32_t{80});
        put(16, std::uint16_t{1});
        put(18, std::uint16_t{1});
        put(24, std::uint16_t{1});
        put(52, std::int32_t{0});
        vtf[56] = 1;
        put(57, std::int32_t{-1});
        vtf[80] = 12; vtf[81] = 34; vtf[82] = 56;
        vtf[83] = static_cast<char>(255);
        fixture.write(addon / L"models\\weapons\\v_models\\as50\\mag_custom.vtf", vtf);
        fixture.write(L"garrysmod\\materials\\models\\weapons\\v_models\\as50\\mag.vmt",
            R"("VertexLitGeneric" { "$basetexture" "original/mag" })");
        const CustomWeaponMaterials skins(fixture.root);
        require(skins.enabled(), "Installed VMTs must enable recorder material variants.");
        require(skins.find_override("Materials\\Models\\Weapons\\V_Models\\AS50\\Mag.VMT")
            == skins.find_override(gun), "Source material spelling must normalize.");
        require(skins.find_override(gun) == fixture.root / data / (gun + ".vmt"),
            "Override paths must mirror materials, without a nested materials directory.");
        require(skins.find_override("models/weapons/not_customized").empty(),
            "Uncustomized materials must use normal content.");
        const auto material = gmod::source::vmt::VmtReader(skins.find_override(gun)).read();
        require(material.alpha_test, "Custom VMT render settings must be preserved.");
        const auto texture = skins.find_file("materials/" + material.base_texture + ".vtf");
        require(texture == fixture.root / addon / L"models\\weapons\\v_models\\as50\\mag_custom.vtf",
            "Renamed textures must resolve from arbitrary loose addon names.");
        const auto pixels = gmod::source::vtf::VtfReader(texture).read_top_mip();
        require(pixels.width == 1 && pixels.height == 1
            && pixels.rgba == std::vector<std::uint8_t>{12,34,56,255},
            "The selected custom texture must decode, not just exist.");
        const auto patch = gmod::source::vmt::VmtReader(skins.find_override(attachment)).read();
        require(!patch.include_path.empty() && !skins.find_file(patch.include_path).empty(),
            "Attachment patch parents must resolve from the skin addon.");
        require(skins.find_file("materials/missing.vtf").empty(),
            "Missing custom textures must remain missing for standard fallback/logging.");
        require(skins.find_file("models/weapon.mdl").empty(),
            "Custom materials must not mount model or other game overrides.");
        const auto normal = gmod::source::vmt::VmtReader(
            fixture.root / L"garrysmod\\materials\\models\\weapons\\v_models\\as50\\mag.vmt").read();
        require(normal.base_texture == "original/mag", "Standard materials must remain unchanged.");
        const auto custom_key = CustomWeaponMaterials::material_key(gun);
        require(custom_key != gun && CustomWeaponMaterials::is_custom(custom_key)
            && CustomWeaponMaterials::original_name(custom_key) == gun
            && CustomWeaponMaterials::original_name(gun) == gun,
            "Custom and standard material descriptors must not share a key.");
        for (const int recorder : {0, 3}) {
            for (const int owner : {0, 1, 3}) {
                require(weapon_material_visible(WeaponMaterialVariant::Recorder, owner, recorder)
                    == (owner == recorder), "Only the recorder may draw custom weapons.");
                require(weapon_material_visible(WeaponMaterialVariant::OtherPlayers, owner, recorder)
                    == (owner != recorder), "Spectated players must keep standard weapons.");
                require(weapon_material_visible(WeaponMaterialVariant::Recorder, owner, recorder)
                    != weapon_material_visible(WeaponMaterialVariant::OtherPlayers, owner, recorder),
                    "Shared view models and transferred back guns must draw exactly one variant after seeking.");
            }
        }
        require(!weapon_material_visible(WeaponMaterialVariant::Recorder, -1, 0)
            && !weapon_material_visible(WeaponMaterialVariant::Recorder, 0, -1)
            && weapon_material_visible(WeaponMaterialVariant::Standard, -1, -1),
            "Unknown ownership must never choose a recorder skin.");
        bool rejected = false;
        try { skins.find_override("../outside"); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Material paths must stay inside their content root.");
        std::cout << "PASS: recorder-only weapon/attachment variants, addon VTF decoding, patch parents, isolation and missing skins\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
