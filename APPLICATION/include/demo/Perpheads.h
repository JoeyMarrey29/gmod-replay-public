#pragma once

#include "demo/DemoData.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gmod::demo {

// Registries exported from the server's own Lua (tools\export\*.py), so nothing about
// clothing or jobs is guessed by the viewer.
struct ClothingSkinDef {
    std::vector<std::pair<std::string, std::string>> sub_materials;  // original -> replacement
    bool can_tint = false;
};

struct ClothingModelVariant {
    bool missing_feet = false;
    bool missing_thighs = false;
    std::string model;
};

struct ClothingType {
    int index = 0;
    std::string identifier;
    std::string model;
    bool accessory = false;
    bool government = false;
    bool body_part = false;
    bool attach_body_parts_for_slots = false;
    std::vector<int> slots;
    std::vector<int> optional_slots;
    std::vector<std::string> excluded_body_parts;
    std::vector<ClothingSkinDef> skins;
    std::vector<ClothingModelVariant> variants;
    std::string data_number_bodygroup;
    std::string data_number_material_base;
};

struct PlatePlacement {
    std::array<float, 3> pos{}, ang{};
};

struct VehiclePlateDef {
    struct Kit {
        std::vector<int> groups;
        std::optional<PlatePlacement> plate;
    };
    std::vector<PlatePlacement> placements;
    std::vector<Kit> kits;
};

struct PlateSkinDef {
    std::string material;
    std::array<float, 3> color{29.0f / 255, 36.0f / 255, 71.0f / 255};
};

struct JobTeam {
    int id = 0;
    std::string name;
    unsigned char color[3]{255, 255, 255};
};

struct RankTable {
    std::unordered_map<int, std::string> names;
    std::unordered_map<int, std::string> short_names;
};

struct GunSoundEntry {
    std::string name;
    std::vector<std::string> files;  // relative to sound/, lower-case
    float level = 120.0f;
    float volume = 1.0f;
};

struct SwepInfo {
    std::string name;   // SWEP.PrintName
    std::string base;   // SWEP.Base
    bool gun = false;   // derived from the firearm base (aiming/passive vars are valid)
    float iron_pos[3]{};
    float iron_ang[3]{};
    std::optional<std::array<float,3>> rail_pos;
    float rail_ang[3]{};
    float fov = 70.0f;  // SWEP.ViewModelFOV (the firearm base's default)
    bool flip = false;
    bool melee = false;
    float melee_primary_delay = 0, melee_secondary_delay = 0;
    GunSoundEntry melee_swing;
    GunSoundEntry doll_shake;
    GunSoundEntry doll_fidget;
};

struct WeaponAttachmentDef {
    int id = 0;
    int slot = 0;
    bool sight = false;
    int sequence = 0;
    int sight_sequence = 0;
    std::string name;
    std::string model;
    std::string sound;  // attachment GetSound override, empty for non-sound attachments
    float magnification = 0.0f;
    std::optional<std::array<float,3>> iron_pos;
    std::array<float,3> iron_ang{};
};

struct PerpheadsRegistry {
    std::unordered_map<int, ClothingType> clothing;
    std::vector<int> required_slots;
    std::unordered_map<int, JobTeam> teams;
    RankTable police;
    RankTable medic;
    RankTable firefighter;
    int team_police = 3;
    int team_medic = 4;
    int team_firefighter = 5;
    int team_dispatcher = 15;
    int team_citizen = 1;
    // Fixed government radio channels (id -> name).
    std::unordered_map<int, std::string> radio_channels;
    // NPC_DATABASE id (perpheads_npc_used) -> NPC.Name.
    std::unordered_map<int, std::string> npc_names;
    // SWEP first-person model -> the world model other players see (both normalized, no .mdl).
    std::unordered_map<std::string, std::string> swep_world_models;
    // SWEP first-person model -> its SWEP.Primary.Sound name (lower-case, e.g. "m1911_shoot").
    std::unordered_map<std::string, std::string> swep_fire_sounds;
    // SWEP first-person model -> script facts (name, base, iron sights, view model FOV).
    std::unordered_map<std::string, SwepInfo> swep_info;
    // GunSounds registration index (ph_bullet_sound) -> the sound (sh_gun_sounds.lua).
    std::unordered_map<int, GunSoundEntry> gun_sounds;
    std::unordered_map<int, WeaponAttachmentDef> weapon_attachments;
    std::unordered_map<std::string, VehiclePlateDef> vehicle_plates;
    std::unordered_map<std::string, PlateSkinDef> plate_skins;
    float gun_sound_distance = 4000.0f;     // GunSounds.MAX_BULLET_SOUND_DISTANCE
    float impact_sound_distance = 2000.0f;  // GunSounds.MAX_IMPACT_SOUND_DISTANCE

    bool empty() const { return clothing.empty(); }
};

// Reads clothing_types.json and perpheads_jobs.json from `directory`.
PerpheadsRegistry load_perpheads_registry(const std::filesystem::path& directory);
std::string weapon_sound_override_at(const PerpheadsRegistry& registry, const DemoData& demo,
                                     int handle, float time);
int weapon_attachment_sequence_at(const PerpheadsRegistry& registry, const DemoData& demo,
                                  int attachment, int handle, float time);

// One drawn piece of a character's outfit.
struct OutfitPart {
    std::string model;  // normalized, no extension
    int skin = 0;       // MDL skin family
    int type = 0;       // clothing type index
    int item_skin = 0;  // 1-based skin of the clothing type, 0 for bare body parts
    bool tint = false;
    unsigned char color[4]{255, 255, 255, 255};
    std::vector<std::pair<std::string, std::string>> sub_materials;
    int data_number = 0;
    std::string number_bodygroup;

    bool operator==(const OutfitPart& other) const {
        return model == other.model && skin == other.skin && type == other.type
            && item_skin == other.item_skin && tint == other.tint
            && data_number == other.data_number && number_bodygroup == other.number_bodygroup
            && sub_materials == other.sub_materials
            && std::equal(color, color + 4, other.color);
    }
};

// Applies the server's attachment rules (cl_clothing.lua): worn items first, then bare
// body parts for every required slot nothing covers.
std::vector<OutfitPart> compose_outfit(const PerpheadsRegistry& registry, const ClothingRecord& record);

// Job short title ("SGT", "Dispatcher", "Courier"...) at `time`; empty for citizens.
std::string job_title_at(const PerpheadsRegistry& registry, const DemoPlayer& player, float time);
// Team colour at `time` (citizen green when unknown).
void job_color_at(const PerpheadsRegistry& registry, const DemoPlayer& player, float time,
                  unsigned char rgb[3]);

} // namespace gmod::demo
