#include "demo/SourceDemoReader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr std::size_t HeaderSize = 1072;
constexpr std::size_t CommandInfoSize = 76;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void write_i32(std::ostream& output, std::int32_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    const std::array<char, 4> bytes{
        static_cast<char>(raw),
        static_cast<char>(raw >> 8),
        static_cast<char>(raw >> 16),
        static_cast<char>(raw >> 24),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_header(std::ostream& output) {
    std::array<char, HeaderSize> header{};
    constexpr std::array<char, 7> magic{'G', 'M', 'O', 'D', 'E', 'M', 'O'};
    std::copy(magic.begin(), magic.end(), header.begin());
    const auto write_i32_at = [&](std::size_t offset, std::int32_t value) {
        const auto raw = static_cast<std::uint32_t>(value);
        for (std::size_t index = 0; index < 4; ++index) {
            header[offset + index] = static_cast<char>(raw >> (index * 8));
        }
    };
    write_i32_at(8, 3);
    write_i32_at(12, 24);
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
}

void write_complete_packet(std::ostream& output) {
    output.put(1);
    write_i32(output, 1);
    const std::array<char, CommandInfoSize> info{};
    output.write(info.data(), static_cast<std::streamsize>(info.size()));
    write_i32(output, 0);
    write_i32(output, 0);
    write_i32(output, 0);
}

class TempDemo {
public:
    TempDemo()
        : path_(std::filesystem::temp_directory_path()
                / ("gmod-replay-source-reader-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()) + ".dem")) {}

    ~TempDemo() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void test_incomplete_final_payload() {
    TempDemo demo;
    {
        std::ofstream output(demo.path(), std::ios::binary);
        require(static_cast<bool>(output), "Could not create synthetic demo.");
        write_header(output);
        write_complete_packet(output);
        output.put(3);
        write_i32(output, 2);
        output.put(6);
        write_i32(output, 3);
        write_i32(output, 64);
        const std::array<char, 10> partial_payload{};
        output.write(partial_payload.data(), static_cast<std::streamsize>(partial_payload.size()));
    }

    const std::atomic<bool> cancel{false};
    const auto scan = gmod::demo::scan_source_demo(demo.path(), cancel);
    require(scan.packet_count == 1, "Complete packet was lost when the tail packet was discarded.");
    require(scan.camera.size() == 1, "Complete packet camera sample was lost.");
    require(scan.last_content_tick == 1, "Incomplete tail or sync tick extended the duration.");
    require(!scan.tail_recovery_message.empty(), "Incomplete tail was not reported.");
}

void test_missing_stop_record() {
    TempDemo demo;
    {
        std::ofstream output(demo.path(), std::ios::binary);
        require(static_cast<bool>(output), "Could not create synthetic demo.");
        write_header(output);
        write_complete_packet(output);
    }

    const std::atomic<bool> cancel{false};
    const auto scan = gmod::demo::scan_source_demo(demo.path(), cancel);
    require(scan.packet_count == 1, "Complete packet was lost when dem_stop was missing.");
    require(!scan.tail_recovery_message.empty(), "Missing dem_stop was not reported.");
}

void test_effective_duration_uses_content_when_header_clock_disagrees() {
    for (const float rate : {20.0f, 25.0f, 33.333333f, 66.666667f, 100.0f}) {
        gmod::demo::SourceDemoScan recording;
        recording.last_content_tick = 12345;
        recording.header.playback_ticks = recording.last_content_tick;
        recording.header.playback_time = recording.last_content_tick / rate;
        const float expected = recording.header.playback_time;
        require(std::fabs(gmod::demo::effective_demo_duration(recording, rate) - expected) < 0.01f,
            "Demo duration assumed a fixed tick rate.");
        recording.header.playback_ticks *= 4;
        recording.header.playback_time *= 4;
        require(std::fabs(gmod::demo::effective_demo_duration(recording, rate) - expected) < 0.01f,
            "Stale header correction assumed a fixed tick rate.");
        recording.header.playback_time = 0.0f;
        require(std::fabs(gmod::demo::effective_demo_duration(recording, rate) - expected) < 0.01f,
            "Unclosed demo duration assumed a fixed tick rate.");
    }
    gmod::demo::SourceDemoScan scan;
    scan.header.playback_time = 15463.89f;
    scan.header.playback_ticks = 1030926;
    scan.last_content_tick = 57969;
    const float actual = gmod::demo::effective_demo_duration(scan, 20.0f);
    require(std::fabs(actual - 2898.45f) < 0.02f,
        "A stale stop tick extended the demo beyond its recorded content.");

    scan.header.playback_time = 3598.25f;
    scan.header.playback_ticks = 71965;
    scan.last_content_tick = 71965;
    require(std::fabs(gmod::demo::effective_demo_duration(scan, 20.0f) - 3598.25f) < 0.001f,
        "A consistent declared duration was unexpectedly changed.");

    scan.header.playback_time = 0;
    require(std::fabs(gmod::demo::effective_demo_duration(scan, 20.0f) - 3598.25f) < 0.001f,
        "An unclosed demo lost its recorded duration.");
    scan.header.playback_time = std::numeric_limits<float>::quiet_NaN();
    require(std::isfinite(gmod::demo::effective_demo_duration(scan,20.0f)),
        "A nonfinite header duration reached playback.");
    scan.header.playback_time = 3598.25f;
    require(gmod::demo::effective_demo_duration(scan,0.0f) == scan.header.playback_time,
        "An unknown clock discarded the available header duration.");
    scan.last_content_tick = 0;
    require(gmod::demo::effective_demo_duration(scan,20.0f) == scan.header.playback_time,
        "A missing content tick discarded the available header duration.");
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--duration") {
            const std::atomic<bool> cancel{false};
            const auto scan = gmod::demo::scan_source_demo(argv[2], cancel);
            const float tick_rate = scan.tick_interval > 0.0f
                ? 1.0f / scan.tick_interval
                : scan.header.playback_ticks > 0 && scan.header.playback_time > 0.0f
                    ? static_cast<float>(scan.header.playback_ticks) / scan.header.playback_time
                    : 0.0f;
            std::cout << "header_duration=" << scan.header.playback_time
                << " content_tick=" << scan.last_content_tick
                << " tick_rate=" << tick_rate
                << " effective_duration=" << gmod::demo::effective_demo_duration(scan, tick_rate)
                << "\n";
            return 0;
        }
        require(argc <= 2, "Usage: gmod-replay-source-demo-tests [<unclosed-demo.dem>]");
        test_effective_duration_uses_content_when_header_clock_disagrees();
        test_incomplete_final_payload();
        test_missing_stop_record();
        if (argc == 2) {
            const std::atomic<bool> cancel{false};
            const auto scan = gmod::demo::scan_source_demo(argv[1], cancel);
            require(scan.packet_count > 0, "Real demo had no complete packets.");
            require(!scan.tail_recovery_message.empty(), "Real unclosed demo tail was not recovered.");
            require(scan.last_content_tick > 0, "Complete content ticks were not retained.");
            std::cout << "PASS: recovered real demo tail: " << scan.tail_recovery_message << "\n";
        }
        std::cout << "PASS: native Source demo tail recovery\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
