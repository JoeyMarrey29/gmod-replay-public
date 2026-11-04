#pragma once

#include <filesystem>
#include <map>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gmod::audio {

inline bool uses_wave_loop_metadata(std::string_view file) {
    return file.starts_with("doors/") || file.starts_with("buttons/") || file.starts_with("plats/");
}

inline std::optional<bool> wav_has_loop(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0
        || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return std::nullopt;
    const auto u32 = [&](std::size_t at) {
        return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8)
            | (std::uint32_t(bytes[at + 2]) << 16) | (std::uint32_t(bytes[at + 3]) << 24);
    };
    const std::uint64_t end = std::uint64_t(u32(4)) + 8;
    if (end > bytes.size() || end < 12) return std::nullopt;
    bool looping = false;
    for (std::size_t at = 12; at < end;) {
        if (end - at < 8) return std::nullopt;
        const std::size_t size = u32(at + 4);
        if (size > end - at - 8) return std::nullopt;
        const auto* tag = bytes.data() + at;
        if (std::memcmp(tag, "cue ", 4) == 0) {
            if (size < 4 || u32(at + 8) > (size - 4) / 24) return std::nullopt;
            looping |= u32(at + 8) > 0;
        } else if (std::memcmp(tag, "smpl", 4) == 0) {
            if (size < 36 || u32(at + 36) > (size - 36) / 24) return std::nullopt;
            looping |= u32(at + 36) > 0;
        }
        at += 8 + size + (size & 1);
        if (at > end) return std::nullopt;
    }
    return looping;
}

// Finds the sound files for a weapon's shot and for a surface's footsteps in the game's
// sound folders. Results are keys relative to those folders ("weapons/m1911/1911_fire1.mp3").
class SoundCatalog {
public:
    // `content_roots` are the extracted game roots; each may hold a `sound` folder.
    explicit SoundCatalog(const std::vector<std::filesystem::path>& content_roots);

    // Fire samples for a SWEP.Primary.Sound name such as "m1911_shoot" (any case).
    // Empty when no matching weapon folder holds a fire sound.
    std::vector<std::string> fire_sounds(const std::string& sound_name) const;

    // Step samples for a surface's step sound set ("concrete", "metalgrate", "default", ...).
    std::vector<std::string> footsteps(const std::string& step_set) const;

    bool empty() const { return folders_.empty(); }
    bool wave_loops(const std::string& file) const;

private:
    // lower-case folder ("weapons/m1911", "player/footsteps") -> file keys in it.
    std::map<std::string, std::vector<std::string>> folders_;
    std::vector<std::filesystem::path> roots_;
    mutable std::map<std::string, bool> loop_cache_;
};

} // namespace gmod::audio
