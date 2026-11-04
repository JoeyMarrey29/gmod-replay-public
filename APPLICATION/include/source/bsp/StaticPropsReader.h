#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gmod::source::bsp {

struct StaticProp {
    std::string model;
    float origin[3]{};
    float angles[3]{};
    int skin = 0;
    // Entity-only bodygroup state (static props always use model 0).
    int body = 0;
    int hardware = 0;
    bool is_door = false;
    float door_distance = 90.0f;
    float door_speed = 100.0f;
    int open_dir = 1;
    // Demo playback: what drives this instance (0 static, 1 player, 2 prop, 3 door,
    // 4 clothing) and the index into the demo's list of that kind.
    int actor_kind = 0;
    int actor_index = -1;
    bool start_hidden = false;
    // Player actors: index into the demo's model table.
    int actor_model = -1;
    // Clothing actors: Perpheads entry and player body type.
    int actor_entry = -1;
    bool actor_female = false;
    std::string actor_weapon;  // held weapons: the class name the player must be holding
    int actor_attachment = -1;
    int actor_sequence = 0;
    std::string actor_parent_model;
    // Skinned actors: bone palette slot when it is not the player's own (view models).
    int actor_palette = -1;
    float fade_min = 0.0f;
    // Source hides a static prop beyond this distance; <= 0 means never fade.
    float fade_max = 0.0f;
};

struct StaticPropsSummary {
    std::size_t model_count = 0;
    std::size_t prop_count = 0;
    std::size_t invalid_model_references = 0;
};

class StaticPropsReader {
public:
    explicit StaticPropsReader(std::filesystem::path path);

    StaticPropsSummary read();
    const std::vector<StaticProp>& props() const noexcept;

private:
    std::filesystem::path path_;
    std::vector<StaticProp> props_;
};

} // namespace gmod::source::bsp
