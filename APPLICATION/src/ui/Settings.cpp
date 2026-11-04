#include "ui/Settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace gmod::ui {
namespace {

std::filesystem::path settings_path() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(buffer) / L"GModReplay" / L"settings.ini";
}

} // namespace

void ViewerSettings::load() {
    const auto path = settings_path();
    if (path.empty()) return;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const int value = std::atoi(line.c_str() + equals + 1);
        const bool flag = value != 0;
        if (key == "names") names = flag;
        else if (key == "names_through_walls") names_through_walls = flag;
        else if (key == "outline_all_players") outline_all_players = flag;
        else if (key == "steam_names") steam_names = flag;
        else if (key == "car_speed") car_speed = flag;
        else if (key == "speed_mph") speed_mph = flag;
        else if (key == "kill_feed") kill_feed = flag;
        else if (key == "radio_hud") radio_hud = flag;
        else if (key == "dialogs") dialogs = flag;
        else if (key == "decals") decals = flag;
        else if (key == "muzzle_flash") muzzle_flash = flag;
        else if (key == "reflections") reflections = flag;
        else if (key == "lighting") lighting = flag;
        else if (key == "bank_raid_lighting") bank_raid_lighting = flag;
        else if (key == "volume") volume = std::clamp(value, 0, 100);
        else if (key == "cars_volume") cars_volume = std::clamp(value, 0, 100);
        else if (key == "voices_volume") voices_volume = std::clamp(value, 0, 100);
        else if (key == "gunshots_volume") gunshots_volume = std::clamp(value, 0, 100);
        else if (key == "doors_volume") doors_volume = std::clamp(value, 0, 100);
        else if (key == "alarms_volume") alarms_volume = std::clamp(value, 0, 100);
        else if (key == "ambient_volume") ambient_volume = std::clamp(value, 0, 100);
        else if (key == "other_volume") other_volume = std::clamp(value, 0, 100);
        else if (key == "cache_gpu_assets") cache_gpu_assets = flag;
        else if (key == "cache_demos_on_disk") cache_demos_on_disk = flag;
        else if (key == "demo_cache_gb")
            demo_cache_gb = std::clamp(value, min_demo_cache_gb, max_demo_cache_gb);
        else if (key == "demo_folder") {
            std::string text = line.substr(equals + 1);
            if (!text.empty() && text.back() == '\r') text.pop_back();
            const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            demo_folder.assign(static_cast<std::size_t>(std::max(size, 0)), L'\0');
            if (size > 0) {
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), demo_folder.data(), size);
            }
        }
    }
}

bool ViewerSettings::save() const {
    const auto path = settings_path();
    if (path.empty()) {
        std::cerr << "Could not save viewer settings: APPDATA is unavailable.\n";
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        std::cerr << "Could not create viewer settings directory: " << error.message() << "\n";
        return false;
    }
    std::ofstream output(path, std::ios::trunc);
    output << "names=" << names << "\nnames_through_walls=" << names_through_walls
           << "\noutline_all_players=" << outline_all_players
           << "\nsteam_names=" << steam_names << "\ncar_speed=" << car_speed
           << "\nspeed_mph=" << speed_mph << "\nkill_feed=" << kill_feed
           << "\nradio_hud=" << radio_hud << "\ndialogs=" << dialogs
           << "\ndecals=" << decals << "\nmuzzle_flash=" << muzzle_flash
           << "\nreflections=" << reflections << "\nlighting=" << lighting
           << "\nbank_raid_lighting=" << bank_raid_lighting
           << "\nvolume=" << volume
           << "\ncars_volume=" << cars_volume
           << "\nvoices_volume=" << voices_volume
           << "\ngunshots_volume=" << gunshots_volume
           << "\ndoors_volume=" << doors_volume
           << "\nalarms_volume=" << alarms_volume
           << "\nambient_volume=" << ambient_volume
           << "\nother_volume=" << other_volume
           << "\ncache_gpu_assets=" << cache_gpu_assets
           << "\ncache_demos_on_disk=" << cache_demos_on_disk
           << "\ndemo_cache_gb=" << demo_cache_gb << "\n";
    if (!demo_folder.empty()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, demo_folder.data(), static_cast<int>(demo_folder.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(std::max(size, 0)), '\0');
        if (size > 0) {
            WideCharToMultiByte(CP_UTF8, 0, demo_folder.data(), static_cast<int>(demo_folder.size()),
                                utf8.data(), size, nullptr, nullptr);
        }
        output << "demo_folder=" << utf8 << "\n";
    }
    output.flush();
    if (!output) {
        std::cerr << "Could not save viewer settings: " << path.string() << "\n";
        return false;
    }
    return true;
}

ViewerSettings& settings() {
    static ViewerSettings instance = [] {
        ViewerSettings loaded;
        loaded.load();
        return loaded;
    }();
    return instance;
}

} // namespace gmod::ui
