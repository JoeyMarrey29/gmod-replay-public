// Developer tool: parses a demo and prints what the viewer would play back.
#include "demo/DemoData.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: demo_info <demo.dem> [time]\n";
        return 2;
    }
    try {
        // build\demo_info.exe lives one level below the application root.
        const auto root = std::filesystem::absolute(argv[0]).parent_path().parent_path();
        const auto parser = gmod::demo::find_parser(root);
        std::atomic<bool> cancel{false};
        const auto start = std::chrono::steady_clock::now();
        float last = -1.0f;
        const auto data = gmod::demo::load_demo(
            argv[1], parser,
            [&](float percent, const std::string& text) {
                if (percent - last >= 10.0f || percent >= 100.0f) {
                    last = percent;
                    std::printf("  %5.1f%%  %s\n", percent, text.c_str());
                }
            },
            cancel);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::size_t samples = 0, keys = 0, door_keys = 0;
        for (const auto& player : data.players) samples += player.samples.size();
        for (const auto& prop : data.props) keys += prop.keys.size();
        for (const auto& door : data.doors) door_keys += door.keys.size();
        std::printf("map=%s duration=%.1fs tickrate=%.2f models=%zu\n", data.map_name.c_str(),
                    data.duration, data.tick_rate, data.models.size());
        std::printf("players=%zu samples=%zu | props=%zu keys=%zu | doors=%zu keys=%zu | total %.2fs\n",
                    data.players.size(), samples, data.props.size(), keys, data.doors.size(),
                    door_keys, seconds);
        const float time = argc > 2 ? static_cast<float>(std::atof(argv[2])) : data.duration * 0.5f;
        std::size_t visible = 0, props_visible = 0, open_doors = 0;
        for (const auto& player : data.players) {
            gmod::demo::PlayerPose pose;
            if (gmod::demo::sample_player(player, time, pose)) {
                if (visible < 6) {
                    std::printf("  player %-24.24s at (%.0f, %.0f, %.0f) yaw %.0f  %s\n",
                                player.name.c_str(), pose.x, pose.y, pose.z, pose.yaw,
                                pose.model >= 0 ? data.models[static_cast<std::size_t>(pose.model)].c_str() : "?");
                }
                ++visible;
            }
        }
        for (const auto& prop : data.props) {
            gmod::demo::PropPose pose;
            props_visible += gmod::demo::sample_prop(prop, time, pose) ? 1 : 0;
        }
        for (const auto& door : data.doors) {
            open_doors += gmod::demo::door_openness(door, time, 0.9f) > 0.5f ? 1 : 0;
        }
        std::printf("at t=%.1fs: players visible=%zu, props=%zu, doors open=%zu of %zu\n", time,
                    visible, props_visible, open_doors, data.doors.size());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
