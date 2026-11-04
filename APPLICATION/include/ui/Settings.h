#pragma once

#include <string>

namespace gmod::ui {

// The viewer's own options, edited from the Esc menu and kept between runs in
// %APPDATA%\GModReplay\settings.ini.
struct ViewerSettings {
    bool names = true;                // floating name tags
    bool names_through_walls = false; // effective only with player outlines enabled
    bool outline_all_players = false; // outline every player, not just the selected one
    bool steam_names = true;          // Steam profile name above the character name
    bool car_speed = true;            // speedometer and speed on driver tags
    bool speed_mph = false;           // mph instead of km/h
    bool kill_feed = true;
    bool radio_hud = true;
    bool dialogs = true;              // NPC dialogue boxes
    bool decals = true;               // bullet holes, scorch marks, blood
    bool muzzle_flash = true;
    bool reflections = false;        // planar water reflections
    bool lighting = true;            // off: fullbright world and models
    bool bank_raid_lighting = false;  // replay baked bank normal/raid light switches
    int volume = 100;                 // 0..100
    int cars_volume = 100;
    int voices_volume = 100;
    int gunshots_volume = 100;
    int doors_volume = 100;
    int alarms_volume = 100;
    int ambient_volume = 100;
    int other_volume = 100;
    bool esp_through_walls() const { return outline_all_players && names_through_walls; }
    bool cache_gpu_assets = false;    // retain visited maps/renderers, assets and pristine parsed demos for this session
    bool cache_demos_on_disk = false;
    static constexpr int min_demo_cache_gb = 1;
    static constexpr int max_demo_cache_gb = 100;
    int demo_cache_gb = 25;           // disk budget in GiB; least-recently-used recordings are evicted first
    std::wstring demo_folder;         // folder the demo browser shows; empty = find Garry's Mod

    void load();
    bool save() const;
};

ViewerSettings& settings();

} // namespace gmod::ui
