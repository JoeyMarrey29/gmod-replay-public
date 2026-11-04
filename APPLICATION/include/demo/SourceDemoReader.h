#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gmod::demo {

struct SourceDemoHeader {
    std::string magic;
    int demo_protocol = 0;
    int network_protocol = 0;
    std::string server_name;
    std::string client_name;
    std::string map_name;
    std::string game_directory;
    float playback_time = 0.0f;
    int playback_ticks = 0;
    int playback_frames = 0;
    int signon_length = 0;
};

// The recording client's view (democmdinfo_t) at one packet.
struct SourceDemoCameraSample {
    int tick = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

struct SourceDemoScan {
    SourceDemoHeader header;
    float tick_interval = 0.0f;  // seconds per server tick, 0 when unknown
    std::vector<SourceDemoCameraSample> camera;
    std::uint64_t packet_count = 0;
    std::uint64_t packet_bytes = 0;
    int last_content_tick = 0;
    std::string tail_recovery_message;
};

// Validates the protocol-3 header and command framing without buffering packet bodies.
SourceDemoScan scan_source_demo(
    const std::filesystem::path& path,
    const std::atomic<bool>& cancel);

// Uses the declared duration when it agrees with the last recorded command. Some GMod demos
// have a dem_stop tick far beyond their actual content, which otherwise creates a huge empty tail.
float effective_demo_duration(const SourceDemoScan& scan, float tick_rate);

} // namespace gmod::demo
