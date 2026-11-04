#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace gmod::audio {

enum class SoundCategory { Cars, Voices, Gunshots, Doors, Alarms, Ambient, Other, Count };

// Recorded effects use resolved sound paths. Alarms take precedence over vehicle
// paths so a car's siren can be adjusted independently of its engine.
inline SoundCategory sound_category(std::string path, bool ambient = false, bool gunshot = false) {
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    const auto has = [&](std::string_view text) { return path.find(text) != std::string::npos; };
    if (has("alarm") || has("siren")) return SoundCategory::Alarms;
    if (ambient) return SoundCategory::Ambient;
    if (gunshot || has("weapons/") || has("gunshot")) return SoundCategory::Gunshots;
    if (has("doors/") || has("door") || has("shutter") || has("garage")) return SoundCategory::Doors;
    if (has("vehicles/") || has("vehicle/") || has("cars/") || has("car/")
        || has("engine") || has("tire") || has("tyre") || has("horn")) return SoundCategory::Cars;
    if (has("ambient/") || has("ambience/") || has("ambient_")) return SoundCategory::Ambient;
    return SoundCategory::Other;
}

} // namespace gmod::audio
